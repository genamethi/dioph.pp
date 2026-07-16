#include "primeparts/catalog/partition_stats.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/manifest/manifest_list.h"
#include "iceberg/manifest/manifest_reader.h"
#include "iceberg/partition_field.h"
#include "iceberg/partition_spec.h"
#include "iceberg/result.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/snapshot.h"
#include "iceberg/statistics_file.h"
#include "iceberg/table.h"
#include "iceberg/table_metadata.h"
#include "iceberg/transform.h"
#include "iceberg/type.h"

#include "primeparts/common/uri.h"

namespace primeparts::catalog {

namespace {

namespace fs = std::filesystem;

constexpr int32_t kPartitionFieldId = 1;
constexpr int32_t kSpecIdFieldId = 2;
constexpr int32_t kDataRecordCountFieldId = 3;
constexpr int32_t kDataFileCountFieldId = 4;
constexpr int32_t kTotalDataFileSizeFieldId = 5;
constexpr int32_t kPositionDeleteRecordCountFieldId = 6;
constexpr int32_t kPositionDeleteFileCountFieldId = 7;
constexpr int32_t kEqualityDeleteRecordCountFieldId = 8;
constexpr int32_t kEqualityDeleteFileCountFieldId = 9;
constexpr int32_t kTotalRecordCountFieldId = 10;
constexpr int32_t kLastUpdatedAtFieldId = 11;
constexpr int32_t kLastUpdatedSnapshotIdFieldId = 12;

std::shared_ptr<arrow::KeyValueMetadata> FieldIdMeta(int32_t id) {
  return arrow::key_value_metadata({{"PARQUET:field_id", std::to_string(id)}});
}

int64_t UnixMs(const iceberg::TimePointMs& tp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             tp.time_since_epoch())
      .count();
}

bool TupleFromPartitionValues(const iceberg::PartitionValues& values,
                              size_t expected_fields, const std::string& where,
                              std::vector<int64_t>* out, std::string* error) {
  if (values.num_fields() != expected_fields) {
    if (error) {
      *error = where + ": partition tuple has " +
               std::to_string(values.num_fields()) + " fields, spec declares " +
               std::to_string(expected_fields);
    }
    return false;
  }
  out->clear();
  out->reserve(expected_fields);
  for (size_t i = 0; i < expected_fields; ++i) {
    auto lit = values.ValueAt(i);
    if (!lit.has_value()) {
      if (error) *error = where + ": " + lit.error().message;
      return false;
    }
    const auto& value = lit.value().get().value();
    if (const auto* v32 = std::get_if<int32_t>(&value)) {
      out->push_back(*v32);
    } else if (const auto* v64 = std::get_if<int64_t>(&value)) {
      out->push_back(*v64);
    } else {
      if (error) {
        *error = where + ": partition value " + std::to_string(i) +
                 " is not an integer type";
      }
      return false;
    }
  }
  return true;
}

PartitionStatsRow* RowForTuple(PartitionStatsSet* stats,
                               std::map<std::vector<int64_t>, size_t>* index,
                               const std::vector<int64_t>& tuple) {
  auto it = index->find(tuple);
  if (it != index->end()) return &stats->rows[it->second];
  PartitionStatsRow row;
  row.partition = tuple;
  row.spec_id = stats->spec_id;
  index->emplace(tuple, stats->rows.size());
  stats->rows.push_back(std::move(row));
  return &stats->rows.back();
}

std::map<std::vector<int64_t>, size_t> IndexRows(PartitionStatsSet* stats) {
  std::map<std::vector<int64_t>, size_t> index;
  for (size_t i = 0; i < stats->rows.size(); ++i) {
    index.emplace(stats->rows[i].partition, i);
  }
  return index;
}

void AccumulateDataFile(PartitionStatsRow* row, const iceberg::DataFile& df) {
  row->data_record_count += df.record_count;
  row->data_file_count += 1;
  row->total_data_file_size_in_bytes += df.file_size_in_bytes;
  row->total_record_count = row->data_record_count;
}

std::shared_ptr<arrow::DataType> ArrowTypeFor(iceberg::TypeId type) {
  return type == iceberg::TypeId::kInt
             ? std::static_pointer_cast<arrow::DataType>(arrow::int32())
             : std::static_pointer_cast<arrow::DataType>(arrow::int64());
}

bool AppendTupleValue(arrow::ArrayBuilder* builder, iceberg::TypeId type,
                      int64_t value, std::string* error) {
  arrow::Status st;
  if (type == iceberg::TypeId::kInt) {
    st = static_cast<arrow::Int32Builder*>(builder)->Append(
        static_cast<int32_t>(value));
  } else {
    st = static_cast<arrow::Int64Builder*>(builder)->Append(value);
  }
  if (!st.ok()) {
    if (error) *error = st.ToString();
    return false;
  }
  return true;
}

bool SingleSpecOnly(const iceberg::Table& table, std::string* error) {
  const auto& metadata = table.metadata();
  if (!metadata) {
    if (error) *error = "table has no metadata";
    return false;
  }
  if (metadata->partition_specs.size() > 1) {
    if (error) {
      *error = "partition stats: tables with evolved partition specs are not "
               "implemented";
    }
    return false;
  }
  return true;
}

}  // namespace

