#include "primeparts/schemas.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/sort_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/type.h"

namespace {

template <typename T>
std::shared_ptr<arrow::Array> FinishOrDie(T& builder) {
  std::shared_ptr<arrow::Array> out;
  auto st = builder.Finish(&out);
  EXPECT_TRUE(st.ok()) << st.ToString();
  return out;
}

std::shared_ptr<arrow::RecordBatch> MakePrimesBatch(
    const std::shared_ptr<arrow::Schema>& schema) {
  arrow::Int64Builder p;
  arrow::Int32Builder k;
  arrow::Int64Builder prime_rank;
  arrow::Int32Builder bucket_version;
  arrow::Int32Builder bucket;

  for (int64_t value : {3, 5, 7}) EXPECT_TRUE(p.Append(value).ok());
  for (int32_t value : {2, 1, 3}) EXPECT_TRUE(k.Append(value).ok());
  for (int64_t value : {0, 1, 2}) EXPECT_TRUE(prime_rank.Append(value).ok());
  for (int32_t value : {1, 1, 1}) EXPECT_TRUE(bucket_version.Append(value).ok());
  for (int32_t value : {2, 2, 2}) EXPECT_TRUE(bucket.Append(value).ok());

  return arrow::RecordBatch::Make(
      schema, 3,
      {FinishOrDie(p), FinishOrDie(k), FinishOrDie(prime_rank),
       FinishOrDie(bucket_version), FinishOrDie(bucket)});
}

int64_t BoundI64(const std::map<int32_t, std::vector<uint8_t>>& bounds,
                 int32_t field_id) {
  auto lit = iceberg::Literal::Deserialize(bounds.at(field_id), iceberg::int64());
  EXPECT_TRUE(lit.has_value()) << (lit.has_value() ? "" : lit.error().message);
  return lit.has_value() ? std::get<int64_t>(lit.value().value()) : 0;
}

int32_t BoundI32(const std::map<int32_t, std::vector<uint8_t>>& bounds,
                 int32_t field_id) {
  auto lit = iceberg::Literal::Deserialize(bounds.at(field_id), iceberg::int32());
  EXPECT_TRUE(lit.has_value()) << (lit.has_value() ? "" : lit.error().message);
  return lit.has_value() ? std::get<int32_t>(lit.value().value()) : 0;
}

primeparts::WriterConfig BaseConfig(
    const std::filesystem::path& out,
    const std::shared_ptr<iceberg::Schema>& schema,
    const std::shared_ptr<iceberg::PartitionSpec>& spec) {
  primeparts::WriterConfig cfg;
  cfg.output_dir = out / "primeparts" / "primes" / "data" /
                   "p_bucket_version=1" / "p_bucket=2";
  cfg.schema = schema;
  cfg.table_name = "primes";
  cfg.filename_prefix = "primes";
  cfg.delta_columns = {"p", "prime_rank"};
  cfg.partition_spec = spec;
  cfg.partition_values = std::make_shared<iceberg::PartitionValues>(
      std::vector<iceberg::Literal>{iceberg::Literal::Int(1),
                                    iceberg::Literal::Int(2)});
  cfg.bucket_version = 1;
  cfg.bucket = 2;
  cfg.compression_level = 1;
  cfg.data_pagesize = 1024;
  return cfg;
}

class WriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    out_ = std::filesystem::temp_directory_path() / "primeparts-test-iceberg-writer";
    std::error_code ec;
    std::filesystem::remove_all(out_, ec);
    schema_ = primeparts::PrimesSchema();
    std::string error;
    spec_ = primeparts::BucketPartitionSpec(*schema_, &error);
    ASSERT_NE(spec_, nullptr) << error;
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(out_, ec);
  }

  std::filesystem::path out_;
  std::shared_ptr<iceberg::Schema> schema_;
  std::shared_ptr<iceberg::PartitionSpec> spec_;
};

TEST_F(WriterTest, RejectsUnknownStatColumn) {
  std::string error;
  auto cfg = BaseConfig(out_, schema_, spec_);
  cfg.stat_columns = {{"nope", true}};
  auto w = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
  EXPECT_EQ(w, nullptr);
}

