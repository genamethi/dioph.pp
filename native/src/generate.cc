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
#include <unordered_map>
#include <utility>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/partition_field.h"
#include "iceberg/partition_spec.h"
#include "iceberg/result.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_metadata.h"
#include "iceberg/transform.h"
#include "iceberg/type.h"

namespace fs = std::filesystem;

using primeparts::AlignedBucketWriter;
using primeparts::AtomKey;
using primeparts::BoundTable;
using primeparts::BucketPartitionSpec;
using primeparts::CommitPlan;
using primeparts::AscendingSortOrder;
using primeparts::LoadAlignedResume;
using primeparts::FlatPartsSchema;
using primeparts::HigherPartsSchema;
using primeparts::PrimesSchema;
using primeparts::ResumeState;
using primeparts::ShapePolicy;
using primeparts::WrittenFile;

namespace {

constexpr int64_t kMinCount = 1'000'000'000;

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
  int64_t chunk_primes = 0;
  int64_t threads = -1;
  int64_t prime_rank_start = 0;
  bool temp = false;
  bool init = false;
  fs::path warehouse;
  fs::path config_path;
  std::string rest_uri;
  iceberg::Namespace ns;
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
  int64_t flat_parts_rows = 0;
  int64_t higher_parts_rows = 0;
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
      "  --config PATH             Config file. Default: ./config.lua, then\n"
      "                            $XDG_CONFIG_HOME/primeparts/config.lua, then\n"
      "                            ~/.config/primeparts/config.lua, else seeded\n"
      "                            next to this binary.\n"
      "  --warehouse PATH          Warehouse root. Default: conf.core.warehouse.\n"
      "  --rest-uri URL            pp-catalogd base. Default: conf.core.rest_uri.\n"
      "  --namespace NS            Catalog namespace. Default: conf.core.namespace.\n"
      "  --init                    Create primes/partitions when absent and start\n"
      "                            from prime_rank=2. Without it, a missing table\n"
      "                            is a hard error rather than a silent restart.\n"
      "  --temp                    Write to $FUNBUNS_DATA_DIR/tmp/iceberg_temp_<ts>/\n"
      "                            warehouse and skip the commit (files-only).\n"
      "  --chunk-primes N          Materialization chunk size.\n"
      "                            Default: conf.generate.chunk_primes.\n"
      "  --threads N               Materialization threads (0 = hw).\n"
      "                            Default: conf.generate.threads.\n"
      "  --help\n"
      "\n"
      "Environment:\n"
      "  PRIMEPARTS_PRIME_RANK_START prime_rank to stamp on the first prime row.\n"
      "                              Default: the resolved start index.\n");
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
                          : fs::path("./data");
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

std::shared_ptr<arrow::Array> dense_int64_range(int64_t start, int64_t length) {
  std::vector<int64_t> values(static_cast<size_t>(length));
  for (int64_t i = 0; i < length; ++i) {
    values[static_cast<size_t>(i)] = start + i;
  }
  return int64_array(values.data(), length);
}

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

struct PartsBatches {
  std::shared_ptr<arrow::RecordBatch> flat;
  std::shared_ptr<arrow::RecordBatch> higher;
  int64_t flat_rows = 0;
  int64_t higher_rows = 0;
};

void count_parts_rows(const pp_batch_result& batch, int64_t* flat_rows,
                      int64_t* higher_rows) {
  size_t i = 0;
  while (i < batch.partition_count) {
    const int64_t p = batch.partition_p[i];
    bool any_flat = false;
    while (i < batch.partition_count && batch.partition_p[i] == p) {
      if (batch.partition_n[i] == 1) {
        any_flat = true;
      } else {
        ++*higher_rows;
      }
      ++i;
    }
    if (any_flat) ++*flat_rows;
  }
}

