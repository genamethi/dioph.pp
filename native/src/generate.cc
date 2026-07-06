// Frontier writer. Generates primes via the C core (FLINT/primesieve),
// materializes (p, k) plus decompositions, assigns prime_rank in stream,
// stamps the coordinator-supplied (p_bucket_version, p_bucket) onto every
// row, and writes Parquet files via primeparts/writer.h. Emits the same
// JSONL manifest format the rewriter produces, so the commit step
// doesn't care whether bytes came from FLINT or from a rewrite pass.

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/core.h"
#include "primeparts/generate.h"
#include "primeparts/schemas.h"
#include "primeparts/writer.h"

#include <arrow/api.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <deque>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// iceberg::Schema is still the source of truth for field IDs; the
// schema factories live in writer.h and the parquet write path lives
// in writer.cc. This file only handles row materialization and
// per-batch arrow array construction.
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/type.h"

namespace fs = std::filesystem;

using primeparts::BucketParquetWriter;
using primeparts::NextFileSeq;
using primeparts::BucketPartitionSpec;
using primeparts::PartitionsSchema;
using primeparts::PrimesSchema;
using primeparts::WriterConfig;
using primeparts::WrittenFile;

namespace {

constexpr int64_t kDefaultChunkPrimes = 500000;

thread_local std::string g_last_error;

void set_last_error(std::string msg) { g_last_error = std::move(msg); }

void log_line(const pp_gen_callbacks* callbacks, const char* fmt, ...) {
  if (!callbacks || !callbacks->on_log) return;
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  callbacks->on_log(callbacks->user_data, buf);
}

struct Options {
  int64_t start_idx = 0;        // 1-indexed prime rank of first prime to generate
  int64_t count = -1;
  int64_t chunk_primes = kDefaultChunkPrimes;
  int64_t threads = 0;
  // Bucket assignment for every parquet file this invocation writes.
  // The coordinator (planner / rewriter handoff) picks these per
  // segment; the writer stamps them onto every row.
  int32_t bucket_version = 1;
  int32_t bucket = 0;
  // prime_rank to stamp on the first prime row. Per spec, the new
  // staging schema uses 0-indexed rank with rank 0 = p=3 (p=2 is
  // intentionally absent). The coordinator passes this explicitly via
  // PRIMEPARTS_PRIME_RANK_START. The default fallback to start_idx is
  // a stand-alone-run convenience that does NOT match the spec
  // convention; production callers must set the env var.
  int64_t prime_rank_start = 0;
  // If true, this invocation opens a brand-new bucket; the writer emits
  // a boundary JSONL row so the commit step extends funbuns.boundaries
  // atomically with the data file appends.
  bool bucket_is_new = false;
  bool temp = false;
  fs::path warehouse;
  fs::path manifest;
  // Catalog target for the end-of-run commit. Non-empty => commit through a
  // pp-catalogd RestCatalog client at this base URI (e.g. http://127.0.0.1:8181,
  // no /v1 suffix — the client appends the IRC routes); empty => commit
  // in-process via MakeLocalCatalog. Both funnel through CommitFiles, so the
  // snapshot path is identical either way.
  std::string rest_uri;
};

struct BatchHolder {
  pp_batch_result batch;

  BatchHolder() { pp_batch_result_init(&batch); }
  ~BatchHolder() { pp_batch_result_clear(&batch); }

  BatchHolder(const BatchHolder&) = delete;
  BatchHolder& operator=(const BatchHolder&) = delete;

  BatchHolder(BatchHolder&& other) noexcept : batch(other.batch) {
    pp_batch_result_init(&other.batch);
  }

  BatchHolder& operator=(BatchHolder&& other) noexcept {
    if (this != &other) {
      pp_batch_result_clear(&batch);
      batch = other.batch;
      pp_batch_result_init(&other.batch);
    }
    return *this;
  }
};

struct FileGroup {
  std::vector<BatchHolder> batches;
  int64_t prime_rows = 0;
  int64_t partitions_rows = 0;
  int64_t first_p = 0;
  int64_t last_p = 0;
  int64_t start_idx = 0;
  int64_t processed_count = 0;
};

// Single-line stderr progress bar. Uses \r so it overwrites itself;
// emits nothing when stderr isn't a TTY so pipes/CI logs stay clean.
class Progress {
 public:
  Progress(int64_t total_groups, int64_t total_primes)
      : total_groups_(total_groups),
        total_primes_(total_primes),
        enabled_(::isatty(STDERR_FILENO) != 0),
        start_(std::chrono::steady_clock::now()) {}

  void update(int64_t groups_done, int64_t primes_done) {
    if (!enabled_) return;
    double elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - start_)
                         .count();
    double rate = elapsed > 0.0 ? static_cast<double>(primes_done) / elapsed : 0.0;
    int64_t remaining = total_primes_ - primes_done;
    int eta_s = rate > 0.0 ? static_cast<int>(remaining / rate) : 0;
    int h = eta_s / 3600, m = (eta_s % 3600) / 60, s = eta_s % 60;

    char rate_buf[32];
    if (rate >= 1.0e6) std::snprintf(rate_buf, sizeof(rate_buf), "%5.2fM/s", rate / 1.0e6);
    else if (rate >= 1.0e3) std::snprintf(rate_buf, sizeof(rate_buf), "%5.1fk/s", rate / 1.0e3);
    else std::snprintf(rate_buf, sizeof(rate_buf), "%5.0f/s", rate);

