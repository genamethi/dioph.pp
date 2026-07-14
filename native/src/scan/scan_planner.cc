#include "primeparts/scan/scan_planner.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include "iceberg/expression/expression.h"
#include "iceberg/expression/inclusive_metrics_evaluator.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"

#include "primeparts/common/uri.h"

namespace primeparts::scan {

namespace {

bool TrivialResidual(const std::shared_ptr<iceberg::Expression>& e) {
  return !e || e->op() == iceberg::Expression::Operation::kTrue;
}

const iceberg::SchemaField* FieldById(const iceberg::Schema& schema,
                                      int32_t field_id) {
  for (const auto& f : schema.fields()) {
    if (f.field_id() == field_id) return &f;
  }
  return nullptr;
}

bool DecodeIntegerBound(const std::vector<uint8_t>& bytes,
                        iceberg::TypeId type, int64_t* out,
                        std::string* error) {
  auto prim = type == iceberg::TypeId::kInt
                  ? std::static_pointer_cast<iceberg::PrimitiveType>(
                        iceberg::int32())
                  : std::static_pointer_cast<iceberg::PrimitiveType>(
                        iceberg::int64());
  auto lit = iceberg::Literal::Deserialize(bytes, prim);
  if (!lit.has_value()) {
    if (error) *error = "Literal::Deserialize: " + lit.error().message;
    return false;
  }
  if (type == iceberg::TypeId::kInt) {
    *out = std::get<int32_t>(lit.value().value());
  } else {
    *out = std::get<int64_t>(lit.value().value());
  }
  return true;
}

bool StatsNames(const iceberg::TableMetadata& metadata,
                const ScanPlanRequest& request, const TableReadTraits& traits,
                std::vector<std::string>* out) {
  (void)metadata;
  for (const auto& key : traits.sort_keys) out->push_back(key.name);
  for (const auto& name : request.stats_fields) {
    if (std::find(out->begin(), out->end(), name) == out->end()) {
      out->push_back(name);
    }
  }
  return !out->empty();
}

template <typename ScanType>
bool BuildAndPlan(const std::shared_ptr<iceberg::TableMetadata>& metadata,
                  const std::shared_ptr<iceberg::FileIO>& io,
                  const ScanPlanRequest& request,
                  const std::vector<std::string>& stats_names,
                  std::shared_ptr<iceberg::Schema>* projected,
                  std::vector<std::shared_ptr<iceberg::FileScanTask>>* tasks,
                  std::string* error) {
  auto builder_r = iceberg::TableScanBuilder<ScanType>::Make(metadata, io);
  if (!builder_r.has_value()) {
    if (error) *error = "TableScanBuilder::Make: " + builder_r.error().message;
    return false;
  }
  auto builder = std::move(builder_r.value());
  builder->Select(request.select).CaseSensitive(request.case_sensitive);
  if (!stats_names.empty()) builder->IncludeColumnStats(stats_names);
  if (request.filter) builder->Filter(request.filter);
  if constexpr (std::is_same_v<ScanType, iceberg::IncrementalAppendScan>) {
    builder->FromSnapshot(*request.start_snapshot_id, false)
        .ToSnapshot(*request.end_snapshot_id);
  } else {
    if (request.snapshot_id) builder->UseSnapshot(*request.snapshot_id);
  }
  auto scan_r = builder->Build();
  if (!scan_r.has_value()) {
    if (error) *error = "TableScanBuilder::Build: " + scan_r.error().message;
    return false;
  }
  auto scan = std::move(scan_r.value());
  auto schema_r = scan->schema();
  if (!schema_r.has_value()) {
    if (error) *error = "scan->schema: " + schema_r.error().message;
    return false;
  }
  *projected = schema_r.value();
  auto tasks_r = scan->PlanFiles();
  if (!tasks_r.has_value()) {
    if (error) *error = "scan->PlanFiles: " + tasks_r.error().message;
    return false;
  }
  *tasks = std::move(tasks_r.value());
  return true;
}

}  // namespace

bool SortTasksByLowerBound(
    std::vector<std::shared_ptr<iceberg::FileScanTask>>* tasks,
    const iceberg::Schema& schema, const TableReadTraits::SortKey& key,
    std::string* error) {
  const auto* field = FieldById(schema, key.field_id);
  if (!field) {
    if (error) {
      *error = "sort key field id " + std::to_string(key.field_id) +
               " not in schema";
    }
    return false;
  }
  const auto type = field->type()->type_id();
  if (type != iceberg::TypeId::kInt && type != iceberg::TypeId::kLong) {
    if (error) *error = "sort key " + key.name + " is not an integer type";
    return false;
  }
  std::vector<std::pair<int64_t, std::shared_ptr<iceberg::FileScanTask>>> keyed;
  keyed.reserve(tasks->size());
  for (auto& task : *tasks) {
    const auto& lb = task->data_file()->lower_bounds;
    auto it = lb.find(key.field_id);
    if (it == lb.end()) {
      if (error) {
        *error = "data file " + task->data_file()->file_path +
                 " has no lower bound for sort key " + key.name;
      }
      return false;
    }
    int64_t v = 0;
    if (!DecodeIntegerBound(it->second, type, &v, error)) return false;
    keyed.emplace_back(v, std::move(task));
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [&](const auto& a, const auto& b) {
                     return key.ascending ? a.first < b.first
                                          : a.first > b.first;
                   });
  tasks->clear();
  for (auto& [v, task] : keyed) tasks->push_back(std::move(task));
  return true;
}

bool SelectRowGroups(const std::string& file_path,
                     const iceberg::Schema& schema,
                     const std::shared_ptr<iceberg::Expression>& residual,
                     bool case_sensitive, std::vector<int32_t>* row_groups,
                     int64_t* planned_rows, std::string* error) {
  row_groups->clear();
  *planned_rows = 0;

  auto evaluator_r = iceberg::InclusiveMetricsEvaluator::Make(
      residual, schema, case_sensitive);
  if (!evaluator_r.has_value()) {
    if (error) {
      *error = "InclusiveMetricsEvaluator::Make: " + evaluator_r.error().message;
    }
    return false;
  }
  const auto& evaluator = evaluator_r.value();

  std::unique_ptr<parquet::ParquetFileReader> reader;
  try {
    reader = parquet::ParquetFileReader::OpenFile(file_path, false);
  } catch (const std::exception& e) {
    if (error) *error = "parquet open " + file_path + ": " + e.what();
    return false;
  }
  const auto* file_meta = reader->metadata().get();
  const auto* descr = file_meta->schema();

  for (int rg = 0; rg < file_meta->num_row_groups(); ++rg) {
    auto rg_meta = file_meta->RowGroup(rg);
    iceberg::DataFile df;
    df.record_count = rg_meta->num_rows();

    for (int c = 0; c < rg_meta->num_columns(); ++c) {
      const auto* col_descr = descr->Column(c);
      const int field_id = col_descr->schema_node()->field_id();
      if (field_id < 0) continue;
      const auto* schema_field = FieldById(schema, field_id);
      if (!schema_field) continue;
      const auto type = schema_field->type()->type_id();
      if (type != iceberg::TypeId::kInt && type != iceberg::TypeId::kLong) {
        continue;
      }
      df.value_counts[field_id] = rg_meta->num_rows();
      auto col_meta = rg_meta->ColumnChunk(c);
      auto stats = col_meta->statistics();
      if (!stats) continue;
      if (stats->HasNullCount()) {
        df.null_value_counts[field_id] = stats->null_count();
      }
      if (!stats->HasMinMax()) continue;
      iceberg::Literal lo = iceberg::Literal::Long(0);
      iceberg::Literal hi = iceberg::Literal::Long(0);
      if (col_descr->physical_type() == parquet::Type::INT32) {
        auto typed = std::static_pointer_cast<parquet::Int32Statistics>(stats);
        if (type == iceberg::TypeId::kInt) {
          lo = iceberg::Literal::Int(typed->min());
          hi = iceberg::Literal::Int(typed->max());
        } else {
          lo = iceberg::Literal::Long(typed->min());
          hi = iceberg::Literal::Long(typed->max());
        }
      } else if (col_descr->physical_type() == parquet::Type::INT64) {
        auto typed = std::static_pointer_cast<parquet::Int64Statistics>(stats);
        lo = iceberg::Literal::Long(typed->min());
        hi = iceberg::Literal::Long(typed->max());
      } else {
        continue;
      }
      auto lo_ser = lo.Serialize();
      auto hi_ser = hi.Serialize();
      if (!lo_ser.has_value() || !hi_ser.has_value()) continue;
      df.lower_bounds[field_id] = std::move(lo_ser.value());
      df.upper_bounds[field_id] = std::move(hi_ser.value());
    }

    auto match = evaluator->Evaluate(df);
    if (!match.has_value() || match.value()) {
      row_groups->push_back(rg);
      *planned_rows += rg_meta->num_rows();
    }
  }
  return true;
}

bool PlanTableScan(const std::shared_ptr<iceberg::TableMetadata>& metadata,
                   const std::shared_ptr<iceberg::FileIO>& io,
                   const ScanPlanRequest& request, ScanPlan* out,
                   std::string* error) {
  *out = ScanPlan{};
  const bool incremental =
      request.start_snapshot_id.has_value() || request.end_snapshot_id.has_value();
  if (incremental && request.snapshot_id.has_value()) {
    if (error) {
      *error = "invalid ScanPlanRequest: both point-in-time and incremental "
               "fields set";
    }
    return false;
  }
  if (incremental && (!request.start_snapshot_id.has_value() ||
                      !request.end_snapshot_id.has_value())) {
    if (error) {
      *error = "invalid ScanPlanRequest: incremental scan requires both "
               "start-snapshot-id and end-snapshot-id";
    }
    return false;
  }

  if (!TableReadTraits::FromMetadata(*metadata, &out->traits, error)) {
    return false;
  }

  std::vector<std::string> stats_names;
  StatsNames(*metadata, request, out->traits, &stats_names);

  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  if (incremental) {
    if (!BuildAndPlan<iceberg::IncrementalAppendScan>(
            metadata, io, request, stats_names, &out->projected_schema, &tasks,
            error)) {
      return false;
    }
  } else {
    if (!BuildAndPlan<iceberg::DataTableScan>(metadata, io, request,
                                              stats_names,
                                              &out->projected_schema, &tasks,
                                              error)) {
      return false;
    }
  }

  auto schema_r = metadata->Schema();
  if (!schema_r.has_value()) {
    if (error) *error = "TableMetadata::Schema: " + schema_r.error().message;
    return false;
  }
  const auto& table_schema = schema_r.value();

  if (out->traits.sorted() && !tasks.empty()) {
    if (!SortTasksByLowerBound(&tasks, *table_schema,
                               out->traits.sort_keys.front(), error)) {
      return false;
    }
  }

  out->tasks.reserve(tasks.size());
  for (auto& task : tasks) {
    FileScanTask planned;
    if (TrivialResidual(task->residual_filter())) {
      planned.planned_rows =
          static_cast<int64_t>(task->data_file()->record_count);
      planned.inner = std::move(task);
      out->tasks.push_back(std::move(planned));
      out->planned_rows += out->tasks.back().planned_rows;
      continue;
    }
    const std::string path =
        primeparts::common::StripFileScheme(task->data_file()->file_path);
    if (!SelectRowGroups(path, *table_schema, task->residual_filter(),
                         request.case_sensitive, &planned.row_groups,
                         &planned.planned_rows, error)) {
      return false;
    }
    if (planned.row_groups.empty()) continue;
    planned.inner = std::move(task);
    out->planned_rows += planned.planned_rows;
    out->tasks.push_back(std::move(planned));
  }
  return true;
}

}  // namespace primeparts::scan