PartsBatches make_parts_batches(const pp_batch_result& batch) {
  arrow::Int64Builder flat_p;
  arrow::Int64Builder flat_mask;
  arrow::Int64Builder hi_p;
  arrow::Int32Builder hi_m;
  arrow::Int32Builder hi_n;
  arrow::Int64Builder hi_q;
  if (!flat_p.Reserve(static_cast<int64_t>(batch.prime_count)).ok() ||
      !flat_mask.Reserve(static_cast<int64_t>(batch.prime_count)).ok()) {
    return PartsBatches{};
  }

  size_t i = 0;
  while (i < batch.partition_count) {
    const int64_t p = batch.partition_p[i];
    uint64_t mask = 0;
    while (i < batch.partition_count && batch.partition_p[i] == p) {
      if (batch.partition_n[i] == 1) {
        mask |= uint64_t{1} << batch.partition_m[i];
      } else {
        if (!hi_p.Append(p).ok() ||
            !hi_m.Append(batch.partition_m[i]).ok() ||
            !hi_n.Append(batch.partition_n[i]).ok() ||
            !hi_q.Append(batch.partition_q[i]).ok()) {
          return PartsBatches{};
        }
      }
      ++i;
    }
    if (mask != 0) {
      flat_p.UnsafeAppend(p);
      flat_mask.UnsafeAppend(static_cast<int64_t>(mask));
    }
  }

  auto flat_schema = arrow::schema({
      arrow::field("p",        arrow::int64()),
      arrow::field("hit_mask", arrow::int64()),
  });
  auto higher_schema = arrow::schema({
      arrow::field("p",   arrow::int64()),
      arrow::field("m_k", arrow::int32()),
      arrow::field("n_k", arrow::int32()),
      arrow::field("q_k", arrow::int64()),
  });

  PartsBatches out;
  out.flat_rows = flat_p.length();
  out.higher_rows = hi_p.length();
  std::shared_ptr<arrow::Array> fp, fm, hp, hm, hn, hq;
  if (!flat_p.Finish(&fp).ok() || !flat_mask.Finish(&fm).ok() ||
      !hi_p.Finish(&hp).ok() || !hi_m.Finish(&hm).ok() ||
      !hi_n.Finish(&hn).ok() || !hi_q.Finish(&hq).ok()) {
    return PartsBatches{};
  }
  out.flat = arrow::RecordBatch::Make(flat_schema, out.flat_rows,
                                      {std::move(fp), std::move(fm)});
  out.higher = arrow::RecordBatch::Make(
      higher_schema, out.higher_rows,
      {std::move(hp), std::move(hm), std::move(hn), std::move(hq)});
  return out;
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
    count_parts_rows(holder.batch, &group->flat_parts_rows,
                     &group->higher_parts_rows);
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
      {"init", no_argument, nullptr, 1007},
      {"rest-uri", required_argument, nullptr, 1005},
      {"namespace", required_argument, nullptr, 1006},
      {"config", required_argument, nullptr, 1008},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  std::string ns_name;
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
      case 1007: options->init = true; break;
      case 1005: options->rest_uri = optarg; break;
      case 1006: ns_name = optarg; break;
      case 1008: options->config_path = optarg; break;
      case 'h': usage(stdout); std::exit(0);
      default: return false;
    }
  }
  if (options->count < kMinCount) {
    std::fprintf(stderr, "--count must be >= %lld\n", (long long)kMinCount);
    return false;
  }

  std::string cfg_err;
  primeparts::config::Conf conf;
  if (!primeparts::config::Load(options->config_path, &conf, &cfg_err)) {
    std::fprintf(stderr, "%s\n", cfg_err.c_str());
    return false;
  }
  primeparts::config::Announce(conf);

  if (options->rest_uri.empty()) options->rest_uri = conf.core.rest_uri;
  if (ns_name.empty()) ns_name = conf.core.ns_name;
  options->ns = primeparts::catalog::ResolveNamespace(ns_name);
  if (options->warehouse.empty()) {
    options->warehouse = options->temp ? default_temp_root() / "warehouse"
                                       : fs::path(conf.core.warehouse);
  }
  if (options->chunk_primes <= 0) options->chunk_primes = conf.generate.chunk_primes;
  if (options->threads < 0) options->threads = conf.generate.threads;
  if (options->chunk_primes <= 0 || options->threads < 0) {
    usage(stderr);
    return false;
  }
  if (options->threads == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    options->threads = hw == 0 ? 1 : static_cast<int64_t>(hw);
  }
  return resolve_rank_start(options);
}