    int width = 30;
    int filled = total_primes_ > 0
                     ? static_cast<int>(static_cast<double>(primes_done) * width /
                                        static_cast<double>(total_primes_))
                     : 0;
    if (filled > width) filled = width;
    char bar[64];
    for (int i = 0; i < width; ++i) bar[i] = i < filled ? '#' : '-';
    bar[width] = '\0';
    double pct = total_primes_ > 0
                     ? 100.0 * static_cast<double>(primes_done) /
                           static_cast<double>(total_primes_)
                     : 0.0;
    std::fprintf(stderr,
                 "\rgenerate [%s] %5.1f%% | %lld/%lld groups | %s | ETA %02d:%02d:%02d ",
                 bar, pct, static_cast<long long>(groups_done),
                 static_cast<long long>(total_groups_), rate_buf, h, m, s);
    std::fflush(stderr);
  }

  void finish() {
    if (!enabled_) return;
    std::fputc('\n', stderr);
    std::fflush(stderr);
  }

 private:
  int64_t total_groups_;
  int64_t total_primes_;
  bool enabled_;
  std::chrono::steady_clock::time_point start_;
};

class StopMonitor {
 public:
  StopMonitor() : enabled_(::isatty(STDIN_FILENO) != 0) {
    if (!enabled_) return;
    if (tcgetattr(STDIN_FILENO, &original_) != 0) { enabled_ = false; return; }
    termios raw = original_;
    raw.c_lflag &= ~(static_cast<tcflag_t>(ICANON) | static_cast<tcflag_t>(ECHO));
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) { enabled_ = false; return; }
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this]() { run(); });
  }

  ~StopMonitor() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    if (enabled_) tcsetattr(STDIN_FILENO, TCSANOW, &original_);
  }

  StopMonitor(const StopMonitor&) = delete;
  StopMonitor& operator=(const StopMonitor&) = delete;

  bool stop_requested() const {
    return stop_requested_.load(std::memory_order_acquire);
  }

 private:
  void run() {
    while (running_.load(std::memory_order_acquire) &&
           !stop_requested_.load(std::memory_order_acquire)) {
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(STDIN_FILENO, &read_fds);
      timeval timeout{0, 100000};
      int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &timeout);
      if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) continue;
      char ch = '\0';
      ssize_t n = read(STDIN_FILENO, &ch, 1);
      if (n != 1) continue;
      if (ch == 'q' || ch == 'Q' || ch == 'c' || ch == 'C') {
        stop_requested_.store(true, std::memory_order_release);
        std::fprintf(stderr, "\nstop requested; finishing current file group...\n");
        std::fflush(stderr);
        return;
      }
    }
  }

  bool enabled_;
  termios original_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread worker_;
};

void usage(FILE* stream) {
  std::fprintf(
      stream,
      "usage: primeparts-generate --start-idx N --count N [options]\n"
      "\n"
      "Native frontier writer for funbuns.{primes,partitions} under the\n"
      "new bucket-partitioned layout. Writes one parquet file per\n"
      "table per file-group under a fixed (bucket_version, bucket)\n"
      "assignment; emits a JSONL manifest the commit step consumes.\n"
      "\n"
      "Options:\n"
      "  --temp                    Create $FUNBUNS_DATA_DIR/tmp/iceberg_temp_<ts>/warehouse\n"
      "                            (FUNBUNS_DATA_DIR default: /media/extssd/research/dioph.pp/data)\n"
      "  --rest-uri URL            Commit through a pp-catalogd RestCatalog\n"
      "                            client at URL, a bare base (e.g.\n"
      "                            http://127.0.0.1:8181, no /v1 suffix);\n"
      "                            default commits in-process\n"
      "  --warehouse PATH          Warehouse root to write under\n"
      "  --manifest PATH           JSONL file list to write\n"
      "  --chunk-primes N          Materialization chunk size (default: 500000)\n"
      "  --threads N               Materialization threads (default: hw)\n"
      "  --help\n"
      "\n"
      "Environment (coordinator-owned):\n"
      "  PRIMEPARTS_BUCKET_VERSION  default: 1\n"
      "  PRIMEPARTS_BUCKET          default: 0\n"
      "  PRIMEPARTS_PRIME_RANK_START prime_rank to stamp on the first prime\n"
      "                              row (spec convention: rank 0 = p=3,\n"
      "                              p=2 absent). Default: --start-idx (the\n"
      "                              FLINT 1-indexed rank), which only matches\n"
      "                              spec when --start-idx is set so that the\n"
      "                              first emitted prime is at the desired\n"
      "                              spec-rank.\n"
      "  PRIMEPARTS_BUCKET_IS_NEW   if set/1, emit a boundary row in the manifest\n");
}

bool parse_i64(const char* text, int64_t* out) {
  char* end = nullptr;
  errno = 0;
  long long value = std::strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') return false;
  *out = static_cast<int64_t>(value);
  return true;
}

bool parse_i32(const char* text, int32_t* out) {
  int64_t value = 0;
  if (!parse_i64(text, &value) || value < INT32_MIN || value > INT32_MAX) return false;
  *out = static_cast<int32_t>(value);
  return true;
}

