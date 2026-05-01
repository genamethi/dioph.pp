#include "primeparts/core.h"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "iceberg/arrow/arrow_file_io.h"
#include "iceberg/file_format.h"
#include "iceberg/file_writer.h"
#include "iceberg/parquet/parquet_register.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/type.h"

namespace fs = std::filesystem;

namespace {

constexpr int64_t kDefaultChunkPrimes = 500000;
constexpr int64_t kDefaultChunksPerFile = 4;

// --- BEGIN timing instrumentation (remove with single revert) ---
// Aggregated phase totals across all threads; nanoseconds.
std::atomic<int64_t> g_compute_ns{0};
std::atomic<int64_t> g_arrow_build_ns{0};
std::atomic<int64_t> g_write_ns{0};
std::atomic<int64_t> g_materialize_wall_ns{0};
// --- END timing instrumentation ---

struct Options {
  int64_t start_idx = 0;
  int64_t count = -1;
  int64_t chunk_primes = kDefaultChunkPrimes;
  int64_t chunks_per_file = kDefaultChunksPerFile;
  int64_t threads = 0;
  int32_t commit_seq = 0;
  bool temp = false;
  fs::path warehouse;
  fs::path manifest;
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
  int64_t decomp_prime_rows = 0;
  int64_t decomp_rows = 0;
  int64_t first_p = 0;
  int64_t last_p = 0;
  int64_t start_idx = 0;
  int64_t processed_count = 0;
  std::map<int32_t, int64_t> k_histogram;
};

struct WrittenFile {
  std::string table;
  fs::path path;
  int32_t commit_seq = 0;
  int64_t rows = 0;
  int64_t p_min = 0;
  int64_t p_max = 0;
  int64_t bytes = 0;
};

// Single-line stderr progress bar. Uses \r so it overwrites itself; emits
// nothing when stderr isn't a TTY so pipes/CI logs stay clean.
class Progress {
 public:
  Progress(int64_t total_groups, int64_t total_primes)
      : total_groups_(total_groups),
        total_primes_(total_primes),
        enabled_(::isatty(STDERR_FILENO) != 0),
        start_(std::chrono::steady_clock::now()) {}

  void update(int64_t groups_done, int64_t primes_done) {
    if (!enabled_) {
      return;
    }
    double elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - start_)
                         .count();
    double rate = elapsed > 0.0 ? static_cast<double>(primes_done) / elapsed : 0.0;
    int64_t remaining = total_primes_ - primes_done;
    int eta_s = rate > 0.0 ? static_cast<int>(remaining / rate) : 0;
    int h = eta_s / 3600;
    int m = (eta_s % 3600) / 60;
    int s = eta_s % 60;

    char rate_buf[32];
    if (rate >= 1.0e6) {
      std::snprintf(rate_buf, sizeof(rate_buf), "%5.2fM/s", rate / 1.0e6);
    } else if (rate >= 1.0e3) {
      std::snprintf(rate_buf, sizeof(rate_buf), "%5.1fk/s", rate / 1.0e3);
    } else {
      std::snprintf(rate_buf, sizeof(rate_buf), "%5.0f/s", rate);
    }

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
                 bar, pct,
                 static_cast<long long>(groups_done),
                 static_cast<long long>(total_groups_), rate_buf, h, m, s);
    std::fflush(stderr);
  }

  void finish() {
    if (!enabled_) {
      return;
    }
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
    if (!enabled_) {
      return;
    }
    if (tcgetattr(STDIN_FILENO, &original_) != 0) {
      enabled_ = false;
      return;
    }
    termios raw = original_;
    raw.c_lflag &= ~(static_cast<tcflag_t>(ICANON) | static_cast<tcflag_t>(ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      enabled_ = false;
      return;
    }
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this]() { run(); });
  }

  ~StopMonitor() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
      worker_.join();
    }
    if (enabled_) {
      tcsetattr(STDIN_FILENO, TCSANOW, &original_);
    }
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
      if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) {
        continue;
      }

      char ch = '\0';
      ssize_t n = read(STDIN_FILENO, &ch, 1);
      if (n != 1) {
        continue;
      }
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
      "Native C core + iceberg-cpp Parquet writer.\n"
      "\n"
      "Options:\n"
      "  --temp                    Create data/tmp/iceberg_temp_native_<ts>/warehouse\n"
      "  --warehouse PATH          Warehouse root to write under\n"
      "  --manifest PATH           JSONL file list to write\n"
      "  --chunk-primes N          Materialization chunk size (default: 500000)\n"
      "  --chunks-per-file N       Materialized chunks per Parquet file (default: 4)\n"
      "  --threads N               Materialization threads per file group (default: hw)\n"
      "  --help                    Show this help\n");
}

