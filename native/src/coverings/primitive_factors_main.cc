#include "primeparts/primitive_factors.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/source_scan.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/io/type_fwd.h>
#include <arrow/util/thread_pool.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <parquet/statistics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <flint/ulong_extras.h>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kReaderBatchSize = 1 << 20;
const fs::path kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";
const fs::path kDefaultOutDir =
    "/media/extssd/research/dioph.pp/data/primitive-factors/k0-v1";

int DefaultWorkerThreads() {
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) return 6;
  return std::max(1, std::min(6, static_cast<int>(hw)));
}

struct Options {
  fs::path metadata;  // optional override; else resolved from the catalog
  fs::path warehouse = kDefaultWarehouse;
  fs::path out_dir = kDefaultOutDir;
  fs::path partials_dir;
  int threads = DefaultWorkerThreads();
  int arrow_threads = 0;
  int32_t p_bucket = -1;
  int64_t limit_k0 = 0;
  int64_t max_files = 0;
  int64_t flush_entries = 250000;
  int64_t sample_limit = 1000;
  bool progress = true;
};

struct CountPair {
  int64_t remainder_count = 0;
  int64_t p_count = 0;
};

struct IntQKey {
  int32_t value = 0;
  uint64_t q = 0;

  bool operator==(const IntQKey& other) const {
    return value == other.value && q == other.q;
  }
};

struct IntQKeyHash {
  size_t operator()(const IntQKey& k) const {
    const uint64_t mixed = static_cast<uint64_t>(k.value) * 0x9e3779b185ebca87ULL;
    return std::hash<uint64_t>{}(mixed ^ (k.q + 0x517cc1b727220a95ULL));
  }
};

struct SampleRow {
  int64_t p = 0;
  int32_t m = 0;
  int64_t q = 0;
  int32_t exponent = 0;
  int32_t p_bucket = -1;
  int64_t remainder = 0;
};

struct Stats {
  int64_t files = 0;
  int64_t rows = 0;
  int64_t k0_primes = 0;
  int64_t max_m_terms = 0;
  int64_t backbone_covered = 0;
  int64_t candidate_remainders = 0;
  int64_t primitive_events = 0;
  int64_t known_factor_hits = 0;
  int64_t residual_factorizations = 0;
  int64_t residual_prime_hits = 0;
  std::unordered_map<uint64_t, CountPair> factors;
  std::unordered_map<IntQKey, CountPair, IntQKeyHash> by_m;
  std::unordered_map<IntQKey, CountPair, IntQKeyHash> by_bucket;
  std::vector<SampleRow> samples;
};

struct ProgressState {
  int64_t total_rows = 0;
  std::atomic<int64_t> files{0};
  std::atomic<int64_t> rows{0};
  std::atomic<int64_t> k0_primes{0};
  std::atomic<int64_t> backbone_covered{0};
  std::atomic<int64_t> candidate_remainders{0};
  std::atomic<int64_t> primitive_events{0};
  std::atomic<int64_t> known_factor_hits{0};
  std::atomic<int64_t> residual_factorizations{0};
  std::atomic<int64_t> residual_prime_hits{0};
  std::atomic<int64_t> partial_flushes{0};
  std::atomic<bool> done{false};
  std::chrono::steady_clock::time_point started;
};

void Usage(const char* argv0) {
  std::fprintf(
      stderr,
      "usage: %s [--metadata PATH] [--out-dir PATH] [--threads N]\n"
      "          [--arrow-threads N] [--p-bucket N] [--limit-k0 N]\n"
      "          [--max-files N] [--flush-entries N] [--partials-dir PATH]\n"
      "          [--sample-limit N] [--no-partial-flush] [--no-progress]\n\n"
      "Scans primeparts.primes k=0 rows from the staging Iceberg metadata,\n"
      "skips m covered by {3,5,7}, and counts primitive factors of p - 2^m.\n",
      argv0);
}