bool resolve_bucket_state(Options* options) {
  const char* rank_env = std::getenv("PRIMEPARTS_PRIME_RANK_START");
  if (rank_env != nullptr && rank_env[0] != '\0') {
    if (!parse_i64(rank_env, &options->prime_rank_start) ||
        options->prime_rank_start <= 0) {
      std::fprintf(stderr, "invalid PRIMEPARTS_PRIME_RANK_START: %s\n", rank_env);
      return false;
    }
  } else {
    options->prime_rank_start = options->start_idx;
  }

  const char* ver_env = std::getenv("PRIMEPARTS_BUCKET_VERSION");
  if (ver_env != nullptr && ver_env[0] != '\0') {
    if (!parse_i32(ver_env, &options->bucket_version) ||
        options->bucket_version <= 0) {
      std::fprintf(stderr, "invalid PRIMEPARTS_BUCKET_VERSION: %s\n", ver_env);
      return false;
    }
  }

  const char* bkt_env = std::getenv("PRIMEPARTS_BUCKET");
  if (bkt_env != nullptr && bkt_env[0] != '\0') {
    if (!parse_i32(bkt_env, &options->bucket) || options->bucket < 0) {
      std::fprintf(stderr, "invalid PRIMEPARTS_BUCKET: %s\n", bkt_env);
      return false;
    }
  }

  const char* new_env = std::getenv("PRIMEPARTS_BUCKET_IS_NEW");
  options->bucket_is_new =
      new_env != nullptr && new_env[0] != '\0' && new_env[0] != '0';
  return true;
}

std::string utc_timestamp_compact() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm);
  return buffer;
}

fs::path default_temp_root() {
  const char* env = std::getenv("FUNBUNS_DATA_DIR");
  fs::path data_dir = env != nullptr && env[0] != '\0'
                          ? fs::path(env)
                          : fs::path("/media/extssd/research/dioph.pp/data");
  return data_dir / "tmp" / ("iceberg_temp_" + utc_timestamp_compact());
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += ch;
    }
  }
  return out;
}

std::shared_ptr<arrow::Array> int64_array(const int64_t* values, int64_t length) {
  arrow::Int64Builder builder;
  if (length > 0) {
    if (!builder.AppendValues(values, length).ok()) return nullptr;
  }
  std::shared_ptr<arrow::Array> out;
  if (!builder.Finish(&out).ok()) return nullptr;
  return out;
}

std::shared_ptr<arrow::Array> int32_array(const int32_t* values, int64_t length) {
  arrow::Int32Builder builder;
  if (length > 0) {
    if (!builder.AppendValues(values, length).ok()) return nullptr;
  }
  std::shared_ptr<arrow::Array> out;
  if (!builder.Finish(&out).ok()) return nullptr;
  return out;
}

std::shared_ptr<arrow::Array> const_int32_array(int32_t value, int64_t length) {
  std::vector<int32_t> values(static_cast<size_t>(length), value);
  return int32_array(values.data(), length);
}

std::shared_ptr<arrow::Array> dense_int64_range(int64_t start, int64_t length) {
  std::vector<int64_t> values(static_cast<size_t>(length));
  for (int64_t i = 0; i < length; ++i) {
    values[static_cast<size_t>(i)] = start + i;
  }
  return int64_array(values.data(), length);
}

// prime_k[i] is the number of partition rows produced by the C core
// for prime_p[i]. The decomp_* arrays are filled in prime order with
// k_i contiguous rows per prime. So the rank column is just each prime's
// rank repeated k_i times — no scan, no comparison, no edge cases.
std::shared_ptr<arrow::Array> partitions_rank_array(const pp_batch_result& batch,
                                                    int64_t rank_start_for_batch) {
  std::vector<int64_t> ranks;
  ranks.reserve(batch.decomp_count);
  for (size_t i = 0; i < batch.prime_count; ++i) {
    int64_t rank_i = rank_start_for_batch + static_cast<int64_t>(i);
    int32_t kk = batch.prime_k[i];
    for (int32_t j = 0; j < kk; ++j) {
      ranks.push_back(rank_i);
    }
  }
  return int64_array(ranks.data(), static_cast<int64_t>(batch.decomp_count));
}

std::shared_ptr<arrow::RecordBatch> make_primes_batch(const pp_batch_result& batch,
                                                      int64_t rank_start_for_batch,
                                                      int32_t bucket_version,
                                                      int32_t bucket) {
  // Column order matches PrimesSchema(): p, k, prime_rank,
  // p_bucket_version, p_bucket.
  auto schema = arrow::schema({
      arrow::field("p",                arrow::int64()),
      arrow::field("k",                arrow::int32()),
      arrow::field("prime_rank",       arrow::int64()),
      arrow::field("p_bucket_version", arrow::int32()),
      arrow::field("p_bucket",         arrow::int32()),
  });
  int64_t rows = static_cast<int64_t>(batch.prime_count);
  return arrow::RecordBatch::Make(
      schema, rows,
      {int64_array(batch.prime_p, rows),
       int32_array(batch.prime_k, rows),
       dense_int64_range(rank_start_for_batch, rows),
       const_int32_array(bucket_version, rows),
       const_int32_array(bucket, rows)});
}