bool parse_i64(const char* text, int64_t* out) {
  char* end = nullptr;
  errno = 0;
  long long value = std::strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *out = static_cast<int64_t>(value);
  return true;
}

bool parse_i32(const char* text, int32_t* out) {
  int64_t value = 0;
  if (!parse_i64(text, &value) || value < INT32_MIN || value > INT32_MAX) {
    return false;
  }
  *out = static_cast<int32_t>(value);
  return true;
}

bool directory_has_regular_files(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return false;
  }
  for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      return true;
    }
  }
  return false;
}

bool existing_warehouse_data(const fs::path& warehouse) {
  return directory_has_regular_files(warehouse / "funbuns" / "primes" / "data") ||
         directory_has_regular_files(warehouse / "funbuns" / "decompositions" / "data");
}

bool resolve_commit_seq_start(Options* options) {
  const char* env = std::getenv("PRIMEPARTS_COMMIT_SEQ_START");
  if (env != nullptr && env[0] != '\0') {
    if (!parse_i32(env, &options->commit_seq) || options->commit_seq < 0) {
      std::fprintf(stderr, "invalid PRIMEPARTS_COMMIT_SEQ_START: %s\n", env);
      return false;
    }
    return true;
  }
  if (existing_warehouse_data(options->warehouse)) {
    std::fprintf(stderr,
                 "refusing to write an existing warehouse without coordinator state; "
                 "run through `primeparts`, not primeparts-generate directly\n");
    return false;
  }
  options->commit_seq = 0;
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

std::string utc_timestamp_iso() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

fs::path default_temp_root() {
  const char* env = std::getenv("FUNBUNS_DATA_DIR");
  fs::path data_dir = env != nullptr && env[0] != '\0' ? fs::path(env) : fs::path("data");
  return data_dir / "tmp" / ("iceberg_temp_native_" + utc_timestamp_compact());
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

std::string k_histogram_json(const std::map<int32_t, int64_t>& hist) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto& [k, n] : hist) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << "\"" << k << "\":" << n;
  }
  out << "}";
  return out.str();
}

std::shared_ptr<iceberg::Schema> primes_schema() {
  return std::make_shared<iceberg::Schema>(
      std::vector<iceberg::SchemaField>{
          iceberg::SchemaField::MakeOptional(1, "p", iceberg::int64()),
          iceberg::SchemaField::MakeOptional(2, "k", iceberg::int32()),
          iceberg::SchemaField::MakeOptional(3, "commit_seq", iceberg::int32()),
      },
      0);
}

std::shared_ptr<iceberg::Schema> decomp_schema() {
  return std::make_shared<iceberg::Schema>(
      std::vector<iceberg::SchemaField>{
          iceberg::SchemaField::MakeOptional(1, "p", iceberg::int64()),
          iceberg::SchemaField::MakeOptional(2, "m_k", iceberg::int32()),
          iceberg::SchemaField::MakeOptional(3, "n_k", iceberg::int32()),
          iceberg::SchemaField::MakeOptional(4, "q_k", iceberg::int64()),
          iceberg::SchemaField::MakeOptional(5, "commit_seq", iceberg::int32()),
      },
      0);
}

std::shared_ptr<arrow::Array> int64_array(const int64_t* values, int64_t length) {
  auto data = arrow::ArrayData::Make(
      arrow::int64(), length,
      std::vector<std::shared_ptr<arrow::Buffer>>{
          nullptr, arrow::Buffer::Wrap(values, length)},
      0);
  return arrow::MakeArray(data);
}

