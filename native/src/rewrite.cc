// primeparts-rewrite — one-shot rewriter from the live `funbuns.primes`
// + `funbuns.decompositions` source tables into the new bucket-
// partitioned staging warehouse.
//
// Architecture
// ============
// Two sequential phases: primes first, then partitions. Each phase
// runs one BucketJob per bucket on a worker pool. With 6 buckets and 6
// workers the system is comfortably under-subscribed on a 12-core box,
// but each parquet reader is configured with ArrowReaderProperties
// use_threads=true + pre_buffer=true so Arrow can use SMT for column
// decode and coalesced I/O. No row-by-row Append anywhere — output
// batches are built from zero-copy slices for passthrough columns and
// `MakeArrayOfNull` / `std::fill_n` allocations for derived columns.
//
// Per phase:
//   1. Plan: read each source table's manifest stats once (no data
//      decode), build a per-bucket list of source files whose
//      [p_min, p_max] overlaps the bucket's [p_min, p_max_excl).
//   2. Build BucketJobs (one per bucket).
//   3. Pool workers Pop() jobs, run their inner loop, push results.
//   4. Main thread joins, emits files.jsonl entries.
//
// prime_rank
// ==========
// Both tables write prime_rank as all-NULL during the rewrite. It is
// the prime-counting function π(p) and will be backfilled in a
// separate pass after staging:
//   - primes: trivial arange [2, 3, ..., N+1] sliced per bucket
//   - partitions: repeat(primes.prime_rank, primes.k) per chunk
// Keeping prime_rank out of the rewrite hot path makes both passes
// structurally identical (read source, slice to bucket, add bucket
// columns, write) and removes the staging-primes-re-read that
// dominated the previous implementation.

#include "primeparts/calibration.h"
#include "primeparts/preflight.h"
#include "primeparts/source_scan.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/thread_pool.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>
#include <parquet/statistics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace cal = primeparts::calibration;

using primeparts::BucketDataDir;
using primeparts::BucketParquetWriter;
using primeparts::IcebergToArrowSchemaWithFieldIds;
using primeparts::NextFileSeq;
using primeparts::PartitionsSchema;
using primeparts::PrimesSchema;
using primeparts::SourceFileInfo;
using primeparts::SourceTableReader;
using primeparts::WriterConfig;
using primeparts::WrittenFile;

namespace {

constexpr int kWorkersPerPhase = 12;
constexpr int kArrowThreadPoolSize = 24;
constexpr int64_t kReaderBatchSize = 1 << 20;        // 1M rows per batch out of readers
constexpr int64_t kTargetFileBytes = 1LL << 30;       // 1 GiB target file size
constexpr int kProgressTickSeconds = 10;

// ============================================================================
// Progress
// ============================================================================
//
// One background thread per binary. Ticks every kProgressTickSeconds and
// writes a single \r-overwritten line to stdout. Workers call AddRows()
// from any thread; the counter is atomic. Rate is shown per minute so
// the number stays human-sized across the multi-hour run.

class Progress {
 public:
  Progress() = default;
  ~Progress() { Stop(); }

  void Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
  }
  void Stop() {
    if (!thread_.joinable()) return;
    {
      std::lock_guard<std::mutex> g(mu_);
      stop_ = true;
      cv_.notify_all();
    }
    thread_.join();
    std::lock_guard<std::mutex> g(mu_);
    if (!stage_label_.empty()) DrawLocked(true);
  }

  void BeginStage(int idx, int total, std::string label, int64_t total_rows) {
    std::lock_guard<std::mutex> g(mu_);
    if (!stage_label_.empty()) DrawLocked(true);  // finalize prior stage
    stage_idx_ = idx;
    stage_total_ = total;
    stage_label_ = std::move(label);
    stage_total_rows_.store(total_rows, std::memory_order_relaxed);
    stage_rows_done_.store(0, std::memory_order_relaxed);
    stage_start_ = std::chrono::steady_clock::now();
  }

  void AddRows(int64_t n) {
    stage_rows_done_.fetch_add(n, std::memory_order_relaxed);
  }