bool ParseI64(const char* s, int64_t* out) {
  char* end = nullptr;
  long long v = std::strtoll(s, &end, 10);
  if (end == s || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool ParseOptions(int argc, char** argv, Options* opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--metadata") {
      const char* v = need_value("--metadata");
      if (!v) return false;
      opts->metadata = v;
    } else if (arg == "--out-dir") {
      const char* v = need_value("--out-dir");
      if (!v) return false;
      opts->out_dir = v;
    } else if (arg == "--threads") {
      const char* v = need_value("--threads");
      int64_t parsed = 0;
      if (!v || !ParseI64(v, &parsed) || parsed <= 0 || parsed > 1024) {
        std::fprintf(stderr, "invalid --threads: %s\n", v ? v : "");
        return false;
      }
      opts->threads = static_cast<int>(parsed);
    } else if (arg == "--arrow-threads") {
      const char* v = need_value("--arrow-threads");
      int64_t parsed = 0;
      if (!v || !ParseI64(v, &parsed) || parsed <= 0 || parsed > 1024) {
        std::fprintf(stderr, "invalid --arrow-threads: %s\n", v ? v : "");
        return false;
      }
      opts->arrow_threads = static_cast<int>(parsed);
    } else if (arg == "--p-bucket") {
      const char* v = need_value("--p-bucket");
      int64_t parsed = 0;
      if (!v || !ParseI64(v, &parsed) || parsed < 0 ||
          parsed > INT32_MAX) {
        std::fprintf(stderr, "invalid --p-bucket: %s\n", v ? v : "");
        return false;
      }
      opts->p_bucket = static_cast<int32_t>(parsed);
    } else if (arg == "--limit-k0") {
      const char* v = need_value("--limit-k0");
      if (!v || !ParseI64(v, &opts->limit_k0) || opts->limit_k0 < 0) {
        std::fprintf(stderr, "invalid --limit-k0: %s\n", v ? v : "");
        return false;
      }
    } else if (arg == "--max-files") {
      const char* v = need_value("--max-files");
      if (!v || !ParseI64(v, &opts->max_files) || opts->max_files < 0) {
        std::fprintf(stderr, "invalid --max-files: %s\n", v ? v : "");
        return false;
      }
    } else if (arg == "--flush-entries") {
      const char* v = need_value("--flush-entries");
      if (!v || !ParseI64(v, &opts->flush_entries) || opts->flush_entries < 0) {
        std::fprintf(stderr, "invalid --flush-entries: %s\n", v ? v : "");
        return false;
      }
    } else if (arg == "--partials-dir") {
      const char* v = need_value("--partials-dir");
      if (!v) return false;
      opts->partials_dir = v;
    } else if (arg == "--sample-limit") {
      const char* v = need_value("--sample-limit");
      if (!v || !ParseI64(v, &opts->sample_limit) || opts->sample_limit < 0) {
        std::fprintf(stderr, "invalid --sample-limit: %s\n", v ? v : "");
        return false;
      }
    } else if (arg == "--no-partial-flush") {
      opts->flush_entries = 0;
    } else if (arg == "--no-progress") {
      opts->progress = false;
    } else if (arg == "--help" || arg == "-h") {
      Usage(argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return false;
    }
  }
  if (opts->threads <= 0) opts->threads = 1;
  if (opts->arrow_threads <= 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    const int available = hw == 0 ? opts->threads : static_cast<int>(hw);
    opts->arrow_threads =
        std::max(1, std::min(DefaultWorkerThreads(), available - opts->threads));
  }
  if (opts->partials_dir.empty()) {
    opts->partials_dir = opts->out_dir / "partial_aggregates";
  }
  return true;
}

int32_t ParseBucketFromPath(const std::string& path) {
  const std::string marker = "/p_bucket=";
  const size_t pos = path.rfind(marker);
  if (pos == std::string::npos) return -1;
  size_t start = pos + marker.size();
  size_t end = start;
  while (end < path.size() && path[end] >= '0' && path[end] <= '9') end++;
  if (end == start) return -1;
  return static_cast<int32_t>(std::strtol(path.substr(start, end - start).c_str(),
                                         nullptr, 10));
}

int FloorLog2(uint64_t x) {
  return 63 - __builtin_clzll(x);
}

void MergeCounts(std::unordered_map<uint64_t, CountPair>* dst,
                 const std::unordered_map<uint64_t, CountPair>& src) {
  for (const auto& [k, v] : src) {
    auto& out = (*dst)[k];
    out.remainder_count += v.remainder_count;
    out.p_count += v.p_count;
  }
}

void MergeCounts(std::unordered_map<IntQKey, CountPair, IntQKeyHash>* dst,
                 const std::unordered_map<IntQKey, CountPair, IntQKeyHash>& src) {
  for (const auto& [k, v] : src) {
    auto& out = (*dst)[k];
    out.remainder_count += v.remainder_count;
    out.p_count += v.p_count;
  }
}

void MergeStats(Stats* dst, const Stats& src, int64_t sample_limit) {
  dst->files += src.files;
  dst->rows += src.rows;
  dst->k0_primes += src.k0_primes;
  dst->max_m_terms += src.max_m_terms;
  dst->backbone_covered += src.backbone_covered;
  dst->candidate_remainders += src.candidate_remainders;
  dst->primitive_events += src.primitive_events;
  dst->known_factor_hits += src.known_factor_hits;
  dst->residual_factorizations += src.residual_factorizations;
  dst->residual_prime_hits += src.residual_prime_hits;
  MergeCounts(&dst->factors, src.factors);
  MergeCounts(&dst->by_m, src.by_m);
  MergeCounts(&dst->by_bucket, src.by_bucket);
  for (const auto& sample : src.samples) {
    if (static_cast<int64_t>(dst->samples.size()) >= sample_limit) break;
    dst->samples.push_back(sample);
  }
}

size_t AggregateEntryCount(const Stats& stats) {
  return stats.factors.size() + stats.by_m.size() + stats.by_bucket.size();
}

bool HasAggregateEntries(const Stats& stats) {
  return !stats.factors.empty() || !stats.by_m.empty() || !stats.by_bucket.empty();
}

void ClearAggregates(Stats* stats) {
  stats->factors.clear();
  stats->by_m.clear();
  stats->by_bucket.clear();
}

void AddPrimitiveFactorHit(Stats* stats, uint64_t p, int32_t m, uint64_t q,
                           int32_t exponent, int32_t p_bucket,
                           uint64_t remainder, int64_t sample_limit) {
  stats->primitive_events++;
  stats->factors[q].remainder_count++;
  stats->factors[q].p_count++;
  stats->by_m[IntQKey{m, q}].remainder_count++;
  stats->by_m[IntQKey{m, q}].p_count++;
  stats->by_bucket[IntQKey{p_bucket, q}].remainder_count++;
  stats->by_bucket[IntQKey{p_bucket, q}].p_count++;
  if (static_cast<int64_t>(stats->samples.size()) < sample_limit) {
    stats->samples.push_back(
        {static_cast<int64_t>(p), m, static_cast<int64_t>(q), exponent,
         p_bucket, static_cast<int64_t>(remainder)});
  }
}

void FactorResidual(uint64_t residual, uint64_t p, int32_t m,
                    int32_t p_bucket, uint64_t original_remainder,
                    int64_t sample_limit, const primeparts::MersenneHelper& helper,
                    Stats* stats) {
  if (residual <= 1) return;
  if (n_is_prime(static_cast<ulong>(residual))) {
    stats->residual_prime_hits++;
    if (helper.ord2_by_q.contains(residual)) {
      stats->known_factor_hits++;
    }
    if (primeparts::IsPrimitiveFactor(p, m, residual, helper)) {
      AddPrimitiveFactorHit(stats, p, m, residual, 1, p_bucket,
                            original_remainder, sample_limit);
    }
    return;
  }

  stats->residual_factorizations++;
  n_factor_t factors;
  n_factor_init(&factors);
  n_factor(&factors, static_cast<ulong>(residual), 1);
  for (int i = 0; i < factors.num; ++i) {
    const uint64_t q = static_cast<uint64_t>(factors.p[i]);
    if (q == 3 || q == 5 || q == 7) continue;
    if (helper.ord2_by_q.contains(q)) {
      stats->known_factor_hits++;
    }
    if (!primeparts::IsPrimitiveFactor(p, m, q, helper)) continue;
    AddPrimitiveFactorHit(stats, p, m, q, static_cast<int32_t>(factors.exp[i]),
                          p_bucket, original_remainder, sample_limit);
  }
}

void RenderProgress(const ProgressState& progress, size_t total_files) {
  const int64_t rows = progress.rows.load();
  const int64_t total = progress.total_rows;
  const double pct = total > 0
                         ? 100.0 * static_cast<double>(rows) /
                               static_cast<double>(total)
                         : 0.0;
  const int width = 36;
  const int filled = total > 0
                         ? std::min(width, static_cast<int>(pct * width / 100.0))
                         : 0;
  std::string bar(width, '.');
  for (int i = 0; i < filled; ++i) bar[static_cast<size_t>(i)] = '#';

  const double elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - progress.started).count();
  const double rate = elapsed > 0.0 ? static_cast<double>(rows) / elapsed : 0.0;
  const double eta = (rate > 0.0 && total > rows)
                         ? static_cast<double>(total - rows) / rate
                         : 0.0;

  std::fprintf(stderr,
               "\r[%s] %6.2f%% rows=%" PRId64 "/%" PRId64
               " files=%" PRId64 "/%zu k0=%" PRId64
               " backbone=%" PRId64 " cand=%" PRId64 " prim=%" PRId64
               " known=%" PRId64 " fact=%" PRId64
               " flush=%" PRId64 " rate=%.1fM/s eta=%.0fs\x1B[K",
               bar.c_str(), pct, rows, total, progress.files.load(),
               total_files, progress.k0_primes.load(),
               progress.backbone_covered.load(),
               progress.candidate_remainders.load(),
               progress.primitive_events.load(),
               progress.known_factor_hits.load(),
               progress.residual_factorizations.load(),
               progress.partial_flushes.load(), rate / 1.0e6, eta);
  std::fflush(stderr);
}