std::shared_ptr<arrow::RecordBatch> make_partitions_batch(const pp_batch_result& batch,
                                                          int64_t rank_start_for_batch,
                                                          int32_t bucket_version,
                                                          int32_t bucket) {
  // Column order matches PartitionsSchema(): p, m_k, n_k, q_k,
  // prime_rank, p_bucket_version, p_bucket.
  auto schema = arrow::schema({
      arrow::field("p",                arrow::int64()),
      arrow::field("m_k",              arrow::int32()),
      arrow::field("n_k",              arrow::int32()),
      arrow::field("q_k",              arrow::int64()),
      arrow::field("prime_rank",       arrow::int64()),
      arrow::field("p_bucket_version", arrow::int32()),
      arrow::field("p_bucket",         arrow::int32()),
  });
  int64_t rows = static_cast<int64_t>(batch.decomp_count);
  return arrow::RecordBatch::Make(
      schema, rows,
      {int64_array(batch.decomp_p, rows),
       int32_array(batch.decomp_m, rows),
       int32_array(batch.decomp_n, rows),
       int64_array(batch.decomp_q, rows),
       partitions_rank_array(batch, rank_start_for_batch),
       const_int32_array(bucket_version, rows),
       const_int32_array(bucket, rows)});
}

// One file per group per table: open a BucketParquetWriter with
// target_rows_per_file=0 (no rolling), push every batch in the group at
// it, close, return the written files.
bool write_group_table(const fs::path& output_dir, const std::string& table,
                       const std::shared_ptr<iceberg::Schema>& schema,
                       const std::vector<BatchHolder>& batches,
                       const std::vector<int64_t>& rank_starts,
                       int32_t bucket_version, int32_t bucket,
                       int32_t starting_file_seq, bool partitions,
                       std::vector<WrittenFile>* out_files,
                       std::string* error) {
  WriterConfig cfg;
  // Staging dir (outside the warehouse table tree); CommitFiles moves these into
  // the catalog-chosen location at end-of-run. See StagingDataDir.
  cfg.output_dir = output_dir;
  cfg.schema = schema;
  cfg.table_name = table;
  cfg.filename_prefix = table;
  cfg.delta_columns = {"p", "prime_rank", "q_k"};
  cfg.bucket_version = bucket_version;
  cfg.bucket = bucket;
  cfg.starting_file_seq = starting_file_seq;
  cfg.target_rows_per_file = 0;

  auto writer = BucketParquetWriter::Make(cfg, error);
  if (!writer) return false;

  for (size_t i = 0; i < batches.size(); ++i) {
    const auto& holder = batches[i];
    int64_t rank_start = rank_starts[i];
    std::shared_ptr<arrow::RecordBatch> batch;
    if (partitions) {
      if (holder.batch.decomp_count == 0) continue;
      batch = make_partitions_batch(holder.batch, rank_start, bucket_version, bucket);
    } else {
      if (holder.batch.prime_count == 0) continue;
      batch = make_primes_batch(holder.batch, rank_start, bucket_version, bucket);
    }
    if (!batch) {
      *error = "failed to build record batch";
      return false;
    }
    BucketParquetWriter::BatchStats st{};
    if (partitions) {
      st.p_min = holder.batch.decomp_p[0];
      st.p_max = holder.batch.decomp_p[holder.batch.decomp_count - 1];
    } else {
      st.p_min = holder.batch.prime_p[0];
      st.p_max = holder.batch.prime_p[holder.batch.prime_count - 1];
    }
    st.rank_min = rank_start;
    st.rank_max = rank_start +
                  static_cast<int64_t>(holder.batch.prime_count) - 1;
    if (!writer->Write(*batch, st, error)) return false;
  }

  return writer->Close(out_files, error);
}