bool PartitionStatsFields(const iceberg::Schema& schema,
                          const iceberg::PartitionSpec& spec,
                          PartitionStatsSet* out, std::string* error) {
  *out = PartitionStatsSet{};
  out->spec_id = spec.spec_id();
  for (const auto& pf : spec.fields()) {
    if (!pf.transform() ||
        pf.transform()->transform_type() != iceberg::TransformType::kIdentity) {
      if (error) {
        *error = "partition stats: non-identity transform on partition field " +
                 std::string(pf.name()) + " is not implemented";
      }
      return false;
    }
    const iceberg::SchemaField* source = nullptr;
    for (const auto& f : schema.fields()) {
      if (f.field_id() == pf.source_id()) {
        source = &f;
        break;
      }
    }
    if (!source) {
      if (error) {
        *error = "partition stats: partition field " + std::string(pf.name()) +
                 " has no source field " + std::to_string(pf.source_id());
      }
      return false;
    }
    const auto type = source->type()->type_id();
    if (type != iceberg::TypeId::kInt && type != iceberg::TypeId::kLong) {
      if (error) {
        *error = "partition stats: non-integer partition field " +
                 std::string(pf.name()) + " is not implemented";
      }
      return false;
    }
    out->field_names.emplace_back(pf.name());
    out->field_types.push_back(type);
    out->field_ids.push_back(pf.field_id());
  }
  return true;
}

bool ComputePartitionStats(const iceberg::Table& table,
                           const iceberg::Snapshot& snapshot,
                           PartitionStatsSet* out, std::string* error) {
  if (!SingleSpecOnly(table, error)) return false;
  auto schema_r = table.schema();
  if (!schema_r.has_value()) {
    if (error) *error = "schema: " + schema_r.error().message;
    return false;
  }
  auto spec_r = table.spec();
  if (!spec_r.has_value()) {
    if (error) *error = "spec: " + spec_r.error().message;
    return false;
  }
  if (!PartitionStatsFields(*schema_r.value(), *spec_r.value(), out, error)) {
    return false;
  }

  auto io = table.io();
  iceberg::SnapshotCache cache(&snapshot);
  auto deletes = cache.DeleteManifests(io);
  if (!deletes.has_value()) {
    if (error) *error = "delete manifests: " + deletes.error().message;
    return false;
  }
  if (!deletes.value().empty()) {
    if (error) {
      *error = "partition stats: snapshots with delete manifests are not "
               "implemented";
    }
    return false;
  }
  auto manifests = cache.DataManifests(io);
  if (!manifests.has_value()) {
    if (error) *error = "manifests: " + manifests.error().message;
    return false;
  }

  auto index = IndexRows(out);
  for (const auto& m : manifests.value()) {
    auto reader =
        iceberg::ManifestReader::Make(m, io, schema_r.value(), spec_r.value());
    if (!reader.has_value()) {
      if (error) *error = "manifest: " + reader.error().message;
      return false;
    }
    auto entries = reader.value()->LiveEntries();
    if (!entries.has_value()) {
      if (error) *error = "entries: " + entries.error().message;
      return false;
    }
    for (const auto& entry : entries.value()) {
      const auto& df = entry.data_file;
      if (!df) continue;
      std::vector<int64_t> tuple;
      if (!TupleFromPartitionValues(df->partition, out->field_names.size(),
                                    df->file_path, &tuple, error)) {
        return false;
      }
      AccumulateDataFile(RowForTuple(out, &index, tuple), *df);
    }
  }
  return true;
}