TEST_F(WriterTest, RejectsPartitionedSpecWithoutValues) {
  std::string error;
  auto cfg = BaseConfig(out_, schema_, spec_);
  cfg.partition_values.reset();
  auto w = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
  EXPECT_EQ(w, nullptr);
}

TEST_F(WriterTest, RejectsNullPartitionSpec) {
  std::string error;
  auto cfg = BaseConfig(out_, schema_, spec_);
  cfg.partition_spec.reset();
  auto w = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
  EXPECT_EQ(w, nullptr);
}

TEST_F(WriterTest, WritesFileWithDescriptorAndBounds) {
  std::string error;
  auto arrow_schema =
      primeparts::IcebergToArrowSchemaWithFieldIds(*schema_, &error);
  ASSERT_NE(arrow_schema, nullptr) << error;

  auto cfg = BaseConfig(out_, schema_, spec_);
  cfg.stat_columns = {{"p", true}, {"prime_rank", true}, {"k", false}};
  auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
  ASSERT_NE(writer, nullptr) << error;

  auto batch = MakePrimesBatch(arrow_schema);
  ASSERT_TRUE(writer->Write(*batch, &error)) << error;

  std::vector<primeparts::WrittenFile> files;
  ASSERT_TRUE(writer->Close(&files, &error)) << error;

  ASSERT_EQ(files.size(), 1u);
  const auto& file = files.front();
  EXPECT_EQ(file.rows, 3);
  EXPECT_EQ(file.bounds.size(), 3u);
  ASSERT_NE(file.data_file, nullptr);
  EXPECT_EQ(file.data_file->file_path, file.path.string());
  EXPECT_EQ(file.data_file->record_count, 3);
  EXPECT_EQ(file.data_file->file_size_in_bytes, file.bytes);
  EXPECT_EQ(file.data_file->partition_spec_id.value_or(-1), 0);
  ASSERT_EQ(file.data_file->partition.num_fields(), 2u);

  auto part_bucket = file.data_file->partition.ValueAt(1);
  ASSERT_TRUE(part_bucket.has_value()) << part_bucket.error().message;
  EXPECT_EQ(std::get<int32_t>(part_bucket.value().get().value()), 2);

  EXPECT_EQ(file.data_file->value_counts.at(1), 3);
  EXPECT_EQ(file.data_file->null_value_counts.at(1), 0);
  EXPECT_EQ(BoundI64(file.data_file->lower_bounds, 1), 3);
  EXPECT_EQ(BoundI64(file.data_file->upper_bounds, 1), 7);
  EXPECT_EQ(BoundI64(file.data_file->lower_bounds, 3), 0);
  EXPECT_EQ(BoundI64(file.data_file->upper_bounds, 3), 2);
  EXPECT_EQ(BoundI32(file.data_file->lower_bounds, 2), 1);
  EXPECT_EQ(BoundI32(file.data_file->upper_bounds, 2), 3);
  EXPECT_EQ(file.data_file->lower_bounds.at(2).size(), 4u);
}

TEST_F(WriterTest, AscendingSortOrder) {
  std::string error;
  auto parts = primeparts::PartitionsSchema();
  auto order = primeparts::AscendingSortOrder(*parts, {"p", "m_k"}, &error);
  ASSERT_NE(order, nullptr) << error;
  auto fields = order->fields();
  EXPECT_EQ(order->order_id(), 1);
  ASSERT_EQ(fields.size(), 2u);
  EXPECT_EQ(fields[0].source_id(), 1);
  EXPECT_EQ(fields[1].source_id(), 2);
  EXPECT_EQ(fields[0].direction(), iceberg::SortDirection::kAscending);
  EXPECT_EQ(fields[1].direction(), iceberg::SortDirection::kAscending);

  auto bad = primeparts::AscendingSortOrder(*parts, {"p", "nope"}, &error);
  EXPECT_EQ(bad, nullptr);
}

}  // namespace
