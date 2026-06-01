// primeparts-drop-bucket-cols — in-place rewriter that removes the
// physical p_bucket_version / p_bucket columns from every parquet file
// in primeparts.primes and primeparts.partitions.
//
// Why
// ===
// p_bucket_version and p_bucket are identity-partition source fields.
// Per the Iceberg spec, identity-partition column values live in the
// manifest's partition tuple and are synthesized by readers at scan
// time — having them physically in every parquet row is redundant.
// (The earlier writer wrote them anyway. writer.cc now skips identity-
// partition source fields when building the arrow schema; this binary
// rewrites the existing corpus to match.)
//
// What it does
// ============
// For each file under primeparts/{primes,partitions}/data/p_bucket_version=*/p_bucket=*/:
//   1. Open the source parquet (5 columns physical for primes, 7 for
//      partitions) and read all rows minus p_bucket_version, p_bucket.
//   2. Write to <orig>.tmp with the new physical schema (writer.cc
//      builds it via IcebergToArrowSchemaWithFieldIds + partition spec).
//   3. Atomic rename <orig>.tmp -> <orig>.
//   4. Stamp the new DataFile metadata into the rebuilt iceberg
//      snapshot at publish time.
//
// Resumability
// ============
// A file is treated as already-converted if its physical schema lacks
// p_bucket (and p_bucket_version). The check is metadata-only — no
// rows decoded — so restart is cheap.
//
// Why no boundaries or sort-merge
// ===============================
// Unlike the prime_rank backfill, every column written here is a
// straight passthrough from the source. There's no cross-file or
// cross-table state, so each file is independent. The worker pool is
// trivially parallel across (bucket, file_index) pairs.

#include "primeparts/source_scan.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/key_value_metadata.h>
#include <arrow/util/thread_pool.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <getopt.h>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "iceberg/arrow/arrow_io_util.h"
#include "iceberg/arrow/arrow_register.h"
#include "iceberg/avro/avro_register.h"
#include "iceberg/catalog.h"
#include "iceberg/catalog/memory/in_memory_catalog.h"
#include "iceberg/catalog/rest/catalog_properties.h"
#include "iceberg/catalog/rest/rest_catalog.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/parquet/parquet_register.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/update/fast_append.h"

namespace fs = std::filesystem;

using primeparts::BucketPartitionSpec;
using primeparts::IcebergToArrowSchemaWithFieldIds;
using primeparts::PartitionsSchema;
using primeparts::PrimesSchema;
using primeparts::SourceTableReader;

