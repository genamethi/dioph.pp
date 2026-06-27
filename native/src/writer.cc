#include "primeparts/writer.h"
#include "primeparts/schemas.h"

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
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>

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

constexpr std::string_view kTmpDotPrefix = ".";

// A per-file random token. Iceberg writers give every data file a unique name
// so a fresh CreateTable+append after a DropTable never collides with an
// orphaned file (and parallel writers never race on a path). Placed BEFORE the
// trailing seq field so NextFileSeq, which reads the last '_'-delimited number,
// still recovers the sequence for resume.
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

int32_t FieldIdByName(const iceberg::Schema& schema, std::string_view name) {
  for (const auto& field : schema.fields()) {
    if (field.name() == name) return field.field_id();
  }
  return -1;
}

std::shared_ptr<parquet::WriterProperties> ParquetWriterProperties(
    const WriterConfig& config, const arrow::Schema& arrow_schema) {
  parquet::WriterProperties::Builder builder;
  builder.compression(parquet::Compression::ZSTD);
  builder.compression_level(config.compression_level);
  builder.data_pagesize(config.data_pagesize);
  // ~256 MiB target row group => 4 row groups per ~1 GiB file. Both
  // output schemas land at ~1.1 B/row with DELTA+zstd on the monotone
  // columns (p, prime_rank, q_k), so 240M rows ~= 264 MiB compressed.
  builder.max_row_group_length(240'000'000);
  for (const auto& col : config.delta_columns) {
    if (arrow_schema.GetFieldByName(col)) {
      builder.disable_dictionary(col);
      builder.encoding(col, parquet::Encoding::DELTA_BINARY_PACKED);
    }
  }
  return builder.build();
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
  if (!config.schema) {
    if (error) *error = "WriterConfig.schema is null";
    return false;
  }
  auto data_file = std::make_shared<iceberg::DataFile>();
  data_file->content = iceberg::DataFile::Content::kData;
  data_file->file_path = file.path.string();
  data_file->file_format = iceberg::FileFormatType::kParquet;
  if (config.partition_values) {
    data_file->partition = *config.partition_values;
  } else {
    data_file->partition = iceberg::PartitionValues({
        iceberg::Literal::Int(config.bucket_version),
        iceberg::Literal::Int(config.bucket),
    });
  }
  data_file->record_count = file.rows;
  data_file->file_size_in_bytes = file.bytes;
  if (spec) data_file->partition_spec_id = spec->spec_id();

  for (const auto& field : config.schema->fields()) {
    data_file->value_counts[field.field_id()] = file.rows;
    if (!field.optional()) {
      data_file->null_value_counts[field.field_id()] = 0;
    }
  }

  const int32_t p_id = FieldIdByName(*config.schema, "p");
  if (p_id >= 0 && file.rows > 0) {
    if (!PutBound(&data_file->lower_bounds, p_id,
                  iceberg::Literal::Long(file.p_min), error) ||
        !PutBound(&data_file->upper_bounds, p_id,
                  iceberg::Literal::Long(file.p_max), error)) {
      return false;
    }
  }

  *out = std::move(data_file);
  return true;
}

}  // namespace

std::shared_ptr<arrow::Schema> IcebergToArrowSchemaWithFieldIds(
    const iceberg::Schema& schema, std::string* error,
    const iceberg::PartitionSpec* partition_spec) {
  std::unordered_set<int32_t> skip_source_ids;
  if (partition_spec) {
    for (const auto& pf : partition_spec->fields()) {
      if (pf.transform() &&
          pf.transform()->transform_type() == iceberg::TransformType::kIdentity) {
        skip_source_ids.insert(pf.source_id());
      }
    }
  }
  arrow::FieldVector fields;
  fields.reserve(schema.fields().size());
  for (const auto& f : schema.fields()) {
    if (skip_source_ids.count(f.field_id())) continue;
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
    // Iceberg's required flag is the truth; mirror it on arrow side so
    // parquet stops emitting null bitmaps for required columns.
    const bool nullable = f.optional();
    fields.push_back(
        arrow::field(std::string(f.name()), at, nullable, kv));
  }
  return arrow::schema(fields);
}

fs::path BucketDataDir(const fs::path& warehouse, std::string_view table,
                       int32_t bucket_version, int32_t bucket) {
  return warehouse / "primeparts" / std::string(table) / "data" /
         ("p_bucket_version=" + std::to_string(bucket_version)) /
         ("p_bucket=" + std::to_string(bucket));
}