bool MergePartitionStats(
    const std::vector<std::shared_ptr<iceberg::DataFile>>& appended,
    const iceberg::Snapshot& snapshot, PartitionStatsSet* stats,
    std::string* error) {
  auto index = IndexRows(stats);
  const int64_t updated_at = UnixMs(snapshot.timestamp_ms);
  for (const auto& df : appended) {
    if (!df) continue;
    std::vector<int64_t> tuple;
    if (!TupleFromPartitionValues(df->partition, stats->field_names.size(),
                                  df->file_path, &tuple, error)) {
      return false;
    }
    auto* row = RowForTuple(stats, &index, tuple);
    AccumulateDataFile(row, *df);
    row->last_updated_at = updated_at;
    row->last_updated_snapshot_id = snapshot.snapshot_id;
  }
  return true;
}

bool WritePartitionStatsFile(
    const PartitionStatsSet& stats, int64_t snapshot_id,
    const std::string& metadata_dir_uri,
    std::shared_ptr<iceberg::PartitionStatisticsFile>* out,
    std::string* error) {
  std::vector<const PartitionStatsRow*> sorted;
  sorted.reserve(stats.rows.size());
  for (const auto& row : stats.rows) sorted.push_back(&row);
  std::sort(sorted.begin(), sorted.end(),
            [](const PartitionStatsRow* a, const PartitionStatsRow* b) {
              return a->partition < b->partition;
            });

  std::vector<std::shared_ptr<arrow::Field>> tuple_fields;
  std::vector<std::unique_ptr<arrow::ArrayBuilder>> tuple_builders;
  for (size_t i = 0; i < stats.field_names.size(); ++i) {
    tuple_fields.push_back(arrow::field(stats.field_names[i],
                                        ArrowTypeFor(stats.field_types[i]),
                                        /*nullable=*/true,
                                        FieldIdMeta(stats.field_ids[i])));
    if (stats.field_types[i] == iceberg::TypeId::kInt) {
      tuple_builders.push_back(std::make_unique<arrow::Int32Builder>());
    } else {
      tuple_builders.push_back(std::make_unique<arrow::Int64Builder>());
    }
  }

  arrow::Int32Builder spec_id_b;
  arrow::Int64Builder data_records_b;
  arrow::Int32Builder data_files_b;
  arrow::Int64Builder total_size_b;
  arrow::Int64Builder pos_del_records_b;
  arrow::Int32Builder pos_del_files_b;
  arrow::Int64Builder eq_del_records_b;
  arrow::Int32Builder eq_del_files_b;
  arrow::Int64Builder total_records_b;
  arrow::Int64Builder updated_at_b;
  arrow::Int64Builder updated_snap_b;

  auto ok = [&](const arrow::Status& st) {
    if (!st.ok()) {
      if (error) *error = st.ToString();
      return false;
    }
    return true;
  };

  for (const auto* row : sorted) {
    for (size_t i = 0; i < tuple_builders.size(); ++i) {
      if (!AppendTupleValue(tuple_builders[i].get(), stats.field_types[i],
                            row->partition[i], error)) {
        return false;
      }
    }
    if (!ok(spec_id_b.Append(row->spec_id))) return false;
    if (!ok(data_records_b.Append(row->data_record_count))) return false;
    if (!ok(data_files_b.Append(row->data_file_count))) return false;
    if (!ok(total_size_b.Append(row->total_data_file_size_in_bytes))) {
      return false;
    }
    if (!ok(pos_del_records_b.Append(row->position_delete_record_count))) {
      return false;
    }
    if (!ok(pos_del_files_b.Append(row->position_delete_file_count))) {
      return false;
    }
    if (!ok(eq_del_records_b.Append(row->equality_delete_record_count))) {
      return false;
    }
    if (!ok(eq_del_files_b.Append(row->equality_delete_file_count))) {
      return false;
    }
    if (!ok(total_records_b.Append(row->total_record_count))) return false;
    if (!ok(row->last_updated_at ? updated_at_b.Append(*row->last_updated_at)
                                 : updated_at_b.AppendNull())) {
      return false;
    }
    if (!ok(row->last_updated_snapshot_id
                ? updated_snap_b.Append(*row->last_updated_snapshot_id)
                : updated_snap_b.AppendNull())) {
      return false;
    }
  }

  std::vector<std::shared_ptr<arrow::Array>> tuple_arrays;
  for (auto& b : tuple_builders) {
    std::shared_ptr<arrow::Array> a;
    if (!ok(b->Finish(&a))) return false;
    tuple_arrays.push_back(std::move(a));
  }
  auto struct_r = arrow::StructArray::Make(tuple_arrays, tuple_fields);
  if (!ok(struct_r.status())) return false;

  auto finish = [&](arrow::ArrayBuilder& b,
                    std::shared_ptr<arrow::Array>* a) { return ok(b.Finish(a)); };
  std::shared_ptr<arrow::Array> spec_id_a, data_records_a, data_files_a,
      total_size_a, pos_del_records_a, pos_del_files_a, eq_del_records_a,
      eq_del_files_a, total_records_a, updated_at_a, updated_snap_a;
  if (!finish(spec_id_b, &spec_id_a) || !finish(data_records_b, &data_records_a) ||
      !finish(data_files_b, &data_files_a) || !finish(total_size_b, &total_size_a) ||
      !finish(pos_del_records_b, &pos_del_records_a) ||
      !finish(pos_del_files_b, &pos_del_files_a) ||
      !finish(eq_del_records_b, &eq_del_records_a) ||
      !finish(eq_del_files_b, &eq_del_files_a) ||
      !finish(total_records_b, &total_records_a) ||
      !finish(updated_at_b, &updated_at_a) ||
      !finish(updated_snap_b, &updated_snap_a)) {
    return false;
  }

  auto schema = arrow::schema({
      arrow::field("partition", arrow::struct_(tuple_fields), false,
                   FieldIdMeta(kPartitionFieldId)),
      arrow::field("spec_id", arrow::int32(), false,
                   FieldIdMeta(kSpecIdFieldId)),
      arrow::field("data_record_count", arrow::int64(), false,
                   FieldIdMeta(kDataRecordCountFieldId)),
      arrow::field("data_file_count", arrow::int32(), false,
                   FieldIdMeta(kDataFileCountFieldId)),
      arrow::field("total_data_file_size_in_bytes", arrow::int64(), false,
                   FieldIdMeta(kTotalDataFileSizeFieldId)),
      arrow::field("position_delete_record_count", arrow::int64(), true,
                   FieldIdMeta(kPositionDeleteRecordCountFieldId)),
      arrow::field("position_delete_file_count", arrow::int32(), true,
                   FieldIdMeta(kPositionDeleteFileCountFieldId)),
      arrow::field("equality_delete_record_count", arrow::int64(), true,
                   FieldIdMeta(kEqualityDeleteRecordCountFieldId)),
      arrow::field("equality_delete_file_count", arrow::int32(), true,
                   FieldIdMeta(kEqualityDeleteFileCountFieldId)),
      arrow::field("total_record_count", arrow::int64(), true,
                   FieldIdMeta(kTotalRecordCountFieldId)),
      arrow::field("last_updated_at", arrow::int64(), true,
                   FieldIdMeta(kLastUpdatedAtFieldId)),
      arrow::field("last_updated_snapshot_id", arrow::int64(), true,
                   FieldIdMeta(kLastUpdatedSnapshotIdFieldId)),
  });
  auto arrow_table = arrow::Table::Make(
      schema,
      {struct_r.ValueOrDie(), spec_id_a, data_records_a, data_files_a,
       total_size_a, pos_del_records_a, pos_del_files_a, eq_del_records_a,
       eq_del_files_a, total_records_a, updated_at_a, updated_snap_a});

  const std::string filename =
      "partition-stats-" + std::to_string(snapshot_id) + ".parquet";
  const std::string uri = metadata_dir_uri + "/" + filename;
  const fs::path path = primeparts::common::StripFileScheme(uri);
  const fs::path tmp =
      path.parent_path() / ("." + path.filename().string() + ".tmp");
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::remove(tmp, ec);

  auto sink_r = arrow::io::FileOutputStream::Open(tmp.string());
  if (!ok(sink_r.status())) return false;
  auto st = parquet::arrow::WriteTable(
      *arrow_table, arrow::default_memory_pool(), sink_r.ValueOrDie(),
      std::max<int64_t>(1, static_cast<int64_t>(sorted.size())));
  if (!ok(st)) return false;
  if (!ok(sink_r.ValueOrDie()->Close())) return false;
  fs::rename(tmp, path, ec);
  if (ec) {
    if (error) {
      *error = "rename " + tmp.string() + " -> " + path.string() + ": " +
               ec.message();
    }
    return false;
  }

  auto result = std::make_shared<iceberg::PartitionStatisticsFile>();
  result->snapshot_id = snapshot_id;
  result->path = uri;
  result->file_size_in_bytes = static_cast<int64_t>(fs::file_size(path, ec));
  *out = std::move(result);
  return true;
}

