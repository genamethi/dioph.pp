#include "primeparts/catalog/partition_stats.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include "iceberg/expression/literal.h"
#include "iceberg/file_format.h"
#include "iceberg/file_io.h"
#include "iceberg/file_reader.h"
#include "iceberg/file_writer.h"
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

namespace primeparts::catalog {

namespace {

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

std::shared_ptr<iceberg::Type> IcebergTypeFor(iceberg::TypeId type) {
  return type == iceberg::TypeId::kInt
             ? std::static_pointer_cast<iceberg::Type>(iceberg::int32())
             : std::static_pointer_cast<iceberg::Type>(iceberg::int64());
}

std::shared_ptr<iceberg::Schema> StatsFileSchema(
    const PartitionStatsSet& stats) {
  std::vector<iceberg::SchemaField> tuple_fields;
  for (size_t i = 0; i < stats.field_names.size(); ++i) {
    tuple_fields.push_back(iceberg::SchemaField::MakeOptional(
        stats.field_ids[i], stats.field_names[i],
        IcebergTypeFor(stats.field_types[i])));
  }
  std::vector<iceberg::SchemaField> fields;
  fields.push_back(iceberg::SchemaField::MakeRequired(
      kPartitionFieldId, "partition",
      std::make_shared<iceberg::StructType>(std::move(tuple_fields))));
  fields.push_back(iceberg::SchemaField::MakeRequired(
      kSpecIdFieldId, "spec_id", iceberg::int32()));
  fields.push_back(iceberg::SchemaField::MakeRequired(
      kDataRecordCountFieldId, "data_record_count", iceberg::int64()));
  fields.push_back(iceberg::SchemaField::MakeRequired(
      kDataFileCountFieldId, "data_file_count", iceberg::int32()));
  fields.push_back(iceberg::SchemaField::MakeRequired(
      kTotalDataFileSizeFieldId, "total_data_file_size_in_bytes",
      iceberg::int64()));
  fields.push_back(iceberg::SchemaField::MakeOptional(
      kPositionDeleteRecordCountFieldId, "position_delete_record_count",
      iceberg::int64()));
  fields.push_back(iceberg::SchemaField::MakeOptional(
      kPositionDeleteFileCountFieldId, "position_delete_file_count",
      iceberg::int32()));
  fields.push_back(iceberg::SchemaField::MakeOptional(
      kEqualityDeleteRecordCountFieldId, "equality_delete_record_count",
      iceberg::int64()));
  fields.push_back(iceberg::SchemaField::MakeOptional(
      kEqualityDeleteFileCountFieldId, "equality_delete_file_count",
      iceberg::int32()));
  fields.push_back(iceberg::SchemaField::MakeOptional(
      kTotalRecordCountFieldId, "total_record_count", iceberg::int64()));
  fields.push_back(iceberg::SchemaField::MakeOptional(
      kLastUpdatedAtFieldId, "last_updated_at", iceberg::int64()));
  fields.push_back(iceberg::SchemaField::MakeOptional(
      kLastUpdatedSnapshotIdFieldId, "last_updated_snapshot_id",
      iceberg::int64()));
  return std::make_shared<iceberg::Schema>(std::move(fields), 0);
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
        *error = "NotImplemented: " + where + ": partition value " +
                 std::to_string(i) + " holds " +
                 lit.value().get().ToString() +
                 ", not an int or long; stats tuples are int/long only — "
                 "supporting it requires carrying iceberg::Literal tuples "
                 "through PartitionStatsRow and the stats file schema";
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
      *error = "NotImplemented: table has " +
               std::to_string(metadata->partition_specs.size()) +
               " partition specs; partition stats handle a single spec only — "
               "supporting evolution requires the spec's unified partition "
               "tuple (field union across specs keyed by partition field id)";
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
        *error = "NotImplemented: partition field " + std::string(pf.name()) +
                 " uses transform '" +
                 (pf.transform() ? pf.transform()->ToString()
                                 : std::string("null")) +
                 "'; partition stats resolve identity transforms only — "
                 "supporting it requires keying stats rows by the transformed "
                 "partition value type";
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
        *error = "NotImplemented: partition field " + std::string(pf.name()) +
                 " has source type " + source->type()->ToString() +
                 "; partition stats tuples are int/long only — supporting it "
                 "requires carrying iceberg::Literal tuples through "
                 "PartitionStatsRow and the stats file schema";
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
      *error = "NotImplemented: snapshot " +
               std::to_string(snapshot.snapshot_id) + " carries " +
               std::to_string(deletes.value().size()) +
               " delete manifests; partition stats accumulate data manifests "
               "only — supporting deletes requires walking delete entries "
               "into the position/equality delete columns of each stats row";
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
    const std::shared_ptr<iceberg::FileIO>& io,
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
                                        true));
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
      arrow::field("partition", arrow::struct_(tuple_fields), false),
      arrow::field("spec_id", arrow::int32(), false),
      arrow::field("data_record_count", arrow::int64(), false),
      arrow::field("data_file_count", arrow::int32(), false),
      arrow::field("total_data_file_size_in_bytes", arrow::int64(), false),
      arrow::field("position_delete_record_count", arrow::int64(), true),
      arrow::field("position_delete_file_count", arrow::int32(), true),
      arrow::field("equality_delete_record_count", arrow::int64(), true),
      arrow::field("equality_delete_file_count", arrow::int32(), true),
      arrow::field("total_record_count", arrow::int64(), true),
      arrow::field("last_updated_at", arrow::int64(), true),
      arrow::field("last_updated_snapshot_id", arrow::int64(), true),
  });
  auto batch = arrow::RecordBatch::Make(
      schema, static_cast<int64_t>(sorted.size()),
      {struct_r.ValueOrDie(), spec_id_a, data_records_a, data_files_a,
       total_size_a, pos_del_records_a, pos_del_files_a, eq_del_records_a,
       eq_del_files_a, total_records_a, updated_at_a, updated_snap_a});

  const std::string filename =
      "partition-stats-" + std::to_string(snapshot_id) + ".parquet";
  const std::string location = metadata_dir_uri + "/" + filename;

  iceberg::WriterOptions wopts;
  wopts.path = location;
  wopts.schema = StatsFileSchema(stats);
  wopts.io = io;
  auto writer_r = iceberg::WriterFactoryRegistry::Open(
      iceberg::FileFormatType::kParquet, wopts);
  if (!writer_r.has_value()) {
    if (error) {
      *error = "WriterFactoryRegistry::Open " + location + ": " +
               writer_r.error().message;
    }
    return false;
  }
  auto writer = std::move(writer_r.value());

  ArrowArray c_array;
  if (!ok(arrow::ExportRecordBatch(*batch, &c_array))) return false;
  auto write_st = writer->Write(&c_array);
  if (!write_st.has_value()) {
    if (error) *error = "Writer::Write: " + write_st.error().message;
    return false;
  }
  auto close_st = writer->Close();
  if (!close_st.has_value()) {
    if (error) *error = "Writer::Close: " + close_st.error().message;
    return false;
  }
  auto length_r = writer->length();
  if (!length_r.has_value()) {
    if (error) *error = "Writer::length: " + length_r.error().message;
    return false;
  }

  auto result = std::make_shared<iceberg::PartitionStatisticsFile>();
  result->snapshot_id = snapshot_id;
  result->path = location;
  result->file_size_in_bytes = length_r.value();
  *out = std::move(result);
  return true;
}