namespace {

// Larger batches reduce reader/writer dispatch overhead and let workers
// stay CPU-bound longer between disk syncs. With 12 buckets on a 12-core
// box, oversubscribed Arrow threads added context-switch churn rather
// than parallelism; matching pool size to cores fixes that.
constexpr int64_t kReaderBatchSize = 2LL << 20;  // 2M rows per batch out of readers
constexpr int kArrowThreadPoolSize = 12;

// ============================================================================
// Options
// ============================================================================

struct Options {
  fs::path staging;
  int32_t p_bucket_version = 1;
  int32_t buckets_limit = -1;  // -1 = all
  bool primes_only = false;
  bool partitions_only = false;
  bool skip_publish = false;
  int threads = 0;             // 0 = bucket count
  std::string rest_uri;
  std::string rest_name = "primeparts";
  std::string rest_warehouse;
  std::string rest_prefix;
};

void usage(FILE* s) {
  std::fprintf(
      s,
      "usage: primeparts-drop-bucket-cols --staging <warehouse> [opts]\n"
      "\n"
      "  --staging PATH         staging warehouse root\n"
      "  --p-bucket-version V   default: 1\n"
      "  --buckets N            limit to first N buckets (smoke test)\n"
      "  --primes-only          rewrite primes only, skip publish\n"
      "  --partitions-only      rewrite partitions only, skip publish\n"
      "  --skip-publish         rewrite files but leave iceberg metadata alone\n"
      "  --threads N            parallel workers (default = bucket count)\n"
      "  --rest-uri URI         optional REST catalog to register\n"
      "  --rest-name NAME       REST catalog name, default: primeparts\n"
      "  --rest-warehouse PATH  REST warehouse, default: --staging\n"
      "  --rest-prefix PREFIX   optional REST catalog path prefix\n"
      "  --help\n");
}

bool parse_i64(const char* s, int64_t* out) {
  char* end = nullptr;
  errno = 0;
  long long v = std::strtoll(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool parse_args(int argc, char** argv, Options* o) {
  static const option longs[] = {
      {"staging", required_argument, nullptr, 's'},
      {"p-bucket-version", required_argument, nullptr, 'v'},
      {"buckets", required_argument, nullptr, 'b'},
      {"primes-only", no_argument, nullptr, 1000},
      {"partitions-only", no_argument, nullptr, 1001},
      {"skip-publish", no_argument, nullptr, 1002},
      {"threads", required_argument, nullptr, 't'},
      {"rest-uri", required_argument, nullptr, 1003},
      {"rest-name", required_argument, nullptr, 1004},
      {"rest-warehouse", required_argument, nullptr, 1005},
      {"rest-prefix", required_argument, nullptr, 1006},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  int opt;
  while ((opt = getopt_long(argc, argv, "s:v:b:t:h", longs, nullptr)) != -1) {
    switch (opt) {
      case 's': o->staging = optarg; break;
      case 'v': {
        int64_t v;
        if (!parse_i64(optarg, &v) || v <= 0) {
          std::fprintf(stderr, "invalid --p-bucket-version\n");
          return false;
        }
        o->p_bucket_version = static_cast<int32_t>(v);
        break;
      }
      case 'b': {
        int64_t v;
        if (!parse_i64(optarg, &v) || v <= 0) {
          std::fprintf(stderr, "invalid --buckets\n");
          return false;
        }
        o->buckets_limit = static_cast<int32_t>(v);
        break;
      }
      case 't': {
        int64_t v;
        if (!parse_i64(optarg, &v) || v <= 0) {
          std::fprintf(stderr, "invalid --threads\n");
          return false;
        }
        o->threads = static_cast<int>(v);
        break;
      }
      case 1000: o->primes_only = true; break;
      case 1001: o->partitions_only = true; break;
      case 1002: o->skip_publish = true; break;
      case 1003: o->rest_uri = optarg; break;
      case 1004: o->rest_name = optarg; break;
      case 1005: o->rest_warehouse = optarg; break;
      case 1006: o->rest_prefix = optarg; break;
      case 'h': usage(stdout); std::exit(0);
      default: return false;
    }
  }
  if (o->staging.empty()) {
    usage(stderr);
    return false;
  }
  return true;
}

// ============================================================================
// File listing per bucket — mirrors backfill_prime_rank_main.cc.
// ============================================================================

fs::path LatestMetadataJson(const fs::path& metadata_dir, std::string* error) {
  if (!fs::exists(metadata_dir)) {
    if (error) *error = "metadata dir not found: " + metadata_dir.string();
    return {};
  }
  fs::path best;
  std::string best_name;
  for (auto& entry : fs::directory_iterator(metadata_dir)) {
    auto name = entry.path().filename().string();
    if (name.size() < 6 || name.find(".metadata.json") == std::string::npos) {
      continue;
    }
    if (name > best_name) {
      best_name = name;
      best = entry.path();
    }
  }
  if (best.empty()) {
    if (error) *error = "no *.metadata.json under " + metadata_dir.string();
  }
  return best;
}

int BucketIdFromPath(const std::string& path) {
  const std::string key = "p_bucket=";
  auto pos = path.find(key);
  if (pos == std::string::npos) return -1;
  pos += key.size();
  int b = 0;
  bool any = false;
  while (pos < path.size() &&
         std::isdigit(static_cast<unsigned char>(path[pos]))) {
    b = b * 10 + (path[pos++] - '0');
    any = true;
  }
  return any ? b : -1;
}

struct TableFiles {
  std::vector<std::vector<std::string>> per_bucket;
};

bool DiscoverTableFiles(const fs::path& staging, const std::string& table,
                        int32_t n_buckets, TableFiles* out,
                        std::string* error) {
  auto md = LatestMetadataJson(
      staging / "primeparts" / table / "metadata", error);
  if (md.empty()) return false;
  auto reader = SourceTableReader::OpenMetadata(md, {"p"}, nullptr, error);
  if (!reader) return false;
  auto files = reader->source_files();
  reader.reset();

  out->per_bucket.assign(n_buckets, {});
  for (const auto& f : files) {
    int b = BucketIdFromPath(f.path);
    if (b < 0) {
      if (error) *error = "file " + f.path + " has no p_bucket=N";
      return false;
    }
    if (b >= n_buckets) continue;
    out->per_bucket[b].push_back(f.path);
  }
  for (int b = 0; b < n_buckets; ++b) {
    std::sort(out->per_bucket[b].begin(), out->per_bucket[b].end());
  }
  return true;
}

// ============================================================================
// Per-file rewrite
// ============================================================================

std::unique_ptr<parquet::arrow::FileReader> OpenSourceFile(
    const std::string& path, std::string* error) {
  auto file_r = arrow::io::ReadableFile::Open(path);
  if (!file_r.ok()) {
    if (error) *error = "open " + path + ": " + file_r.status().ToString();
    return nullptr;
  }
  parquet::ArrowReaderProperties arrow_props;
  arrow_props.set_use_threads(true);
  arrow_props.set_pre_buffer(true);
  arrow_props.set_batch_size(kReaderBatchSize);
  parquet::ReaderProperties reader_props;
  parquet::arrow::FileReaderBuilder builder;
  auto bs = builder.Open(file_r.ValueOrDie(), reader_props);
  if (!bs.ok()) {
    if (error) *error = "FileReaderBuilder::Open: " + bs.ToString();
    return nullptr;
  }
  builder.properties(arrow_props);
  std::unique_ptr<parquet::arrow::FileReader> reader;
  auto bs2 = builder.Build(&reader);
  if (!bs2.ok()) {
    if (error) *error = "FileReaderBuilder::Build: " + bs2.ToString();
    return nullptr;
  }
  return reader;
}

// True if the physical parquet schema does NOT include p_bucket (used as
// the resumability sentinel — if absent we treat the file as already
// converted; the partition columns are paired in the writer so checking
// one is enough).
bool AlreadyConverted(const std::string& path, bool* out_converted,
                      std::string* error) {
  auto reader = OpenSourceFile(path, error);
  if (!reader) return false;
  auto md = reader->parquet_reader()->metadata();
  *out_converted = md->schema()->ColumnIndex("p_bucket") < 0;
  return true;
}

std::shared_ptr<parquet::WriterProperties> WriterProps(
    const arrow::Schema& schema,
    const std::vector<std::string>& delta_cols) {
  parquet::WriterProperties::Builder builder;
  builder.compression(parquet::Compression::ZSTD);
  builder.compression_level(3);
  builder.data_pagesize(1 << 20);
  builder.max_row_group_length(240'000'000);
  for (const auto& col : delta_cols) {
    if (schema.GetFieldByName(col)) {
      builder.disable_dictionary(col);
      builder.encoding(col, parquet::Encoding::DELTA_BINARY_PACKED);
    }
  }
  return builder.build();
}

// Project the source file into target_schema by reading exactly the
// columns that remain in target_schema (in target order). Returns row
// count and p min/max from the read.
bool RewriteFile(const std::string& src_path,
                 const std::shared_ptr<arrow::Schema>& target_schema,
                 const std::vector<std::string>& delta_cols,
                 int64_t* out_rows, int64_t* out_p_min, int64_t* out_p_max,
                 std::string* error) {
  auto reader = OpenSourceFile(src_path, error);
  if (!reader) return false;
  auto md = reader->parquet_reader()->metadata();

  std::vector<int> col_indices;
  col_indices.reserve(target_schema->num_fields());
  for (const auto& f : target_schema->fields()) {
    int idx = md->schema()->ColumnIndex(f->name());
    if (idx < 0) {
      *error = "target field absent in source: " + f->name() + " (" +
               src_path + ")";
      return false;
    }
    col_indices.push_back(idx);
  }
  int target_p_idx = -1;
  for (int i = 0; i < target_schema->num_fields(); ++i) {
    if (target_schema->field(i)->name() == "p") {
      target_p_idx = i;
      break;
    }
  }

  std::vector<int> row_groups(md->num_row_groups());
  for (int i = 0; i < md->num_row_groups(); ++i) row_groups[i] = i;
  auto rbr_r = reader->GetRecordBatchReader(row_groups, col_indices);
  if (!rbr_r.ok()) {
    *error = "GetRecordBatchReader: " + rbr_r.status().ToString();
    return false;
  }
  auto rbr = std::move(rbr_r).ValueOrDie();

  fs::path tmp = fs::path(src_path).parent_path() /
                 ("." + fs::path(src_path).filename().string() + ".tmp");
  std::error_code ec;
  fs::remove(tmp, ec);
  auto sink_r = arrow::io::FileOutputStream::Open(tmp.string());
  if (!sink_r.ok()) {
    *error = "open tmp: " + sink_r.status().ToString();
    return false;
  }
  auto sink = sink_r.ValueOrDie();
  auto props = WriterProps(*target_schema, delta_cols);
  auto fw_r = parquet::arrow::FileWriter::Open(
      *target_schema, arrow::default_memory_pool(), sink, props,
      parquet::default_arrow_writer_properties());
  if (!fw_r.ok()) {
    *error = "FileWriter::Open: " + fw_r.status().ToString();
    return false;
  }
  auto writer = std::move(fw_r).ValueOrDie();

  int64_t rows = 0;
  int64_t p_min = std::numeric_limits<int64_t>::max();
  int64_t p_max = std::numeric_limits<int64_t>::min();
  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    auto rs = rbr->ReadNext(&batch);
    if (!rs.ok()) { *error = "ReadNext: " + rs.ToString(); return false; }
    if (!batch) break;
    int64_t n = batch->num_rows();
    if (n == 0) continue;
    // The reader's batch schema is derived from the source's metadata;
    // re-wrap with target_schema so PARQUET:field_id from the writer's
    // schema is what gets stamped on output.
    auto out_batch = arrow::RecordBatch::Make(
        target_schema, n,
        std::vector<std::shared_ptr<arrow::Array>>(batch->columns().begin(),
                                                    batch->columns().end()));
    auto ws = writer->WriteRecordBatch(*out_batch);
    if (!ws.ok()) { *error = "WriteRecordBatch: " + ws.ToString(); return false; }

    if (target_p_idx >= 0) {
      const int64_t* p_data =
          static_cast<const arrow::Int64Array*>(batch->column(target_p_idx).get())
              ->raw_values();
      if (p_data[0] < p_min) p_min = p_data[0];
      if (p_data[n - 1] > p_max) p_max = p_data[n - 1];
    }
    rows += n;
  }
  auto cs = writer->Close();
  if (!cs.ok()) { *error = "writer close: " + cs.ToString(); return false; }
  auto ss = sink->Close();
  if (!ss.ok()) { *error = "sink close: " + ss.ToString(); return false; }
  fs::rename(tmp, src_path, ec);
  if (ec) {
    *error = "rename " + tmp.string() + " -> " + src_path + ": " + ec.message();
    return false;
  }
  *out_rows = rows;
  *out_p_min = p_min;
  *out_p_max = p_max;
  return true;
}

// ============================================================================
// Per-bucket result + iceberg publish helpers
// ============================================================================

struct RewrittenFile {
  std::string path;
  int32_t p_bucket = 0;
  int64_t rows = 0;
  int64_t p_min = 0;
  int64_t p_max = 0;
  int64_t bytes = 0;
};

struct BucketResult {
  std::vector<RewrittenFile> files;
  std::string error;
};

std::shared_ptr<iceberg::FileIO> LocalIO() {
  return std::shared_ptr<iceberg::FileIO>(iceberg::arrow::MakeLocalFileIO());
}

bool PutBound(std::map<int32_t, std::vector<uint8_t>>* m, int32_t fid,
              const iceberg::Literal& v, std::string* error) {
  auto s = v.Serialize();
  if (!s.has_value()) { *error = s.error().message; return false; }
  (*m)[fid] = std::move(s.value());
  return true;
}

int32_t FieldIdByName(const iceberg::Schema& schema, std::string_view name) {
  for (const auto& f : schema.fields()) {
    if (f.name() == name) return f.field_id();
  }
  return -1;
}

bool MakeDataFile(const std::shared_ptr<iceberg::Schema>& schema,
                  const std::shared_ptr<iceberg::PartitionSpec>& spec,
                  const RewrittenFile& bf, int32_t p_bucket_version,
                  std::shared_ptr<iceberg::DataFile>* out,
                  std::string* error) {
  auto df = std::make_shared<iceberg::DataFile>();
  df->content = iceberg::DataFile::Content::kData;
  df->file_path = bf.path;
  df->file_format = iceberg::FileFormatType::kParquet;
  df->partition = iceberg::PartitionValues({
      iceberg::Literal::Int(p_bucket_version),
      iceberg::Literal::Int(bf.p_bucket),
  });
  df->record_count = bf.rows;
  df->file_size_in_bytes = bf.bytes;
  df->partition_spec_id = spec->spec_id();
  // value_counts / null_value_counts only cover fields physically in the
  // file. Identity-partition source fields are absent on disk and live
  // in df->partition, so iceberg readers synthesize them from there.
  for (const auto& field : schema->fields()) {
    if (field.name() == "p_bucket_version" || field.name() == "p_bucket") {
      continue;
    }
    df->value_counts[field.field_id()] = bf.rows;
    df->null_value_counts[field.field_id()] = 0;
  }
  const int32_t p_id = FieldIdByName(*schema, "p");
  if (p_id >= 0 && bf.rows > 0) {
    if (!PutBound(&df->lower_bounds, p_id,
                  iceberg::Literal::Long(bf.p_min), error) ||
        !PutBound(&df->upper_bounds, p_id,
                  iceberg::Literal::Long(bf.p_max), error)) {
      return false;
    }
  }
  *out = std::move(df);
  return true;
}

std::shared_ptr<iceberg::Catalog> MakeCatalog(const Options& opts,
                                              std::string* mode,
                                              std::string* error) {
  if (!opts.rest_uri.empty()) {
    iceberg::arrow::RegisterAll();
    auto config = iceberg::rest::RestCatalogProperties::default_properties();
    config.Set(iceberg::rest::RestCatalogProperties::kUri, opts.rest_uri)
        .Set(iceberg::rest::RestCatalogProperties::kName, opts.rest_name)
        .Set(iceberg::rest::RestCatalogProperties::kWarehouse,
             opts.rest_warehouse.empty() ? opts.staging.string()
                                         : opts.rest_warehouse);
    if (!opts.rest_prefix.empty()) {
      config.Set(iceberg::rest::RestCatalogProperties::kPrefix,
                 opts.rest_prefix);
    }
    auto r = iceberg::rest::RestCatalog::Make(config);
    if (!r.has_value()) { *error = r.error().message; return nullptr; }
    *mode = "rest";
    return std::move(r.value());
  }
  *mode = "in-memory";
  return std::make_shared<iceberg::InMemoryCatalog>(
      "primeparts-staging", LocalIO(), opts.staging.string(),
      std::unordered_map<std::string, std::string>{});
}

bool EnsureNamespace(const std::shared_ptr<iceberg::Catalog>& catalog,
                     const iceberg::Namespace& ns, std::string* error) {
  auto exists = catalog->NamespaceExists(ns);
  if (!exists.has_value()) { *error = exists.error().message; return false; }
  if (exists.value()) return true;
  auto status = catalog->CreateNamespace(ns, {});
  if (!status.has_value()) { *error = status.error().message; return false; }
  return true;
}

// Publish the table to the catalog.
//
// Two paths:
// 1. An on-disk metadata.json already exists (e.g. from a previous in-memory
//    publish, or a re-run): register that metadata location via RegisterTable.
//    Preserves snapshot history and the existing metadata file; IRC writes the
//    HMS row that exposes the table to Hive readers.
// 2. No metadata.json: fall back to CreateTable + FastAppend. The catalog
//    writes a fresh metadata.json referencing every DataFile we built. Used by
//    the in-memory bootstrap path (no REST catalog wired).
bool PublishTable(const std::shared_ptr<iceberg::Catalog>& catalog,
                  const fs::path& staging, const std::string& table_name,
                  const std::shared_ptr<iceberg::Schema>& schema,
                  const std::shared_ptr<iceberg::PartitionSpec>& spec,
                  const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
                  std::string* metadata_location, std::string* error) {
  fs::path md_dir = staging / "primeparts" / table_name / "metadata";
  iceberg::TableIdentifier ident{
      .ns = iceberg::Namespace{{"primeparts"}}, .name = table_name};

  std::string find_err;
  fs::path existing = LatestMetadataJson(md_dir, &find_err);
  if (!existing.empty()) {
    // If the catalog already knows this table at the same metadata location,
    // there's nothing to do.
    auto loaded = catalog->LoadTable(ident);
    if (loaded.has_value() &&
        std::string(loaded.value()->metadata_file_location()) == existing.string()) {
      *metadata_location = std::string(loaded.value()->metadata_file_location());
      return true;
    }
    // The catalog row points at a stale metadata location (or none): drop the
    // entry (purge=false leaves data files untouched) and re-register against
    // the on-disk metadata. RegisterTable preserves the existing metadata.json
    // and just stamps the location pointer into HMS.
    auto del = catalog->DropTable(ident, false);
    (void)del;  // NotFound is fine — fresh table path.
    auto reg = catalog->RegisterTable(ident, existing.string());
    if (!reg.has_value()) {
      *error = "RegisterTable " + table_name + ": " + reg.error().message;
      return false;
    }
    *metadata_location = std::string(reg.value()->metadata_file_location());
    return true;
  }

  // Fresh-publish fallback: no metadata.json on disk yet.
  std::error_code ec;
  fs::create_directories(md_dir, ec);
  auto created = catalog->CreateTable(
      ident, schema, spec, iceberg::SortOrder::Unsorted(),
      (staging / "primeparts" / table_name).string(),
      {{"write.parquet.compression-codec", "zstd"},
       {"write.parquet.compression-level", "3"}});
  if (!created.has_value()) {
    *error = "CreateTable " + table_name + ": " + created.error().message;
    return false;
  }
  auto table = std::move(created.value());
  if (!files.empty()) {
    auto app_r = table->NewFastAppend();
    if (!app_r.has_value()) {
      *error = "NewFastAppend: " + app_r.error().message;
      return false;
    }
    auto app = std::move(app_r.value());
    for (const auto& f : files) app->AppendFile(f);
    auto cs = app->Commit();
    if (!cs.has_value()) {
      *error = "Commit: " + cs.error().message;
      return false;
    }
    auto rs = table->Refresh();
    if (!rs.has_value()) {
      *error = "Refresh: " + rs.error().message;
      return false;
    }
  }
  *metadata_location = std::string(table->metadata_file_location());
  return true;
}

bool BucketCountFromBoundaries(const fs::path& staging, int32_t bucket_version,
                               int32_t* out_n, std::string* error) {
  auto md = LatestMetadataJson(
      staging / "primeparts" / "boundaries" / "metadata", error);
  if (md.empty()) return false;
  auto reader = SourceTableReader::OpenMetadata(
      md, {"p_bucket_version", "p_bucket"}, nullptr, error);
  if (!reader) return false;
  int32_t n = 0;
  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (!reader->Next(&batch, error)) return false;
    if (!batch) break;
    auto v = std::static_pointer_cast<arrow::Int32Array>(batch->column(0));
    auto b = std::static_pointer_cast<arrow::Int32Array>(batch->column(1));
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      if (v->Value(i) != bucket_version) continue;
      if (b->Value(i) + 1 > n) n = b->Value(i) + 1;
    }
  }
  *out_n = n;
  return n > 0;
}

bool RewriteTable(const Options& opts, const std::string& table,
                  const std::shared_ptr<iceberg::Schema>& schema,
                  const std::shared_ptr<iceberg::PartitionSpec>& spec,
                  const std::vector<std::string>& delta_cols,
                  int32_t n_buckets,
                  std::vector<BucketResult>* results,
                  std::string* error) {
  auto target_arrow = IcebergToArrowSchemaWithFieldIds(*schema, error,
                                                       spec.get());
  if (!target_arrow) {
    *error = "target arrow schema (" + table + "): " + *error;
    return false;
  }

  TableFiles files;
  if (!DiscoverTableFiles(opts.staging, table, n_buckets, &files, error)) {
    *error = "discover " + table + ": " + *error;
    return false;
  }

  int n_workers = opts.threads > 0 ? opts.threads : n_buckets;
  results->assign(n_buckets, {});

  auto t0 = std::chrono::steady_clock::now();
  std::fprintf(stdout, "\n== %s phase: %d bucket(s), %d worker(s) ==\n",
               table.c_str(), n_buckets, n_workers);
  std::fflush(stdout);

  std::atomic<int32_t> cursor{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < n_workers; ++t) {
    ts.emplace_back([&] {
      while (true) {
        int32_t b = cursor.fetch_add(1);
        if (b >= n_buckets) return;
        BucketResult& res = (*results)[b];
        const auto& paths = files.per_bucket[b];
        for (size_t i = 0; i < paths.size(); ++i) {
          const std::string& path = paths[i];
          bool already = false;
          std::string err;
          if (!AlreadyConverted(path, &already, &err)) {
            res.error = err;
            return;
          }
          RewrittenFile rf;
          rf.path = path;
          rf.p_bucket = b;
          if (already) {
            auto rd = OpenSourceFile(path, &err);
            if (!rd) { res.error = err; return; }
            auto md = rd->parquet_reader()->metadata();
            rf.rows = md->num_rows();
            int p_idx = md->schema()->ColumnIndex("p");
            int64_t pmin = std::numeric_limits<int64_t>::max();
            int64_t pmax = std::numeric_limits<int64_t>::min();
            for (int rg = 0; rg < md->num_row_groups(); ++rg) {
              auto col = md->RowGroup(rg)->ColumnChunk(p_idx);
              auto s = std::static_pointer_cast<parquet::Int64Statistics>(
                  col->statistics());
              pmin = std::min(pmin, s->min());
              pmax = std::max(pmax, s->max());
            }
            rf.p_min = pmin;
            rf.p_max = pmax;
          } else {
            int64_t rows = 0, pmin = 0, pmax = 0;
            if (!RewriteFile(path, target_arrow, delta_cols, &rows, &pmin,
                             &pmax, &err)) {
              res.error = err;
              return;
            }
            rf.rows = rows;
            rf.p_min = pmin;
            rf.p_max = pmax;
          }
          std::error_code ec;
          rf.bytes = static_cast<int64_t>(fs::file_size(path, ec));
          res.files.push_back(std::move(rf));
          std::fprintf(stdout,
                       "  %s bucket=%d file=%zu/%zu rows=%lld %s\n",
                       table.c_str(), b, i + 1, paths.size(),
                       static_cast<long long>(res.files.back().rows),
                       already ? "(skip)" : "(rewrite)");
          std::fflush(stdout);
        }
      }
    });
  }
  for (auto& t : ts) t.join();

  for (int b = 0; b < n_buckets; ++b) {
    if (!(*results)[b].error.empty()) {
      *error = table + " bucket " + std::to_string(b) + ": " +
               (*results)[b].error;
      return false;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  std::fprintf(stdout, "%s phase: done (%.1fs)\n", table.c_str(),
               std::chrono::duration<double>(t1 - t0).count());
  return true;
}

}  // namespace

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  Options opts;
  if (!parse_args(argc, argv, &opts)) return 2;

  auto set_s = arrow::SetCpuThreadPoolCapacity(kArrowThreadPoolSize);
  if (!set_s.ok()) {
    std::fprintf(stderr, "SetCpuThreadPoolCapacity: %s\n",
                 set_s.ToString().c_str());
    return 1;
  }

  iceberg::avro::RegisterAll();
  iceberg::parquet::RegisterAll();

  std::string error;
  int32_t n_buckets = 0;
  if (!BucketCountFromBoundaries(opts.staging, opts.p_bucket_version,
                                 &n_buckets, &error)) {
    std::fprintf(stderr, "bucket count: %s\n", error.c_str());
    return 1;
  }
  if (opts.buckets_limit > 0 && opts.buckets_limit < n_buckets) {
    n_buckets = opts.buckets_limit;
    std::fprintf(stdout, "limiting to first %d bucket(s)\n", n_buckets);
  }
  std::fprintf(stdout, "buckets: %d for p_bucket_version=%d\n", n_buckets,
               opts.p_bucket_version);

  auto primes_schema = PrimesSchema();
  auto primes_spec = BucketPartitionSpec(*primes_schema, &error);
  if (!primes_spec) {
    std::fprintf(stderr, "primes spec: %s\n", error.c_str());
    return 1;
  }
  auto parts_schema = PartitionsSchema();
  auto parts_spec = BucketPartitionSpec(*parts_schema, &error);
  if (!parts_spec) {
    std::fprintf(stderr, "partitions spec: %s\n", error.c_str());
    return 1;
  }

  std::vector<BucketResult> primes_results;
  std::vector<BucketResult> parts_results;

  if (!opts.partitions_only) {
    if (!RewriteTable(opts, "primes", primes_schema, primes_spec,
                      {"p", "prime_rank"}, n_buckets, &primes_results,
                      &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  }
  if (!opts.primes_only) {
    if (!RewriteTable(opts, "partitions", parts_schema, parts_spec,
                      {"p", "prime_rank", "q_k"}, n_buckets, &parts_results,
                      &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  }

  if (opts.skip_publish) {
    std::fprintf(stdout, "\n--skip-publish set; leaving iceberg metadata.\n");
    return 0;
  }

  std::vector<std::shared_ptr<iceberg::DataFile>> primes_dfs;
  for (int b = 0; b < static_cast<int>(primes_results.size()); ++b) {
    for (const auto& rf : primes_results[b].files) {
      std::shared_ptr<iceberg::DataFile> df;
      if (!MakeDataFile(primes_schema, primes_spec, rf,
                        opts.p_bucket_version, &df, &error)) {
        std::fprintf(stderr, "primes MakeDataFile: %s\n", error.c_str());
        return 1;
      }
      primes_dfs.push_back(std::move(df));
    }
  }
  std::vector<std::shared_ptr<iceberg::DataFile>> parts_dfs;
  for (int b = 0; b < static_cast<int>(parts_results.size()); ++b) {
    for (const auto& rf : parts_results[b].files) {
      std::shared_ptr<iceberg::DataFile> df;
      if (!MakeDataFile(parts_schema, parts_spec, rf,
                        opts.p_bucket_version, &df, &error)) {
        std::fprintf(stderr, "partitions MakeDataFile: %s\n", error.c_str());
        return 1;
      }
      parts_dfs.push_back(std::move(df));
    }
  }

  std::string catalog_mode;
  auto catalog = MakeCatalog(opts, &catalog_mode, &error);
  if (!catalog) {
    std::fprintf(stderr, "open catalog: %s\n", error.c_str());
    return 1;
  }
  std::fprintf(stdout, "\ncatalog: %s\n", catalog_mode.c_str());
  if (!EnsureNamespace(catalog, iceberg::Namespace{{"primeparts"}}, &error)) {
    std::fprintf(stderr, "ensure namespace: %s\n", error.c_str());
    return 1;
  }

  if (!opts.partitions_only) {
    std::string md;
    if (!PublishTable(catalog, opts.staging, "primes", primes_schema,
                      primes_spec, primes_dfs, &md, &error)) {
      std::fprintf(stderr, "publish primes: %s\n", error.c_str());
      return 1;
    }
    std::fprintf(stdout, "primeparts.primes published: %s\n", md.c_str());
  }
  if (!opts.primes_only) {
    std::string md;
    if (!PublishTable(catalog, opts.staging, "partitions", parts_schema,
                      parts_spec, parts_dfs, &md, &error)) {
      std::fprintf(stderr, "publish partitions: %s\n", error.c_str());
      return 1;
    }
    std::fprintf(stdout, "primeparts.partitions published: %s\n", md.c_str());
  }

  std::fprintf(stdout, "\ndrop-bucket-cols complete.\n");
  return 0;
}
