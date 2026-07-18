#include "primeparts/common/arrow_init.h"
#include "primeparts/scan/scan_planner.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iceberg/arrow/arrow_io_util.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/file_format.h"
#include "iceberg/file_io.h"
#include "iceberg/file_reader.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/snapshot.h"
#include "iceberg/sort_order.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"
#include "iceberg/util/uuid.h"

namespace {

std::shared_ptr<iceberg::Schema> TestSchema() {
  std::vector<iceberg::SchemaField> fields;
  fields.push_back(iceberg::SchemaField::MakeRequired(1, "p", iceberg::int64()));
  fields.push_back(iceberg::SchemaField::MakeRequired(2, "k", iceberg::int32()));
  return std::make_shared<iceberg::Schema>(std::move(fields), 0);
}

std::shared_ptr<iceberg::FileScanTask> TaskWithLowerBound(
    const std::string& path, std::optional<int64_t> p_lower) {
  auto df = std::make_shared<iceberg::DataFile>();
  df->file_path = path;
  if (p_lower) {
    auto ser = iceberg::Literal::Long(*p_lower).Serialize();
    df->lower_bounds[1] = ser.value();
  }
  return std::make_shared<iceberg::FileScanTask>(std::move(df));
}

std::shared_ptr<iceberg::Schema> SchemaWithId(int32_t schema_id) {
  std::vector<iceberg::SchemaField> fields;
  fields.push_back(iceberg::SchemaField::MakeRequired(1, "p", iceberg::int64()));
  fields.push_back(iceberg::SchemaField::MakeRequired(2, "k", iceberg::int32()));
  return std::make_shared<iceberg::Schema>(std::move(fields), schema_id);
}

std::shared_ptr<iceberg::TableMetadata> MetadataWithSnapshotSchema(
    int32_t snapshot_schema_id, int32_t current_schema_id) {
  auto meta = std::make_shared<iceberg::TableMetadata>();
  meta->format_version = 2;
  meta->table_uuid = "00000000-0000-0000-0000-000000000001";
  meta->location = "/nonexistent/table";
  meta->schemas = {SchemaWithId(0), SchemaWithId(1)};
  meta->current_schema_id = current_schema_id;
  meta->partition_specs = {iceberg::PartitionSpec::Unpartitioned()};
  meta->default_spec_id = 0;
  meta->sort_orders = {iceberg::SortOrder::Unsorted()};
  meta->default_sort_order_id = 0;

  auto snapshot = std::make_shared<iceberg::Snapshot>();
  snapshot->snapshot_id = 42;
  snapshot->sequence_number = 1;
  snapshot->schema_id = snapshot_schema_id;
  snapshot->manifest_list = "/nonexistent/manifest-list.avro";
  meta->snapshots = {snapshot};
  meta->current_snapshot_id = 42;
  return meta;
}

class ScanPlannerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    out_ = std::filesystem::temp_directory_path() / "primeparts-test-scan-planner";
    std::error_code ec;
    std::filesystem::remove_all(out_, ec);
    schema_ = TestSchema();

    std::string error;
    primeparts::WriterConfig cfg;
    cfg.output_dir = out_;
    cfg.schema = schema_;
    cfg.table_name = "t";
    cfg.filename_prefix = "t";
    cfg.delta_columns = {"p"};
    cfg.stat_columns = {{"p", true}};
    cfg.partition_spec = iceberg::PartitionSpec::Unpartitioned();
    cfg.simple_filename = true;
    cfg.compression_level = 1;
    cfg.max_row_group_rows = 100;

    auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
    ASSERT_NE(writer, nullptr) << error;

    constexpr int64_t kRows = 1000;
    arrow::Int64Builder pb;
    arrow::Int32Builder kb;
    for (int64_t i = 0; i < kRows; ++i) {
      ASSERT_TRUE(pb.Append(i).ok());
      ASSERT_TRUE(kb.Append(static_cast<int32_t>(i % 5)).ok());
    }
    std::shared_ptr<arrow::Array> pa, ka;
    ASSERT_TRUE(pb.Finish(&pa).ok());
    ASSERT_TRUE(kb.Finish(&ka).ok());
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("p", arrow::int64(), false),
                       arrow::field("k", arrow::int32(), false)}),
        kRows, {pa, ka});
    ASSERT_TRUE(writer->Write(*batch, &error)) << error;
    std::vector<primeparts::WrittenFile> files;
    ASSERT_TRUE(writer->Close(&files, &error)) << error;
    ASSERT_EQ(files.size(), 1u);

    path_ = files.front().path.string();
    file_length_ = files.front().bytes;
    primeparts::common::EnsureArrowRegistration();
    io_ = iceberg::arrow::MakeLocalFileIO();
  }

  static void TearDownTestSuite() {
    std::error_code ec;
    std::filesystem::remove_all(out_, ec);
    io_.reset();
    schema_.reset();
  }

  static std::filesystem::path out_;
  static std::string path_;
  static int64_t file_length_;
  static std::shared_ptr<iceberg::FileIO> io_;
  static std::shared_ptr<iceberg::Schema> schema_;
};

