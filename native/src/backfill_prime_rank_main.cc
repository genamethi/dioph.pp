// primeparts-backfill-rank — populate the all-null `prime_rank` column
// on the existing bucket-partitioned staging warehouse, in place, and
// rebuild the iceberg metadata against the all-required schema.
//
// Input state (what primeparts-rewrite left on disk)
// ==================================================
//   <staging>/primeparts/primes/data/p_bucket_version=1/p_bucket=B/*.parquet
//   <staging>/primeparts/partitions/data/p_bucket_version=1/p_bucket=B/*.parquet
//   <staging>/primeparts/boundaries/data/boundaries_0000.parquet
//   <staging>/primeparts/{primes,partitions,boundaries}/metadata/*
//
// Every primes/partitions parquet has `prime_rank` present but all-null;
// the iceberg schema currently marks it optional with the wrong field
// ids. boundaries.rank_min[B] is the row-count offset (not π) of the
// first row in bucket B.
//
// What this binary does
// =====================
// 1. Read funbuns.boundaries via SourceTableReader::OpenMetadata. Gather
//    rank_min per bucket (sorted ascending).
// 2. For each table (primes, partitions), open its current metadata
//    via SourceTableReader::OpenMetadata. The reader's source_files()
//    gives us (path, p_min, p_max, record_count); group by `p_bucket=`
//    from the path and sort within bucket by filename so writes land in
//    the same order on output.
// 3. Primes phase — parallel across buckets. Each bucket's worker
//    iterates its files in sequence; per file:
//      a. Compute start_prime_rank = rank_min[B] + 2 + cum_rows_seen_so_far.
//         (The `+2` is because π(2)=1, and p=2 is intentionally absent
//         from primeparts.primes — the first present row, p=3, has
//         prime_rank=2.)
//      b. Read the source file into batches, replace the prime_rank
//         column with an arange slice, write to <orig>.tmp under the
//         all-required schema, atomically rename to <orig>.
//      c. Increment cum_rows_seen_so_far by file row count.
// 4. Partitions phase — parallel across buckets. For each bucket, run
//    a streaming sort-merge between (a) primes batches projected to
//    {p, prime_rank, k} (now backfilled) and (b) the bucket's existing
//    partitions parquet files. For each partitions input file F, open
//    <F>.tmp for write, stream F in batches, and for each batch consume
//    sum(k)==batch_size rows worth of primes-side state to fill
//    prime_rank by repetition (std::fill_n per prime). Close and
//    atomically rename.
// 5. After both phases complete: wipe <staging>/primeparts/{primes,
//    partitions}/metadata/ and re-publish via InMemoryCatalog
//    CreateTable + FastAppend. Optional --rest-uri to also register
//    with an Iceberg REST catalog.
//
// Resumability
// ============
// Each file rewrite is atomic (.tmp + rename). On restart the binary
// skips any parquet whose `prime_rank` column already has null_count=0
// in every row group — those are already backfilled. Row counts come
// from parquet metadata and don't change across rewrites, so
// start_prime_rank is deterministic even on partial-state restart.

#include "primeparts/calibration.h"
#include "primeparts/source_scan.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/key_value_metadata.h>
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
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arrow/util/thread_pool.h>

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

using primeparts::BucketDataDir;
using primeparts::BucketParquetWriter;
using primeparts::BucketPartitionSpec;
using primeparts::IcebergToArrowSchemaWithFieldIds;
using primeparts::NextFileSeq;
using primeparts::PartitionsSchema;
using primeparts::PrimesSchema;
using primeparts::SourceFileInfo;
using primeparts::SourceTableReader;
using primeparts::WriterConfig;
using primeparts::WrittenFile;

namespace {

constexpr int64_t kReaderBatchSize = 1 << 20;  // 1M rows per batch out of readers
constexpr int kArrowThreadPoolSize = 24;

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
  int threads = 0;             // 0 = N_BUCKETS
  std::string rest_uri;
  std::string rest_name = "primeparts";
  std::string rest_warehouse;
  std::string rest_prefix;
};