 private:
  void Run() {
    std::unique_lock<std::mutex> g(mu_);
    while (!stop_) {
      cv_.wait_for(g, std::chrono::seconds(kProgressTickSeconds));
      if (stop_) break;
      DrawLocked(false);
    }
  }
  void DrawLocked(bool final) {
    if (stage_label_.empty()) return;
    int64_t done = stage_rows_done_.load(std::memory_order_relaxed);
    int64_t total = stage_total_rows_.load(std::memory_order_relaxed);
    double pct = total > 0 ? 100.0 * static_cast<double>(done) / total : 0.0;
    if (final) { done = total; pct = 100.0; }
    int bar_w = 24;
    int filled = total > 0 ? static_cast<int>(bar_w * done / total) : 0;
    if (filled > bar_w) filled = bar_w;
    std::string bar(filled, '#');
    bar.append(bar_w - filled, '.');
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - stage_start_).count();
    double rate_per_min = elapsed > 0.0 ? 60.0 * done / elapsed : 0.0;
    int64_t eta_s = (rate_per_min > 0.0 && done < total)
                      ? static_cast<int64_t>(60.0 * (total - done) / rate_per_min)
                      : 0;
    int eh = static_cast<int>(eta_s / 3600);
    int em = static_cast<int>((eta_s % 3600) / 60);
    char rate_buf[32];
    if (rate_per_min >= 1.0e9)
      std::snprintf(rate_buf, sizeof(rate_buf), "%.2fB/min", rate_per_min / 1.0e9);
    else if (rate_per_min >= 1.0e6)
      std::snprintf(rate_buf, sizeof(rate_buf), "%.1fM/min", rate_per_min / 1.0e6);
    else if (rate_per_min >= 1.0e3)
      std::snprintf(rate_buf, sizeof(rate_buf), "%.1fk/min", rate_per_min / 1.0e3);
    else
      std::snprintf(rate_buf, sizeof(rate_buf), "%.0f/min", rate_per_min);
    std::fprintf(stdout,
                 "\r[%d/%d] %-10s [%s] %5.1f%%  %lld/%lld rows  %s  ETA %dh%02dm    ",
                 stage_idx_, stage_total_, stage_label_.c_str(), bar.c_str(),
                 pct, static_cast<long long>(done), static_cast<long long>(total),
                 rate_buf, eh, em);
    if (final) std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  int stage_idx_ = 0;
  int stage_total_ = 0;
  std::string stage_label_;
  std::atomic<int64_t> stage_total_rows_{0};
  std::atomic<int64_t> stage_rows_done_{0};
  std::chrono::steady_clock::time_point stage_start_;
};

// ============================================================================
// Options
// ============================================================================

struct Options {
  fs::path source_warehouse;
  fs::path staging_warehouse;
  std::string source_namespace = "funbuns";
  std::string source_primes_table = "primes";
  std::string source_partitions_table = "decompositions";
  std::string staging_primes_table = "primes";
  std::string staging_partitions_table = "partitions";
  int32_t p_bucket_version = 1;
  int64_t limit_primes = -1;          // smoke test cap; -1 = unlimited
  int32_t buckets = -1;               // smoke test: process only first N buckets; -1 = all
  bool preflight_only = false;
  bool skip_preflight = false;
};

void usage(FILE* s) {
  std::fprintf(
      s,
      "usage: primeparts-rewrite --source <warehouse> --staging <warehouse> [opts]\n"
      "\n"
      "  --source PATH        warehouse root containing catalog.db (read-only)\n"
      "  --staging PATH       output warehouse root (must not exist)\n"
      "  --source-namespace NS    default: funbuns\n"
      "  --source-primes NAME     default: primes\n"
      "  --source-partitions NAME default: decompositions\n"
      "  --p-bucket-version V     default: 1\n"
      "  --limit-primes N         smoke test: cap pass 1 at N primes\n"
      "  --buckets N              smoke test: process only the first N buckets\n"
      "  --skip-preflight         skip Pass 0 (dev iteration only)\n"
      "  --preflight-only         run Pass 0 against --source and exit\n"
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
      {"source", required_argument, nullptr, 's'},
      {"staging", required_argument, nullptr, 'S'},
      {"source-namespace", required_argument, nullptr, 1000},
      {"source-primes", required_argument, nullptr, 1001},
      {"source-partitions", required_argument, nullptr, 1002},
      {"p-bucket-version", required_argument, nullptr, 'v'},
      {"limit-primes", required_argument, nullptr, 'l'},
      {"buckets", required_argument, nullptr, 1005},
      {"skip-preflight", no_argument, nullptr, 1004},
      {"preflight-only", no_argument, nullptr, 1003},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  int opt;
  while ((opt = getopt_long(argc, argv, "s:S:v:l:h", longs, nullptr)) != -1) {
    switch (opt) {
      case 's': o->source_warehouse = optarg; break;
      case 'S': o->staging_warehouse = optarg; break;
      case 1000: o->source_namespace = optarg; break;
      case 1001: o->source_primes_table = optarg; break;
      case 1002: o->source_partitions_table = optarg; break;
      case 'v': {
        int64_t v;
        if (!parse_i64(optarg, &v) || v <= 0 ||
            v > std::numeric_limits<int32_t>::max()) {
          std::fprintf(stderr, "invalid --p-bucket-version\n");
          return false;
        }
        o->p_bucket_version = static_cast<int32_t>(v);
        break;
      }
      case 'l':
        if (!parse_i64(optarg, &o->limit_primes) || o->limit_primes < 0) {
          std::fprintf(stderr, "invalid --limit-primes\n");
          return false;
        }
        break;
      case 1003: o->preflight_only = true; break;
      case 1004: o->skip_preflight = true; break;
      case 1005: {
        int64_t v;
        if (!parse_i64(optarg, &v) || v <= 0 ||
            v > std::numeric_limits<int32_t>::max()) {
          std::fprintf(stderr, "invalid --buckets\n");
          return false;
        }
        o->buckets = static_cast<int32_t>(v);
        break;
      }
      case 'h': usage(stdout); std::exit(0);
      default: return false;
    }
  }
  if (o->source_warehouse.empty() ||
      (!o->preflight_only && o->staging_warehouse.empty())) {
    usage(stderr);
    return false;
  }
  return true;
}

// ============================================================================
// Boundary + bucket plan
// ============================================================================

struct BoundaryRow {
  int32_t p_bucket;
  int64_t p_min;
  int64_t rank_min;
};

bool load_boundaries(const fs::path& sqlite, std::string_view ns,
                     int32_t version, std::vector<BoundaryRow>* out,
                     std::string* error) {
  auto reader = SourceTableReader::Open(
      sqlite, ns, "boundaries",
      {"p_bucket_version", "p_bucket", "p_min", "rank_min"}, error);
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
      if (v->Value(i) != version) continue;
      if (rm->IsNull(i)) {
        *error = "boundaries.rank_min is null — run scripts/add_rank_min_to_boundaries.py --apply";
        return false;
      }
      out->push_back(BoundaryRow{b->Value(i), pm->Value(i), rm->Value(i)});
    }
  }
  std::sort(out->begin(), out->end(),
            [](const BoundaryRow& a, const BoundaryRow& b) {
              return a.p_bucket < b.p_bucket;
            });
  if (out->empty()) {
    *error = "no boundary rows for p_bucket_version=" + std::to_string(version);
    return false;
  }
  for (size_t i = 0; i < out->size(); ++i) {
    if ((*out)[i].p_bucket != static_cast<int32_t>(i)) {
      *error = "boundary p_bucket values not contiguous 0..B-1";
      return false;
    }
  }
  for (size_t i = 1; i < out->size(); ++i) {
    if ((*out)[i].p_min <= (*out)[i - 1].p_min) {
      *error = "boundary p_min not strictly increasing";
      return false;
    }
  }
  if ((*out)[0].p_min != cal::kExpectedPMin0) {
    *error = "boundary[0].p_min != " + std::to_string(cal::kExpectedPMin0);
    return false;
  }
  return true;
}