void ProgressLoop(ProgressState* progress, size_t total_files) {
  while (!progress->done.load()) {
    RenderProgress(*progress, total_files);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  RenderProgress(*progress, total_files);
  std::fprintf(stderr, "\r");
}

std::unique_ptr<parquet::arrow::FileReader> OpenParquetFile(
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
  auto st = builder.Open(file_r.ValueOrDie(), reader_props);
  if (!st.ok()) {
    *error = "FileReaderBuilder::Open " + path + ": " + st.ToString();
    return nullptr;
  }
  builder.properties(arrow_props);
  std::unique_ptr<parquet::arrow::FileReader> reader;
  st = builder.Build(&reader);
  if (!st.ok()) {
    *error = "FileReaderBuilder::Build " + path + ": " + st.ToString();
    return nullptr;
  }
  return reader;
}

bool ResolveColumnIndices(const parquet::SchemaDescriptor* schema,
                          const std::vector<std::string>& names,
                          std::vector<int>* out, std::string* error) {
  out->clear();
  out->reserve(names.size());
  for (const auto& name : names) {
    int idx = schema->ColumnIndex(name);
    if (idx < 0) {
      *error = "column not found in source file: " + name;
      return false;
    }
    out->push_back(idx);
  }
  return true;
}

bool WriteFactorFrequencies(const fs::path& path, const Stats& stats,
                            std::string* error);
bool WriteIntQCounts(const fs::path& path, const char* first_name,
                     const std::unordered_map<IntQKey, CountPair, IntQKeyHash>& rows,
                     std::string* error);

std::string PaddedSeq(int64_t seq) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06" PRId64, seq);
  return std::string(buf);
}