std::filesystem::path ScanPlannerTest::out_;
std::string ScanPlannerTest::path_;
int64_t ScanPlannerTest::file_length_ = 0;
std::shared_ptr<iceberg::FileIO> ScanPlannerTest::io_;
std::shared_ptr<iceberg::Schema> ScanPlannerTest::schema_;

TEST_F(ScanPlannerTest, PrunesToSingleRowGroupAndReadsIt) {
  std::string error;
  auto residual = iceberg::Expressions::And(
      iceberg::Expressions::GreaterThanOrEqual("p", iceberg::Literal::Long(250)),
      iceberg::Expressions::LessThanOrEqual("p", iceberg::Literal::Long(260)));
  std::vector<primeparts::scan::SplitSelection> splits;
  bool all_kept = false;
  ASSERT_TRUE(primeparts::scan::SelectSplits(path_, file_length_, io_, *schema_,
                                             residual, true, &splits, &all_kept,
                                             &error))
      << error;
  EXPECT_FALSE(all_kept);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_EQ(splits[0].planned_rows, 100);

  iceberg::ReaderOptions opts;
  opts.path = path_;
  opts.length = static_cast<size_t>(file_length_);
  opts.split = splits[0].split;
  opts.io = io_;
  opts.projection = schema_;
  auto reader_r = iceberg::ReaderFactoryRegistry::Open(
      iceberg::FileFormatType::kParquet, opts);
  ASSERT_TRUE(reader_r.has_value())
      << (reader_r.has_value() ? "" : reader_r.error().message);
  auto reader = std::move(reader_r.value());
  auto cschema_r = reader->Schema();
  ASSERT_TRUE(cschema_r.has_value());
  ArrowSchema cschema = cschema_r.value();
  auto arrow_schema_r = arrow::ImportSchema(&cschema);
  ASSERT_TRUE(arrow_schema_r.ok());
  auto arrow_schema = std::move(arrow_schema_r).ValueOrDie();

  int64_t read_rows = 0;
  int64_t key_lo = INT64_MAX;
  int64_t key_hi = INT64_MIN;
  while (true) {
    auto next_r = reader->Next();
    ASSERT_TRUE(next_r.has_value());
    if (!next_r.value().has_value()) break;
    ArrowArray array = std::move(next_r.value().value());
    auto batch_r = arrow::ImportRecordBatch(&array, arrow_schema);
    ASSERT_TRUE(batch_r.ok());
    auto rb = std::move(batch_r).ValueOrDie();
    auto pcol =
        std::static_pointer_cast<arrow::Int64Array>(rb->GetColumnByName("p"));
    for (int64_t i = 0; i < rb->num_rows(); ++i) {
      key_lo = std::min(key_lo, pcol->Value(i));
      key_hi = std::max(key_hi, pcol->Value(i));
    }
    read_rows += rb->num_rows();
  }
  EXPECT_EQ(read_rows, 100);
  EXPECT_EQ(key_lo, 200);
  EXPECT_EQ(key_hi, 299);
}

TEST_F(ScanPlannerTest, PrunesAllRowGroups) {
  std::string error;
  auto residual =
      iceberg::Expressions::GreaterThanOrEqual("p", iceberg::Literal::Long(5000));
  std::vector<primeparts::scan::SplitSelection> splits;
  bool all_kept = false;
  ASSERT_TRUE(primeparts::scan::SelectSplits(path_, file_length_, io_, *schema_,
                                             residual, true, &splits, &all_kept,
                                             &error))
      << error;
  EXPECT_FALSE(all_kept);
  EXPECT_TRUE(splits.empty());
}

TEST_F(ScanPlannerTest, KeepsAllRowGroups) {
  std::string error;
  auto residual =
      iceberg::Expressions::GreaterThanOrEqual("p", iceberg::Literal::Long(0));
  std::vector<primeparts::scan::SplitSelection> splits;
  bool all_kept = false;
  ASSERT_TRUE(primeparts::scan::SelectSplits(path_, file_length_, io_, *schema_,
                                             residual, true, &splits, &all_kept,
                                             &error))
      << error;
  EXPECT_TRUE(all_kept);
  EXPECT_TRUE(splits.empty());
}