struct BucketPlanRow {
  int32_t p_bucket;
  int64_t p_min;
  int64_t p_max_excl;
  int64_t rank_min;
  int64_t primes_rows;
  int64_t partitions_rows;
  int64_t target_files_primes;
  int64_t target_files_partitions;
};

std::vector<BucketPlanRow> derive_plan(
    const std::vector<BoundaryRow>& boundaries, int64_t n_primes_total) {
  const int64_t B = static_cast<int64_t>(boundaries.size());
  std::vector<BucketPlanRow> plan;
  plan.reserve(B);
  for (int64_t i = 0; i < B; ++i) {
    BucketPlanRow r{};
    r.p_bucket = boundaries[i].p_bucket;
    r.p_min = boundaries[i].p_min;
    r.p_max_excl = (i + 1 < B) ? boundaries[i + 1].p_min
                               : std::numeric_limits<int64_t>::max();
    r.rank_min = boundaries[i].rank_min;
    r.primes_rows = (i + 1 < B)
        ? boundaries[i + 1].rank_min - boundaries[i].rank_min
        : n_primes_total - boundaries[i].rank_min;
    r.partitions_rows = static_cast<int64_t>(
        std::ceil(cal::kKMean * static_cast<double>(r.primes_rows)));
    int64_t pbytes = static_cast<int64_t>(
        std::round(static_cast<double>(r.primes_rows) * cal::kBprPrimes));
    int64_t qbytes = static_cast<int64_t>(
        std::round(static_cast<double>(r.partitions_rows) * cal::kBprPartitions));
    r.target_files_primes = std::max<int64_t>(
        1, static_cast<int64_t>(std::lround(
               static_cast<double>(pbytes) /
               static_cast<double>(kTargetFileBytes))));
    r.target_files_partitions = std::max<int64_t>(
        1, static_cast<int64_t>(std::lround(
               static_cast<double>(qbytes) /
               static_cast<double>(kTargetFileBytes))));
    plan.push_back(r);
  }
  return plan;
}

// ============================================================================
// BucketJob + worker pool
// ============================================================================

struct BucketJob {
  enum Table { Primes, Partitions };
  Table table;

  int32_t p_bucket_version;
  int32_t p_bucket;
  int64_t p_min;
  int64_t p_max_excl;
  int64_t rank_min;

  std::vector<std::string> sources;       // file paths overlapping this bucket
  fs::path output_dir;
  int64_t target_rows_per_file;

