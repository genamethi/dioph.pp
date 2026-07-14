#include "primeparts/scan/scan_planner.h"
#include "primeparts/writer.h"

#include <arrow/api.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/expression/expression.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"

namespace {

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

std::shared_ptr<iceberg::Schema> TestSchema() {
  std::vector<iceberg::SchemaField> fields;
  fields.push_back(
      iceberg::SchemaField::MakeRequired(1, "p", iceberg::int64()));
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

}  // namespace

int main() {
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / "primeparts-test-scan-planner";
  std::error_code ec;
  std::filesystem::remove_all(out, ec);

  std::string error;
  auto schema = TestSchema();
  auto spec = iceberg::PartitionSpec::Unpartitioned();

  primeparts::WriterConfig cfg;
  cfg.output_dir = out;
  cfg.schema = schema;
  cfg.table_name = "t";
  cfg.filename_prefix = "t";
  cfg.delta_columns = {"p"};
  cfg.stat_columns = {{"p", true}};
  cfg.partition_spec = spec;
  cfg.simple_filename = true;
  cfg.compression_level = 1;
  cfg.max_row_group_rows = 100;

  auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
  if (!writer) {
    std::cerr << error << "\n";
    return 1;
  }

  constexpr int64_t kRows = 1000;
  arrow::Int64Builder pb;
  arrow::Int32Builder kb;
  for (int64_t i = 0; i < kRows; ++i) {
    if (!pb.Append(i).ok() ||
        !kb.Append(static_cast<int32_t>(i % 5)).ok()) {
      return 1;
    }
  }
  std::shared_ptr<arrow::Array> pa, ka;
  if (!pb.Finish(&pa).ok() || !kb.Finish(&ka).ok()) return 1;
  auto batch = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("p", arrow::int64(), false),
                     arrow::field("k", arrow::int32(), false)}),
      kRows, {pa, ka});
  if (!writer->Write(*batch, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  std::vector<primeparts::WrittenFile> files;
  if (!writer->Close(&files, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!Check(files.size() == 1, "expected one file")) return 1;
  const std::string path = files.front().path.string();

  {
    auto residual = iceberg::Expressions::And(
        iceberg::Expressions::GreaterThanOrEqual("p",
                                                 iceberg::Literal::Long(250)),
        iceberg::Expressions::LessThanOrEqual("p", iceberg::Literal::Long(260)));
    std::vector<int32_t> groups;
    int64_t rows = 0;
    if (!primeparts::scan::SelectRowGroups(path, *schema, residual, true,
                                           &groups, &rows, &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    if (!Check(groups.size() == 1, "expected 1 row group kept, got " +
                                       std::to_string(groups.size()))) {
      return 1;
    }
    if (!Check(groups[0] == 2, "expected row group index 2")) return 1;
    if (!Check(rows == 100, "expected planned_rows=100")) return 1;
  }

  {
    auto residual = iceberg::Expressions::GreaterThanOrEqual(
        "p", iceberg::Literal::Long(5000));
    std::vector<int32_t> groups;
    int64_t rows = 0;
    if (!primeparts::scan::SelectRowGroups(path, *schema, residual, true,
                                           &groups, &rows, &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    if (!Check(groups.empty(), "expected all row groups pruned")) return 1;
  }

  {
    std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
    tasks.push_back(TaskWithLowerBound("b", 500));
    tasks.push_back(TaskWithLowerBound("a", 0));
    tasks.push_back(TaskWithLowerBound("c", 900));
    primeparts::scan::TableReadTraits::SortKey key{1, "p", true};
    if (!primeparts::scan::SortTasksByLowerBound(&tasks, *schema, key,
                                                 &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    if (!Check(tasks[0]->data_file()->file_path == "a" &&
                   tasks[1]->data_file()->file_path == "b" &&
                   tasks[2]->data_file()->file_path == "c",
               "expected tasks ordered by p lower bound")) {
      return 1;
    }

    tasks.push_back(TaskWithLowerBound("d", std::nullopt));
    if (!Check(!primeparts::scan::SortTasksByLowerBound(&tasks, *schema, key,
                                                        &error),
               "expected missing lower bound to be a loud error")) {
      return 1;
    }
  }

  std::filesystem::remove_all(out, ec);
  return 0;
}