bool ReadPartitionStatsFile(const iceberg::PartitionStatisticsFile& file,
                            const std::shared_ptr<iceberg::FileIO>& io,
                            PartitionStatsSet* stats, std::string* error) {
  stats->rows.clear();

  iceberg::ReaderOptions ropts;
  ropts.path = file.path;
  if (file.file_size_in_bytes > 0) {
    ropts.length = static_cast<size_t>(file.file_size_in_bytes);
  }
  ropts.io = io;
  ropts.projection = StatsFileSchema(*stats);
  auto reader_r = iceberg::ReaderFactoryRegistry::Open(
      iceberg::FileFormatType::kParquet, ropts);
  if (!reader_r.has_value()) {
    if (error) {
      *error = "ReaderFactoryRegistry::Open " + file.path + ": " +
               reader_r.error().message;
    }
    return false;
  }
  auto reader = std::move(reader_r.value());
  auto cschema_r = reader->Schema();
  if (!cschema_r.has_value()) {
    if (error) *error = "Reader::Schema: " + cschema_r.error().message;
    return false;
  }
  ArrowSchema cschema = cschema_r.value();
  auto arrow_schema_r = arrow::ImportSchema(&cschema);
  if (!arrow_schema_r.ok()) {
    if (error) *error = arrow_schema_r.status().ToString();
    return false;
  }
  auto arrow_schema = std::move(arrow_schema_r).ValueOrDie();

  auto i64_at = [](const arrow::Array& a, int64_t i) {
    return a.type_id() == arrow::Type::INT32
               ? static_cast<int64_t>(
                     static_cast<const arrow::Int32Array&>(a).Value(i))
               : static_cast<const arrow::Int64Array&>(a).Value(i);
  };

  while (true) {
    auto next_r = reader->Next();
    if (!next_r.has_value()) {
      if (error) *error = "Reader::Next: " + next_r.error().message;
      return false;
    }
    if (!next_r.value().has_value()) break;
    ArrowArray c_array = std::move(next_r.value().value());
    auto batch_r = arrow::ImportRecordBatch(&c_array, arrow_schema);
    if (!batch_r.ok()) {
      if (error) *error = batch_r.status().ToString();
      return false;
    }
    auto rb = std::move(batch_r).ValueOrDie();
    if (rb->num_columns() != 12) {
      if (error) {
        *error = file.path + ": expected 12 stats columns, got " +
                 std::to_string(rb->num_columns());
      }
      return false;
    }
    const auto& partition_a = rb->column(0);
    if (partition_a->type_id() != arrow::Type::STRUCT) {
      if (error) *error = file.path + ": partition column is not a struct";
      return false;
    }
    const auto& tuple = static_cast<const arrow::StructArray&>(*partition_a);
    if (tuple.num_fields() != static_cast<int>(stats->field_names.size())) {
      if (error) {
        *error = file.path + ": partition struct has " +
                 std::to_string(tuple.num_fields()) +
                 " fields, spec declares " +
                 std::to_string(stats->field_names.size());
      }
      return false;
    }
    const auto& spec_id_a = rb->column(1);
    const auto& data_records_a = rb->column(2);
    const auto& data_files_a = rb->column(3);
    const auto& total_size_a = rb->column(4);
    const auto& pos_del_records_a = rb->column(5);
    const auto& pos_del_files_a = rb->column(6);
    const auto& eq_del_records_a = rb->column(7);
    const auto& eq_del_files_a = rb->column(8);
    const auto& total_records_a = rb->column(9);
    const auto& updated_at_a = rb->column(10);
    const auto& updated_snap_a = rb->column(11);

    for (int64_t i = 0; i < rb->num_rows(); ++i) {
      PartitionStatsRow row;
      for (int f = 0; f < tuple.num_fields(); ++f) {
        row.partition.push_back(i64_at(*tuple.field(f), i));
      }
      row.spec_id = static_cast<const arrow::Int32Array&>(*spec_id_a).Value(i);
      row.data_record_count = i64_at(*data_records_a, i);
      row.data_file_count =
          static_cast<const arrow::Int32Array&>(*data_files_a).Value(i);
      row.total_data_file_size_in_bytes = i64_at(*total_size_a, i);
      if (!pos_del_records_a->IsNull(i)) {
        row.position_delete_record_count = i64_at(*pos_del_records_a, i);
      }
      if (!pos_del_files_a->IsNull(i)) {
        row.position_delete_file_count = static_cast<int32_t>(
            i64_at(*pos_del_files_a, i));
      }
      if (!eq_del_records_a->IsNull(i)) {
        row.equality_delete_record_count = i64_at(*eq_del_records_a, i);
      }
      if (!eq_del_files_a->IsNull(i)) {
        row.equality_delete_file_count = static_cast<int32_t>(
            i64_at(*eq_del_files_a, i));
      }
      row.total_record_count = !total_records_a->IsNull(i)
                                   ? i64_at(*total_records_a, i)
                                   : row.data_record_count;
      if (!updated_at_a->IsNull(i)) {
        row.last_updated_at = i64_at(*updated_at_a, i);
      }
      if (!updated_snap_a->IsNull(i)) {
        row.last_updated_snapshot_id = i64_at(*updated_snap_a, i);
      }
      stats->rows.push_back(std::move(row));
    }
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
      return ReadPartitionStatsFile(*entry, table.io(), out, error);
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
                               metadata_location.substr(0, slash), table.io(),
                               &out, error)) {
    return nullptr;
  }
  return out;
}

}  // namespace primeparts::catalog