bool FlushAggregates(const Options& opts, Stats* stats,
                     std::atomic<int64_t>* partial_seq,
                     std::atomic<int64_t>* partial_files,
                     ProgressState* progress, std::string* error) {
  if (opts.flush_entries <= 0 || !HasAggregateEntries(*stats)) return true;
  const int64_t seq = partial_seq->fetch_add(1);
  const std::string tag = PaddedSeq(seq);
  const fs::path base = opts.partials_dir;
  std::error_code ec;
  fs::create_directories(base, ec);
  if (ec) {
    *error = "create partials dir " + base.string() + ": " + ec.message();
    return false;
  }
  if (!WriteFactorFrequencies(base / ("partial_" + tag + "_factor_frequencies.parquet"),
                              *stats, error)) return false;
  if (!WriteIntQCounts(base / ("partial_" + tag + "_factor_by_m.parquet"), "m",
                       stats->by_m, error)) return false;
  if (!WriteIntQCounts(base / ("partial_" + tag + "_factor_by_bucket.parquet"),
                       "p_bucket", stats->by_bucket, error)) return false;
  ClearAggregates(stats);
  partial_files->fetch_add(3);
  if (progress) progress->partial_flushes.fetch_add(1);
  return true;
}

void ProcessPrime(uint64_t p, int32_t p_bucket, const primeparts::MersenneHelper& helper,
                  Stats* stats, int64_t sample_limit, ProgressState* progress) {
  const int max_m = FloorLog2(p);
  int64_t backbone = 0;
  int64_t candidates = 0;
  int64_t events = 0;
  int64_t known_hits = 0;
  int64_t residual_fact = 0;
  int64_t residual_prime = 0;
  stats->max_m_terms += max_m;
  for (int32_t m = 1; m <= max_m; ++m) {
    if (primeparts::IsBackboneCovered(p, m)) {
      stats->backbone_covered++;
      backbone++;
      continue;
    }
    stats->candidate_remainders++;
    candidates++;

    const uint64_t remainder = p - (uint64_t{1} << m);
    const int64_t primitive_before = stats->primitive_events;
    const int64_t known_before = stats->known_factor_hits;
    const int64_t fact_before = stats->residual_factorizations;
    const int64_t prime_before = stats->residual_prime_hits;

    FactorResidual(remainder, p, m, p_bucket, remainder, sample_limit, helper,
                   stats);

    events += stats->primitive_events - primitive_before;
    known_hits += stats->known_factor_hits - known_before;
    residual_fact += stats->residual_factorizations - fact_before;
    residual_prime += stats->residual_prime_hits - prime_before;
  }

  if (progress) {
    progress->backbone_covered.fetch_add(backbone);
    progress->candidate_remainders.fetch_add(candidates);
    progress->primitive_events.fetch_add(events);
    progress->known_factor_hits.fetch_add(known_hits);
    progress->residual_factorizations.fetch_add(residual_fact);
    progress->residual_prime_hits.fetch_add(residual_prime);
  }
}

