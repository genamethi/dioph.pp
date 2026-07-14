#include "primeparts/writer.h"
#include "primeparts/schemas.h"

#include <arrow/api.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/type.h"

namespace {

template <typename T>
std::shared_ptr<arrow::Array> FinishOrDie(T& builder) {
  std::shared_ptr<arrow::Array> out;
  auto st = builder.Finish(&out);
  if (!st.ok()) {
    std::cerr << st.ToString() << "\n";
    std::exit(1);
  }
  return out;
}

std::shared_ptr<arrow::RecordBatch> MakePrimesBatch(
    const std::shared_ptr<arrow::Schema>& schema) {
  arrow::Int64Builder p;
  arrow::Int32Builder k;
  arrow::Int64Builder prime_rank;
  arrow::Int32Builder bucket_version;
  arrow::Int32Builder bucket;

  for (int64_t value : {3, 5, 7}) {
    if (!p.Append(value).ok()) std::exit(1);
  }
  for (int32_t value : {2, 1, 3}) {
    if (!k.Append(value).ok()) std::exit(1);
  }
  for (int64_t value : {0, 1, 2}) {
    if (!prime_rank.Append(value).ok()) std::exit(1);
  }
  for (int32_t value : {1, 1, 1}) {
    if (!bucket_version.Append(value).ok()) std::exit(1);
  }
  for (int32_t value : {2, 2, 2}) {
    if (!bucket.Append(value).ok()) std::exit(1);
  }

  return arrow::RecordBatch::Make(
      schema, 3,
      {FinishOrDie(p), FinishOrDie(k), FinishOrDie(prime_rank),
       FinishOrDie(bucket_version), FinishOrDie(bucket)});
}

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

int64_t BoundI64(const std::map<int32_t, std::vector<uint8_t>>& bounds,
                 int32_t field_id) {
  auto lit = iceberg::Literal::Deserialize(bounds.at(field_id), iceberg::int64());
  if (!lit.has_value()) {
    std::cerr << lit.error().message << "\n";
    std::exit(1);
  }
  return std::get<int64_t>(lit.value().value());
}

int32_t BoundI32(const std::map<int32_t, std::vector<uint8_t>>& bounds,
                 int32_t field_id) {
  auto lit = iceberg::Literal::Deserialize(bounds.at(field_id), iceberg::int32());
  if (!lit.has_value()) {
    std::cerr << lit.error().message << "\n";
    std::exit(1);
  }
  return std::get<int32_t>(lit.value().value());
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

}  // namespace

int main() {
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / "primeparts-test-iceberg-writer";
  std::error_code ec;
  std::filesystem::remove_all(out, ec);

  std::string error;
  auto schema = primeparts::PrimesSchema();
  auto spec = primeparts::BucketPartitionSpec(*schema, &error);
  if (!spec) {
    std::cerr << error << "\n";
    return 1;
  }
  auto arrow_schema = primeparts::IcebergToArrowSchemaWithFieldIds(*schema, &error);
  if (!arrow_schema) {
    std::cerr << error << "\n";
    return 1;
  }

  {
    auto cfg = BaseConfig(out, schema, spec);
    cfg.stat_columns = {{"nope", true}};
    auto w = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
    if (!Check(w == nullptr, "expected Make to reject unknown stat column")) {
      return 1;
    }
  }
  {
    auto cfg = BaseConfig(out, schema, spec);
    cfg.partition_values.reset();
    auto w = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
    if (!Check(w == nullptr,
               "expected Make to reject partitioned spec without "
               "partition_values")) {
      return 1;
    }
  }
  {
    auto cfg = BaseConfig(out, schema, spec);
    cfg.partition_spec.reset();
    auto w = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
    if (!Check(w == nullptr, "expected Make to reject null partition_spec")) {
      return 1;
    }
  }

  auto cfg = BaseConfig(out, schema, spec);
  cfg.stat_columns = {{"p", true}, {"prime_rank", true}, {"k", false}};
  auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
  if (!writer) {
    std::cerr << error << "\n";
    return 1;
  }

  auto batch = MakePrimesBatch(arrow_schema);
  if (!writer->Write(*batch, &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  std::vector<primeparts::WrittenFile> files;
  if (!writer->Close(&files, &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  if (!Check(files.size() == 1, "expected one written file")) return 1;
  const auto& file = files.front();
  if (!Check(file.rows == 3, "expected WrittenFile rows=3")) return 1;
  if (!Check(file.bounds.size() == 3, "expected bounds for 3 stat columns")) {
    return 1;
  }
  if (!Check(file.data_file != nullptr, "expected Iceberg DataFile metadata")) {
    return 1;
  }
  if (!Check(file.data_file->file_path == file.path.string(),
             "expected DataFile path to match final parquet path")) {
    return 1;
  }
  if (!Check(file.data_file->record_count == 3,
             "expected DataFile record_count=3")) {
    return 1;
  }
  if (!Check(file.data_file->file_size_in_bytes == file.bytes,
             "expected DataFile file size to match WrittenFile bytes")) {
    return 1;
  }
  if (!Check(file.data_file->partition_spec_id.value_or(-1) == 0,
             "expected DataFile partition_spec_id=0")) {
    return 1;
  }
  if (!Check(file.data_file->partition.num_fields() == 2,
             "expected two partition values")) {
    return 1;
  }
  auto part_bucket = file.data_file->partition.ValueAt(1);
  if (!part_bucket.has_value()) {
    std::cerr << part_bucket.error().message << "\n";
    return 1;
  }
  if (!Check(std::get<int32_t>(part_bucket.value().get().value()) == 2,
             "expected p_bucket partition value=2")) {
    return 1;
  }
  if (!Check(file.data_file->value_counts.at(1) == 3,
             "expected p value_count=3")) {
    return 1;
  }
  if (!Check(file.data_file->null_value_counts.at(1) == 0,
             "expected p null_count=0")) {
    return 1;
  }
  if (!Check(BoundI64(file.data_file->lower_bounds, 1) == 3,
             "expected p lower_bound=3")) {
    return 1;
  }
  if (!Check(BoundI64(file.data_file->upper_bounds, 1) == 7,
             "expected p upper_bound=7")) {
    return 1;
  }
  if (!Check(BoundI64(file.data_file->lower_bounds, 3) == 0,
             "expected prime_rank lower_bound=0")) {
    return 1;
  }
  if (!Check(BoundI64(file.data_file->upper_bounds, 3) == 2,
             "expected prime_rank upper_bound=2")) {
    return 1;
  }
  if (!Check(BoundI32(file.data_file->lower_bounds, 2) == 1,
             "expected k lower_bound=1 (int32, unsorted scan)")) {
    return 1;
  }
  if (!Check(BoundI32(file.data_file->upper_bounds, 2) == 3,
             "expected k upper_bound=3 (int32, unsorted scan)")) {
    return 1;
  }
  if (!Check(file.data_file->lower_bounds.at(2).size() == 4,
             "expected int32 bound serialized as 4 bytes")) {
    return 1;
  }

  std::filesystem::remove_all(out, ec);
  return 0;
}