void usage(FILE* s) {
  std::fprintf(
      s,
      "usage: primeparts-backfill-rank --staging <warehouse> [opts]\n"
      "\n"
      "  --staging PATH         staging warehouse root (contains primeparts/{primes,partitions,boundaries})\n"
      "  --p-bucket-version V   default: 1\n"
      "  --buckets N            limit to first N buckets (smoke test)\n"
      "  --primes-only          run primes phase, skip partitions + publish\n"
      "  --partitions-only      run partitions phase, skip primes + publish\n"
      "  --skip-publish         skip iceberg metadata rebuild at the end\n"
      "  --threads N            parallel workers (default = bucket count)\n"
      "  --rest-uri URI         optional REST catalog to register\n"
      "  --rest-name NAME       REST catalog name, default: primeparts\n"
      "  --rest-warehouse PATH  REST warehouse config, default: --staging\n"
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
// Boundaries (rank_min per bucket)
// ============================================================================

struct BoundaryRow {
  int32_t p_bucket;
  int64_t p_min;
  int64_t rank_min;
};

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

bool LoadBoundaries(const fs::path& staging, int32_t bucket_version,
                    std::vector<BoundaryRow>* out, std::string* error) {
  auto md = LatestMetadataJson(
      staging / "primeparts" / "boundaries" / "metadata", error);
  if (md.empty()) return false;
  auto reader = SourceTableReader::OpenMetadata(
      md, {"p_bucket_version", "p_bucket", "p_min", "rank_min"}, nullptr, error);
  if (!reader) return false;
  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (!reader->Next(&batch, error)) return false;
    if (!batch) break;
    auto v  = std::static_pointer_cast<arrow::Int32Array>(batch->column(0));
    auto b  = std::static_pointer_cast<arrow::Int32Array>(batch->column(1));
    auto pm = std::static_pointer_cast<arrow::Int64Array>(batch->column(2));
    auto rm = std::static_pointer_cast<arrow::Int64Array>(batch->column(3));
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      if (v->Value(i) != bucket_version) continue;
      out->push_back(BoundaryRow{b->Value(i), pm->Value(i), rm->Value(i)});
    }
  }
  std::sort(out->begin(), out->end(),
            [](const BoundaryRow& a, const BoundaryRow& b) {
              return a.p_bucket < b.p_bucket;
            });
  if (out->empty()) {
    if (error) *error = "no boundary rows for p_bucket_version=" +
                        std::to_string(bucket_version);
    return false;
  }
  return true;
}

// ============================================================================
// File listing per bucket
// ============================================================================

int BucketIdFromPath(const std::string& path) {
  const std::string key = "p_bucket=";
  auto pos = path.find(key);
  if (pos == std::string::npos) return -1;
  pos += key.size();
  int b = 0;
  bool any = false;
  while (pos < path.size() && std::isdigit(static_cast<unsigned char>(path[pos]))) {
    b = b * 10 + (path[pos++] - '0');
    any = true;
  }
  return any ? b : -1;
}

struct TableFiles {
  std::vector<std::vector<std::string>> per_bucket;  // sorted by filename
  std::vector<std::vector<int64_t>>      rows;        // parallel to per_bucket
};

bool DiscoverTableFiles(const fs::path& staging, const std::string& table,
                        int32_t n_buckets, TableFiles* out, std::string* error) {
  auto md = LatestMetadataJson(
      staging / "primeparts" / table / "metadata", error);
  if (md.empty()) return false;
  auto reader = SourceTableReader::OpenMetadata(md, {"p"}, nullptr, error);
  if (!reader) return false;
  auto files = reader->source_files();
  reader.reset();

  out->per_bucket.assign(n_buckets, {});
  out->rows.assign(n_buckets, {});
  for (const auto& f : files) {
    int b = BucketIdFromPath(f.path);
    if (b < 0) {
      if (error) *error = "file " + f.path + " has no p_bucket=N";
      return false;
    }
    if (b >= n_buckets) continue;  // outside --buckets limit; ignore
    out->per_bucket[b].push_back(f.path);
    out->rows[b].push_back(f.record_count);
  }
  // Sort each bucket's files by filename (stable mirror of file_seq).
  for (int b = 0; b < n_buckets; ++b) {
    auto& paths = out->per_bucket[b];
    auto& counts = out->rows[b];
    std::vector<size_t> order(paths.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t i, size_t j) {
      return paths[i] < paths[j];
    });
    std::vector<std::string> sp(paths.size());
    std::vector<int64_t>     sc(paths.size());
    for (size_t i = 0; i < order.size(); ++i) {
      sp[i] = std::move(paths[order[i]]);
      sc[i] = counts[order[i]];
    }
    paths = std::move(sp);
    counts = std::move(sc);
  }
  return true;
}