struct TableDecl {
  std::string name;
  std::shared_ptr<iceberg::Schema> schema;
  std::shared_ptr<iceberg::PartitionSpec> spec;
  std::vector<std::string> sort_columns;
};

bool commit_plan(const Options& options,
                 const std::shared_ptr<iceberg::Catalog>& catalog,
                 const CommitPlan& plan, const ShapePolicy& policy,
                 const std::vector<TableDecl>& decls, std::string* error) {
  std::vector<primeparts::catalog::TableCommitSpec> specs;
  for (const auto& tf : plan.tables) {
    const TableDecl* decl = nullptr;
    for (const auto& d : decls) {
      if (d.name == tf.name) {
        decl = &d;
        break;
      }
    }
    if (!decl) {
      *error = "commit plan names unknown table " + tf.name;
      return false;
    }
    primeparts::catalog::TableCommitSpec spec;
    spec.table_name = tf.name;
    spec.schema = decl->schema;
    spec.spec = decl->spec;
    spec.declare.sort_order =
        AscendingSortOrder(*spec.schema, decl->sort_columns, error);
    if (!spec.declare.sort_order) return false;
    spec.declare.properties = policy.AsTableProperties();
    spec.declare.properties["pp.buckets.self-contained"] = "true";
    for (const auto& wf : tf.files) {
      if (wf.data_file) spec.files.push_back(wf.data_file);
    }
    specs.push_back(std::move(spec));
  }
  return primeparts::catalog::CommitFilesAtomic(catalog, nullptr, options.rest_uri,
                                                options.ns, options.warehouse,
                                                specs, error);
}

bool schema_matches(const iceberg::Schema& compiled,
                    const iceberg::Schema& adopted, const std::string& table,
                    std::string* error) {
  if (compiled.fields().size() != adopted.fields().size()) {
    *error = "NotImplemented: " + table + " declares " +
             std::to_string(adopted.fields().size()) + " fields but generate "
             "produces rows for " + std::to_string(compiled.fields().size()) +
             "; writing into an evolved schema requires the generator to build "
             "batches from the table's schema rather than from schemas.cc";
    return false;
  }
  for (const auto& want : compiled.fields()) {
    const iceberg::SchemaField* got = nullptr;
    for (const auto& f : adopted.fields()) {
      if (f.field_id() == want.field_id()) {
        got = &f;
        break;
      }
    }
    if (!got) {
      *error = "NotImplemented: " + table + " has no field id " +
               std::to_string(want.field_id()) + " ('" +
               std::string(want.name()) + "'); writing into an evolved schema "
               "requires the generator to build batches from the table's "
               "schema rather than from schemas.cc";
      return false;
    }
    if (got->name() != want.name() ||
        got->type()->type_id() != want.type()->type_id()) {
      *error = "NotImplemented: " + table + " field id " +
               std::to_string(want.field_id()) + " is '" +
               std::string(got->name()) + " " + got->type()->ToString() +
               "' but generate produces '" + std::string(want.name()) + " " +
               want.type()->ToString() +
               "'; writing into an evolved schema requires the generator to "
               "build batches from the table's schema rather than from "
               "schemas.cc";
      return false;
    }
  }
  return true;
}

