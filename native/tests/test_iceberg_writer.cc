#include "primeparts/writer.h"

#include <arrow/api.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
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
  for (int32_t value : {1, 1, 1}) {
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

}  // namespace

int main() {
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / "primeparts-test-iceberg-writer";
  std::error_code ec;
  std::filesystem::remove_all(out, ec);

  std::string error;
  auto schema = primeparts::PrimesSchema();
  auto arrow_schema = primeparts::IcebergToArrowSchemaWithFieldIds(*schema, &error);
  if (!arrow_schema) {
    std::cerr << error << "\n";
    return 1;
  }

  primeparts::WriterConfig cfg;
  cfg.output_dir = out / "primeparts" / "primes" / "data" /
                   "p_bucket_version=1" / "p_bucket=2";
  cfg.schema = schema;
  cfg.table_name = "primes";
  cfg.filename_prefix = "primes";
  cfg.delta_columns = {"p", "prime_rank"};
  cfg.bucket_version = 1;
  cfg.bucket = 2;
  cfg.compression_level = 1;
  cfg.data_pagesize = 1024;

  auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
  if (!writer) {
    std::cerr << error << "\n";
    return 1;
  }

  auto batch = MakePrimesBatch(arrow_schema);
  if (!writer->Write(*batch, {.p_min = 3, .p_max = 7, .rank_min = 0, .rank_max = 2},
                     &error)) {
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
  auto lower = iceberg::Literal::Deserialize(
      file.data_file->lower_bounds.at(1), iceberg::int64());
  if (!lower.has_value()) {
    std::cerr << lower.error().message << "\n";
    return 1;
  }
  auto upper = iceberg::Literal::Deserialize(
      file.data_file->upper_bounds.at(1), iceberg::int64());
  if (!upper.has_value()) {
    std::cerr << upper.error().message << "\n";
    return 1;
  }
  if (!Check(std::get<int64_t>(lower.value().value()) == 3,
             "expected p lower_bound=3")) {
    return 1;
  }
  if (!Check(std::get<int64_t>(upper.value().value()) == 7,
             "expected p upper_bound=7")) {
    return 1;
  }

  std::filesystem::remove_all(out, ec);
  return 0;
}