  std::shared_ptr<iceberg::Schema> iceberg_schema;
  std::shared_ptr<arrow::Schema>   arrow_schema;

  Progress* progress = nullptr;           // shared across all jobs in a phase
};

struct BucketJobResult {
  int32_t p_bucket = 0;
  BucketJob::Table table = BucketJob::Primes;
  std::vector<WrittenFile> files;
  std::string error;
};

class JobQueue {
 public:
  explicit JobQueue(std::vector<BucketJob> jobs) : jobs_(std::move(jobs)) {}
  std::optional<BucketJob> Pop() {
    size_t i = cursor_.fetch_add(1, std::memory_order_relaxed);
    if (i >= jobs_.size()) return std::nullopt;
    return std::move(jobs_[i]);
  }
 private:
  std::vector<BucketJob> jobs_;
  std::atomic<size_t> cursor_{0};
};

// ============================================================================
// Inner-loop helpers
// ============================================================================

std::shared_ptr<arrow::Array> NullInt64(int64_t length) {
  return arrow::MakeArrayOfNull(arrow::int64(), length).ValueOrDie();
}

std::shared_ptr<arrow::Array> ConstInt32(int32_t value, int64_t length) {
  auto buf = arrow::AllocateBuffer(length * sizeof(int32_t)).MoveValueUnsafe();
  std::fill_n(reinterpret_cast<int32_t*>(buf->mutable_data()), length, value);
  return std::make_shared<arrow::Int32Array>(length, std::move(buf));
}

// Resolve column indices in a parquet file's schema by name.
bool ResolveColumnIndices(const parquet::SchemaDescriptor* schema,
                          const std::vector<std::string>& names,
                          std::vector<int>* out, std::string* error) {
  out->clear();
  out->reserve(names.size());
  for (const auto& n : names) {
    int idx = schema->ColumnIndex(n);
    if (idx < 0) {
      *error = "column not found in source file: " + n;
      return false;
    }
    out->push_back(idx);
  }
  return true;
}

// Statistics min/max for the `p` column on one row group.
std::pair<int64_t, int64_t> RowGroupPRange(parquet::FileMetaData* md,
                                            int rg, int p_col_idx) {
  auto col = md->RowGroup(rg)->ColumnChunk(p_col_idx);
  auto stats = std::static_pointer_cast<parquet::Int64Statistics>(col->statistics());
  return {stats->min(), stats->max()};
}

// Pick which row groups overlap [p_min, p_max_excl). Cheap: parquet
// metadata only, no row decode.
std::vector<int> OverlappingRowGroups(parquet::FileMetaData* md, int p_col_idx,
                                       int64_t p_min, int64_t p_max_excl) {
  std::vector<int> out;
  out.reserve(md->num_row_groups());
  for (int rg = 0; rg < md->num_row_groups(); ++rg) {
    auto [lo, hi] = RowGroupPRange(md, rg, p_col_idx);
    if (hi < p_min || lo >= p_max_excl) continue;
    out.push_back(rg);
  }
  return out;
}

// Build the parquet::arrow::FileReader configured the way our hot path
// wants it: column-parallel decode, pre-buffered I/O, 1M-row batches.
std::unique_ptr<parquet::arrow::FileReader> OpenSourceFile(
    const std::string& path, std::string* error) {
  auto file_r = arrow::io::ReadableFile::Open(path);
  if (!file_r.ok()) {
    *error = "open " + path + ": " + file_r.status().ToString();
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
    *error = "FileReaderBuilder::Open: " + bs.ToString();
    return nullptr;
  }
  builder.properties(arrow_props);
  std::unique_ptr<parquet::arrow::FileReader> reader;
  auto bs2 = builder.Build(&reader);
  if (!bs2.ok()) {
    *error = "FileReaderBuilder::Build: " + bs2.ToString();
    return nullptr;
  }
  return reader;
}

// ============================================================================
// Primes inner loop
// ============================================================================