bool ProcessFile(const primeparts::SourceFileInfo& file, Options opts,
                 std::atomic<int64_t>* claimed_k0, Stats* stats,
                 const primeparts::MersenneHelper& helper,
                 std::atomic<int64_t>* partial_seq,
                 std::atomic<int64_t>* partial_files,
                 ProgressState* progress, std::string* error) {
  stats->files++;
  const int32_t p_bucket = ParseBucketFromPath(file.path);
  auto reader = OpenParquetFile(file.path, error);
  if (!reader) return false;
  auto md = reader->parquet_reader()->metadata();
  std::vector<int> col_indices;
  if (!ResolveColumnIndices(md->schema(), {"p", "k"}, &col_indices, error)) {
    return false;
  }
  std::vector<int> row_groups;
  row_groups.reserve(md->num_row_groups());
  for (int rg = 0; rg < md->num_row_groups(); ++rg) {
    auto rg_md = md->RowGroup(rg);
    auto k_stats = rg_md->ColumnChunk(col_indices[1])->statistics();
    if (k_stats && k_stats->HasMinMax()) {
      auto s32 = std::dynamic_pointer_cast<parquet::Int32Statistics>(k_stats);
      if (s32 && (s32->min() > 0 || s32->max() < 0)) continue;
    }
    row_groups.push_back(rg);
  }

  if (row_groups.empty()) {
    if (progress) progress->files.fetch_add(1);
    return true;
  }

  auto rbr_r = reader->GetRecordBatchReader(row_groups, col_indices);
  if (!rbr_r.ok()) {
    *error = "GetRecordBatchReader " + file.path + ": " + rbr_r.status().ToString();
    return false;
  }
  auto rbr = std::move(rbr_r).ValueOrDie();

  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    auto st = rbr->ReadNext(&batch);
    if (!st.ok()) {
      *error = "ReadNext " + file.path + ": " + st.ToString();
      return false;
    }
    if (!batch) break;
    stats->rows += batch->num_rows();
    if (progress) progress->rows.fetch_add(batch->num_rows());
    auto p_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
    auto k_arr = std::static_pointer_cast<arrow::Int32Array>(batch->column(1));
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      if (k_arr->Value(i) != 0) continue;
      if (opts.limit_k0 > 0) {
        const int64_t claim = claimed_k0->fetch_add(1);
        if (claim >= opts.limit_k0) {
          if (!FlushAggregates(opts, stats, partial_seq, partial_files,
                               progress, error)) {
            return false;
          }
          if (progress) progress->files.fetch_add(1);
          return true;
        }
      } else {
        claimed_k0->fetch_add(1);
      }
      stats->k0_primes++;
      if (progress) progress->k0_primes.fetch_add(1);
      ProcessPrime(static_cast<uint64_t>(p_arr->Value(i)), p_bucket, helper, stats,
                   opts.sample_limit, progress);
    }
    if (opts.flush_entries > 0 &&
        AggregateEntryCount(*stats) >= static_cast<size_t>(opts.flush_entries) &&
        !FlushAggregates(opts, stats, partial_seq, partial_files, progress, error)) {
      return false;
    }
  }
  if (!FlushAggregates(opts, stats, partial_seq, partial_files, progress, error)) {
    return false;
  }
  if (progress) progress->files.fetch_add(1);
  return true;
}

template <typename Builder>
std::shared_ptr<arrow::Array> FinishOrThrow(Builder* builder) {
  std::shared_ptr<arrow::Array> out;
  auto st = builder->Finish(&out);
  if (!st.ok()) throw std::runtime_error(st.ToString());
  return out;
}

bool WriteTable(const fs::path& path, const std::shared_ptr<arrow::Table>& table,
                std::string* error) {
  auto out_r = arrow::io::FileOutputStream::Open(path);
  if (!out_r.ok()) {
    *error = "open output " + path.string() + ": " + out_r.status().ToString();
    return false;
  }
  parquet::WriterProperties::Builder builder;
  builder.compression(parquet::Compression::ZSTD);
  builder.compression_level(3);
  auto props = builder.build();
  auto st = parquet::arrow::WriteTable(
      *table, arrow::default_memory_pool(), out_r.ValueOrDie(), 1 << 20, props);
  if (!st.ok()) {
    *error = "write " + path.string() + ": " + st.ToString();
    return false;
  }
  return true;
}

bool WriteFactorFrequencies(const fs::path& path, const Stats& stats,
                            std::string* error) {
  arrow::Int64Builder q, rem, p;
  std::vector<std::pair<uint64_t, CountPair>> rows(stats.factors.begin(),
                                                   stats.factors.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if (a.second.remainder_count != b.second.remainder_count) {
      return a.second.remainder_count > b.second.remainder_count;
    }
    return a.first < b.first;
  });
  for (const auto& [factor, counts] : rows) {
    if (!q.Append(static_cast<int64_t>(factor)).ok() ||
        !rem.Append(counts.remainder_count).ok() ||
        !p.Append(counts.p_count).ok()) {
      *error = "append factor_frequencies row failed";
      return false;
    }
  }
  auto schema = arrow::schema({
      arrow::field("q", arrow::int64()),
      arrow::field("primitive_remainder_count", arrow::int64()),
      arrow::field("primitive_p_count", arrow::int64()),
  });
  return WriteTable(path,
                    arrow::Table::Make(schema, {FinishOrThrow(&q),
                                                FinishOrThrow(&rem),
                                                FinishOrThrow(&p)}),
                    error);
}