std::shared_ptr<arrow::Array> int32_array(const int32_t* values, int64_t length) {
  auto data = arrow::ArrayData::Make(
      arrow::int32(), length,
      std::vector<std::shared_ptr<arrow::Buffer>>{
          nullptr, arrow::Buffer::Wrap(values, length)},
      0);
  return arrow::MakeArray(data);
}

std::shared_ptr<arrow::Array> commit_seq_array(int32_t commit_seq, int64_t length) {
  std::vector<int32_t> values(static_cast<size_t>(length), commit_seq);
  auto data = arrow::ArrayData::Make(
      arrow::int32(), length,
      std::vector<std::shared_ptr<arrow::Buffer>>{
          nullptr, arrow::Buffer::FromVector(std::move(values))},
      0);
  return arrow::MakeArray(data);
}

std::shared_ptr<arrow::RecordBatch> make_primes_batch(const pp_batch_result& batch,
                                                      int32_t commit_seq) {
  auto schema = arrow::schema({
      arrow::field("p", arrow::int64()),
      arrow::field("k", arrow::int32()),
      arrow::field("commit_seq", arrow::int32()),
  });
  int64_t rows = static_cast<int64_t>(batch.prime_count);
  return arrow::RecordBatch::Make(
      schema, rows,
      {int64_array(batch.prime_p, rows), int32_array(batch.prime_k, rows),
       commit_seq_array(commit_seq, rows)});
}

std::shared_ptr<arrow::RecordBatch> make_decomp_batch(const pp_batch_result& batch,
                                                      int32_t commit_seq) {
  auto schema = arrow::schema({
      arrow::field("p", arrow::int64()),
      arrow::field("m_k", arrow::int32()),
      arrow::field("n_k", arrow::int32()),
      arrow::field("q_k", arrow::int64()),
      arrow::field("commit_seq", arrow::int32()),
  });
  int64_t rows = static_cast<int64_t>(batch.decomp_count);
  return arrow::RecordBatch::Make(
      schema, rows,
      {int64_array(batch.decomp_p, rows), int32_array(batch.decomp_m, rows),
       int32_array(batch.decomp_n, rows), int64_array(batch.decomp_q, rows),
       commit_seq_array(commit_seq, rows)});
}

bool write_record_batch(iceberg::Writer& writer,
                        const std::shared_ptr<arrow::RecordBatch>& batch,
                        std::string* error) {
  ArrowArray exported{};
  auto status = arrow::ExportRecordBatch(*batch, &exported);
  if (!status.ok()) {
    *error = status.ToString();
    return false;
  }

  auto write_status = writer.Write(&exported);
  if (exported.release != nullptr) {
    exported.release(&exported);
  }
  if (!write_status.has_value()) {
    *error = write_status.error().message;
    return false;
  }
  return true;
}

std::unordered_map<std::string, std::string> build_metadata(
    const std::string& table, int32_t commit_seq, int64_t p_min, int64_t p_max,
    int64_t rows, int64_t n_primes,
    const std::map<int32_t, int64_t>& k_histogram) {
  std::unordered_map<std::string, std::string> metadata;
  metadata["funbuns.schema_version"] = "1";
  metadata["funbuns.algorithm_version"] = "native";
  metadata["funbuns.algorithm_git_sha"] = "unknown";
  metadata["funbuns.table"] = table;
  metadata["funbuns.commit_seq"] = std::to_string(commit_seq);
  metadata["funbuns.p_min"] = std::to_string(p_min);
  metadata["funbuns.p_max"] = std::to_string(p_max);
  metadata["funbuns.n_rows"] = std::to_string(rows);
  metadata["funbuns.n_primes"] = std::to_string(n_primes);
  metadata["funbuns.generated_at"] = utc_timestamp_iso();
  metadata["funbuns.generator"] = "primeparts-native-iceberg-cpp";
  if (table == "primes") {
    metadata["funbuns.k_histogram"] = k_histogram_json(k_histogram);
  }
  return metadata;
}