void ProcessPrimesJob(const BucketJob& job, BucketJobResult* result) {
  WriterConfig cfg;
  cfg.output_dir = job.output_dir;
  cfg.schema = job.iceberg_schema;
  cfg.table_name = "primes";
  cfg.filename_prefix = "primes";
  // p_bucket_version and p_bucket are constant within a file; force
  // DELTA on them so parquet doesn't auto-pick dictionary on some files
  // and PLAIN on others (which makes pyarrow's dataset reader complain
  // about field-type mismatches across files).
  cfg.delta_columns = {"p", "prime_rank", "p_bucket_version", "p_bucket"};
  cfg.bucket_version = job.p_bucket_version;
  cfg.bucket = job.p_bucket;
  cfg.starting_file_seq = NextFileSeq(cfg.output_dir, cfg.filename_prefix);
  cfg.target_rows_per_file = job.target_rows_per_file;
  auto writer = BucketParquetWriter::Make(std::move(cfg), &result->error);
  if (!writer) return;

  for (const auto& path : job.sources) {
    auto reader = OpenSourceFile(path, &result->error);
    if (!reader) return;
    auto md = reader->parquet_reader()->metadata();

    std::vector<int> col_indices;
    if (!ResolveColumnIndices(md->schema(), {"p", "k"},
                              &col_indices, &result->error)) {
      return;
    }
    int p_idx = col_indices[0];
    auto rgs = OverlappingRowGroups(md.get(), p_idx, job.p_min, job.p_max_excl);
    if (rgs.empty()) continue;

    auto rbr_r = reader->GetRecordBatchReader(rgs, col_indices);
    if (!rbr_r.ok()) {
      result->error = "GetRecordBatchReader: " + rbr_r.status().ToString();
      return;
    }
    auto rbr = std::move(rbr_r).ValueOrDie();

    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      auto rs = rbr->ReadNext(&batch);
      if (!rs.ok()) {
        result->error = "ReadNext (primes): " + rs.ToString();
        return;
      }
      if (!batch) break;
      const int64_t n_in = batch->num_rows();
      if (n_in == 0) continue;

      const int64_t* p_data =
          static_cast<const arrow::Int64Array*>(batch->column(0).get())
              ->raw_values();
      int64_t lo = std::lower_bound(p_data, p_data + n_in, job.p_min) - p_data;
      int64_t hi = std::lower_bound(p_data, p_data + n_in, job.p_max_excl) - p_data;
      int64_t n = hi - lo;
      if (n == 0) continue;

      auto p_arr = batch->column(0)->Slice(lo, n);                  // zero-copy
      auto k_arr = batch->column(1)->Slice(lo, n);                  // zero-copy
      auto pr_arr = NullInt64(n);                                   // backfilled later
      auto bv_arr = ConstInt32(job.p_bucket_version, n);
      auto bk_arr = ConstInt32(job.p_bucket, n);

      // PrimesSchema order: p, k, prime_rank, p_bucket_version, p_bucket
      auto out_batch = arrow::RecordBatch::Make(
          job.arrow_schema, n, {p_arr, k_arr, pr_arr, bv_arr, bk_arr});

      BucketParquetWriter::BatchStats stats{
          .p_min = p_data[lo],
          .p_max = p_data[hi - 1],
          .rank_min = 0,        // prime_rank is null until backfill
          .rank_max = 0,
      };
      if (!writer->Write(*out_batch, stats, &result->error)) return;
      if (job.progress) job.progress->AddRows(n);
    }
  }

  writer->Close(&result->files, &result->error);
}

// ============================================================================
// Partitions inner loop
// ============================================================================

void ProcessPartitionsJob(const BucketJob& job, BucketJobResult* result) {
  WriterConfig cfg;
  cfg.output_dir = job.output_dir;
  cfg.schema = job.iceberg_schema;
  cfg.table_name = "partitions";
  cfg.filename_prefix = "partitions";
  cfg.delta_columns = {"p", "prime_rank", "q_k", "p_bucket_version", "p_bucket"};
  cfg.bucket_version = job.p_bucket_version;
  cfg.bucket = job.p_bucket;
  cfg.starting_file_seq = NextFileSeq(cfg.output_dir, cfg.filename_prefix);
  cfg.target_rows_per_file = job.target_rows_per_file;
  auto writer = BucketParquetWriter::Make(std::move(cfg), &result->error);
  if (!writer) return;

  for (const auto& path : job.sources) {
    auto reader = OpenSourceFile(path, &result->error);
    if (!reader) return;
    auto md = reader->parquet_reader()->metadata();

    std::vector<int> col_indices;
    if (!ResolveColumnIndices(md->schema(), {"p", "m_k", "n_k", "q_k"},
                              &col_indices, &result->error)) {
      return;
    }
    int p_idx = col_indices[0];
    auto rgs = OverlappingRowGroups(md.get(), p_idx, job.p_min, job.p_max_excl);
    if (rgs.empty()) continue;

    auto rbr_r = reader->GetRecordBatchReader(rgs, col_indices);
    if (!rbr_r.ok()) {
      result->error = "GetRecordBatchReader: " + rbr_r.status().ToString();
      return;
    }
    auto rbr = std::move(rbr_r).ValueOrDie();

    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      auto rs = rbr->ReadNext(&batch);
      if (!rs.ok()) {
        result->error = "ReadNext (partitions): " + rs.ToString();
        return;
      }
      if (!batch) break;
      const int64_t n_in = batch->num_rows();
      if (n_in == 0) continue;

      const int64_t* p_data =
          static_cast<const arrow::Int64Array*>(batch->column(0).get())
              ->raw_values();
      int64_t lo = std::lower_bound(p_data, p_data + n_in, job.p_min) - p_data;
      int64_t hi = std::lower_bound(p_data, p_data + n_in, job.p_max_excl) - p_data;
      int64_t n = hi - lo;
      if (n == 0) continue;

      auto p_arr   = batch->column(0)->Slice(lo, n);
      auto mk_arr  = batch->column(1)->Slice(lo, n);
      auto nk_arr  = batch->column(2)->Slice(lo, n);
      auto qk_arr  = batch->column(3)->Slice(lo, n);
      auto pr_arr  = NullInt64(n);
      auto bv_arr  = ConstInt32(job.p_bucket_version, n);
      auto bk_arr  = ConstInt32(job.p_bucket, n);

      // PartitionsSchema order: p, m_k, n_k, q_k, prime_rank, p_bucket_version, p_bucket
      auto out_batch = arrow::RecordBatch::Make(
          job.arrow_schema, n,
          {p_arr, mk_arr, nk_arr, qk_arr, pr_arr, bv_arr, bk_arr});

      BucketParquetWriter::BatchStats stats{
          .p_min = p_data[lo],
          .p_max = p_data[hi - 1],
          .rank_min = 0,
          .rank_max = 0,
      };
      if (!writer->Write(*out_batch, stats, &result->error)) return;
      if (job.progress) job.progress->AddRows(n);
    }
  }

  writer->Close(&result->files, &result->error);
}

