#include "primeparts/scan/scan_planner.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include "iceberg/arrow/arrow_io_internal.h"
#include "iceberg/expression/expression.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/inclusive_metrics_evaluator.h"
#include "iceberg/expression/literal.h"
#include "iceberg/expression/predicate.h"
#include "iceberg/expression/term.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/snapshot.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"

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

void TightenLo(std::optional<iceberg::Literal>* lo, iceberg::Literal v) {
  if (!lo->has_value()) {
    *lo = std::move(v);
    return;
  }
  const auto cmp = **lo <=> v;
  if (cmp == std::partial_ordering::unordered) return;
  if (cmp < 0) *lo = std::move(v);
}

void TightenHi(std::optional<iceberg::Literal>* hi, iceberg::Literal v) {
  if (!hi->has_value()) {
    *hi = std::move(v);
    return;
  }
  const auto cmp = **hi <=> v;
  if (cmp == std::partial_ordering::unordered) return;
  if (cmp > 0) *hi = std::move(v);
}

void FoldKeyConjunct(const std::shared_ptr<iceberg::Expression>& expr,
                     const std::string& key_name,
                     const std::shared_ptr<iceberg::PrimitiveType>& key_type,
                     std::optional<iceberg::Literal>* lo,
                     std::optional<iceberg::Literal>* hi) {
  auto pred = std::dynamic_pointer_cast<iceberg::UnboundPredicate>(expr);
  if (!pred) return;
  auto ref = pred->reference();
  if (!ref || ref->name() != key_name) return;
  auto lits = pred->literals();
  if (lits.size() != 1) return;

  auto cast = lits[0].CastTo(key_type);
  if (!cast.has_value()) return;
  auto lit = std::move(cast.value());
  if (lit.IsAboveMax() || lit.IsBelowMin() || lit.IsNull()) return;

  switch (pred->op()) {
    case iceberg::Expression::Operation::kGtEq:
    case iceberg::Expression::Operation::kGt:
      TightenLo(lo, std::move(lit));
      return;
    case iceberg::Expression::Operation::kLtEq:
    case iceberg::Expression::Operation::kLt:
      TightenHi(hi, std::move(lit));
      return;
    case iceberg::Expression::Operation::kEq:
      TightenLo(lo, lit);
      TightenHi(hi, std::move(lit));
      return;
    default:
      return;
  }
}

}  // namespace

void DeriveKeyWindow(const std::shared_ptr<iceberg::Expression>& filter,
                     const std::string& key_name,
                     const std::shared_ptr<iceberg::PrimitiveType>& key_type,
                     std::optional<iceberg::Literal>* lo,
                     std::optional<iceberg::Literal>* hi) {
  if (!filter || !key_type) return;
  std::vector<std::shared_ptr<iceberg::Expression>> pending{filter};
  while (!pending.empty()) {
    auto expr = std::move(pending.back());
    pending.pop_back();
    if (expr->op() == iceberg::Expression::Operation::kAnd) {
      auto conj = std::static_pointer_cast<iceberg::And>(expr);
      pending.push_back(conj->left());
      pending.push_back(conj->right());
      continue;
    }
    FoldKeyConjunct(expr, key_name, key_type, lo, hi);
  }
}