bool write_parquet_file(const fs::path& final_path,
                        const std::shared_ptr<iceberg::Schema>& schema,
                        const std::unordered_map<std::string, std::string>& metadata,
                        const std::vector<BatchHolder>& batches, int32_t commit_seq,
                        bool decompositions, WrittenFile* out, std::string* error) {
  fs::create_directories(final_path.parent_path());
  if (fs::exists(final_path)) {
    *error = "refusing to overwrite existing file: " + final_path.string();
    return false;
  }

  fs::path tmp_path = final_path.parent_path() / ("." + final_path.filename().string() + ".tmp");
  fs::remove(tmp_path);

  auto unique_io = iceberg::arrow::MakeLocalFileIO();
  std::shared_ptr<iceberg::FileIO> io(std::move(unique_io));
  iceberg::WriterProperties properties;
  properties.Set(iceberg::WriterProperties::kParquetCompression, std::string("zstd"));
  properties.Set(iceberg::WriterProperties::kParquetCompressionLevel, std::string("3"));

  iceberg::WriterOptions options{
      .path = fs::absolute(tmp_path).string(),
      .schema = schema,
      .io = io,
      .metadata = metadata,
      .properties = properties,
  };

  auto writer_result = iceberg::WriterFactoryRegistry::Open(iceberg::FileFormatType::kParquet,
                                                            options);
  if (!writer_result.has_value()) {
    *error = writer_result.error().message;
    return false;
  }
  auto writer = std::move(writer_result.value());

  for (const auto& holder : batches) {
    if (decompositions) {
      if (holder.batch.decomp_count == 0) {
        continue;
      }
      if (!write_record_batch(*writer, make_decomp_batch(holder.batch, commit_seq), error)) {
        return false;
      }
    } else {
      if (!write_record_batch(*writer, make_primes_batch(holder.batch, commit_seq), error)) {
        return false;
      }
    }
  }

  auto close_status = writer->Close();
  if (!close_status.has_value()) {
    *error = close_status.error().message;
    return false;
  }
  auto length = writer->length();
  if (!length.has_value()) {
    *error = length.error().message;
    return false;
  }

  fs::rename(tmp_path, final_path);
  out->path = final_path;
  out->bytes = length.value();
  return true;
}

bool materialize_group(int64_t* next_idx, int64_t end_idx, const Options& options,
                       FileGroup* group, std::string* error) {
  group->start_idx = *next_idx;
  int64_t remaining_total = end_idx - *next_idx;
  int64_t chunk_count =
      (remaining_total + options.chunk_primes - 1) / options.chunk_primes;
  if (chunk_count > options.chunks_per_file) {
    chunk_count = options.chunks_per_file;
  }

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
  if (worker_count <= 0) {
    worker_count = 1;
  }
  if (worker_count > chunk_count) {
    worker_count = chunk_count;
  }

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
          if (failed || next_chunk >= chunk_count) {
            return;
          }
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
  for (auto& worker : workers) {
    worker.join();
  }
  if (failed) {
    *error = first_error;
    return false;
  }

  for (const auto& holder : group->batches) {
    if (group->first_p == 0 || holder.batch.first_p < group->first_p) {
      group->first_p = holder.batch.first_p;
    }
    if (holder.batch.last_p > group->last_p) {
      group->last_p = holder.batch.last_p;
    }
    group->prime_rows += static_cast<int64_t>(holder.batch.prime_count);
    group->decomp_rows += static_cast<int64_t>(holder.batch.decomp_count);
    group->processed_count += holder.batch.processed_count;
    for (size_t row = 0; row < holder.batch.prime_count; ++row) {
      group->k_histogram[holder.batch.prime_k[row]]++;
      if (holder.batch.prime_k[row] > 0) {
        group->decomp_prime_rows++;
      }
    }
  }

  *next_idx += group->processed_count;
  return true;
}