// ============================================================================
// Phase orchestration
// ============================================================================

bool RunPhase(std::vector<BucketJob> jobs, int n_workers,
              std::vector<BucketJobResult>* out, std::string* error) {
  JobQueue queue(std::move(jobs));
  std::vector<BucketJobResult> results;
  std::mutex mu;
  std::vector<std::thread> workers;
  workers.reserve(n_workers);
  for (int t = 0; t < n_workers; ++t) {
    workers.emplace_back([&] {
      while (auto job = queue.Pop()) {
        BucketJobResult r;
        r.p_bucket = job->p_bucket;
        r.table = job->table;
        if (job->table == BucketJob::Primes) {
          ProcessPrimesJob(*job, &r);
        } else {
          ProcessPartitionsJob(*job, &r);
        }
        std::lock_guard<std::mutex> g(mu);
        results.push_back(std::move(r));
      }
    });
  }
  for (auto& t : workers) t.join();
  std::sort(results.begin(), results.end(),
            [](const BucketJobResult& a, const BucketJobResult& b) {
              return a.p_bucket < b.p_bucket;
            });
  for (const auto& r : results) {
    if (!r.error.empty()) {
      *error = "bucket " + std::to_string(r.p_bucket) + ": " + r.error;
      return false;
    }
  }
  *out = std::move(results);
  return true;
}

// ============================================================================
// Per-bucket source-file filtering
// ============================================================================

// Pick source files whose [p_min, p_max] overlaps [p_min, p_max_excl).
// One straddling file may appear in two adjacent buckets; each bucket
// trims via lower_bound on read. Bounded waste — at most B-1 doubled
// files for B buckets.
std::vector<std::string> SourcesForBucket(
    const std::vector<SourceFileInfo>& all,
    int64_t p_min, int64_t p_max_excl) {
  std::vector<std::string> out;
  for (const auto& f : all) {
    if (f.p_max < p_min || f.p_min >= p_max_excl) continue;
    out.push_back(f.path);
  }
  return out;
}

// ============================================================================
// Manifest output
// ============================================================================

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

void emit_file_row(std::ofstream& out, const WrittenFile& f) {
  out << "{\"table\":\"" << json_escape(f.table) << "\","
      << "\"path\":\"" << json_escape(fs::absolute(f.path).string()) << "\","
      << "\"rows\":" << f.rows << ","
      << "\"p_min\":" << f.p_min << ","
      << "\"p_max\":" << f.p_max << ","
      << "\"rank_min\":" << f.rank_min << ","
      << "\"rank_max\":" << f.rank_max << ","
      << "\"p_bucket_version\":" << f.bucket_version << ","
      << "\"p_bucket\":" << f.bucket << ","
      << "\"bytes\":" << f.bytes << "}\n";
}