bool spec_matches(const iceberg::PartitionSpec& compiled,
                  const iceberg::PartitionSpec& adopted,
                  const std::string& table, std::string* error) {
  if (compiled.fields().size() != adopted.fields().size()) {
    *error = "NotImplemented: " + table + " has a default partition spec with " +
             std::to_string(adopted.fields().size()) + " fields but generate "
             "partitions on " + std::to_string(compiled.fields().size()) +
             "; writing under an evolved spec requires the generator to derive "
             "partition values from the table's default spec";
    return false;
  }
  for (size_t i = 0; i < compiled.fields().size(); ++i) {
    const auto& want = compiled.fields()[i];
    const auto& got = adopted.fields()[i];
    if (got.source_id() != want.source_id() || got.name() != want.name() ||
        !got.transform() ||
        got.transform()->transform_type() != iceberg::TransformType::kIdentity) {
      *error = "NotImplemented: " + table + " partition field " +
               std::to_string(i) + " is '" + std::string(got.name()) +
               "' over source " + std::to_string(got.source_id()) +
               " with transform '" +
               (got.transform() ? got.transform()->ToString()
                                : std::string("null")) +
               "' but generate writes identity '" + std::string(want.name()) +
               "' over source " + std::to_string(want.source_id()) +
               "; writing under an evolved spec requires the generator to "
               "derive partition values from the table's default spec";
      return false;
    }
  }
  return true;
}

bool adopt_table(const std::shared_ptr<iceberg::Catalog>& catalog,
                 const iceberg::Namespace& ns, const std::string& name,
                 std::shared_ptr<iceberg::Schema>* schema,
                 std::shared_ptr<iceberg::PartitionSpec>* spec,
                 std::unordered_map<std::string, std::string>* properties,
                 bool* adopted, std::string* error) {
  *adopted = false;
  auto loaded = catalog->LoadTable(iceberg::TableIdentifier{.ns = ns, .name = name});
  if (!loaded.has_value()) {
    if (loaded.error().kind == iceberg::ErrorKind::kNoSuchTable) return true;
    *error = "LoadTable " + name + ": " + loaded.error().message;
    return false;
  }
  const auto& table = loaded.value();

  auto schema_r = table->schema();
  if (!schema_r.has_value()) {
    *error = name + " schema: " + schema_r.error().message;
    return false;
  }
  auto spec_r = table->spec();
  if (!spec_r.has_value()) {
    *error = name + " spec: " + spec_r.error().message;
    return false;
  }
  if (!schema_matches(**schema, *schema_r.value(), name, error)) return false;
  if (!spec_matches(**spec, *spec_r.value(), name, error)) return false;

  *schema = schema_r.value();
  *spec = spec_r.value();
  if (properties && table->metadata())
    *properties = table->metadata()->properties.configs();
  *adopted = true;
  return true;
}