int32_t NextFileSeq(const fs::path& output_dir, std::string_view prefix) {
  std::error_code ec;
  if (!fs::exists(output_dir, ec)) return 0;
  int32_t max_seq = -1;
  for (auto& entry : fs::directory_iterator(output_dir, ec)) {
    if (ec || !entry.is_regular_file()) continue;
    auto name = entry.path().filename().string();
    if (name.size() <= prefix.size() ||
        name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    auto pos = name.rfind('_');
    auto dot = name.find('.', pos == std::string::npos ? 0 : pos);
    if (pos == std::string::npos || dot == std::string::npos) continue;
    try {
      int32_t seq = static_cast<int32_t>(
          std::stoi(name.substr(pos + 1, dot - pos - 1)));
      if (seq > max_seq) max_seq = seq;
    } catch (...) {}
  }
  return max_seq + 1;
}

struct BucketParquetWriter::Impl {
  WriterConfig config;
  std::shared_ptr<arrow::Schema> arrow_schema;
  std::shared_ptr<iceberg::PartitionSpec> partition_spec;
  std::shared_ptr<parquet::WriterProperties> props;
  int32_t next_seq = 0;
  bool closed = false;

  // Current open file (none when between rolls).
  std::shared_ptr<arrow::io::FileOutputStream> sink;
  std::unique_ptr<parquet::arrow::FileWriter> writer;
  fs::path tmp_path;
  fs::path final_path;
  WrittenFile current_record;

  // Lifetime accumulator.
  std::vector<WrittenFile> done;

  bool OpenIfNeeded(std::string* error);
  bool CloseCurrent(std::string* error);
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
  return true;
}

bool BucketParquetWriter::Impl::CloseCurrent(std::string* error) {
  if (!writer) return true;
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
  if (config.partition_spec) {
      impl->partition_spec = config.partition_spec;
  } else {
      impl->partition_spec = BucketPartitionSpec(*config.schema, error);
      if (!impl->partition_spec) return nullptr;
  }
  // Identity-partition source fields live in the manifest's partition tuple
  // and are synthesized by readers at scan time. Pass the spec so the helper
  // omits them from the physical arrow/parquet schema.
  impl->arrow_schema = IcebergToArrowSchemaWithFieldIds(
      *config.schema, error, impl->partition_spec.get());
  if (!impl->arrow_schema) return nullptr;

  impl->props = ParquetWriterProperties(config, *impl->arrow_schema);
  impl->next_seq = config.starting_file_seq;
  impl->config = std::move(config);

  return std::unique_ptr<BucketParquetWriter>(
      new BucketParquetWriter(std::move(impl)));
}

BucketParquetWriter::BucketParquetWriter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

BucketParquetWriter::~BucketParquetWriter() {
  // Best-effort cleanup if user forgot to call Close(). We can't return
  // the WrittenFile records here, but we can at least flush bytes.
  if (impl_ && !impl_->closed && impl_->writer) {
    std::string ignored;
    impl_->CloseCurrent(&ignored);
  }
}

bool BucketParquetWriter::Write(const arrow::RecordBatch& batch,
                                BatchStats stats, std::string* error) {
  if (impl_->closed) {
    if (error) *error = "Write after Close";
    return false;
  }
  // Roll-before-write: if the current file is full, close it first so
  // this batch lands at the head of the next file.
  if (impl_->writer && impl_->config.target_rows_per_file > 0 &&
      impl_->current_record.rows >= impl_->config.target_rows_per_file) {
    if (!impl_->CloseCurrent(error)) return false;
  }
  if (!impl_->OpenIfNeeded(error)) return false;

  // Project the input batch onto the writer's target schema by column
  // name. Callers can pass the full iceberg-shaped batch (e.g. with
  // p_bucket_version / p_bucket columns); identity-partition source
  // columns get dropped here since they live in the manifest's
  // partition tuple, not on disk.
  std::vector<std::shared_ptr<arrow::Array>> projected;
  projected.reserve(impl_->arrow_schema->num_fields());
  for (const auto& f : impl_->arrow_schema->fields()) {
    auto col = batch.GetColumnByName(f->name());
    if (!col) {
      if (error) {
        *error = "input batch missing column: " + f->name();
      }
      return false;
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

  // Accumulate into the current file's record.
  auto& cur = impl_->current_record;
  if (cur.rows == 0) {
    cur.p_min = stats.p_min;
    cur.p_max = stats.p_max;
    cur.rank_min = stats.rank_min;
    cur.rank_max = stats.rank_max;
  } else {
    cur.p_min = std::min(cur.p_min, stats.p_min);
    cur.p_max = std::max(cur.p_max, stats.p_max);
    cur.rank_min = std::min(cur.rank_min, stats.rank_min);
    cur.rank_max = std::max(cur.rank_max, stats.rank_max);
  }
  cur.rows += batch.num_rows();
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
