#include "primeparts/aligned_writer.h"
#include "primeparts/catalog/pp_commit.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/config.h"
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
#include <getopt.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "iceberg/schema.h"

namespace fs = std::filesystem;

using primeparts::AlignedBucketWriter;
using primeparts::AtomKey;
using primeparts::BoundTable;
using primeparts::BucketPartitionSpec;
using primeparts::CommitPlan;
using primeparts::LoadAlignedResume;
using primeparts::PartitionsSchema;
using primeparts::PrimesSchema;
using primeparts::ResumeState;
using primeparts::ShapePolicy;
using primeparts::WrittenFile;

namespace {

constexpr int64_t kDefaultChunkPrimes = 500000;

// Minimum --count. Smaller runs fragment the byte-aligned buckets and aren't
// worth a full generate invocation.
constexpr int64_t kMinCount = 1'000'000'000;

// Fresh/empty warehouse (or --temp) default start index. prime_rank == 1-based
// prime index; index 1 is p=2, intentionally omitted, so a from-scratch build
// starts at index 2 (p=3).
constexpr int64_t kFreshStartIdx = 2;

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
  int64_t start_idx = 0;
  int64_t count = -1;
  int64_t chunk_primes = kDefaultChunkPrimes;
  int64_t threads = 0;
  int64_t prime_rank_start = 0;
  bool temp = false;
  fs::path warehouse;
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
      "usage: primeparts-generate --count N [options]\n"
      "\n"
      "Streams primes+partitions to the pp REST client, which shapes byte-\n"
      "driven p-aligned buckets (primes >= 1 GiB) and commits both tables in\n"
      "one atomic transaction. Only --count is required; --start-idx resumes\n"
      "from the committed frontier by default.\n"
      "\n"
      "Options:\n"
      "  --count N                 REQUIRED. Number of primes to generate\n"
      "                            (minimum 1000000000).\n"
      "  --start-idx N             First prime index to generate. Default: resume\n"
      "                            from the frontier (catalogd's committed max\n"
      "                            prime_rank + 1). Applies only to a fresh\n"
      "                            warehouse or --temp, where it defaults to 2\n"
      "                            (index 1 is p=2, omitted by convention).\n"
      "  --warehouse PATH          Warehouse root. Default: config.lua 'warehouse'.\n"
      "  --rest-uri URL            pp-catalogd base (e.g. http://127.0.0.1:8181).\n"
      "                            Default: config.lua 'rest_uri' or 127.0.0.1:8181.\n"
      "  --temp                    Write to $FUNBUNS_DATA_DIR/tmp/iceberg_temp_<ts>/\n"
      "                            warehouse and skip the commit (files-only).\n"
      "  --chunk-primes N          Materialization chunk size (default: 500000).\n"
      "  --threads N               Materialization threads (default: hw).\n"
      "  --help\n"
      "\n"
      "Environment:\n"
      "  PRIMEPARTS_PRIME_RANK_START prime_rank to stamp on the first prime row.\n"
      "                              Default: the resolved start index.\n"
      "  PRIMEPARTS_REST_URI         Overrides --rest-uri / config.lua.\n");
}

bool parse_i64(const char* text, int64_t* out) {
  char* end = nullptr;
  errno = 0;
  long long value = std::strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') return false;
  *out = static_cast<int64_t>(value);
  return true;
}