TEST_F(ScanPlannerTest, SortsTasksByLowerBoundAndErrorsOnMissing) {
  std::string error;
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  tasks.push_back(TaskWithLowerBound("b", 500));
  tasks.push_back(TaskWithLowerBound("a", 0));
  tasks.push_back(TaskWithLowerBound("c", 900));
  primeparts::scan::TableReadTraits::SortKey key{1, "p", true};
  ASSERT_TRUE(
      primeparts::scan::SortTasksByLowerBound(&tasks, *schema_, key, &error))
      << error;
  EXPECT_EQ(tasks[0]->data_file()->file_path, "a");
  EXPECT_EQ(tasks[1]->data_file()->file_path, "b");
  EXPECT_EQ(tasks[2]->data_file()->file_path, "c");

  tasks.push_back(TaskWithLowerBound("d", std::nullopt));
  EXPECT_FALSE(
      primeparts::scan::SortTasksByLowerBound(&tasks, *schema_, key, &error));
}

std::shared_ptr<iceberg::FileScanTask> TaskWithLiteralBound(
    const std::string& path, int32_t field_id,
    const std::optional<iceberg::Literal>& bound) {
  auto df = std::make_shared<iceberg::DataFile>();
  df->file_path = path;
  if (bound) {
    auto ser = bound->Serialize();
    EXPECT_TRUE(ser.has_value());
    if (ser.has_value()) df->lower_bounds[field_id] = ser.value();
  }
  return std::make_shared<iceberg::FileScanTask>(std::move(df));
}

TEST(TaskOrdering, SortsByStringBounds) {
  std::vector<iceberg::SchemaField> fields;
  fields.push_back(
      iceberg::SchemaField::MakeRequired(1, "name", iceberg::string()));
  iceberg::Schema schema(std::move(fields), 0);

  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  tasks.push_back(
      TaskWithLiteralBound("mid", 1, iceberg::Literal::String("m")));
  tasks.push_back(
      TaskWithLiteralBound("first", 1, iceberg::Literal::String("a")));
  tasks.push_back(
      TaskWithLiteralBound("last", 1, iceberg::Literal::String("z")));

  primeparts::scan::TableReadTraits::SortKey key{1, "name", true};
  std::string error;
  ASSERT_TRUE(
      primeparts::scan::SortTasksByLowerBound(&tasks, schema, key, &error))
      << error;
  EXPECT_EQ(tasks[0]->data_file()->file_path, "first");
  EXPECT_EQ(tasks[1]->data_file()->file_path, "mid");
  EXPECT_EQ(tasks[2]->data_file()->file_path, "last");
}

TEST(TaskOrdering, RefusesASortKeyWithoutATotalOrder) {
  std::vector<iceberg::SchemaField> fields;
  fields.push_back(iceberg::SchemaField::MakeRequired(1, "id", iceberg::uuid()));
  iceberg::Schema schema(std::move(fields), 0);

  std::array<uint8_t, iceberg::Uuid::kLength> lhs{};
  std::array<uint8_t, iceberg::Uuid::kLength> rhs{};
  rhs[15] = 1;

  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  tasks.push_back(
      TaskWithLiteralBound("a", 1, iceberg::Literal::UUID(iceberg::Uuid(lhs))));
  tasks.push_back(
      TaskWithLiteralBound("b", 1, iceberg::Literal::UUID(iceberg::Uuid(rhs))));

  primeparts::scan::TableReadTraits::SortKey key{1, "id", true};
  std::string error;
  EXPECT_FALSE(
      primeparts::scan::SortTasksByLowerBound(&tasks, schema, key, &error));
  EXPECT_NE(error.find("unordered"), std::string::npos) << error;
}

TEST(KeyWindow, FoldsInclusiveAndStrictBoundsInclusively) {
  std::optional<iceberg::Literal> lo, hi;
  auto filter = iceberg::Expressions::And(
      iceberg::Expressions::GreaterThan("p", iceberg::Literal::Long(10)),
      iceberg::Expressions::LessThanOrEqual("p", iceberg::Literal::Long(20)));

  primeparts::scan::DeriveKeyWindow(filter, "p", iceberg::int64(), &lo, &hi);

  ASSERT_TRUE(lo.has_value());
  ASSERT_TRUE(hi.has_value());
  EXPECT_EQ(*std::get_if<int64_t>(&lo->value()), 10)
      << "a strict > must fold to an inclusive bound: the window may "
         "over-include, never under-include";
  EXPECT_EQ(*std::get_if<int64_t>(&hi->value()), 20);
}