bool resolve_start_idx(const Options& options, const pp_gen_callbacks* callbacks,
                       int64_t* out_start) {
  if (options.temp) {
    *out_start = options.start_idx > 0 ? options.start_idx : kFreshStartIdx;
    return true;
  }
  int64_t ub = 0;
  auto state = primeparts::catalog::FieldBoundState::kNoSnapshot;
  std::string err;
  if (!primeparts::catalog::FetchFieldBound(options.rest_uri, options.ns,
                                            "primes", "prime_rank", &ub, &state,
                                            &err)) {
    if (options.start_idx > 0) {
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

  switch (state) {
    case primeparts::catalog::FieldBoundState::kPresent:
      *out_start = ub + 1;
      if (options.start_idx > 0 && options.start_idx != *out_start) {
        log_line(callbacks,
                 "note: ignoring --start-idx=%" PRId64 "; resuming from frontier "
                 "prime_rank=%" PRId64 " (start_idx=%" PRId64 ")",
                 options.start_idx, ub, *out_start);
      } else {
        log_line(callbacks,
                 "resume: frontier prime_rank=%" PRId64 ", start_idx=%" PRId64,
                 ub, *out_start);
      }
      return true;

    case primeparts::catalog::FieldBoundState::kNoSnapshot:
      *out_start = options.start_idx > 0 ? options.start_idx : kFreshStartIdx;
      log_line(callbacks,
               "primes exists with no snapshot (empty): start_idx=%" PRId64,
               *out_start);
      return true;

    case primeparts::catalog::FieldBoundState::kTableAbsent:
      if (!options.init) {
        set_last_error(
            "resume: table " + options.ns.levels.back() +
            ".primes does not exist; this warehouse is not initialized. Pass "
            "--init to create it and generate from prime_rank=" +
            std::to_string(kFreshStartIdx) + ", or use --temp");
        log_line(callbacks, "%s", g_last_error.c_str());
        return false;
      }
      *out_start = options.start_idx > 0 ? options.start_idx : kFreshStartIdx;
      log_line(callbacks, "--init: creating primes/partitions, start_idx=%" PRId64,
               *out_start);
      return true;

    case primeparts::catalog::FieldBoundState::kSnapshotNoBound:
      set_last_error(
          "resume: primes has a snapshot but no upper bound for prime_rank; the "
          "frontier cannot be located and generating would duplicate rows into "
          "an append-only table. Repair the table's manifest bounds or start a "
          "new warehouse (--temp / --init on an empty prefix)");
      log_line(callbacks, "%s", g_last_error.c_str());
      return false;
  }
  return false;
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
    auto f_schema = FlatPartsSchema();
    auto h_schema = HigherPartsSchema();
    auto p_spec = BucketPartitionSpec(*p_schema, &error);
    auto f_spec = BucketPartitionSpec(*f_schema, &error);
    auto h_spec = BucketPartitionSpec(*h_schema, &error);
    if (!p_spec || !f_spec || !h_spec) {
      set_last_error("build partition spec: " + error);
      log_line(callbacks, "%s", g_last_error.c_str());
      pp_shutdown();
      return 1;
    }

    std::shared_ptr<iceberg::Catalog> catalog;
    if (!options.temp) {
      std::string mode;
      catalog = primeparts::catalog::OpenCatalog(options.warehouse,
                                                 options.rest_uri, &mode, &error);
      if (!catalog) {
        set_last_error("open catalog: " + error);
        log_line(callbacks, "%s", g_last_error.c_str());
        pp_shutdown();
        return 1;
      }
    }

    ShapePolicy policy;
    if (catalog) {
      std::unordered_map<std::string, std::string> p_properties;
      bool p_adopted = false;
      bool f_adopted = false;
      bool h_adopted = false;
      if (!adopt_table(catalog, options.ns, "primes", &p_schema, &p_spec,
                       &p_properties, &p_adopted, &error) ||
          !adopt_table(catalog, options.ns, "flat_parts", &f_schema, &f_spec,
                       nullptr, &f_adopted, &error) ||
          !adopt_table(catalog, options.ns, "higher_parts", &h_schema, &h_spec,
                       nullptr, &h_adopted, &error)) {
        set_last_error("adopt table: " + error);
        log_line(callbacks, "%s", g_last_error.c_str());
        pp_shutdown();
        return 1;
      }
      if (p_adopted) {
        std::vector<std::string> absent;
        if (!policy.FromTableProperties(p_properties, &absent, &error)) {
          set_last_error("shape: " + error);
          log_line(callbacks, "%s", g_last_error.c_str());
          pp_shutdown();
          return 1;
        }
        if (absent.empty()) {
          log_line(callbacks, "shape: read from primes metadata");
        } else {
          std::string joined;
          for (const auto& k : absent) {
            if (!joined.empty()) joined += ", ";
            joined += k;
          }
          log_line(callbacks,
                   "shape: primes declares no %s; using the built-in value",
                   joined.c_str());
        }
      }
      log_line(callbacks,
               "shape: file_target_bytes=%" PRId64 " rgs_per_file=%d "
               "bucket_target_bytes=%" PRId64 " bucket_version=%d",
               policy.file_target_bytes, policy.rgs_per_file,
               policy.bucket_target_bytes, policy.bucket_version);
    }

    ResumeState resume;
    if (catalog &&
        !LoadAlignedResume(catalog, options.ns,
                           {"primes", "flat_parts", "higher_parts"}, "primes",
                           primeparts::BucketFields{"p_bucket_version",
                                                    "p_bucket"},
                           policy.bucket_version, &resume, &error)) {
      set_last_error("resume: " + error);
      log_line(callbacks, "%s", g_last_error.c_str());
      pp_shutdown();
      return 1;
    }

    const std::vector<TableDecl> decls{
        TableDecl{"primes", p_schema, p_spec, {"p"}},
        TableDecl{"flat_parts", f_schema, f_spec, {"p"}},
        TableDecl{"higher_parts", h_schema, h_spec, {"p", "m_k"}},
    };

    std::vector<BoundTable> tables;
    tables.push_back(BoundTable{"primes", p_schema, p_spec,
                                {"p", "prime_rank"},
                                {{"p", true}, {"prime_rank", true}},
                                true, nullptr});
    tables.push_back(BoundTable{"flat_parts", f_schema, f_spec,
                                {"p"},
                                {{"p", true}},
                                false, nullptr});
    tables.push_back(BoundTable{"higher_parts", h_schema, h_spec,
                                {"p"},
                                {{"p", true}},
                                false, nullptr});
    auto writer = AlignedBucketWriter::Make(options.warehouse, options.ns,
                                            std::move(tables), AtomKey{"p"},
                                            policy, resume, &error);
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
    int64_t total_flat_parts = 0;
    int64_t total_higher_parts = 0;
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
    const size_t kMaxPending = 1;
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
          auto parts = make_parts_batches(holder.batch);
          if (!pb || !parts.flat || !parts.higher) {
            std::lock_guard<std::mutex> guard(q_mu);
            writer_failed = true;
            writer_error = "build record batch failed";
            q_can_push.notify_one();
            return;
          }
          std::string werr;
          if (!writer->Append({pb, parts.flat, parts.higher}, &werr)) {
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
      total_flat_parts += group.flat_parts_rows;
      total_higher_parts += group.higher_parts_rows;
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
      if (!commit_plan(options, catalog, plan, policy, decls,
                       &error)) {
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
      out->flat_parts_rows = total_flat_parts;
      out->higher_parts_rows = total_higher_parts;
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
  std::string cfg_err;
  primeparts::config::Conf conf;
  if (!primeparts::config::Load({}, &conf, &cfg_err)) {
    set_last_error(cfg_err);
    return 1;
  }
  Options internal;
  internal.start_idx = options->start_idx;
  internal.count = options->count;
  internal.chunk_primes = options->chunk_primes > 0 ? options->chunk_primes
                                                    : conf.generate.chunk_primes;
  internal.threads = options->threads;
  internal.prime_rank_start = options->prime_rank_start;
  internal.temp = options->temp != 0;
  if (options->warehouse) internal.warehouse = options->warehouse;
  if (options->rest_uri) internal.rest_uri = options->rest_uri;
  internal.ns = primeparts::catalog::ResolveNamespace(
      options->ns && options->ns[0] ? options->ns : conf.core.ns_name);
  if (internal.count < kMinCount || internal.chunk_primes <= 0 || internal.threads < 0) {
    set_last_error("invalid pp_gen_options values (count must be >= 1000000000)");
    return 1;
  }
  if (internal.rest_uri.empty()) internal.rest_uri = conf.core.rest_uri;
  if (internal.warehouse.empty() && !internal.temp) {
    internal.warehouse = conf.core.warehouse;
  }
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
void gen_stdout_log(void*, const char* line) {
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
            << "\"flat_parts_rows\":" << out.flat_parts_rows << ","
            << "\"higher_parts_rows\":" << out.higher_parts_rows << ","
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
