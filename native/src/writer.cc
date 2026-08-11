#include "primeparts/writer.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <parquet/types.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/transform.h"
#include "iceberg/type.h"

namespace primeparts {

namespace {

uint32_t FileToken() {
  static thread_local std::mt19937 rng(
      std::random_device{}() ^
      static_cast<uint32_t>(
          std::hash<std::thread::id>{}(std::this_thread::get_id())));
  return rng();
}

fs::path FilePathFor(const fs::path& dir, std::string_view prefix,
                     int32_t bucket_version, int32_t bucket, int32_t seq,
                     bool simple) {
  char name[176];
  const uint32_t tok = FileToken();
  if (simple) {
    std::snprintf(name, sizeof(name), "%.*s_%08x_%04d.parquet",
                  static_cast<int>(prefix.size()), prefix.data(), tok, seq);
  } else {
    std::snprintf(name, sizeof(name), "%.*s_v%04d_b%06d_%08x_%04d.parquet",
                  static_cast<int>(prefix.size()), prefix.data(),
                  bucket_version, bucket, tok, seq);
  }
  return dir / name;
}

std::shared_ptr<parquet::WriterProperties> ParquetWriterProperties(
    const WriterConfig& config, const arrow::Schema& arrow_schema) {
  parquet::WriterProperties::Builder builder;
  builder.compression(parquet::Compression::ZSTD);
  builder.compression_level(config.compression_level);
  builder.data_pagesize(config.data_pagesize);
  builder.max_row_group_length(config.max_row_group_rows);
  for (const auto& col : config.delta_columns) {
    if (arrow_schema.GetFieldByName(col)) {
      builder.disable_dictionary(col);
      builder.encoding(col, parquet::Encoding::DELTA_BINARY_PACKED);
    }
  }
  return builder.build();
}

struct ResolvedStatColumn {
  int32_t field_id = -1;
  iceberg::TypeId type = iceberg::TypeId::kLong;
  int arrow_index = -1;
  bool sorted = false;
};

std::pair<iceberg::Literal, iceberg::Literal> BatchColumnBounds(
    const arrow::Array& array, iceberg::TypeId type, bool sorted) {
  const int64_t n = array.length();
  if (type == iceberg::TypeId::kInt) {
    const auto& a = static_cast<const arrow::Int32Array&>(array);
    if (sorted)
      return {iceberg::Literal::Int(a.Value(0)),
              iceberg::Literal::Int(a.Value(n - 1))};
    int32_t lo = a.Value(0), hi = a.Value(0);
    for (int64_t i = 1; i < n; ++i) {
      lo = std::min(lo, a.Value(i));
      hi = std::max(hi, a.Value(i));
    }
    return {iceberg::Literal::Int(lo), iceberg::Literal::Int(hi)};
  }
  if (type == iceberg::TypeId::kString) {
    const auto& a = static_cast<const arrow::StringArray&>(array);
    if (sorted)
      return {iceberg::Literal::String(a.GetString(0)),
              iceberg::Literal::String(a.GetString(n - 1))};
    std::string lo = a.GetString(0), hi = a.GetString(0);
    for (int64_t i = 1; i < n; ++i) {
      std::string v = a.GetString(i);
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    return {iceberg::Literal::String(std::move(lo)),
            iceberg::Literal::String(std::move(hi))};
  }
  const auto& a = static_cast<const arrow::Int64Array&>(array);
  if (sorted)
    return {iceberg::Literal::Long(a.Value(0)),
            iceberg::Literal::Long(a.Value(n - 1))};
  int64_t lo = a.Value(0), hi = a.Value(0);
  for (int64_t i = 1; i < n; ++i) {
    lo = std::min(lo, a.Value(i));
    hi = std::max(hi, a.Value(i));
  }
  return {iceberg::Literal::Long(lo), iceberg::Literal::Long(hi)};
}

bool LiteralLess(const iceberg::Literal& a, const iceberg::Literal& b) {
  return (a <=> b) < 0;
}

bool PutBound(std::map<int32_t, std::vector<uint8_t>>* bounds, int32_t field_id,
              const iceberg::Literal& literal, std::string* error) {
  auto serialized = literal.Serialize();
  if (!serialized.has_value()) {
    if (error) *error = serialized.error().message;
    return false;
  }
  (*bounds)[field_id] = std::move(serialized.value());
  return true;
}

bool BuildDataFile(const WriterConfig& config,
                   const std::shared_ptr<iceberg::PartitionSpec>& spec,
                   const WrittenFile& file,
                   std::shared_ptr<iceberg::DataFile>* out,
                   std::string* error) {
  auto data_file = std::make_shared<iceberg::DataFile>();
  data_file->content = iceberg::DataFile::Content::kData;
  data_file->file_path = file.path.string();
  data_file->file_format = iceberg::FileFormatType::kParquet;
  if (config.partition_values) {
    data_file->partition = *config.partition_values;
  } else {
    data_file->partition = iceberg::PartitionValues();
  }
  data_file->record_count = file.rows;
  data_file->file_size_in_bytes = file.bytes;
  data_file->partition_spec_id = spec->spec_id();

  for (const auto& field : config.schema->fields()) {
    data_file->value_counts[field.field_id()] = file.rows;
    if (!field.optional()) {
      data_file->null_value_counts[field.field_id()] = 0;
    }
  }

  for (const auto& [field_id, bounds] : file.bounds) {
    if (!PutBound(&data_file->lower_bounds, field_id, bounds.first, error) ||
        !PutBound(&data_file->upper_bounds, field_id, bounds.second, error)) {
      return false;
    }
  }

  data_file->split_offsets = file.split_offsets;

  *out = std::move(data_file);
  return true;
}

}  // namespace

std::shared_ptr<arrow::Schema> IcebergToArrowSchemaWithFieldIds(
    const iceberg::Schema& schema, std::string* error,
    const iceberg::PartitionSpec* partition_spec) {
  (void)partition_spec;
  arrow::FieldVector fields;
  fields.reserve(schema.fields().size());
  for (const auto& f : schema.fields()) {
    std::shared_ptr<arrow::DataType> at;
    auto tid = f.type()->type_id();
    if (tid == iceberg::TypeId::kLong) {
      at = arrow::int64();
    } else if (tid == iceberg::TypeId::kInt) {
      at = arrow::int32();
    } else if (tid == iceberg::TypeId::kString) {
      at = arrow::utf8();
    } else {
      if (error) *error = std::string("unsupported field type for: ") +
                          std::string(f.name());
      return nullptr;
    }
    auto kv = arrow::key_value_metadata(
        {{"PARQUET:field_id", std::to_string(f.field_id())}});
    const bool nullable = f.optional();
    fields.push_back(
        arrow::field(std::string(f.name()), at, nullable, kv));
  }
  return arrow::schema(fields);
}

struct BucketParquetWriter::Impl {
  WriterConfig config;
  std::shared_ptr<arrow::Schema> arrow_schema;
  std::shared_ptr<iceberg::PartitionSpec> partition_spec;
  std::shared_ptr<parquet::WriterProperties> props;
  std::vector<ResolvedStatColumn> stat_columns;
  int32_t next_seq = 0;
  bool closed = false;

