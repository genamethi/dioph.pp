#include "primeparts/common/arrow_init.h"
#include "primeparts/scan/scan_planner.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/arrow/arrow_io_util.h"
#include "iceberg/expression/expression.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/file_format.h"
#include "iceberg/file_io.h"
#include "iceberg/file_reader.h"
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

  primeparts::common::EnsureArrowRegistration();
  std::shared_ptr<iceberg::FileIO> io = iceberg::arrow::MakeLocalFileIO();
  const int64_t file_length = files.front().bytes;

  {
    auto residual = iceberg::Expressions::And(
        iceberg::Expressions::GreaterThanOrEqual("p",
                                                 iceberg::Literal::Long(250)),
        iceberg::Expressions::LessThanOrEqual("p", iceberg::Literal::Long(260)));
    std::vector<primeparts::scan::SplitSelection> splits;
    bool all_kept = false;
    if (!primeparts::scan::SelectSplits(path, file_length, io, *schema,
                                        residual, true, &splits, &all_kept,
                                        &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    if (!Check(!all_kept, "expected pruning to drop row groups")) return 1;
    if (!Check(splits.size() == 1, "expected 1 split kept, got " +
                                       std::to_string(splits.size()))) {
      return 1;
    }
    if (!Check(splits[0].planned_rows == 100, "expected planned_rows=100")) {
      return 1;
    }

    iceberg::ReaderOptions opts;
    opts.path = path;
    opts.length = static_cast<size_t>(file_length);
    opts.split = splits[0].split;
    opts.io = io;
    opts.projection = schema;
    auto reader_r = iceberg::ReaderFactoryRegistry::Open(
        iceberg::FileFormatType::kParquet, opts);
    if (!Check(reader_r.has_value(),
               "ReaderFactoryRegistry::Open: " +
                   (reader_r.has_value() ? "" : reader_r.error().message))) {
      return 1;
    }
    auto reader = std::move(reader_r.value());
    auto cschema_r = reader->Schema();
    if (!Check(cschema_r.has_value(), "Reader::Schema failed")) return 1;
    ArrowSchema cschema = cschema_r.value();
    auto arrow_schema_r = arrow::ImportSchema(&cschema);
    if (!Check(arrow_schema_r.ok(), "ImportSchema failed")) return 1;
    auto arrow_schema = std::move(arrow_schema_r).ValueOrDie();

    int64_t read_rows = 0;
    int64_t p_min = INT64_MAX;
    int64_t p_max = INT64_MIN;
    while (true) {
      auto next_r = reader->Next();
      if (!Check(next_r.has_value(), "Reader::Next failed")) return 1;
      if (!next_r.value().has_value()) break;
      ArrowArray array = std::move(next_r.value().value());
      auto batch_r = arrow::ImportRecordBatch(&array, arrow_schema);
      if (!Check(batch_r.ok(), "ImportRecordBatch failed")) return 1;
      auto rb = std::move(batch_r).ValueOrDie();
      auto pcol = std::static_pointer_cast<arrow::Int64Array>(
          rb->GetColumnByName("p"));
      for (int64_t i = 0; i < rb->num_rows(); ++i) {
        p_min = std::min(p_min, pcol->Value(i));
        p_max = std::max(p_max, pcol->Value(i));
      }
      read_rows += rb->num_rows();
    }
    if (!Check(read_rows == 100 && p_min == 200 && p_max == 299,
               "expected split to read exactly row group 2 (rows 200..299), "
               "got rows=" +
                   std::to_string(read_rows) + " p=[" + std::to_string(p_min) +
                   "," + std::to_string(p_max) + "]")) {
      return 1;
    }
  }

  {
    auto residual = iceberg::Expressions::GreaterThanOrEqual(
        "p", iceberg::Literal::Long(5000));
    std::vector<primeparts::scan::SplitSelection> splits;
    bool all_kept = false;
    if (!primeparts::scan::SelectSplits(path, file_length, io, *schema,
                                        residual, true, &splits, &all_kept,
                                        &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    if (!Check(!all_kept && splits.empty(), "expected all row groups pruned")) {
      return 1;
    }
  }

  {
    auto residual = iceberg::Expressions::GreaterThanOrEqual(
        "p", iceberg::Literal::Long(0));
    std::vector<primeparts::scan::SplitSelection> splits;
    bool all_kept = false;
    if (!primeparts::scan::SelectSplits(path, file_length, io, *schema,
                                        residual, true, &splits, &all_kept,
                                        &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    if (!Check(all_kept && splits.empty(),
               "expected all row groups kept as all_kept")) {
      return 1;
    }
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