bool ReadPartitionStatsFile(const iceberg::PartitionStatisticsFile& file,
                            PartitionStatsSet* stats, std::string* error) {
  stats->rows.clear();
  const std::string path = primeparts::common::StripFileScheme(file.path);
  auto rf = arrow::io::ReadableFile::Open(path);
  if (!rf.ok()) {
    if (error) *error = rf.status().ToString();
    return false;
  }
  auto reader_r =
      parquet::arrow::OpenFile(rf.ValueOrDie(), arrow::default_memory_pool());
  if (!reader_r.ok()) {
    if (error) *error = reader_r.status().ToString();
    return false;
  }
  std::shared_ptr<arrow::Table> table;
  auto st = reader_r.ValueOrDie()->ReadTable(&table);
  if (!st.ok()) {
    if (error) *error = st.ToString();
    return false;
  }
  auto combined = table->CombineChunks();
  if (!combined.ok()) {
    if (error) *error = combined.status().ToString();
    return false;
  }
  table = combined.ValueOrDie();

  auto column = [&](const std::string& name) -> std::shared_ptr<arrow::Array> {
    auto col = table->GetColumnByName(name);
    if (!col || col->num_chunks() == 0) return nullptr;
    return col->chunk(0);
  };
  auto required = [&](const std::string& name,
                      std::shared_ptr<arrow::Array>* out_arr) {
    *out_arr = column(name);
    if (!*out_arr) {
      if (error) *error = file.path + ": missing column " + name;
      return false;
    }
    return true;
  };

  std::shared_ptr<arrow::Array> partition_a, spec_id_a, data_records_a,
      data_files_a, total_size_a;
  if (!required("partition", &partition_a) ||
      !required("spec_id", &spec_id_a) ||
      !required("data_record_count", &data_records_a) ||
      !required("data_file_count", &data_files_a) ||
      !required("total_data_file_size_in_bytes", &total_size_a)) {
    return false;
  }
  auto total_records_a = column("total_record_count");
  auto updated_at_a = column("last_updated_at");
  auto updated_snap_a = column("last_updated_snapshot_id");

  if (partition_a->type_id() != arrow::Type::STRUCT) {
    if (error) *error = file.path + ": partition column is not a struct";
    return false;
  }
  const auto& tuple = static_cast<const arrow::StructArray&>(*partition_a);
  std::vector<std::shared_ptr<arrow::Array>> tuple_cols;
  for (size_t i = 0; i < stats->field_names.size(); ++i) {
    auto child = tuple.GetFieldByName(stats->field_names[i]);
    if (!child) {
      if (error) {
        *error = file.path + ": partition struct missing field " +
                 stats->field_names[i];
      }
      return false;
    }
    const auto want = stats->field_types[i] == iceberg::TypeId::kInt
                          ? arrow::Type::INT32
                          : arrow::Type::INT64;
    if (child->type_id() != want) {
      if (error) {
        *error = file.path + ": partition field " + stats->field_names[i] +
                 " has unexpected physical type";
      }
      return false;
    }
    tuple_cols.push_back(std::move(child));
  }

  const int64_t n = table->num_rows();
  auto i64_at = [](const arrow::Array& a, int64_t i) {
    return a.type_id() == arrow::Type::INT32
               ? static_cast<int64_t>(
                     static_cast<const arrow::Int32Array&>(a).Value(i))
               : static_cast<const arrow::Int64Array&>(a).Value(i);
  };
  for (int64_t i = 0; i < n; ++i) {
    PartitionStatsRow row;
    for (const auto& arr : tuple_cols) {
      row.partition.push_back(i64_at(*arr, i));
    }
    row.spec_id = static_cast<const arrow::Int32Array&>(*spec_id_a).Value(i);
    row.data_record_count = i64_at(*data_records_a, i);
    row.data_file_count =
        static_cast<const arrow::Int32Array&>(*data_files_a).Value(i);
    row.total_data_file_size_in_bytes = i64_at(*total_size_a, i);
    row.total_record_count = total_records_a && !total_records_a->IsNull(i)
                                 ? i64_at(*total_records_a, i)
                                 : row.data_record_count;
    if (updated_at_a && !updated_at_a->IsNull(i)) {
      row.last_updated_at = i64_at(*updated_at_a, i);
    }
    if (updated_snap_a && !updated_snap_a->IsNull(i)) {
      row.last_updated_snapshot_id = i64_at(*updated_snap_a, i);
    }
    stats->rows.push_back(std::move(row));
  }
  return true;
}