bool materialize_group(int64_t* next_idx, int64_t end_idx, const Options& options,
                       FileGroup* group, std::string* error) {
  group->start_idx = *next_idx;
  int64_t remaining_total = end_idx - *next_idx;
  int64_t chunk_count =
      (remaining_total + options.chunk_primes - 1) / options.chunk_primes;
  int64_t max_group_chunks = options.threads > 0 ? options.threads : 1;
  if (chunk_count > max_group_chunks) chunk_count = max_group_chunks;

  std::vector<int64_t> starts(static_cast<size_t>(chunk_count));
  std::vector<int64_t> counts(static_cast<size_t>(chunk_count));
  for (int64_t i = 0; i < chunk_count; ++i) {
    starts[static_cast<size_t>(i)] = *next_idx + i * options.chunk_primes;
    int64_t remaining = end_idx - starts[static_cast<size_t>(i)];
    counts[static_cast<size_t>(i)] =
        remaining < options.chunk_primes ? remaining : options.chunk_primes;
  }

  group->batches.resize(static_cast<size_t>(chunk_count));
  int64_t worker_count = options.threads;
  if (worker_count <= 0) worker_count = 1;
  if (worker_count > chunk_count) worker_count = chunk_count;

  std::mutex lock;
  int64_t next_chunk = 0;
  bool failed = false;
  std::string first_error;
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(worker_count));
  for (int64_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&]() {
      for (;;) {
        int64_t chunk_id;
        {
          std::lock_guard<std::mutex> guard(lock);
          if (failed || next_chunk >= chunk_count) return;
          chunk_id = next_chunk++;
        }
        auto& holder = group->batches[static_cast<size_t>(chunk_id)];
        int status = pp_process_rank_batch(starts[static_cast<size_t>(chunk_id)],
                                           counts[static_cast<size_t>(chunk_id)],
                                           &holder.batch);
        if (status != PP_OK ||
            holder.batch.processed_count != counts[static_cast<size_t>(chunk_id)]) {
          std::lock_guard<std::mutex> guard(lock);
          failed = true;
          if (first_error.empty()) {
            first_error =
                status != PP_OK ? pp_status_message(status) : "short native batch without interrupt";
          }
          return;
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();
  if (failed) { *error = first_error; return false; }

  for (const auto& holder : group->batches) {
    if (group->first_p == 0 || holder.batch.first_p < group->first_p) {
      group->first_p = holder.batch.first_p;
    }
    if (holder.batch.last_p > group->last_p) {
      group->last_p = holder.batch.last_p;
    }
    group->prime_rows += static_cast<int64_t>(holder.batch.prime_count);
    group->partitions_rows += static_cast<int64_t>(holder.batch.decomp_count);
    group->processed_count += holder.batch.processed_count;
  }

  *next_idx += group->processed_count;
  return true;
}

void append_manifest_file(std::ofstream& out, const WrittenFile& f) {
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

void append_manifest_boundary(std::ofstream& out,
                              int32_t bucket_version, int32_t bucket,
                              int64_t p_min, int64_t rank_min) {
  out << "{\"boundary\":true,"
      << "\"p_bucket_version\":" << bucket_version << ","
      << "\"p_bucket\":" << bucket << ","
      << "\"p_min\":" << p_min << ","
      << "\"rank_min\":" << rank_min << "}\n";
}

bool parse_args(int argc, char** argv, Options* options) {
  static const option long_options[] = {
      {"start-idx", required_argument, nullptr, 1000},
      {"count", required_argument, nullptr, 'n'},
      {"chunk-primes", required_argument, nullptr, 'c'},
      {"threads", required_argument, nullptr, 1003},
      {"warehouse", required_argument, nullptr, 'w'},
      {"manifest", required_argument, nullptr, 'm'},
      {"temp", no_argument, nullptr, 1004},
      {"rest-uri", required_argument, nullptr, 1005},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  int opt;
  while ((opt = getopt_long(argc, argv, "n:c:w:m:h", long_options, nullptr)) != -1) {
    switch (opt) {
      case 1000:
        if (!parse_i64(optarg, &options->start_idx)) {
          std::fprintf(stderr, "invalid --start-idx: %s\n", optarg);
          return false;
        }
        break;
      case 'n':
        if (!parse_i64(optarg, &options->count)) {
          std::fprintf(stderr, "invalid --count: %s\n", optarg);
          return false;
        }
        break;
      case 'c':
        if (!parse_i64(optarg, &options->chunk_primes)) {
          std::fprintf(stderr, "invalid --chunk-primes: %s\n", optarg);
          return false;
        }
        break;
      case 1003:
        if (!parse_i64(optarg, &options->threads)) {
          std::fprintf(stderr, "invalid --threads: %s\n", optarg);
          return false;
        }
        break;
      case 'w': options->warehouse = optarg; break;
      case 'm': options->manifest = optarg; break;
      case 1004: options->temp = true; break;
      case 1005: options->rest_uri = optarg; break;
      case 'h': usage(stdout); std::exit(0);
      default: return false;
    }
  }
  if (options->start_idx <= 0 || options->count < 0 || options->chunk_primes <= 0 ||
      options->threads < 0) {
    usage(stderr);
    return false;
  }
  if (options->warehouse.empty()) {
    if (!options->temp) {
      std::fprintf(stderr, "either --warehouse or --temp is required\n");
      return false;
    }
    fs::path temp_root = default_temp_root();
    options->warehouse = temp_root / "warehouse";
    if (options->manifest.empty()) {
      options->manifest = temp_root / "files.jsonl";
    }
  } else if (options->manifest.empty()) {
    options->manifest = options->warehouse / "files.jsonl";
  }
  if (options->threads == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    options->threads = hw == 0 ? 1 : static_cast<int64_t>(hw);
  }
  return resolve_bucket_state(options);
}

}  // namespace

int run_generation(const Options& options, const pp_gen_callbacks* callbacks, pp_gen_result* out) {
  std::string error;
  if (out) {
    std::memset(out, 0, sizeof(*out));
    out->start_idx = options.start_idx;
    out->count = options.count;
  }
  int init_status = pp_init();
  if (init_status != PP_OK) {
    set_last_error(std::string("pp_init failed: ") + pp_status_message(init_status));
    log_line(callbacks, "%s", g_last_error.c_str());
    return 1;
  }

  auto run_start = std::chrono::steady_clock::now();
  try {
    fs::create_directories(options.warehouse);
    fs::create_directories(options.manifest.parent_path());
    std::ofstream manifest(options.manifest, std::ios::out | std::ios::trunc);
    if (!manifest) {
      set_last_error("failed to open manifest: " + options.manifest.string());
      log_line(callbacks, "%s", g_last_error.c_str());
      pp_shutdown();
      return 1;
    }

    int64_t next_idx = options.start_idx;
    int64_t end_idx = options.start_idx + options.count;
    int64_t prime_rank_cursor = options.prime_rank_start;
    // Writers emit to staging dirs outside the warehouse; CommitFiles moves the
    // files into the catalog-chosen location at end-of-run (the catalog seam),
    // preserving the path relative to the staging root. Keep the bucket partition
    // sub-path so files land at <table>/data/p_bucket_version=N/p_bucket=M/ as
    // before — the client owns its own partition layout, the catalog owns the
    // table location.
    const fs::path part_sub =
        fs::path("p_bucket_version=" + std::to_string(options.bucket_version)) /
        ("p_bucket=" + std::to_string(options.bucket));
    const fs::path primes_staging =
        primeparts::catalog::StagingDataDir(options.warehouse, "primes") / part_sub;
    const fs::path partitions_staging =
        primeparts::catalog::StagingDataDir(options.warehouse, "partitions") / part_sub;
    int32_t primes_file_seq = NextFileSeq(primes_staging, "primes");
    int32_t partitions_file_seq = NextFileSeq(partitions_staging, "partitions");
    int64_t total_primes = 0;
    int64_t total_partitions = 0;
    int64_t files_written = 0;
    int64_t bytes_written = 0;
    int64_t first_p = 0;
    int64_t last_p = 0;
    bool stop_requested = false;
    bool boundary_pending = options.bucket_is_new;

    auto p_schema = PrimesSchema();
    auto d_schema = PartitionsSchema();

    // DataFiles accumulated across all groups for the end-of-run catalog
    // commit. Each WrittenFile already carries a built iceberg::DataFile;
    // we FastAppend them in one snapshot per table after the run.
    std::vector<std::shared_ptr<iceberg::DataFile>> primes_data_files;
    std::vector<std::shared_ptr<iceberg::DataFile>> partitions_data_files;

    int64_t total_chunks =
        (options.count + options.chunk_primes - 1) / options.chunk_primes;
    int64_t group_width = options.threads > 0 ? options.threads : 1;
    int64_t total_groups =
        (total_chunks + group_width - 1) / group_width;
    Progress progress(total_groups, options.count);
    StopMonitor stop_monitor;
    int64_t groups_done = 0;
    progress.update(0, 0);

    // Pipelined write path. The compute pool (materialize_group) and the
    // Parquet encode/write run concurrently so the pool never stalls on I/O.
    // The main thread is the producer: it materializes a group, derives its
    // rank starts / file seqs / boundary, and hands the filled buffer to a
    // single writer thread that encodes both tables, appends the manifest, and
    // accumulates the committed DataFiles. A depth-1 job queue bounds resident
    // memory to at most 3 groups (writing + queued + materializing);
    // backpressure blocks the producer rather than letting groups pile up.
    struct WriteJob {
      FileGroup group;
      std::vector<int64_t> rank_starts;
      int64_t group_rank_min = 0;
      bool emit_boundary = false;
      int64_t boundary_p_min = 0;
      int32_t primes_seq = 0;
      int32_t partitions_seq = 0;
      bool has_partitions = false;
    };

    std::mutex q_mu;
    std::condition_variable q_can_push;
    std::condition_variable q_can_pop;
    std::deque<WriteJob> jobs;
    const size_t kMaxPending = 1;  // one job may wait while another is written
    bool producer_done = false;
    bool writer_failed = false;
    std::string writer_error;

    // The manifest, file-seq counters, byte/file tallies and DataFile vectors
    // are touched ONLY by this writer thread during the run; the producer reads
    // them again after join(). No locking needed on them beyond the queue.
    std::thread writer_thread([&]() {
      int64_t w_primes = 0;
      for (;;) {
        WriteJob job;
        {
          std::unique_lock<std::mutex> lk(q_mu);
          q_can_pop.wait(lk, [&]() { return !jobs.empty() || producer_done; });
          if (jobs.empty()) return;  // producer_done and drained
          job = std::move(jobs.front());
          jobs.pop_front();
          q_can_push.notify_one();
        }

        std::string werr;
        if (job.emit_boundary) {
          append_manifest_boundary(manifest, options.bucket_version,
                                   options.bucket, job.boundary_p_min,
                                   job.group_rank_min);
        }

        std::vector<WrittenFile> primes_files;
        if (!write_group_table(primes_staging, "primes", p_schema,
                               job.group.batches, job.rank_starts,
                               options.bucket_version, options.bucket,
                               job.primes_seq, /*partitions=*/false,
                               &primes_files, &werr)) {
          std::lock_guard<std::mutex> guard(q_mu);
          writer_failed = true;
          writer_error = "write primes failed: " + werr;
          q_can_push.notify_one();
          return;
        }
        for (const auto& wf : primes_files) {
          append_manifest_file(manifest, wf);
          files_written++;
          bytes_written += wf.bytes;
          if (wf.data_file) primes_data_files.push_back(wf.data_file);
        }

        if (job.has_partitions) {
          std::vector<WrittenFile> partitions_files;
          if (!write_group_table(partitions_staging, "partitions", d_schema,
                                 job.group.batches, job.rank_starts,
                                 options.bucket_version, options.bucket,
                                 job.partitions_seq, /*partitions=*/true,
                                 &partitions_files, &werr)) {
            std::lock_guard<std::mutex> guard(q_mu);
            writer_failed = true;
            writer_error = "write partitions failed: " + werr;
            q_can_push.notify_one();
            return;
          }
          for (const auto& wf : partitions_files) {
            append_manifest_file(manifest, wf);
            files_written++;
            bytes_written += wf.bytes;
            if (wf.data_file) partitions_data_files.push_back(wf.data_file);
          }
        }

        w_primes += job.group.prime_rows;
        log_line(callbacks,
                 "group written | primes=%" PRId64 " | files=%" PRId64,
                 w_primes, files_written);

        manifest.flush();
        if (!manifest) {
          std::lock_guard<std::mutex> guard(q_mu);
          writer_failed = true;
          writer_error =
              "failed to flush manifest: " + options.manifest.string();
          q_can_push.notify_one();
          return;
        }
      }
    });

    // Ensure the writer thread is always signalled and joined, even if the
    // producer path throws before its explicit join below.
    struct WriterGuard {
      std::thread& t;
      std::mutex& m;
      std::condition_variable& cv;
      bool& done;
      ~WriterGuard() {
        {
          std::lock_guard<std::mutex> g(m);
          done = true;
          cv.notify_one();
        }
        if (t.joinable()) t.join();
      }
    } writer_guard{writer_thread, q_mu, q_can_pop, producer_done};

    bool producer_error = false;
    while (next_idx < end_idx) {
      FileGroup group;
      if (!materialize_group(&next_idx, end_idx, options, &group, &error)) {
        set_last_error("materialize failed: " + error);
        producer_error = true;
        break;
      }

      if (first_p == 0 || group.first_p < first_p) first_p = group.first_p;
      if (group.last_p > last_p) last_p = group.last_p;

      WriteJob job;
      // Per-batch rank starts: rank of batches[0].prime_p[0] is the cursor;
      // batch i starts at cursor + sum_prime_count(0..i-1).
      job.rank_starts.assign(group.batches.size(), 0);
      int64_t acc = 0;
      for (size_t i = 0; i < group.batches.size(); ++i) {
        job.rank_starts[i] = prime_rank_cursor + acc;
        acc += static_cast<int64_t>(group.batches[i].batch.prime_count);
      }
      job.group_rank_min = prime_rank_cursor;

      if (boundary_pending && group.prime_rows > 0) {
        job.emit_boundary = true;
        job.boundary_p_min = group.first_p;
        boundary_pending = false;
      }

      // One file per table per group (target_rows_per_file == 0), so seqs are
      // assigned deterministically here without waiting on the write.
      job.primes_seq = primes_file_seq;
      primes_file_seq += 1;
      if (group.partitions_rows > 0) {
        job.has_partitions = true;
        job.partitions_seq = partitions_file_seq;
        partitions_file_seq += 1;
      }

      total_primes += group.prime_rows;
      total_partitions += group.partitions_rows;
      prime_rank_cursor += group.prime_rows;
      groups_done++;
      progress.update(groups_done, total_primes);

      job.group = std::move(group);
      {
        std::unique_lock<std::mutex> lk(q_mu);
        q_can_push.wait(
            lk, [&]() { return jobs.size() < kMaxPending || writer_failed; });
        if (writer_failed) break;
        jobs.push_back(std::move(job));
        q_can_pop.notify_one();
      }

      if (stop_monitor.stop_requested()) {
        stop_requested = true;
        break;
      }
    }

    {
      std::lock_guard<std::mutex> guard(q_mu);
      producer_done = true;
      q_can_pop.notify_one();
    }
    writer_thread.join();

    if (producer_error || writer_failed) {
      if (writer_failed && !producer_error) set_last_error(writer_error);
      log_line(callbacks, "%s", g_last_error.c_str());
      pp_shutdown();
      return 1;
    }
    progress.finish();

    manifest.close();

    // End-of-run commit. --temp is the ephemeral mode (files only, no
    // catalog publish). Otherwise publish both tables as one FastAppend
    // snapshot each, partitions BEFORE primes so a resume always sees a
    // hole-free primes frontier (HANDOFF §5.2 write model). Both commits go
    // through CommitFiles — over a pp-catalogd RestCatalog client when
    // --rest-uri is set, else in-process MakeLocalCatalog.
    if (!options.temp) {
      // REST is the default pathway (via --rest-uri or PRIMEPARTS_REST_URI);
      // OpenCatalog falls back to the in-process LMDB catalog of record when no
      // server is configured or reachable. Both modes commit through CommitFiles.
      std::string mode;
      std::shared_ptr<iceberg::Catalog> catalog =
          primeparts::catalog::OpenCatalog(options.warehouse, options.rest_uri,
                                           &mode, &error);
      if (!catalog) {
        set_last_error("commit: open catalog: " + error);
        log_line(callbacks, "%s", g_last_error.c_str());
        pp_shutdown();
        return 1;
      }
      auto p_spec = BucketPartitionSpec(*p_schema, &error);
      auto d_spec = BucketPartitionSpec(*d_schema, &error);
      if (!p_spec || !d_spec) {
        set_last_error("commit: build partition spec: " + error);
        log_line(callbacks, "%s", g_last_error.c_str());
        pp_shutdown();
        return 1;
      }
      std::string meta_loc;
      if (!partitions_data_files.empty() &&
          !primeparts::catalog::CommitFiles(
              catalog, options.warehouse, "partitions", d_schema, d_spec,
              partitions_data_files, &meta_loc, &error)) {
        set_last_error("commit partitions: " + error);
        log_line(callbacks, "%s", g_last_error.c_str());
        pp_shutdown();
        return 1;
      }
      if (!primeparts::catalog::CommitFiles(
              catalog, options.warehouse, "primes", p_schema, p_spec,
              primes_data_files, &meta_loc, &error)) {
        set_last_error("commit primes: " + error);
        log_line(callbacks, "%s", g_last_error.c_str());
        pp_shutdown();
        return 1;
      }
      log_line(callbacks,
               "committed | partitions_files=%zu | primes_files=%zu | primes=%s",
               partitions_data_files.size(), primes_data_files.size(),
               meta_loc.c_str());
    }

    auto run_end = std::chrono::steady_clock::now();
    double elapsed_s =
        std::chrono::duration<double>(run_end - run_start).count();
    double primes_per_s =
        elapsed_s > 0.0 ? static_cast<double>(total_primes) / elapsed_s : 0.0;
    if (out) {
      out->prime_rows = total_primes;
      out->partitions_rows = total_partitions;
      out->files_written = files_written;
      out->bytes_written = bytes_written;
      out->first_p = first_p;
      out->last_p = last_p;
      out->stop_requested = stop_requested ? 1 : 0;
      out->elapsed_s = elapsed_s;
      out->primes_per_s = primes_per_s;
    }
    log_line(callbacks, "generation finished | primes=%" PRId64 " | rate=%.0f/s", total_primes, primes_per_s);
  } catch (const std::exception& exc) {
    set_last_error(std::string("native writer failed: ") + exc.what());
    log_line(callbacks, "%s", g_last_error.c_str());
    pp_shutdown();
    return 1;
  }

  pp_shutdown();
  return 0;
}

extern "C" {

const char* pp_gen_last_error(void) { return g_last_error.c_str(); }

int pp_gen_run(const pp_gen_options* options,
               const pp_gen_callbacks* callbacks,
               pp_gen_result* out) {
  if (!options) {
    set_last_error("null options");
    return 1;
  }
  Options internal;
  internal.start_idx = options->start_idx;
  internal.count = options->count;
  internal.chunk_primes = options->chunk_primes > 0 ? options->chunk_primes : kDefaultChunkPrimes;
  internal.threads = options->threads;
  internal.bucket_version = options->bucket_version > 0 ? options->bucket_version : 1;
  internal.bucket = options->bucket;
  internal.prime_rank_start = options->prime_rank_start > 0
                                   ? options->prime_rank_start
                                   : options->start_idx;
  internal.bucket_is_new = options->bucket_is_new != 0;
  internal.temp = options->temp != 0;
  if (options->warehouse) internal.warehouse = options->warehouse;
  if (options->manifest) internal.manifest = options->manifest;
  if (options->rest_uri) internal.rest_uri = options->rest_uri;
  if (internal.start_idx <= 0 || internal.count < 0 || internal.chunk_primes <= 0 || internal.threads < 0) {
    set_last_error("invalid pp_gen_options values");
    return 1;
  }
  if (internal.warehouse.empty()) {
    if (!internal.temp) {
      set_last_error("either warehouse or temp mode is required");
      return 1;
    }
    fs::path temp_root = default_temp_root();
    internal.warehouse = temp_root / "warehouse";
    if (internal.manifest.empty()) {
      internal.manifest = temp_root / "files.jsonl";
    }
  } else if (internal.manifest.empty()) {
    internal.manifest = internal.warehouse / "files.jsonl";
  }
  if (internal.threads == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    internal.threads = hw == 0 ? 1 : static_cast<int64_t>(hw);
  }
  return run_generation(internal, callbacks, out);
}

}  // extern "C"

#ifndef PRIMEPARTS_GENERATE_NO_MAIN
// Forward each engine log line to stdout, newline-terminated and flushed, so a
// pipe consumer (the TUI Generate pane, CI logs) sees per-group progress live.
// The interactive \r progress bar (Progress) covers the TTY case; this covers
// the non-TTY case, where that bar stays silent.
void gen_stdout_log(void* /*user_data*/, const char* line) {
  std::fputs(line, stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

int main(int argc, char** argv) {
  Options options;
  if (!parse_args(argc, argv, &options)) return 2;
  pp_gen_result out{};
  // On a TTY keep the clean self-overwriting progress bar (no log spam); when
  // stdout is piped, stream the per-group log lines instead so progress is
  // visible through the pipe.
  pp_gen_callbacks cbs{};
  cbs.on_log = gen_stdout_log;
  const bool stdout_tty = ::isatty(STDOUT_FILENO) != 0;
  int rc = run_generation(options, stdout_tty ? nullptr : &cbs, &out);
  if (rc != 0) {
    std::cerr << pp_gen_last_error() << "\n";
    return rc;
  }
  std::cout << "{"
            << "\"warehouse\":\"" << json_escape(fs::absolute(options.warehouse).string())
            << "\","
            << "\"manifest\":\"" << json_escape(fs::absolute(options.manifest).string())
            << "\","
            << "\"start_idx\":" << out.start_idx << ","
            << "\"count\":" << out.count << ","
            << "\"prime_rows\":" << out.prime_rows << ","
            << "\"partitions_rows\":" << out.partitions_rows << ","
            << "\"files_written\":" << out.files_written << ","
            << "\"bytes_written\":" << out.bytes_written << ","
            << "\"first_p\":" << out.first_p << ","
            << "\"last_p\":" << out.last_p << ","
            << "\"stop_requested\":" << (out.stop_requested ? "true" : "false") << ","
            << "\"elapsed_s\":" << out.elapsed_s << ","
            << "\"primes_per_s\":" << out.primes_per_s << "}\n";
  return 0;
}
#endif
