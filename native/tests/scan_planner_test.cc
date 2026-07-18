#include "primeparts/common/arrow_init.h"
#include "primeparts/scan/scan_planner.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
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