  std::shared_ptr<arrow::io::FileOutputStream> sink;
  std::unique_ptr<parquet::arrow::FileWriter> writer;
  fs::path tmp_path;
  fs::path final_path;
  WrittenFile current_record;
  std::vector<std::optional<std::pair<iceberg::Literal, iceberg::Literal>>>
      current_bounds;

  std::vector<WrittenFile> done;
  std::unordered_map<std::string, int64_t> partition_constants;

  bool OpenIfNeeded(std::string* error);
  bool CloseCurrent(std::string* error);
  bool CutRowGroup(int64_t* flushed_bytes, std::string* error);
};

bool BucketParquetWriter::Impl::OpenIfNeeded(std::string* error) {
  if (writer) return true;
  std::error_code ec;
  fs::create_directories(config.output_dir, ec);
  if (ec) {
    if (error) *error = "mkdir " + config.output_dir.string() + ": " + ec.message();
    return false;
  }

  final_path = FilePathFor(config.output_dir, config.filename_prefix,
                           config.bucket_version, config.bucket, next_seq,
                           config.simple_filename);
  if (fs::exists(final_path)) {
    if (error) *error = "refusing to overwrite: " + final_path.string();
    return false;
  }
  tmp_path = final_path.parent_path() /
             ("." + final_path.filename().string() + ".tmp");
  fs::remove(tmp_path, ec);

  auto sink_r = arrow::io::FileOutputStream::Open(tmp_path.string());
  if (!sink_r.ok()) {
    if (error) *error = sink_r.status().ToString();
    return false;
  }
  sink = sink_r.ValueOrDie();
  auto fw_r = parquet::arrow::FileWriter::Open(
      *arrow_schema, arrow::default_memory_pool(), sink, props,
      parquet::default_arrow_writer_properties());
  if (!fw_r.ok()) {
    if (error) *error = fw_r.status().ToString();
    sink.reset();
    return false;
  }
  writer = std::move(fw_r).ValueOrDie();

  current_record = WrittenFile{};
  current_record.table = config.table_name;
  current_record.bucket_version = config.bucket_version;
  current_record.bucket = config.bucket;
  current_bounds.assign(stat_columns.size(), std::nullopt);
  return true;
}

bool BucketParquetWriter::Impl::CutRowGroup(int64_t* flushed_bytes,
                                            std::string* error) {
  if (!writer) {
    if (flushed_bytes) *flushed_bytes = 0;
    return true;
  }
  auto start_r = sink->Tell();
  if (!start_r.ok()) {
    if (error) *error = start_r.status().ToString();
    return false;
  }
  const int64_t start = start_r.ValueOrDie();
  current_record.split_offsets.push_back(start);
  auto ns = writer->NewBufferedRowGroup();
  if (!ns.ok()) {
    if (error) *error = ns.ToString();
    return false;
  }
  auto end_r = sink->Tell();
  if (!end_r.ok()) {
    if (error) *error = end_r.status().ToString();
    return false;
  }
  if (flushed_bytes) *flushed_bytes = end_r.ValueOrDie() - start;
  return true;
}

bool BucketParquetWriter::Impl::CloseCurrent(std::string* error) {
  if (!writer) return true;
  if (!current_record.split_offsets.empty()) {
    auto pos = sink->Tell();
    if (!pos.ok()) {
      if (error) *error = pos.status().ToString();
      return false;
    }
    current_record.split_offsets.push_back(pos.ValueOrDie());
  }
  auto cs = writer->Close();
  if (!cs.ok()) {
    if (error) *error = cs.ToString();
    return false;
  }
  auto ss = sink->Close();
  if (!ss.ok()) {
    if (error) *error = ss.ToString();
    return false;
  }
  std::error_code ec;
  current_record.bytes = static_cast<int64_t>(fs::file_size(tmp_path, ec));
  fs::rename(tmp_path, final_path, ec);
  if (ec) {
    if (error) *error = "rename " + tmp_path.string() + " -> " +
                        final_path.string() + ": " + ec.message();
    return false;
  }
  current_record.path = final_path;
  if (current_record.rows > 0) {
    for (size_t i = 0; i < stat_columns.size(); ++i) {
      if (!current_bounds[i]) continue;
      current_record.bounds.emplace(stat_columns[i].field_id,
                                    *current_bounds[i]);
    }
  }
  if (!BuildDataFile(config, partition_spec, current_record,
                     &current_record.data_file, error)) {
    return false;
  }
  done.push_back(current_record);

  writer.reset();
  sink.reset();
  next_seq++;
  return true;
}

std::unique_ptr<BucketParquetWriter> BucketParquetWriter::Make(
    WriterConfig config, std::string* error) {
  auto impl = std::make_unique<Impl>();
  if (!config.schema) {
    if (error) *error = "WriterConfig.schema is null";
    return nullptr;
  }
  if (!config.partition_spec) {
    if (error) *error = "WriterConfig.partition_spec is null";
    return nullptr;
  }
  impl->partition_spec = config.partition_spec;
  if (!impl->partition_spec->fields().empty() && !config.partition_values) {
    if (error) {
      *error = "WriterConfig.partition_values required for partitioned table " +
               config.table_name;
    }
    return nullptr;
  }
  impl->arrow_schema = IcebergToArrowSchemaWithFieldIds(
      *config.schema, error, impl->partition_spec.get());
  if (!impl->arrow_schema) return nullptr;

  {
    const auto& pfs = impl->partition_spec->fields();
    for (size_t i = 0; i < pfs.size(); ++i) {
      if (!pfs[i].transform() ||
          pfs[i].transform()->transform_type() !=
              iceberg::TransformType::kIdentity) {
        continue;
      }
      auto at = config.partition_values->ValueAt(i);
      if (!at.has_value()) {
        if (error) {
          *error = "partition value " + std::to_string(i) + " missing for " +
                   config.table_name;
        }
        return nullptr;
      }
      const auto& v = at.value().get().value();
      int64_t as_int = 0;
      if (const auto* v32 = std::get_if<int32_t>(&v)) {
        as_int = *v32;
      } else if (const auto* v64 = std::get_if<int64_t>(&v)) {
        as_int = *v64;
      } else {
        if (error) {
          *error = "partition value " + std::to_string(i) +
                   " is not an integer for " + config.table_name;
        }
        return nullptr;
      }
      for (const auto& f : config.schema->fields()) {
        if (f.field_id() == pfs[i].source_id()) {
          impl->partition_constants.emplace(std::string(f.name()), as_int);
          break;
        }
      }
    }
  }

  for (const auto& sc : config.stat_columns) {
    ResolvedStatColumn rs;
    rs.sorted = sc.sorted;
    std::string type_name;
    for (const auto& f : config.schema->fields()) {
      if (f.name() == sc.name) {
        rs.field_id = f.field_id();
        rs.type = f.type()->type_id();
        type_name = f.type()->ToString();
        break;
      }
    }
    if (rs.field_id < 0) {
      if (error) *error = "stat column not in schema: " + sc.name;
      return nullptr;
    }
    if (rs.type != iceberg::TypeId::kInt && rs.type != iceberg::TypeId::kLong &&
        rs.type != iceberg::TypeId::kString) {
      if (error) {
        *error = "NotImplemented: declared stat column '" + sc.name +
                 "' has type " + type_name +
                 "; bound capture is implemented for int, long, and string only";
      }
      return nullptr;
    }
    rs.arrow_index = impl->arrow_schema->GetFieldIndex(sc.name);
    if (rs.arrow_index < 0) {
      if (error) *error = "stat column not physical (partition-identity): " + sc.name;
      return nullptr;
    }
    impl->stat_columns.push_back(rs);
  }

  impl->props = ParquetWriterProperties(config, *impl->arrow_schema);
  impl->next_seq = config.starting_file_seq;
  impl->config = std::move(config);

  return std::unique_ptr<BucketParquetWriter>(
      new BucketParquetWriter(std::move(impl)));
}

BucketParquetWriter::BucketParquetWriter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

BucketParquetWriter::~BucketParquetWriter() {
  if (impl_ && !impl_->closed && impl_->writer) {
    std::string ignored;
    impl_->CloseCurrent(&ignored);
  }
}

bool BucketParquetWriter::Write(const arrow::RecordBatch& batch,
                                std::string* error) {
  if (impl_->closed) {
    if (error) *error = "Write after Close";
    return false;
  }
  if (impl_->writer && impl_->config.target_rows_per_file > 0 &&
      impl_->current_record.rows >= impl_->config.target_rows_per_file) {
    if (!impl_->CloseCurrent(error)) return false;
  }
  if (!impl_->OpenIfNeeded(error)) return false;

  std::vector<std::shared_ptr<arrow::Array>> projected;
  projected.reserve(impl_->arrow_schema->num_fields());
  for (const auto& f : impl_->arrow_schema->fields()) {
    auto col = batch.GetColumnByName(f->name());
    if (!col) {
      auto pv = impl_->partition_constants.find(f->name());
      if (pv == impl_->partition_constants.end()) {
        if (error) {
          *error = "input batch missing column: " + f->name();
        }
        return false;
      }
      auto scalar = arrow::MakeScalar(f->type(), pv->second);
      if (!scalar.ok()) {
        if (error) *error = scalar.status().ToString();
        return false;
      }
      auto arr = arrow::MakeArrayFromScalar(*scalar.ValueOrDie(),
                                            batch.num_rows());
      if (!arr.ok()) {
        if (error) *error = arr.status().ToString();
        return false;
      }
      projected.push_back(arr.MoveValueUnsafe());
      continue;
    }
    projected.push_back(std::move(col));
  }
  auto rb = arrow::RecordBatch::Make(impl_->arrow_schema, batch.num_rows(),
                                     std::move(projected));
  auto ws = impl_->writer->WriteRecordBatch(*rb);
  if (!ws.ok()) {
    if (error) *error = ws.ToString();
    return false;
  }

  auto& cur = impl_->current_record;
  if (batch.num_rows() > 0) {
    for (size_t i = 0; i < impl_->stat_columns.size(); ++i) {
      const auto& rs = impl_->stat_columns[i];
      auto [lo, hi] = BatchColumnBounds(*rb->column(rs.arrow_index), rs.type,
                                        rs.sorted);
      auto& acc = impl_->current_bounds[i];
      if (!acc) {
        acc = std::make_pair(std::move(lo), std::move(hi));
      } else {
        if (LiteralLess(lo, acc->first)) acc->first = std::move(lo);
        if (LiteralLess(acc->second, hi)) acc->second = std::move(hi);
      }
    }
  }
  cur.rows += batch.num_rows();
  return true;
}

bool BucketParquetWriter::CutRowGroup(int64_t* flushed_bytes,
                                      std::string* error) {
  if (impl_->closed) {
    if (error) *error = "CutRowGroup after Close";
    return false;
  }
  return impl_->CutRowGroup(flushed_bytes, error);
}

bool BucketParquetWriter::RollFile(std::string* error,
                                   int64_t* closed_file_bytes) {
  if (impl_->closed) {
    if (error) *error = "RollFile after Close";
    return false;
  }
  const bool had_file = impl_->writer != nullptr;
  if (!impl_->CloseCurrent(error)) return false;
  if (closed_file_bytes && had_file && !impl_->done.empty()) {
    *closed_file_bytes = impl_->done.back().bytes;
  }
  return true;
}

bool BucketParquetWriter::Close(std::vector<WrittenFile>* out,
                                std::string* error) {
  if (impl_->closed) {
    if (error) *error = "double Close";
    return false;
  }
  if (!impl_->CloseCurrent(error)) return false;
  impl_->closed = true;
  if (out) *out = std::move(impl_->done);
  return true;
}

}  // namespace primeparts