void emit_boundary(std::ofstream& out, int32_t version, int32_t bucket,
                   int64_t p_min, int64_t rank_min) {
  out << "{\"boundary\":true,"
      << "\"p_bucket_version\":" << version << ","
      << "\"p_bucket\":" << bucket << ","
      << "\"p_min\":" << p_min << ","
      << "\"rank_min\":" << rank_min << "}\n";
}

}  // namespace

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
  Options opts;
  if (!parse_args(argc, argv, &opts)) return 2;
  std::error_code ec;

  fs::path sqlite_path = opts.source_warehouse / "catalog.db";

  // Pass 0: preflight (unchanged).
  if (!opts.skip_preflight) {
    auto report = primeparts::RunPreflight(
        sqlite_path, opts.source_namespace, opts.source_primes_table,
        opts.source_partitions_table);
    std::fprintf(stdout, "preflight: %zu checks\n", report.checks.size());
    for (const auto& c : report.checks) {
      std::fprintf(stdout, "  [%s] %s%s%s\n", c.passed ? "ok  " : "FAIL",
                   c.name.c_str(), c.detail.empty() ? "" : " — ",
                   c.detail.c_str());
    }
    std::fflush(stdout);
    if (!report.all_passed()) {
      fs::path report_path = opts.preflight_only
                                 ? fs::path("/tmp/preflight_report.json")
                                 : opts.staging_warehouse / "preflight_report.json";
      if (!opts.preflight_only) fs::create_directories(opts.staging_warehouse, ec);
      std::ofstream f(report_path);
      f << report.ToJson();
      std::fprintf(stderr, "preflight: %s\n", report_path.c_str());
      return 2;
    }
    if (opts.preflight_only) {
      std::fprintf(stdout, "preflight-only: all clean, exiting.\n");
      return 0;
    }
  }

  fs::create_directories(opts.staging_warehouse, ec);
  if (ec) {
    std::fprintf(stderr, "mkdir staging: %s\n", ec.message().c_str());
    return 1;
  }
  fs::path manifest_path = opts.staging_warehouse / "files.jsonl";
  std::ofstream manifest(manifest_path, std::ios::out | std::ios::trunc);
  if (!manifest) {
    std::fprintf(stderr, "open manifest %s\n", manifest_path.c_str());
    return 1;
  }

  // Expand Arrow's CPU pool so per-reader use_threads has SMT to play with.
  auto set_s = arrow::SetCpuThreadPoolCapacity(kArrowThreadPoolSize);
  if (!set_s.ok()) {
    std::fprintf(stderr, "SetCpuThreadPoolCapacity: %s\n",
                 set_s.ToString().c_str());
    return 1;
  }

  std::string error;

  // Load boundaries (now includes rank_min).
  std::vector<BoundaryRow> boundaries;
  if (!load_boundaries(sqlite_path, opts.source_namespace,
                       opts.p_bucket_version, &boundaries, &error)) {
    std::fprintf(stderr, "load boundaries: %s\n", error.c_str());
    return 1;
  }

  // Open source readers once to (a) get manifest-aggregated total
  // records, (b) get the per-file info list. After this we close them
  // and the rewriter workers open parquet files directly.
  auto primes_reader = SourceTableReader::Open(
      sqlite_path, opts.source_namespace, opts.source_primes_table,
      {"p", "k"}, &error);
  if (!primes_reader) {
    std::fprintf(stderr, "open source primes: %s\n", error.c_str());
    return 1;
  }
  auto parts_reader = SourceTableReader::Open(
      sqlite_path, opts.source_namespace, opts.source_partitions_table,
      {"p", "m_k", "n_k", "q_k"}, &error);
  if (!parts_reader) {
    std::fprintf(stderr, "open source partitions: %s\n", error.c_str());
    return 1;
  }
  int64_t n_primes_total = primes_reader->total_records();
  std::vector<SourceFileInfo> primes_files = primes_reader->source_files();
  std::vector<SourceFileInfo> parts_files = parts_reader->source_files();
  primes_reader.reset();
  parts_reader.reset();

  std::vector<BucketPlanRow> plan = derive_plan(boundaries, n_primes_total);

  // Per-file row count is bucket-independent: pick a single target so the
  // writer rolls at the same threshold every file, and only the bucket's
  // *last* (frontier) file ends up below target. The previous form
  // `bucket_rows / target_files_for_bucket` evenly-split the last bucket
  // (e.g. 2.67 GiB bucket → 3 files of ~890 MB instead of two full + one
  // frontier). target_files_* in BucketPlanRow now only feeds progress
  // sizing.
  const int64_t rows_per_primes_file = std::max<int64_t>(
      1, static_cast<int64_t>(static_cast<double>(kTargetFileBytes) /
                              cal::kBprPrimes));
  const int64_t rows_per_partitions_file = std::max<int64_t>(
      1, static_cast<int64_t>(static_cast<double>(kTargetFileBytes) /
                              cal::kBprPartitions));
  if (opts.buckets > 0 && opts.buckets < static_cast<int32_t>(plan.size())) {
    plan.resize(opts.buckets);
    std::fprintf(stdout, "smoke: limiting to first %d bucket(s)\n", opts.buckets);
  }

  std::shared_ptr<arrow::Schema> primes_arrow =
      IcebergToArrowSchemaWithFieldIds(*PrimesSchema(), &error);
  if (!primes_arrow) {
    std::fprintf(stderr, "build primes arrow schema: %s\n", error.c_str());
    return 1;
  }
  std::shared_ptr<arrow::Schema> partitions_arrow =
      IcebergToArrowSchemaWithFieldIds(*PartitionsSchema(), &error);
  if (!partitions_arrow) {
    std::fprintf(stderr, "build partitions arrow schema: %s\n", error.c_str());
    return 1;
  }

  // Emit boundary rows up front (consumer-friendly).
  for (const auto& b : boundaries) {
    emit_boundary(manifest, opts.p_bucket_version, b.p_bucket,
                  b.p_min, b.rank_min);
  }

  int64_t plan_primes_rows = 0;
  int64_t plan_parts_rows = 0;
  for (const auto& bp : plan) {
    plan_primes_rows += bp.primes_rows;
    plan_parts_rows += bp.partitions_rows;
  }

  Progress progress;
  progress.Start();

  // -------- Phase 1: primes --------
  progress.BeginStage(1, 2, "primes", plan_primes_rows);
  auto t_phase1 = std::chrono::steady_clock::now();
  std::vector<BucketJob> primes_jobs;
  primes_jobs.reserve(plan.size());
  for (const auto& bp : plan) {
    BucketJob j{};
    j.table = BucketJob::Primes;
    j.p_bucket_version = opts.p_bucket_version;
    j.p_bucket = bp.p_bucket;
    j.p_min = bp.p_min;
    j.p_max_excl = bp.p_max_excl;
    j.rank_min = bp.rank_min;
    j.sources = SourcesForBucket(primes_files, bp.p_min, bp.p_max_excl);
    j.output_dir = BucketDataDir(opts.staging_warehouse,
                                 opts.staging_primes_table,
                                 opts.p_bucket_version, bp.p_bucket);
    j.target_rows_per_file = rows_per_primes_file;
    j.iceberg_schema = PrimesSchema();
    j.arrow_schema = primes_arrow;
    j.progress = &progress;
    primes_jobs.push_back(std::move(j));
  }

  std::vector<BucketJobResult> primes_results;
  if (!RunPhase(std::move(primes_jobs), kWorkersPerPhase,
                &primes_results, &error)) {
    std::fprintf(stderr, "primes phase: %s\n", error.c_str());
    return 1;
  }
  int64_t primes_files_total = 0;
  int64_t primes_rows_total = 0;
  for (const auto& r : primes_results) {
    for (const auto& f : r.files) {
      emit_file_row(manifest, f);
      primes_files_total++;
      primes_rows_total += f.rows;
    }
  }
  manifest.flush();
  auto t_phase1_end = std::chrono::steady_clock::now();
  std::fprintf(stdout, "primes phase: %lld rows in %lld files (%.1fs)\n",
               static_cast<long long>(primes_rows_total),
               static_cast<long long>(primes_files_total),
               std::chrono::duration<double>(t_phase1_end - t_phase1).count());
  std::fflush(stdout);

  // -------- Phase 2: partitions --------
  progress.BeginStage(2, 2, "partitions", plan_parts_rows);
  auto t_phase2 = std::chrono::steady_clock::now();
  std::vector<BucketJob> parts_jobs;
  parts_jobs.reserve(plan.size());
  for (const auto& bp : plan) {
    BucketJob j{};
    j.table = BucketJob::Partitions;
    j.p_bucket_version = opts.p_bucket_version;
    j.p_bucket = bp.p_bucket;
    j.p_min = bp.p_min;
    j.p_max_excl = bp.p_max_excl;
    j.rank_min = bp.rank_min;
    j.sources = SourcesForBucket(parts_files, bp.p_min, bp.p_max_excl);
    j.output_dir = BucketDataDir(opts.staging_warehouse,
                                 opts.staging_partitions_table,
                                 opts.p_bucket_version, bp.p_bucket);
    j.target_rows_per_file = rows_per_partitions_file;
    j.iceberg_schema = PartitionsSchema();
    j.arrow_schema = partitions_arrow;
    j.progress = &progress;
    parts_jobs.push_back(std::move(j));
  }

  std::vector<BucketJobResult> parts_results;
  if (!RunPhase(std::move(parts_jobs), kWorkersPerPhase,
                &parts_results, &error)) {
    std::fprintf(stderr, "partitions phase: %s\n", error.c_str());
    return 1;
  }
  int64_t parts_files_total = 0;
  int64_t parts_rows_total = 0;
  for (const auto& r : parts_results) {
    for (const auto& f : r.files) {
      emit_file_row(manifest, f);
      parts_files_total++;
      parts_rows_total += f.rows;
    }
  }
  manifest.flush();
  manifest.close();
  auto t_phase2_end = std::chrono::steady_clock::now();
  std::fprintf(stdout, "partitions phase: %lld rows in %lld files (%.1fs)\n",
               static_cast<long long>(parts_rows_total),
               static_cast<long long>(parts_files_total),
               std::chrono::duration<double>(t_phase2_end - t_phase2).count());
  progress.Stop();
  std::fprintf(stdout,
               "rewrite done: %lld primes + %lld partitions (prime_rank null; backfill next)\n",
               static_cast<long long>(primes_rows_total),
               static_cast<long long>(parts_rows_total));
  std::fflush(stdout);
  return 0;
}