// ============================================================================
// Parquet read/write helpers
// ============================================================================

// Open a source parquet file with the same reader knobs rewrite.cc uses
// (use_threads=true, pre_buffer=true, 1M-row batches).
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

// Return true if every row group reports null_count==0 for prime_rank.
// Used to skip already-backfilled files on restart.
bool PrimeRankFullyPopulated(const std::string& path, bool* out_populated,
                             std::string* error) {
  auto reader = OpenSourceFile(path, error);
  if (!reader) return false;
  auto md = reader->parquet_reader()->metadata();
  int pr_idx = md->schema()->ColumnIndex("prime_rank");
  if (pr_idx < 0) {
    if (error) *error = "no prime_rank column in " + path;
    return false;
  }
  for (int rg = 0; rg < md->num_row_groups(); ++rg) {
    auto col = md->RowGroup(rg)->ColumnChunk(pr_idx);
    if (!col->is_stats_set()) {
      *out_populated = false;
      return true;
    }
    auto stats = col->statistics();
    if (!stats || stats->null_count() != 0) {
      *out_populated = false;
      return true;
    }
  }
  *out_populated = true;
  return true;
}

// Parquet writer properties matching writer.cc: zstd-3, 240M-row row
// groups, DELTA on the monotone columns (and on the bucket constants so
// encodings stay homogeneous across files).
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


// Construct an int64 array [start, start+length).
std::shared_ptr<arrow::Array> ArangeInt64(int64_t start, int64_t length) {
  auto buf = arrow::AllocateBuffer(length * sizeof(int64_t)).MoveValueUnsafe();
  int64_t* data = reinterpret_cast<int64_t*>(buf->mutable_data());
  for (int64_t i = 0; i < length; ++i) data[i] = start + i;
  return std::make_shared<arrow::Int64Array>(length, std::move(buf));
}

std::shared_ptr<arrow::Array> ConstInt32(int32_t value, int64_t length) {
  auto buf = arrow::AllocateBuffer(length * sizeof(int32_t)).MoveValueUnsafe();
  std::fill_n(reinterpret_cast<int32_t*>(buf->mutable_data()), length, value);
  return std::make_shared<arrow::Int32Array>(length, std::move(buf));
}

// ============================================================================
// Primes phase — per-file in-place rewrite
// ============================================================================