TEST(KeyWindow, TightensToTheNarrowestBound) {
  std::optional<iceberg::Literal> lo, hi;
  auto filter = iceberg::Expressions::And(
      iceberg::Expressions::GreaterThanOrEqual("p", iceberg::Literal::Long(10)),
      iceberg::Expressions::GreaterThanOrEqual("p", iceberg::Literal::Long(50)));

  primeparts::scan::DeriveKeyWindow(filter, "p", iceberg::int64(), &lo, &hi);

  ASSERT_TRUE(lo.has_value());
  EXPECT_EQ(*std::get_if<int64_t>(&lo->value()), 50);
  EXPECT_FALSE(hi.has_value());
}

TEST(KeyWindow, CastsPredicateLiteralToTheKeyType) {
  std::optional<iceberg::Literal> lo, hi;
  auto filter =
      iceberg::Expressions::GreaterThanOrEqual("k", iceberg::Literal::Long(7));

  primeparts::scan::DeriveKeyWindow(filter, "k", iceberg::int32(), &lo, &hi);

  ASSERT_TRUE(lo.has_value());
  EXPECT_NE(std::get_if<int32_t>(&lo->value()), nullptr)
      << "the window must carry the key's type, not the predicate's";
  EXPECT_EQ(*std::get_if<int32_t>(&lo->value()), 7);
}

TEST(KeyWindow, IgnoresPredicatesOverOtherColumnsAndUnfoldableOps) {
  std::optional<iceberg::Literal> lo, hi;
  auto filter = iceberg::Expressions::And(
      iceberg::Expressions::Equal("k", iceberg::Literal::Int(1)),
      iceberg::Expressions::NotEqual("p", iceberg::Literal::Long(3)));

  primeparts::scan::DeriveKeyWindow(filter, "p", iceberg::int64(), &lo, &hi);

  EXPECT_FALSE(lo.has_value());
  EXPECT_FALSE(hi.has_value());
}

TEST(KeyWindow, DoesNotFoldAnOutOfRangeLiteral) {
  std::optional<iceberg::Literal> lo, hi;
  auto filter = iceberg::Expressions::GreaterThanOrEqual(
      "k", iceberg::Literal::Long(std::numeric_limits<int64_t>::max()));

  primeparts::scan::DeriveKeyWindow(filter, "k", iceberg::int32(), &lo, &hi);

  EXPECT_FALSE(lo.has_value())
      << "a literal that saturates to AboveMax is not a usable window bound";
}

TEST_F(ScanPlannerTest, RefusesBranchSchemaWhenSnapshotSchemaDiffers) {
  auto meta = MetadataWithSnapshotSchema(0, 1);
  primeparts::scan::ScanPlanRequest request;
  request.snapshot_id = 42;
  request.use_snapshot_schema = false;

  primeparts::scan::ScanPlan plan;
  std::string error;
  EXPECT_FALSE(
      primeparts::scan::PlanTableScan(meta, io_, request, &plan, &error));
  EXPECT_NE(error.find("use-snapshot-schema is false"), std::string::npos)
      << error;
}

TEST_F(ScanPlannerTest, RefusesSnapshotSchemaWithoutSnapshotId) {
  auto meta = MetadataWithSnapshotSchema(0, 1);
  primeparts::scan::ScanPlanRequest request;
  request.use_snapshot_schema = true;

  primeparts::scan::ScanPlan plan;
  std::string error;
  EXPECT_FALSE(
      primeparts::scan::PlanTableScan(meta, io_, request, &plan, &error));
  EXPECT_NE(error.find("use-snapshot-schema is true"), std::string::npos)
      << error;
}

TEST_F(ScanPlannerTest, AllowsSnapshotSchemaWhenRequestMatchesResolution) {
  auto meta = MetadataWithSnapshotSchema(0, 1);
  primeparts::scan::ScanPlanRequest request;
  request.snapshot_id = 42;
  request.use_snapshot_schema = true;

  primeparts::scan::ScanPlan plan;
  std::string error;
  primeparts::scan::PlanTableScan(meta, io_, request, &plan, &error);
  EXPECT_EQ(error.find("use-snapshot-schema"), std::string::npos) << error;
}

TEST_F(ScanPlannerTest, AllowsEitherFlagWhenSnapshotSchemaIsCurrent) {
  auto meta = MetadataWithSnapshotSchema(1, 1);
  primeparts::scan::ScanPlan plan;

  for (bool use_snapshot_schema : {false, true}) {
    primeparts::scan::ScanPlanRequest request;
    request.snapshot_id = 42;
    request.use_snapshot_schema = use_snapshot_schema;
    std::string error;
    primeparts::scan::PlanTableScan(meta, io_, request, &plan, &error);
    EXPECT_EQ(error.find("use-snapshot-schema"), std::string::npos)
        << "use_snapshot_schema=" << use_snapshot_schema << ": " << error;
  }
}

}  // namespace