bool WriteIntQCounts(const fs::path& path, const char* first_name,
                     const std::unordered_map<IntQKey, CountPair, IntQKeyHash>& rows,
                     std::string* error) {
  arrow::Int32Builder first;
  arrow::Int64Builder q, rem, p;
  std::vector<std::pair<IntQKey, CountPair>> ordered(rows.begin(), rows.end());
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    if (a.first.value != b.first.value) return a.first.value < b.first.value;
    return a.first.q < b.first.q;
  });
  for (const auto& [key, counts] : ordered) {
    if (!first.Append(key.value).ok() ||
        !q.Append(static_cast<int64_t>(key.q)).ok() ||
        !rem.Append(counts.remainder_count).ok() ||
        !p.Append(counts.p_count).ok()) {
      *error = std::string("append ") + path.string() + " row failed";
      return false;
    }
  }
  auto schema = arrow::schema({
      arrow::field(first_name, arrow::int32()),
      arrow::field("q", arrow::int64()),
      arrow::field("primitive_remainder_count", arrow::int64()),
      arrow::field("primitive_p_count", arrow::int64()),
  });
  return WriteTable(path,
                    arrow::Table::Make(schema, {FinishOrThrow(&first),
                                                FinishOrThrow(&q),
                                                FinishOrThrow(&rem),
                                                FinishOrThrow(&p)}),
                    error);
}

bool WriteSamples(const fs::path& path, const Stats& stats, std::string* error) {
  arrow::Int64Builder p, q, remainder;
  arrow::Int32Builder m, exponent, bucket;
  for (const auto& row : stats.samples) {
    if (!p.Append(row.p).ok() || !m.Append(row.m).ok() ||
        !q.Append(row.q).ok() || !exponent.Append(row.exponent).ok() ||
        !bucket.Append(row.p_bucket).ok() || !remainder.Append(row.remainder).ok()) {
      *error = "append sample row failed";
      return false;
    }
  }
  auto schema = arrow::schema({
      arrow::field("p", arrow::int64()),
      arrow::field("m", arrow::int32()),
      arrow::field("q", arrow::int64()),
      arrow::field("exponent", arrow::int32()),
      arrow::field("p_bucket", arrow::int32()),
      arrow::field("remainder", arrow::int64()),
  });
  return WriteTable(path,
                    arrow::Table::Make(schema, {FinishOrThrow(&p),
                                                FinishOrThrow(&m),
                                                FinishOrThrow(&q),
                                                FinishOrThrow(&exponent),
                                                FinishOrThrow(&bucket),
                                                FinishOrThrow(&remainder)}),
                    error);
}

bool WriteMersenneFactors(const fs::path& path,
                          const primeparts::MersenneHelper& helper,
                          std::string* error) {
  arrow::Int32Builder d, exponent;
  arrow::Int64Builder q;
  for (const auto& row : helper.factors) {
    if (!d.Append(row.d).ok() || !q.Append(static_cast<int64_t>(row.q)).ok() ||
        !exponent.Append(row.exponent).ok()) {
      *error = "append mersenne factor row failed";
      return false;
    }
  }
  auto schema = arrow::schema({
      arrow::field("d", arrow::int32()),
      arrow::field("q", arrow::int64()),
      arrow::field("exponent_in_mersenne", arrow::int32()),
  });
  return WriteTable(path,
                    arrow::Table::Make(schema, {FinishOrThrow(&d),
                                                FinishOrThrow(&q),
                                                FinishOrThrow(&exponent)}),
                    error);
}

bool WriteMersenneOrders(const fs::path& path,
                         const primeparts::MersenneHelper& helper,
                         std::string* error) {
  arrow::Int64Builder q;
  arrow::Int32Builder ord2;
  std::vector<std::pair<uint64_t, int32_t>> rows(helper.ord2_by_q.begin(),
                                                 helper.ord2_by_q.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second < b.second;
    return a.first < b.first;
  });
  for (const auto& [factor, order] : rows) {
    if (!q.Append(static_cast<int64_t>(factor)).ok() || !ord2.Append(order).ok()) {
      *error = "append mersenne order row failed";
      return false;
    }
  }
  auto schema = arrow::schema({
      arrow::field("q", arrow::int64()),
      arrow::field("ord2", arrow::int32()),
  });
  return WriteTable(path,
                    arrow::Table::Make(schema, {FinishOrThrow(&q),
                                                FinishOrThrow(&ord2)}),
                    error);
}