namespace {

bool StatsForSnapshot(const iceberg::Table& table,
                      const iceberg::Snapshot& snapshot,
                      PartitionStatsSet* out, std::string* error) {
  const auto& metadata = table.metadata();
  if (!metadata) {
    if (error) *error = "table has no metadata";
    return false;
  }
  for (const auto& entry : metadata->partition_statistics) {
    if (entry && entry->snapshot_id == snapshot.snapshot_id) {
      auto schema_r = table.schema();
      if (!schema_r.has_value()) {
        if (error) *error = "schema: " + schema_r.error().message;
        return false;
      }
      auto spec_r = table.spec();
      if (!spec_r.has_value()) {
        if (error) *error = "spec: " + spec_r.error().message;
        return false;
      }
      if (!PartitionStatsFields(*schema_r.value(), *spec_r.value(), out,
                                error)) {
        return false;
      }
      return ReadPartitionStatsFile(*entry, out, error);
    }
  }
  return ComputePartitionStats(table, snapshot, out, error);
}

}  // namespace

bool LoadPartitionStats(const iceberg::Table& table, PartitionStatsSet* out,
                        std::string* error) {
  if (!SingleSpecOnly(table, error)) return false;
  auto snap_r = table.current_snapshot();
  if (!snap_r.has_value()) {
    if (error) *error = "current snapshot: " + snap_r.error().message;
    return false;
  }
  if (!snap_r.value()) {
    if (error) *error = "table has no current snapshot";
    return false;
  }
  return StatsForSnapshot(table, *snap_r.value(), out, error);
}