namespace {

bool CheckSnapshotSchemaSupported(const iceberg::TableMetadata& metadata,
                                  const ScanPlanRequest& request,
                                  std::string* error) {
  std::shared_ptr<iceberg::Snapshot> snapshot;
  if (request.snapshot_id.has_value() || request.end_snapshot_id.has_value()) {
    const int64_t id = request.snapshot_id.value_or(
        request.end_snapshot_id.value_or(0));
    auto r = metadata.SnapshotById(id);
    if (!r.has_value()) {
      if (error) *error = "TableMetadata::SnapshotById: " + r.error().message;
      return false;
    }
    snapshot = r.value();
  } else {
    auto r = metadata.Snapshot();
    if (!r.has_value()) return true;
    snapshot = r.value();
  }
  if (snapshot == nullptr) return true;

  const int32_t snapshot_schema_id =
      snapshot->schema_id.value_or(metadata.current_schema_id);
  if (snapshot_schema_id == metadata.current_schema_id) return true;

  const bool resolves_snapshot_schema = request.snapshot_id.has_value();
  if (resolves_snapshot_schema == request.use_snapshot_schema) return true;

  if (error) {
    *error =
        request.use_snapshot_schema
            ? "NotImplemented: use-snapshot-schema is true and snapshot " +
                  std::to_string(snapshot->snapshot_id) + " was written under "
                  "schema " + std::to_string(snapshot_schema_id) +
                  " rather than the current schema " +
                  std::to_string(metadata.current_schema_id) + ", but no "
                  "snapshot-id was given; the vendored TableScanBuilder "
                  "resolves the snapshot schema only when a snapshot-id is "
                  "set, so this scan would silently use the table schema"
            : "NotImplemented: use-snapshot-schema is false but snapshot " +
                  std::to_string(snapshot->snapshot_id) + " was written under "
                  "schema " + std::to_string(snapshot_schema_id) +
                  " rather than the current schema " +
                  std::to_string(metadata.current_schema_id) + "; the vendored "
                  "TableScanBuilder always resolves the snapshot schema when a "
                  "snapshot-id is set, so branch-schema resolution is "
                  "unreachable without building a TableScanContext and calling "
                  "DataTableScan::Make directly";
  }
  return false;
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
    if (error) {
      *error = "NotImplemented: declared sort key '" + key.name +
               "' has type " + field->type()->ToString() +
               "; task ordering decodes int and long bounds only — ordering "
               "on this key requires deserializing manifest bounds as "
               "iceberg::Literal of that type and comparing literals";
    }
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

bool SelectSplits(const std::string& file_location, int64_t file_length,
                  const std::shared_ptr<iceberg::FileIO>& io,
                  const iceberg::Schema& schema,
                  const std::shared_ptr<iceberg::Expression>& residual,
                  bool case_sensitive, std::vector<SplitSelection>* splits,
                  bool* all_kept, std::string* error) {
  splits->clear();
  *all_kept = false;

  auto evaluator_r = iceberg::InclusiveMetricsEvaluator::Make(
      residual, schema, case_sensitive);
  if (!evaluator_r.has_value()) {
    if (error) {
      *error = "InclusiveMetricsEvaluator::Make: " + evaluator_r.error().message;
    }
    return false;
  }
  const auto& evaluator = evaluator_r.value();

  auto input_r = iceberg::arrow::OpenArrowInputStream(
      io, file_location, static_cast<size_t>(file_length));
  if (!input_r.has_value()) {
    if (error) {
      *error = "OpenArrowInputStream " + file_location + ": " +
               input_r.error().message;
    }
    return false;
  }
  std::unique_ptr<parquet::ParquetFileReader> reader;
  try {
    reader = parquet::ParquetFileReader::Open(input_r.value());
  } catch (const std::exception& e) {
    if (error) *error = "parquet open " + file_location + ": " + e.what();
    return false;
  }
  const auto* file_meta = reader->metadata().get();
  const auto* descr = file_meta->schema();
  const int num_rg = file_meta->num_row_groups();

  std::vector<int> kept;
  kept.reserve(static_cast<size_t>(num_rg));
  std::vector<int64_t> rg_rows(static_cast<size_t>(num_rg), 0);

  for (int rg = 0; rg < num_rg; ++rg) {
    auto rg_meta = file_meta->RowGroup(rg);
    if (rg > 0 && rg_meta->file_offset() <= 0) {
      if (error) {
        *error = "row group " + std::to_string(rg) + " of " + file_location +
                 " has no file_offset; split planning requires footers that "
                 "record row-group offsets";
      }
      return false;
    }
    rg_rows[static_cast<size_t>(rg)] = rg_meta->num_rows();
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
      kept.push_back(rg);
    }
  }

  if (static_cast<int>(kept.size()) == num_rg) {
    *all_kept = true;
    return true;
  }

  size_t i = 0;
  while (i < kept.size()) {
    size_t j = i;
    while (j + 1 < kept.size() && kept[j + 1] == kept[j] + 1) ++j;
    const int first = kept[i];
    const int last = kept[j];
    const int64_t begin = first == 0 ? 0 : file_meta->RowGroup(first)->file_offset();
    const int64_t end = last + 1 < num_rg
                            ? file_meta->RowGroup(last + 1)->file_offset()
                            : file_length;
    SplitSelection sel;
    sel.split = iceberg::Split{static_cast<size_t>(begin),
                               static_cast<size_t>(end - begin)};
    for (size_t k = i; k <= j; ++k) {
      sel.planned_rows += rg_rows[static_cast<size_t>(kept[k])];
    }
    splits->push_back(sel);
    i = j + 1;
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

  if (!CheckSnapshotSchemaSupported(*metadata, request, error)) return false;

  if (!TableReadTraits::FromMetadata(*metadata, &out->traits, error)) {
    return false;
  }

  {
    auto schema_r = metadata->Schema();
    if (!schema_r.has_value()) {
      if (error) *error = "TableMetadata::Schema: " + schema_r.error().message;
      return false;
    }
    out->table_schema = schema_r.value();
  }

  out->residual = request.filter;

  if (out->traits.sorted() && out->traits.sort_keys.front().ascending) {
    const auto& key = out->traits.sort_keys.front();
    const auto* field = FieldById(*out->table_schema, key.field_id);
    if (field != nullptr) {
      auto key_type =
          std::dynamic_pointer_cast<iceberg::PrimitiveType>(field->type());
      DeriveKeyWindow(request.filter, key.name, key_type, &out->key_lo,
                      &out->key_hi);
    }
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

  const auto& table_schema = out->table_schema;

  if (out->traits.sorted() && !tasks.empty()) {
    if (!SortTasksByLowerBound(&tasks, *table_schema,
                               out->traits.sort_keys.front(), error)) {
      return false;
    }
  }

  int64_t guaranteed_rows = 0;
  const auto min_rows_met = [&]() {
    return request.min_rows_requested.has_value() &&
           guaranteed_rows >= *request.min_rows_requested;
  };

  out->tasks.reserve(tasks.size());
  for (auto& task : tasks) {
    const bool trivial = TrivialResidual(task->residual_filter());
    const bool has_deletes = !task->delete_files().empty();
    if (trivial || has_deletes) {
      FileScanTask planned;
      planned.planned_rows =
          static_cast<int64_t>(task->data_file()->record_count);
      planned.inner = std::move(task);
      out->planned_rows += planned.planned_rows;
      if (trivial && !has_deletes) guaranteed_rows += planned.planned_rows;
      out->tasks.push_back(std::move(planned));
      if (min_rows_met()) break;
      continue;
    }
    const auto& df = task->data_file();
    std::vector<SplitSelection> selected;
    bool all_kept = false;
    if (!SelectSplits(df->file_path,
                      static_cast<int64_t>(df->file_size_in_bytes), io,
                      *table_schema, task->residual_filter(),
                      request.case_sensitive, &selected, &all_kept, error)) {
      return false;
    }
    if (all_kept) {
      FileScanTask planned;
      planned.planned_rows = static_cast<int64_t>(df->record_count);
      planned.inner = std::move(task);
      out->planned_rows += planned.planned_rows;
      out->tasks.push_back(std::move(planned));
      continue;
    }
    for (auto& sel : selected) {
      FileScanTask planned;
      planned.inner = task;
      planned.split = sel.split;
      planned.planned_rows = sel.planned_rows;
      out->planned_rows += sel.planned_rows;
      out->tasks.push_back(std::move(planned));
    }
  }
  return true;
}

}  // namespace primeparts::scan