bool WriteOutputs(const Options& opts, const Stats& stats,
                  const fs::path& metadata, int64_t manifest_rows,
                  const primeparts::MersenneHelper& helper,
                  int64_t partial_files, double elapsed_s, std::string* error) {
  std::error_code ec;
  fs::create_directories(opts.out_dir, ec);
  if (ec) {
    *error = "create output dir " + opts.out_dir.string() + ": " + ec.message();
    return false;
  }

  const bool partial_mode = opts.flush_entries > 0;
  if (!partial_mode) {
    if (!WriteFactorFrequencies(opts.out_dir / "factor_frequencies.parquet", stats,
                                error)) return false;
    if (!WriteIntQCounts(opts.out_dir / "factor_by_m.parquet", "m", stats.by_m,
                         error)) return false;
    if (!WriteIntQCounts(opts.out_dir / "factor_by_bucket.parquet", "p_bucket",
                         stats.by_bucket, error)) return false;
  }
  if (!WriteSamples(opts.out_dir / "samples.parquet", stats, error)) return false;
  if (!WriteMersenneFactors(opts.out_dir / "mersenne_factors.parquet", helper,
                            error)) return false;
  if (!WriteMersenneOrders(opts.out_dir / "mersenne_factor_orders.parquet", helper,
                           error)) return false;

  nlohmann::json summary = {
      {"metadata", metadata.string()},
      {"manifest_total_records", manifest_rows},
      {"files_processed", stats.files},
      {"rows_scanned", stats.rows},
      {"k0_primes", stats.k0_primes},
      {"max_m_terms", stats.max_m_terms},
      {"backbone_covered_terms", stats.backbone_covered},
      {"candidate_remainders", stats.candidate_remainders},
      {"primitive_events", stats.primitive_events},
      {"known_factor_hits", stats.known_factor_hits},
      {"residual_factorizations", stats.residual_factorizations},
      {"residual_prime_hits", stats.residual_prime_hits},
      {"distinct_primitive_factors", partial_mode ? nlohmann::json(nullptr)
                                                  : nlohmann::json(stats.factors.size())},
      {"aggregate_mode", partial_mode ? "partial_parquet" : "in_memory_final"},
      {"partial_flush_entry_target", opts.flush_entries},
      {"partial_aggregate_dir", partial_mode ? opts.partials_dir.string() : ""},
      {"partial_aggregate_files", partial_files},
      {"mersenne_helper_max_d", helper.max_d},
      {"mersenne_factor_rows", helper.factors.size()},
      {"mersenne_factor_order_rows", helper.ord2_by_q.size()},
      {"threads", opts.threads},
      {"arrow_threads", opts.arrow_threads},
      {"limit_k0", opts.limit_k0},
      {"max_files", opts.max_files},
      {"elapsed_seconds", elapsed_s},
  };
  std::ofstream out(opts.out_dir / "summary.json");
  if (!out) {
    *error = "open summary.json failed";
    return false;
  }
  out << summary.dump(2) << "\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) {
    Usage(argv[0]);
    return 2;
  }
  if (opts.metadata.empty()) {  // resolve through the catalog, not the filesystem
    std::string cerr;
    auto cat = primeparts::catalog::MakeLocalCatalog(opts.warehouse, &cerr);
    if (!cat) {
      std::fprintf(stderr, "MakeLocalCatalog: %s\n", cerr.c_str());
      return 1;
    }
    fs::path mp = primeparts::catalog::TableMetadataPath(cat, "primes", &cerr);
    if (mp.empty()) {
      std::fprintf(stderr, "resolve primes metadata: %s\n", cerr.c_str());
      return 1;
    }
    opts.metadata = mp;
  }
  auto cpu_st = arrow::SetCpuThreadPoolCapacity(opts.arrow_threads);
  if (!cpu_st.ok()) {
    std::fprintf(stderr, "set Arrow CPU pool: %s\n", cpu_st.ToString().c_str());
    return 1;
  }
  auto io_st = arrow::io::SetIOThreadPoolCapacity(opts.arrow_threads);
  if (!io_st.ok()) {
    std::fprintf(stderr, "set Arrow IO pool: %s\n", io_st.ToString().c_str());
    return 1;
  }
  if (opts.flush_entries > 0) {
    std::error_code ec;
    fs::create_directories(opts.partials_dir, ec);
    if (ec) {
      std::fprintf(stderr, "create partials dir %s: %s\n",
                   opts.partials_dir.c_str(), ec.message().c_str());
      return 1;
    }
  }

  std::string error;
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      opts.metadata, {"p", "k"}, nullptr, &error);
  if (!reader) {
    std::fprintf(stderr, "open metadata: %s\n", error.c_str());
    return 1;
  }
  auto files = reader->source_files();
  const int64_t manifest_rows = reader->total_records();
  reader.reset();

  if (opts.p_bucket >= 0) {
    std::vector<primeparts::SourceFileInfo> filtered;
    filtered.reserve(files.size());
    for (const auto& file : files) {
      if (ParseBucketFromPath(file.path) == opts.p_bucket) {
        filtered.push_back(file);
      }
    }
    files = std::move(filtered);
  }
  if (opts.max_files > 0 && opts.max_files < static_cast<int64_t>(files.size())) {
    files.resize(static_cast<size_t>(opts.max_files));
  }
  if (files.empty()) {
    std::fprintf(stderr, "metadata planned no source files\n");
    return 1;
  }

  std::fprintf(stderr, "metadata=%s\n", opts.metadata.c_str());
  int64_t selected_rows = 0;
  int64_t selected_p_max = 0;
  for (const auto& file : files) {
    selected_rows += file.record_count;
    if (file.p_max > selected_p_max) selected_p_max = file.p_max;
  }
  const int32_t helper_max_d =
      selected_p_max > 0 ? static_cast<int32_t>(FloorLog2(selected_p_max)) : 0;
  auto helper = primeparts::BuildMersenneHelper(helper_max_d);
  std::fprintf(stderr,
               "files=%zu selected_rows=%" PRId64
               " manifest_rows=%" PRId64
               " threads=%d arrow_threads=%d helper_d=%d helper_q=%zu",
               files.size(), selected_rows, manifest_rows, opts.threads,
               opts.arrow_threads,
               helper.max_d, helper.ord2_by_q.size());
  if (opts.p_bucket >= 0) {
    std::fprintf(stderr, " p_bucket=%d", opts.p_bucket);
  }
  if (opts.flush_entries > 0) {
    std::fprintf(stderr, " partial_flush_entries=%" PRId64 " partials=%s",
                 opts.flush_entries, opts.partials_dir.c_str());
  }
  std::fprintf(stderr, "\n");

  auto started = std::chrono::steady_clock::now();
  std::atomic<size_t> next_file{0};
  std::atomic<int64_t> claimed_k0{0};
  std::atomic<int64_t> partial_seq{0};
  std::atomic<int64_t> partial_files{0};
  std::atomic<bool> failed{false};
  std::mutex error_mu;
  std::string first_error;
  std::mutex merge_mu;
  Stats global;
  ProgressState progress;
  progress.total_rows = selected_rows;
  progress.started = started;

  auto worker = [&]() {
    Stats local;
    while (!failed.load()) {
      size_t idx = next_file.fetch_add(1);
      if (idx >= files.size()) break;
      std::string local_error;
      if (!ProcessFile(files[idx], opts, &claimed_k0, &local,
                       helper, &partial_seq, &partial_files,
                       opts.progress ? &progress : nullptr,
                       &local_error)) {
        bool expected = false;
        if (failed.compare_exchange_strong(expected, true)) {
          std::lock_guard<std::mutex> lock(error_mu);
          first_error = local_error;
        }
        break;
      }
      if (opts.limit_k0 > 0 && claimed_k0.load() >= opts.limit_k0) break;
    }
    std::lock_guard<std::mutex> lock(merge_mu);
    MergeStats(&global, local, opts.sample_limit);
  };

  std::vector<std::thread> threads;
  const int n_threads = std::min<int>(opts.threads, static_cast<int>(files.size()));
  threads.reserve(n_threads);
  std::thread progress_thread;
  if (opts.progress) {
    progress_thread = std::thread(ProgressLoop, &progress, files.size());
  }
  for (int i = 0; i < n_threads; ++i) threads.emplace_back(worker);
  for (auto& t : threads) t.join();
  if (opts.progress) {
    progress.done.store(true);
    progress_thread.join();
  }

  if (failed.load()) {
    std::fprintf(stderr, "scan failed: %s\n", first_error.c_str());
    return 1;
  }

  const double elapsed_s = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started).count();
  if (!WriteOutputs(opts, global, opts.metadata, manifest_rows, helper,
                    partial_files.load(), elapsed_s, &error)) {
    std::fprintf(stderr, "write outputs: %s\n", error.c_str());
    return 1;
  }
  const bool partial_mode = opts.flush_entries > 0;

  nlohmann::json stdout_summary = {
      {"out_dir", opts.out_dir.string()},
      {"files_processed", global.files},
      {"rows_scanned", global.rows},
      {"k0_primes", global.k0_primes},
      {"candidate_remainders", global.candidate_remainders},
      {"primitive_events", global.primitive_events},
      {"known_factor_hits", global.known_factor_hits},
      {"residual_factorizations", global.residual_factorizations},
      {"residual_prime_hits", global.residual_prime_hits},
      {"distinct_primitive_factors", partial_mode ? nlohmann::json(nullptr)
                                                  : nlohmann::json(global.factors.size())},
      {"aggregate_mode", partial_mode ? "partial_parquet" : "in_memory_final"},
      {"partial_aggregate_dir", partial_mode ? opts.partials_dir.string() : ""},
      {"partial_aggregate_files", partial_files.load()},
      {"elapsed_seconds", elapsed_s},
  };
  std::cout << stdout_summary.dump() << "\n";
  return 0;
}