std::shared_ptr<iceberg::PartitionStatisticsFile> BuildPartitionStatsForAppend(
    const iceberg::Table& table, const iceberg::Snapshot& new_snapshot,
    const std::vector<std::shared_ptr<iceberg::DataFile>>& appended,
    std::string* error) {
  if (!SingleSpecOnly(table, error)) return nullptr;
  PartitionStatsSet stats;
  auto current_r = table.current_snapshot();
  if (!current_r.has_value()) {
    if (error) *error = "current snapshot: " + current_r.error().message;
    return nullptr;
  }
  if (current_r.value()) {
    if (!StatsForSnapshot(table, *current_r.value(), &stats, error)) {
      return nullptr;
    }
  } else {
    auto schema_r = table.schema();
    if (!schema_r.has_value()) {
      if (error) *error = "schema: " + schema_r.error().message;
      return nullptr;
    }
    auto spec_r = table.spec();
    if (!spec_r.has_value()) {
      if (error) *error = "spec: " + spec_r.error().message;
      return nullptr;
    }
    if (!PartitionStatsFields(*schema_r.value(), *spec_r.value(), &stats,
                              error)) {
      return nullptr;
    }
  }
  if (!MergePartitionStats(appended, new_snapshot, &stats, error)) {
    return nullptr;
  }

  const std::string metadata_location(table.metadata_file_location());
  const auto slash = metadata_location.rfind('/');
  if (slash == std::string::npos) {
    if (error) {
      *error = "cannot derive metadata dir from " + metadata_location;
    }
    return nullptr;
  }
  std::shared_ptr<iceberg::PartitionStatisticsFile> out;
  if (!WritePartitionStatsFile(stats, new_snapshot.snapshot_id,
                               metadata_location.substr(0, slash), &out,
                               error)) {
    return nullptr;
  }
  return out;
}

}  // namespace primeparts::catalog