fs::path table_file_path(const fs::path& warehouse, const std::string& table,
                         int32_t commit_seq) {
  char name[128];
  std::snprintf(name, sizeof(name), "%s_b%06d_000.parquet", table.c_str(), commit_seq);
  return warehouse / "funbuns" / table / "data" /
         ("commit_seq=" + std::to_string(commit_seq)) / name;
}

void append_manifest(std::ofstream& out, const WrittenFile& file) {
  out << "{\"table\":\"" << json_escape(file.table) << "\","
      << "\"path\":\"" << json_escape(fs::absolute(file.path).string()) << "\","
      << "\"commit_seq\":" << file.commit_seq << ","
      << "\"rows\":" << file.rows << ","
      << "\"p_min\":" << file.p_min << ","
      << "\"p_max\":" << file.p_max << ","
      << "\"bytes\":" << file.bytes << "}\n";
}

bool parse_args(int argc, char** argv, Options* options) {
  static const option long_options[] = {
      {"start-idx", required_argument, nullptr, 1000},
      {"count", required_argument, nullptr, 'n'},
      {"chunk-primes", required_argument, nullptr, 'c'},
      {"chunks-per-file", required_argument, nullptr, 1001},
      {"threads", required_argument, nullptr, 1003},
      {"warehouse", required_argument, nullptr, 'w'},
      {"manifest", required_argument, nullptr, 'm'},
      {"temp", no_argument, nullptr, 1004},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "n:c:w:m:t:h", long_options, nullptr)) != -1) {
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
      case 1001:
        if (!parse_i64(optarg, &options->chunks_per_file)) {
          std::fprintf(stderr, "invalid --chunks-per-file: %s\n", optarg);
          return false;
        }
        break;
      case 1003:
      case 't':
        if (!parse_i64(optarg, &options->threads)) {
          std::fprintf(stderr, "invalid --threads: %s\n", optarg);
          return false;
        }
        break;
      case 'w':
        options->warehouse = optarg;
        break;
      case 'm':
        options->manifest = optarg;
        break;
      case 1004:
        options->temp = true;
        break;
      case 'h':
        usage(stdout);
        std::exit(0);
      default:
        return false;
    }
  }

  if (options->start_idx <= 0 || options->count < 0 || options->chunk_primes <= 0 ||
      options->chunks_per_file <= 0 || options->threads < 0) {
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
      options->manifest = temp_root / "native_files.jsonl";
    }
  } else if (options->manifest.empty()) {
    options->manifest = options->warehouse / "native_files.jsonl";
  }
  if (options->threads == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    options->threads = hw == 0 ? 1 : static_cast<int64_t>(hw);
  }
  if (!resolve_commit_seq_start(options)) {
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_args(argc, argv, &options)) {
    return 2;
  }

  std::string error;
  int init_status = pp_init();
  if (init_status != PP_OK) {
    std::cerr << "pp_init failed: " << pp_status_message(init_status) << "\n";
    return 1;
  }

  auto run_start = std::chrono::steady_clock::now();
  try {
    iceberg::parquet::RegisterAll();
    fs::create_directories(options.warehouse);
    fs::create_directories(options.manifest.parent_path());
    std::ofstream manifest(options.manifest, std::ios::out | std::ios::trunc);
    if (!manifest) {
      std::cerr << "failed to open manifest: " << options.manifest << "\n";
      pp_shutdown();
      return 1;
    }

    int64_t next_idx = options.start_idx;
    int64_t end_idx = options.start_idx + options.count;
    int32_t next_commit_seq = options.commit_seq;
    int64_t total_primes = 0;
    int64_t total_decomp = 0;
    int64_t files_written = 0;
    int64_t bytes_written = 0;
    int64_t first_p = 0;
    int64_t last_p = 0;
    bool stop_requested = false;

    auto p_schema = primes_schema();
    auto d_schema = decomp_schema();

    int64_t total_chunks =
        (options.count + options.chunk_primes - 1) / options.chunk_primes;
    int64_t total_groups =
        (total_chunks + options.chunks_per_file - 1) / options.chunks_per_file;
    Progress progress(total_groups, options.count);
    StopMonitor stop_monitor;
    int64_t groups_done = 0;
    progress.update(0, 0);

    while (next_idx < end_idx) {
      FileGroup group;
      if (!materialize_group(&next_idx, end_idx, options, &group, &error)) {
        std::cerr << "materialize failed: " << error << "\n";
        pp_shutdown();
        return 1;
      }

      int32_t commit_seq = next_commit_seq++;
      if (first_p == 0 || group.first_p < first_p) {
        first_p = group.first_p;
      }
      if (group.last_p > last_p) {
        last_p = group.last_p;
      }

      WrittenFile primes_file{
          .table = "primes",
          .path = table_file_path(options.warehouse, "primes", commit_seq),
          .commit_seq = commit_seq,
          .rows = group.prime_rows,
          .p_min = group.first_p,
          .p_max = group.last_p,
      };
      auto primes_metadata = build_metadata("primes", commit_seq, group.first_p,
                                            group.last_p, group.prime_rows,
                                            group.prime_rows, group.k_histogram);
      if (!write_parquet_file(primes_file.path, p_schema, primes_metadata, group.batches,
                              commit_seq, false, &primes_file, &error)) {
        std::cerr << "write primes failed: " << error << "\n";
        pp_shutdown();
        return 1;
      }
      append_manifest(manifest, primes_file);
      files_written++;
      bytes_written += primes_file.bytes;

      if (group.decomp_rows > 0) {
        WrittenFile decomp_file{
            .table = "decompositions",
            .path = table_file_path(options.warehouse, "decompositions", commit_seq),
            .commit_seq = commit_seq,
            .rows = group.decomp_rows,
            .p_min = group.first_p,
            .p_max = group.last_p,
        };
        auto decomp_metadata = build_metadata("decompositions", commit_seq, group.first_p,
                                              group.last_p, group.decomp_rows,
                                              group.decomp_prime_rows,
                                              group.k_histogram);
        if (!write_parquet_file(decomp_file.path, d_schema, decomp_metadata, group.batches,
                                commit_seq, true, &decomp_file, &error)) {
          std::cerr << "write decompositions failed: " << error << "\n";
          pp_shutdown();
          return 1;
        }
        append_manifest(manifest, decomp_file);
        files_written++;
        bytes_written += decomp_file.bytes;
      }

      total_primes += group.prime_rows;
      total_decomp += group.decomp_rows;
      groups_done++;
      progress.update(groups_done, total_primes);

      manifest.flush();
      if (!manifest) {
        std::cerr << "failed to flush manifest: " << options.manifest << "\n";
        pp_shutdown();
        return 1;
      }
      if (stop_monitor.stop_requested()) {
        stop_requested = true;
        break;
      }
    }
    progress.finish();

    manifest.close();
    auto run_end = std::chrono::steady_clock::now();
    double elapsed_s =
        std::chrono::duration<double>(run_end - run_start).count();
    double primes_per_s =
        elapsed_s > 0.0 ? static_cast<double>(total_primes) / elapsed_s : 0.0;
    std::cout << "{"
              << "\"warehouse\":\"" << json_escape(fs::absolute(options.warehouse).string())
              << "\","
              << "\"manifest\":\"" << json_escape(fs::absolute(options.manifest).string())
              << "\","
              << "\"start_idx\":" << options.start_idx << ","
              << "\"count\":" << options.count << ","
              << "\"prime_rows\":" << total_primes << ","
              << "\"decomp_rows\":" << total_decomp << ","
              << "\"files_written\":" << files_written << ","
              << "\"bytes_written\":" << bytes_written << ","
              << "\"first_p\":" << first_p << ","
              << "\"last_p\":" << last_p << ","
              << "\"stop_requested\":" << (stop_requested ? "true" : "false") << ","
              << "\"elapsed_s\":" << elapsed_s << ","
              << "\"primes_per_s\":" << primes_per_s << "}\n";
  } catch (const std::exception& exc) {
    std::cerr << "native writer failed: " << exc.what() << "\n";
    pp_shutdown();
    return 1;
  }

  pp_shutdown();
  return 0;
}