bool RewritePrimesFile(const std::string& src_path, int64_t start_prime_rank,
                       const std::shared_ptr<arrow::Schema>& target_schema,
                       int32_t p_bucket_version, int32_t p_bucket,
                       int64_t* out_rows, int64_t* out_p_min,
                       int64_t* out_p_max, std::string* error) {
  auto reader = OpenSourceFile(src_path, error);
  if (!reader) return false;
  auto md = reader->parquet_reader()->metadata();
  int p_idx = md->schema()->ColumnIndex("p");
  int k_idx = md->schema()->ColumnIndex("k");
  if (p_idx < 0 || k_idx < 0) {
    *error = "primes file missing p or k column: " + src_path;
    return false;
  }
  std::vector<int> col_indices = {p_idx, k_idx};
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
  auto props = WriterProps(
      *target_schema, {"p", "prime_rank", "p_bucket_version", "p_bucket"});
  auto fw_r = parquet::arrow::FileWriter::Open(
      *target_schema, arrow::default_memory_pool(), sink, props,
      parquet::default_arrow_writer_properties());
  if (!fw_r.ok()) {
    *error = "FileWriter::Open: " + fw_r.status().ToString();
    return false;
  }
  auto writer = std::move(fw_r).ValueOrDie();

  int64_t rank_cursor = start_prime_rank;
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
    auto p_arr = batch->column(0);
    auto k_arr = batch->column(1);
    auto pr_arr = ArangeInt64(rank_cursor, n);
    auto bv_arr = ConstInt32(p_bucket_version, n);
    auto bk_arr = ConstInt32(p_bucket, n);
    // PrimesSchema order: p, k, prime_rank, p_bucket_version, p_bucket.
    auto out_batch = arrow::RecordBatch::Make(
        target_schema, n, {p_arr, k_arr, pr_arr, bv_arr, bk_arr});
    auto ws = writer->WriteRecordBatch(*out_batch);
    if (!ws.ok()) { *error = "WriteRecordBatch: " + ws.ToString(); return false; }

    const int64_t* p_data =
        static_cast<const arrow::Int64Array*>(p_arr.get())->raw_values();
    if (p_data[0] < p_min) p_min = p_data[0];
    if (p_data[n - 1] > p_max) p_max = p_data[n - 1];
    rank_cursor += n;
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
// Partitions phase — per-bucket streaming repeat
// ============================================================================

// Primes-side expansion buffer for one bucket. The "pre-expand" pattern:
// for each pulled primes batch (~1M primes), expand once into a
// contiguous int64 buffer of length sum(k) — each prime_rank value
// repeated k_p times — and then hand whole slices to the partitions
// writer. No per-row state on the consume path.
//
// Granularity is per primes batch (~15 MB expanded for 1M primes at
// k̄=1.88) rather than per bucket (~58 GB, RAM-bound) or per primes
// file (~14 GB). Six parallel workers × ~15 MB working set fits
// trivially.
class PrimesExpander {
 public:
  bool Init(std::vector<std::string> files, std::string* error) {
    files_ = std::move(files);
    return OpenNextFile(error);
  }

  // Pull the next expanded chunk. On success returns a non-null
  // pointer/length valid until the next call; on EOF returns
  // {nullptr, 0}. Caller consumes the chunk fully before calling
  // again.
  bool NextChunk(const int64_t** out_data, int64_t* out_len,
                 std::string* error) {
    while (true) {
      if (!rbr_) {                                      // no more files
        *out_data = nullptr;
        *out_len = 0;
        return true;
      }
      std::shared_ptr<arrow::RecordBatch> batch;
      auto st = rbr_->ReadNext(&batch);
      if (!st.ok()) { *error = "primes ReadNext: " + st.ToString(); return false; }
      if (!batch || batch->num_rows() == 0) {
        if (!OpenNextFile(error)) return false;
        continue;
      }
      Expand(*batch);                                   // fills expanded_
      *out_data = expanded_.data();
      *out_len = static_cast<int64_t>(expanded_.size());
      return true;
    }
  }

 private:
  // Open the next primes file, project (prime_rank, k), set up the
  // RecordBatchReader. When no more files: clears rbr_.
  bool OpenNextFile(std::string* error) {
    rbr_.reset();
    reader_.reset();
    if (file_idx_ >= static_cast<int64_t>(files_.size())) return true;
    reader_ = OpenSourceFile(files_[file_idx_], error);
    if (!reader_) return false;
    auto md = reader_->parquet_reader()->metadata();
    int pr_idx = md->schema()->ColumnIndex("prime_rank");
    int k_idx = md->schema()->ColumnIndex("k");
    if (pr_idx < 0 || k_idx < 0) {
      *error = "primes file missing prime_rank/k: " + files_[file_idx_];
      return false;
    }
    std::vector<int> col_indices = {pr_idx, k_idx};
    std::vector<int> row_groups(md->num_row_groups());
    for (int i = 0; i < md->num_row_groups(); ++i) row_groups[i] = i;
    auto rbr_r = reader_->GetRecordBatchReader(row_groups, col_indices);
    if (!rbr_r.ok()) {
      *error = "GetRecordBatchReader: " + rbr_r.status().ToString();
      return false;
    }
    rbr_ = std::move(rbr_r).ValueOrDie();
    file_idx_++;
    return true;
  }

  // Build `expanded_` = repeat(prime_rank, k) for this primes batch.
  // sum(k) is the new length; resize once and walk the primes once
  // with std::fill_n per prime (the inner block is memset-rate).
  void Expand(const arrow::RecordBatch& batch) {
    const int64_t m = batch.num_rows();
    auto pr_arr = static_cast<const arrow::Int64Array*>(batch.column(0).get());
    auto k_arr  = static_cast<const arrow::Int32Array*>(batch.column(1).get());
    const int64_t* pr = pr_arr->raw_values();
    const int32_t* k  = k_arr->raw_values();
    int64_t total = 0;
    for (int64_t i = 0; i < m; ++i) total += k[i];
    expanded_.resize(total);
    int64_t pos = 0;
    for (int64_t i = 0; i < m; ++i) {
      const int32_t kp = k[i];
      if (kp > 0) {
        std::fill_n(expanded_.data() + pos, kp, pr[i]);
        pos += kp;
      }
    }
  }

  std::vector<std::string> files_;
  int64_t file_idx_ = 0;
  std::unique_ptr<parquet::arrow::FileReader> reader_;
  std::shared_ptr<arrow::RecordBatchReader> rbr_;
  std::vector<int64_t> expanded_;
};

// Cursor that walks expanded primes-side chunks for the partitions
// writer. Pulls a new chunk only when the current one is fully
// consumed. The consume path is a single memcpy per partitions batch.
class ExpandedSink {
 public:
  explicit ExpandedSink(PrimesExpander* expander) : expander_(expander) {}

  // Copy the next n int64 values into out. Returns false on error or
  // if the primes side runs out before n is satisfied.
  bool Consume(int64_t* out, int64_t n, std::string* error) {
    int64_t written = 0;
    while (written < n) {
      if (cur_pos_ >= cur_len_) {
        if (!expander_->NextChunk(&cur_data_, &cur_len_, error)) return false;
        cur_pos_ = 0;
        if (cur_len_ == 0) {
          *error = "primes stream exhausted before partitions";
          return false;
        }
      }
      const int64_t take = std::min(n - written, cur_len_ - cur_pos_);
      std::memcpy(out + written, cur_data_ + cur_pos_, take * sizeof(int64_t));
      cur_pos_ += take;
      written += take;
    }
    return true;
  }

  // After all partitions consumed: the primes side must also be at EOF.
  bool VerifyDrained(std::string* error) {
    if (cur_pos_ < cur_len_) {
      *error = "primes cursor not drained (residual=" +
               std::to_string(cur_len_ - cur_pos_) + ")";
      return false;
    }
    if (!expander_->NextChunk(&cur_data_, &cur_len_, error)) return false;
    if (cur_len_ != 0) {
      *error = "primes cursor not drained (extra chunk of " +
               std::to_string(cur_len_) + ")";
      return false;
    }
    return true;
  }

 private:
  PrimesExpander* expander_;
  const int64_t* cur_data_ = nullptr;
  int64_t cur_len_ = 0;
  int64_t cur_pos_ = 0;
};

bool RewritePartitionsFile(const std::string& src_path, ExpandedSink* primes,
                           const std::shared_ptr<arrow::Schema>& target_schema,
                           int32_t p_bucket_version, int32_t p_bucket,
                           int64_t* out_rows, int64_t* out_p_min,
                           int64_t* out_p_max, std::string* error) {
  auto reader = OpenSourceFile(src_path, error);
  if (!reader) return false;
  auto md = reader->parquet_reader()->metadata();
  int p_idx  = md->schema()->ColumnIndex("p");
  int mk_idx = md->schema()->ColumnIndex("m_k");
  int nk_idx = md->schema()->ColumnIndex("n_k");
  int qk_idx = md->schema()->ColumnIndex("q_k");
  if (p_idx < 0 || mk_idx < 0 || nk_idx < 0 || qk_idx < 0) {
    *error = "partitions file missing column: " + src_path;
    return false;
  }
  std::vector<int> col_indices = {p_idx, mk_idx, nk_idx, qk_idx};
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
  auto props = WriterProps(
      *target_schema,
      {"p", "prime_rank", "q_k", "p_bucket_version", "p_bucket"});
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

    auto rank_buf =
        arrow::AllocateBuffer(n * sizeof(int64_t)).MoveValueUnsafe();
    int64_t* rank_data = reinterpret_cast<int64_t*>(rank_buf->mutable_data());
    if (!primes->Consume(rank_data, n, error)) return false;
    auto pr_arr = std::make_shared<arrow::Int64Array>(n, std::move(rank_buf));
    auto bv_arr = ConstInt32(p_bucket_version, n);
    auto bk_arr = ConstInt32(p_bucket, n);

    // PartitionsSchema order: p, m_k, n_k, q_k, prime_rank, p_bucket_version, p_bucket
    auto out_batch = arrow::RecordBatch::Make(
        target_schema, n,
        {batch->column(0), batch->column(1), batch->column(2),
         batch->column(3), pr_arr, bv_arr, bk_arr});
    auto ws = writer->WriteRecordBatch(*out_batch);
    if (!ws.ok()) { *error = "WriteRecordBatch: " + ws.ToString(); return false; }

    const int64_t* p_data =
        static_cast<const arrow::Int64Array*>(batch->column(0).get())
            ->raw_values();
    if (p_data[0] < p_min) p_min = p_data[0];
    if (p_data[n - 1] > p_max) p_max = p_data[n - 1];
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
// Per-bucket worker output
// ============================================================================

struct BackfillFile {
  std::string path;
  int32_t p_bucket = 0;
  int64_t rows = 0;
  int64_t p_min = 0;
  int64_t p_max = 0;
  int64_t bytes = 0;
};

struct BucketResult {
  std::vector<BackfillFile> files;
  std::string error;
};

// ============================================================================
// Iceberg publish helpers
// ============================================================================

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
                  const BackfillFile& bf, int32_t p_bucket_version,
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
  for (const auto& field : schema->fields()) {
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

bool PublishTable(const std::shared_ptr<iceberg::Catalog>& catalog,
                  const fs::path& staging, const std::string& table_name,
                  const std::shared_ptr<iceberg::Schema>& schema,
                  const std::shared_ptr<iceberg::PartitionSpec>& spec,
                  const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
                  std::string* metadata_location, std::string* error) {
  // Wipe existing metadata/ — the previous snapshot referenced the
  // pre-backfill (null prime_rank) state with the old schema.
  fs::path md_dir = staging / "primeparts" / table_name / "metadata";
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(md_dir, ec)) {
    fs::remove(entry.path(), ec);
  }
  fs::create_directories(md_dir, ec);

  iceberg::TableIdentifier ident{
      .ns = iceberg::Namespace{{"primeparts"}}, .name = table_name};
  auto del = catalog->DropTable(ident, false);
  (void)del;  // ignore: may not exist if this is a fresh in-memory catalog
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
    if (!cs.has_value()) { *error = "Commit: " + cs.error().message; return false; }
    auto rs = table->Refresh();
    if (!rs.has_value()) { *error = "Refresh: " + rs.error().message; return false; }
  }
  *metadata_location = std::string(table->metadata_file_location());
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
  std::vector<BoundaryRow> boundaries;
  if (!LoadBoundaries(opts.staging, opts.p_bucket_version, &boundaries,
                      &error)) {
    std::fprintf(stderr, "load boundaries: %s\n", error.c_str());
    return 1;
  }
  int32_t n_buckets = static_cast<int32_t>(boundaries.size());
  if (opts.buckets_limit > 0 && opts.buckets_limit < n_buckets) {
    n_buckets = opts.buckets_limit;
    boundaries.resize(n_buckets);
    std::fprintf(stdout, "limiting to first %d bucket(s)\n", n_buckets);
  }
  std::fprintf(stdout, "boundaries: %d bucket(s) for p_bucket_version=%d\n",
               n_buckets, opts.p_bucket_version);

  TableFiles primes_files;
  TableFiles parts_files;
  if (!DiscoverTableFiles(opts.staging, "primes", n_buckets, &primes_files,
                          &error)) {
    std::fprintf(stderr, "discover primes files: %s\n", error.c_str());
    return 1;
  }
  if (!DiscoverTableFiles(opts.staging, "partitions", n_buckets, &parts_files,
                          &error)) {
    std::fprintf(stderr, "discover partitions files: %s\n", error.c_str());
    return 1;
  }
  for (int32_t b = 0; b < n_buckets; ++b) {
    std::fprintf(stdout, "  bucket %d: rank_min=%lld  primes_files=%zu  partitions_files=%zu\n",
                 b, static_cast<long long>(boundaries[b].rank_min),
                 primes_files.per_bucket[b].size(),
                 parts_files.per_bucket[b].size());
  }
  std::fflush(stdout);

  auto primes_schema = PrimesSchema();
  auto primes_arrow = IcebergToArrowSchemaWithFieldIds(*primes_schema, &error);
  if (!primes_arrow) {
    std::fprintf(stderr, "primes arrow schema: %s\n", error.c_str());
    return 1;
  }
  auto parts_schema = PartitionsSchema();
  auto parts_arrow = IcebergToArrowSchemaWithFieldIds(*parts_schema, &error);
  if (!parts_arrow) {
    std::fprintf(stderr, "partitions arrow schema: %s\n", error.c_str());
    return 1;
  }
  auto primes_spec = BucketPartitionSpec(*primes_schema, &error);
  if (!primes_spec) {
    std::fprintf(stderr, "primes spec: %s\n", error.c_str());
    return 1;
  }
  auto parts_spec = BucketPartitionSpec(*parts_schema, &error);
  if (!parts_spec) {
    std::fprintf(stderr, "partitions spec: %s\n", error.c_str());
    return 1;
  }

  int n_workers = opts.threads > 0 ? opts.threads : n_buckets;

  std::vector<BucketResult> primes_results(n_buckets);
  std::vector<BucketResult> parts_results(n_buckets);
  std::atomic<int32_t> cursor{0};

  auto run_pool = [&](auto&& body) {
    cursor.store(0);
    std::vector<std::thread> ts;
    for (int t = 0; t < n_workers; ++t) {
      ts.emplace_back([&] {
        while (true) {
          int32_t b = cursor.fetch_add(1);
          if (b >= n_buckets) return;
          body(b);
        }
      });
    }
    for (auto& t : ts) t.join();
  };

  // -------- Primes phase --------
  if (!opts.partitions_only) {
    auto t0 = std::chrono::steady_clock::now();
    std::fprintf(stdout, "\n== primes phase: %d bucket(s), %d worker(s) ==\n",
                 n_buckets, n_workers);
    std::fflush(stdout);

    run_pool([&](int32_t b) {
      BucketResult& res = primes_results[b];
      int64_t cum_rows = 0;
      const int64_t base_rank = boundaries[b].rank_min + 2;
      const auto& paths = primes_files.per_bucket[b];
      for (size_t i = 0; i < paths.size(); ++i) {
        const std::string& path = paths[i];
        // Skip if already populated.
        bool already = false;
        std::string err;
        if (!PrimeRankFullyPopulated(path, &already, &err)) {
          res.error = err;
          return;
        }
        BackfillFile bf;
        bf.path = path;
        bf.p_bucket = b;
        if (already) {
          // Read row count + p min/max from metadata to keep the
          // DataFile record consistent without rewriting.
          auto rd = OpenSourceFile(path, &err);
          if (!rd) { res.error = err; return; }
          auto md = rd->parquet_reader()->metadata();
          bf.rows = md->num_rows();
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
          bf.p_min = pmin;
          bf.p_max = pmax;
        } else {
          int64_t rows = 0, pmin = 0, pmax = 0;
          if (!RewritePrimesFile(path, base_rank + cum_rows, primes_arrow,
                                 opts.p_bucket_version, b, &rows, &pmin, &pmax,
                                 &err)) {
            res.error = err;
            return;
          }
          bf.rows = rows;
          bf.p_min = pmin;
          bf.p_max = pmax;
        }
        std::error_code ec;
        bf.bytes = static_cast<int64_t>(fs::file_size(path, ec));
        cum_rows += bf.rows;
        res.files.push_back(std::move(bf));
        std::fprintf(stdout, "  primes bucket=%d file=%zu/%zu rows=%lld %s\n",
                     b, i + 1, paths.size(),
                     static_cast<long long>(res.files.back().rows),
                     already ? "(skip)" : "(rewrite)");
        std::fflush(stdout);
      }
    });
    for (int b = 0; b < n_buckets; ++b) {
      if (!primes_results[b].error.empty()) {
        std::fprintf(stderr, "primes bucket %d: %s\n", b,
                     primes_results[b].error.c_str());
        return 1;
      }
    }
    auto t1 = std::chrono::steady_clock::now();
    std::fprintf(stdout, "primes phase: done (%.1fs)\n",
                 std::chrono::duration<double>(t1 - t0).count());
  }

  // -------- Partitions phase --------
  if (!opts.primes_only) {
    auto t0 = std::chrono::steady_clock::now();
    std::fprintf(stdout, "\n== partitions phase: %d bucket(s), %d worker(s) ==\n",
                 n_buckets, n_workers);
    std::fflush(stdout);

    run_pool([&](int32_t b) {
      BucketResult& res = parts_results[b];
      // Pre-expand primes-side prime_rank by k once per primes batch;
      // sink hands contiguous int64 slices to the partitions writer.
      PrimesExpander expander;
      std::string err;
      if (!expander.Init(primes_files.per_bucket[b], &err)) {
        res.error = err;
        return;
      }
      ExpandedSink sink(&expander);

      const auto& paths = parts_files.per_bucket[b];
      for (size_t i = 0; i < paths.size(); ++i) {
        const std::string& path = paths[i];
        bool already = false;
        if (!PrimeRankFullyPopulated(path, &already, &err)) {
          res.error = err;
          return;
        }
        BackfillFile bf;
        bf.path = path;
        bf.p_bucket = b;
        if (already) {
          // Same metadata-only fast path as the primes phase. We also
          // need to advance the primes cursor by this file's row count
          // so subsequent files in the bucket see the right state.
          auto rd = OpenSourceFile(path, &err);
          if (!rd) { res.error = err; return; }
          auto md = rd->parquet_reader()->metadata();
          bf.rows = md->num_rows();
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
          bf.p_min = pmin;
          bf.p_max = pmax;
          // Drain n primes rows from the sink without writing.
          std::vector<int64_t> drain(bf.rows);
          if (!sink.Consume(drain.data(), bf.rows, &err)) { res.error = err; return; }
        } else {
          int64_t rows = 0, pmin = 0, pmax = 0;
          if (!RewritePartitionsFile(path, &sink, parts_arrow,
                                     opts.p_bucket_version, b, &rows, &pmin,
                                     &pmax, &err)) {
            res.error = err;
            return;
          }
          bf.rows = rows;
          bf.p_min = pmin;
          bf.p_max = pmax;
        }
        std::error_code ec;
        bf.bytes = static_cast<int64_t>(fs::file_size(path, ec));
        res.files.push_back(std::move(bf));
        std::fprintf(stdout, "  partitions bucket=%d file=%zu/%zu rows=%lld %s\n",
                     b, i + 1, paths.size(),
                     static_cast<long long>(res.files.back().rows),
                     already ? "(skip)" : "(rewrite)");
        std::fflush(stdout);
      }
      // Sanity: primes side must be fully drained at bucket end.
      if (!sink.VerifyDrained(&err)) {
        res.error = "bucket " + std::to_string(b) + ": " + err;
        return;
      }
    });
    for (int b = 0; b < n_buckets; ++b) {
      if (!parts_results[b].error.empty()) {
        std::fprintf(stderr, "partitions bucket %d: %s\n", b,
                     parts_results[b].error.c_str());
        return 1;
      }
    }
    auto t1 = std::chrono::steady_clock::now();
    std::fprintf(stdout, "partitions phase: done (%.1fs)\n",
                 std::chrono::duration<double>(t1 - t0).count());
  }

  // -------- Publish --------
  if (opts.skip_publish) {
    std::fprintf(stdout, "\n--skip-publish set; leaving existing iceberg metadata in place.\n");
    return 0;
  }

  std::vector<std::shared_ptr<iceberg::DataFile>> primes_dfs;
  for (int b = 0; b < n_buckets; ++b) {
    for (const auto& bf : primes_results[b].files) {
      std::shared_ptr<iceberg::DataFile> df;
      if (!MakeDataFile(primes_schema, primes_spec, bf, opts.p_bucket_version,
                        &df, &error)) {
        std::fprintf(stderr, "primes MakeDataFile: %s\n", error.c_str());
        return 1;
      }
      primes_dfs.push_back(std::move(df));
    }
  }
  std::vector<std::shared_ptr<iceberg::DataFile>> parts_dfs;
  for (int b = 0; b < n_buckets; ++b) {
    for (const auto& bf : parts_results[b].files) {
      std::shared_ptr<iceberg::DataFile> df;
      if (!MakeDataFile(parts_schema, parts_spec, bf, opts.p_bucket_version,
                        &df, &error)) {
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

  std::fprintf(stdout, "\nbackfill complete.\n");
  return 0;
}