bool resolve_rank_start(Options* options) {
  const char* rank_env = std::getenv("PRIMEPARTS_PRIME_RANK_START");
  if (rank_env != nullptr && rank_env[0] != '\0') {
    if (!parse_i64(rank_env, &options->prime_rank_start) ||
        options->prime_rank_start <= 0) {
      std::fprintf(stderr, "invalid PRIMEPARTS_PRIME_RANK_START: %s\n", rank_env);
      return false;
    }
  }
  // else: leave prime_rank_start = 0; run_generation defaults it to the resolved
  // start_idx once the frontier is known.
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

// --- producer: pp_batch_result -> arrow RecordBatch (this producer's columns) -

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

std::shared_ptr<arrow::Array> dense_int64_range(int64_t start, int64_t length) {
  std::vector<int64_t> values(static_cast<size_t>(length));
  for (int64_t i = 0; i < length; ++i) {
    values[static_cast<size_t>(i)] = start + i;
  }
  return int64_array(values.data(), length);
}

std::shared_ptr<arrow::Array> partitions_rank_array(const pp_batch_result& batch,
                                                    int64_t rank_start) {
  std::vector<int64_t> ranks;
  ranks.reserve(batch.decomp_count);
  for (size_t i = 0; i < batch.prime_count; ++i) {
    int64_t rank_i = rank_start + static_cast<int64_t>(i);
    int32_t kk = batch.prime_k[i];
    for (int32_t j = 0; j < kk; ++j) ranks.push_back(rank_i);
  }
  return int64_array(ranks.data(), static_cast<int64_t>(batch.decomp_count));
}

// Physical primes batch: (p, k, prime_rank). Bucket columns live in the
// manifest partition tuple and are stripped by the writer.
std::shared_ptr<arrow::RecordBatch> make_primes_batch(const pp_batch_result& batch,
                                                      int64_t rank_start) {
  auto schema = arrow::schema({
      arrow::field("p",          arrow::int64()),
      arrow::field("k",          arrow::int32()),
      arrow::field("prime_rank", arrow::int64()),
  });
  int64_t rows = static_cast<int64_t>(batch.prime_count);
  return arrow::RecordBatch::Make(
      schema, rows,
      {int64_array(batch.prime_p, rows),
       int32_array(batch.prime_k, rows),
       dense_int64_range(rank_start, rows)});
}

// Physical partitions batch: (p, m_k, n_k, q_k, prime_rank).
std::shared_ptr<arrow::RecordBatch> make_partitions_batch(const pp_batch_result& batch,
                                                          int64_t rank_start) {
  auto schema = arrow::schema({
      arrow::field("p",          arrow::int64()),
      arrow::field("m_k",        arrow::int32()),
      arrow::field("n_k",        arrow::int32()),
      arrow::field("q_k",        arrow::int64()),
      arrow::field("prime_rank", arrow::int64()),
  });
  int64_t rows = static_cast<int64_t>(batch.decomp_count);
  return arrow::RecordBatch::Make(
      schema, rows,
      {int64_array(batch.decomp_p, rows),
       int32_array(batch.decomp_m, rows),
       int32_array(batch.decomp_n, rows),
       int64_array(batch.decomp_q, rows),
       partitions_rank_array(batch, rank_start)});
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

bool parse_args(int argc, char** argv, Options* options) {
  static const option long_options[] = {
      {"start-idx", required_argument, nullptr, 1000},
      {"count", required_argument, nullptr, 'n'},
      {"chunk-primes", required_argument, nullptr, 'c'},
      {"threads", required_argument, nullptr, 1003},
      {"warehouse", required_argument, nullptr, 'w'},
      {"temp", no_argument, nullptr, 1004},
      {"rest-uri", required_argument, nullptr, 1005},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  int opt;
  while ((opt = getopt_long(argc, argv, "n:c:w:h", long_options, nullptr)) != -1) {
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
      case 1004: options->temp = true; break;
      case 1005: options->rest_uri = optarg; break;
      case 'h': usage(stdout); std::exit(0);
      default: return false;
    }
  }
  if (options->chunk_primes <= 0 || options->threads < 0) {
    usage(stderr);
    return false;
  }
  if (options->count < kMinCount) {
    std::fprintf(stderr, "--count must be >= %lld\n", (long long)kMinCount);
    return false;
  }

  // Defaults from config.lua when the flags are absent (CLI flag wins).
  std::string cfg_err;
  auto cfg = primeparts::config::Load(&cfg_err);
  if (options->rest_uri.empty()) {
    if (const char* env = std::getenv("PRIMEPARTS_REST_URI"); env && env[0])
      options->rest_uri = env;
    else if (auto it = cfg.find("rest_uri"); it != cfg.end() && !it->second.empty())
      options->rest_uri = it->second;
    else
      options->rest_uri = primeparts::catalog::kDefaultRestUri;
  }
  if (options->warehouse.empty()) {
    if (options->temp) {
      options->warehouse = default_temp_root() / "warehouse";
    } else if (auto it = cfg.find("warehouse"); it != cfg.end() && !it->second.empty()) {
      options->warehouse = it->second;
    } else {
      std::fprintf(stderr,
                   "no warehouse: pass --warehouse, set warehouse in %s, or use --temp\n",
                   primeparts::config::ConfigFilePath().string().c_str());
      return false;
    }
  }
  if (options->threads == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    options->threads = hw == 0 ? 1 : static_cast<int64_t>(hw);
  }
  return resolve_rank_start(options);
}

// Turn a finished CommitPlan into the atomic commit's per-table specs and hand
// them to the client's commit path (in-process store, or daemon over rest_uri).
bool commit_plan(const Options& options, const CommitPlan& plan,
                 const std::shared_ptr<iceberg::Schema>& p_schema,
                 const std::shared_ptr<iceberg::Schema>& d_schema,
                 const std::shared_ptr<iceberg::PartitionSpec>& p_spec,
                 const std::shared_ptr<iceberg::PartitionSpec>& d_spec,
                 std::string* error) {
  std::vector<primeparts::catalog::TableCommitSpec> specs;
  for (const auto& tf : plan.tables) {
    primeparts::catalog::TableCommitSpec spec;
    spec.table_name = tf.name;
    spec.schema = tf.name == "primes" ? p_schema : d_schema;
    spec.spec = tf.name == "primes" ? p_spec : d_spec;
    for (const auto& wf : tf.files) {
      if (wf.data_file) spec.files.push_back(wf.data_file);
    }
    specs.push_back(std::move(spec));
  }

  std::shared_ptr<iceberg::Catalog> catalog;
  std::shared_ptr<iceberg::sql::CatalogStore> store;
  if (!options.rest_uri.empty()) {
    std::string mode;
    catalog = primeparts::catalog::OpenCatalog(options.warehouse,
                                               options.rest_uri, &mode, error);
    if (!catalog) return false;
  } else {
    auto local = primeparts::catalog::MakeLocalCatalogWithStore(options.warehouse,
                                                                error);
    if (!local.catalog) return false;
    catalog = local.catalog;
    store = local.store;
  }
  return primeparts::catalog::CommitFilesAtomic(catalog, store, options.rest_uri,
                                                options.warehouse, specs, error);
}

// Resolve the first prime index to generate. --temp or an unset start_idx on a
// fresh warehouse defaults to kFreshStartIdx (p=3). Otherwise ask catalogd for
// the committed `prime_rank` frontier and continue at frontier+1; an explicit
// --start-idx is honored only when there is no frontier (fresh) or when catalogd
// is unreachable (offline override). False + last_error on an unrecoverable
// failure.
bool resolve_start_idx(const Options& options, const pp_gen_callbacks* callbacks,
                       int64_t* out_start) {
  if (options.temp) {
    *out_start = options.start_idx > 0 ? options.start_idx : kFreshStartIdx;
    return true;
  }
  int64_t ub = 0;
  bool present = false;
  std::string err;
  if (!primeparts::catalog::FetchFieldUpperBound(options.rest_uri, "primeparts",
                                                 "primes", "prime_rank", &ub,
                                                 &present, &err)) {
    if (options.start_idx > 0) {  // offline override
      *out_start = options.start_idx;
      log_line(callbacks,
               "warning: catalogd unreachable (%s); using --start-idx=%" PRId64,
               err.c_str(), options.start_idx);
      return true;
    }
    set_last_error("resume: cannot reach catalogd at " + options.rest_uri + " (" +
                   err + "); pass --start-idx, use --temp, or start pp-catalogd");
    log_line(callbacks, "%s", g_last_error.c_str());
    return false;
  }
  if (present) {
    *out_start = ub + 1;
    if (options.start_idx > 0 && options.start_idx != *out_start) {
      log_line(callbacks,
               "note: ignoring --start-idx=%" PRId64 "; resuming from frontier "
               "prime_rank=%" PRId64 " (start_idx=%" PRId64 ")",
               options.start_idx, ub, *out_start);
    } else {
      log_line(callbacks,
               "resume: frontier prime_rank=%" PRId64 ", start_idx=%" PRId64, ub,
               *out_start);
    }
  } else {
    *out_start = options.start_idx > 0 ? options.start_idx : kFreshStartIdx;
    log_line(callbacks, "fresh warehouse: start_idx=%" PRId64, *out_start);
  }
  return true;
}

}  // namespace

int run_generation(const Options& options, const pp_gen_callbacks* callbacks, pp_gen_result* out) {
  std::string error;
  int64_t start_idx = 0;
  if (!resolve_start_idx(options, callbacks, &start_idx)) return 1;
  int64_t prime_rank_start =
      options.prime_rank_start > 0 ? options.prime_rank_start : start_idx;
  if (out) {
    std::memset(out, 0, sizeof(*out));
    out->start_idx = start_idx;
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

    auto p_schema = PrimesSchema();
    auto d_schema = PartitionsSchema();
    auto p_spec = BucketPartitionSpec(*p_schema, &error);
    auto d_spec = BucketPartitionSpec(*d_schema, &error);
    if (!p_spec || !d_spec) {
      set_last_error("build partition spec: " + error);
      log_line(callbacks, "%s", g_last_error.c_str());
      pp_shutdown();
      return 1;
    }

    ShapePolicy policy;
    ResumeState resume;
    if (!LoadAlignedResume(options.warehouse, {"primes", "partitions"}, "primes",
                           policy.bucket_version, &resume, &error)) {
      set_last_error("resume: " + error);
      log_line(callbacks, "%s", g_last_error.c_str());
      pp_shutdown();
      return 1;
    }

    std::vector<BoundTable> tables;
    tables.push_back(BoundTable{"primes", p_schema, p_spec,
                                {"p", "prime_rank"}, /*reference=*/true, nullptr});
    tables.push_back(BoundTable{"partitions", d_schema, d_spec,
                                {"p", "prime_rank", "q_k"}, /*reference=*/false, nullptr});
    auto writer = AlignedBucketWriter::Make(options.warehouse, std::move(tables),
                                            AtomKey{"p"}, policy, resume, &error);
    if (!writer) {
      set_last_error("open writer: " + error);
      log_line(callbacks, "%s", g_last_error.c_str());
      pp_shutdown();
      return 1;
    }

    int64_t next_idx = start_idx;
    int64_t end_idx = start_idx + options.count;
    int64_t prime_rank_cursor = prime_rank_start;
    int64_t total_primes = 0;
    int64_t total_partitions = 0;
    int64_t first_p = 0;
    int64_t last_p = 0;
    bool stop_requested = false;

    int64_t total_chunks =
        (options.count + options.chunk_primes - 1) / options.chunk_primes;
    int64_t group_width = options.threads > 0 ? options.threads : 1;
    int64_t total_groups = (total_chunks + group_width - 1) / group_width;
    Progress progress(total_groups, options.count);
    StopMonitor stop_monitor;
    int64_t groups_done = 0;
    progress.update(0, 0);

    struct WriteJob {
      FileGroup group;
      std::vector<int64_t> rank_starts;
    };

    std::mutex q_mu;
    std::condition_variable q_can_push;
    std::condition_variable q_can_pop;
    std::deque<WriteJob> jobs;
    const size_t kMaxPending = 1;  // one job may wait while another is written
    bool producer_done = false;
    bool writer_failed = false;
    std::string writer_error;

    std::thread writer_thread([&]() {
      int64_t w_primes = 0;
      for (;;) {
        WriteJob job;
        {
          std::unique_lock<std::mutex> lk(q_mu);
          q_can_pop.wait(lk, [&]() { return !jobs.empty() || producer_done; });
          if (jobs.empty()) return;
          job = std::move(jobs.front());
          jobs.pop_front();
          q_can_push.notify_one();
        }

        for (size_t i = 0; i < job.group.batches.size(); ++i) {
          const auto& holder = job.group.batches[i];
          if (holder.batch.prime_count == 0) continue;
          int64_t rank = job.rank_starts[i];
          auto pb = make_primes_batch(holder.batch, rank);
          auto db = make_partitions_batch(holder.batch, rank);
          if (!pb || !db) {
            std::lock_guard<std::mutex> guard(q_mu);
            writer_failed = true;
            writer_error = "build record batch failed";
            q_can_push.notify_one();
            return;
          }
          std::string werr;
          if (!writer->Append({pb, db}, &werr)) {
            std::lock_guard<std::mutex> guard(q_mu);
            writer_failed = true;
            writer_error = "append failed: " + werr;
            q_can_push.notify_one();
            return;
          }
          w_primes += static_cast<int64_t>(holder.batch.prime_count);
        }
        log_line(callbacks, "group written | primes=%" PRId64, w_primes);
      }
    });

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
      job.rank_starts.assign(group.batches.size(), 0);
      int64_t acc = 0;
      for (size_t i = 0; i < group.batches.size(); ++i) {
        job.rank_starts[i] = prime_rank_cursor + acc;
        acc += static_cast<int64_t>(group.batches[i].batch.prime_count);
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

    CommitPlan plan;
    if (!writer->Finish(&plan, &error)) {
      set_last_error("finish writer: " + error);
      log_line(callbacks, "%s", g_last_error.c_str());
      pp_shutdown();
      return 1;
    }

    int64_t files_written = 0;
    int64_t bytes_written = 0;
    for (const auto& tf : plan.tables) {
      for (const auto& wf : tf.files) {
        files_written++;
        bytes_written += wf.bytes;
      }
    }

    if (!options.temp) {
      if (!commit_plan(options, plan, p_schema, d_schema, p_spec, d_spec, &error)) {
        set_last_error("commit: " + error);
        log_line(callbacks, "%s", g_last_error.c_str());
        pp_shutdown();
        return 1;
      }
      log_line(callbacks, "committed | files=%" PRId64 " | primes=%" PRId64,
               files_written, total_primes);
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
  internal.prime_rank_start = options->prime_rank_start;  // 0 => derive from start_idx
  internal.temp = options->temp != 0;
  if (options->warehouse) internal.warehouse = options->warehouse;
  if (options->rest_uri) internal.rest_uri = options->rest_uri;
  if (internal.count < kMinCount || internal.chunk_primes <= 0 || internal.threads < 0) {
    set_last_error("invalid pp_gen_options values (count must be >= 1000000000)");
    return 1;
  }
  if (internal.rest_uri.empty())
    internal.rest_uri = primeparts::catalog::kDefaultRestUri;
  if (internal.warehouse.empty()) {
    if (!internal.temp) {
      set_last_error("either warehouse or temp mode is required");
      return 1;
    }
    internal.warehouse = default_temp_root() / "warehouse";
  }
  if (internal.threads == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    internal.threads = hw == 0 ? 1 : static_cast<int64_t>(hw);
  }
  return run_generation(internal, callbacks, out);
}

}  // extern "C"

#ifndef PRIMEPARTS_GENERATE_NO_MAIN
void gen_stdout_log(void* /*user_data*/, const char* line) {
  std::fputs(line, stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

int main(int argc, char** argv) {
  Options options;
  if (!parse_args(argc, argv, &options)) return 2;
  pp_gen_result out{};
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
