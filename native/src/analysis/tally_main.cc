// tally — per-k m-tuple frequency tables.
//
// For each prime p = 2^m + q^n with k >= 2 representations, the set of exponents
// m is the prime's m-tuple (the m values are distinct). tally counts, per k, how
// many primes carry each distinct tuple and publishes one table per k:
// primeparts.mtuple_k{K}, schema PresenceSchema() = (b1..b38 base-shifted shape,
// shift, count). See markdown/data_eng and the plan for the analysis that reads
// these back (the `cluster` binary).
//
// Read path is strictly the catalog seam and threads BY BUCKET: partitions is
// sorted by (p, m_k) and partitioned into self-contained p-ranges (a prime never
// straddles a bucket, though it may straddle files within one). Each worker opens
// a SourceTableReader over primeparts.partitions filtered to one bucket's p-range
// (p >= p_min AND p <= p_max) — iceberg prunes to that bucket's files, which the
// reader streams p-ascending in a single stream, so run-detection ("flush on
// p-change", carry across batches) spans intra-bucket file boundaries and never
// splits a prime. No per-file sharding, no boundary handling, no direct parquet.
// p is streamed ascending, so a --p-hi window is an early stop.
//
// Write path is the catalog seam too: BucketParquetWriter into a staging dir +
// ppc::DropTable(purge) + ppc::CommitFiles (replace semantics per table). The
// output is UNPARTITIONED (its own spec, distinct from the partitions bucket
// spec) — the frequency tables are tiny.

#include "primeparts/analysis/tuples.h"
#include "primeparts/schemas.h"
#include "primeparts/source_scan.h"
#include "primeparts/writer.h"
#include "primeparts/catalog/pp_iceberg_rest.h"

#include <arrow/api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"

namespace fs = std::filesystem;
namespace ppc = primeparts::catalog;
namespace ana = primeparts::analysis;

namespace {

const fs::path kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";

struct Options {
  fs::path warehouse = kDefaultWarehouse;
  int threads = 8;
  int64_t p_lo = 0;  // 0 = open
  int64_t p_hi = 0;  // 0 = open
  bool progress = true;
  std::string rest_uri;
};

void Usage(const char* argv0) {
  std::fprintf(
      stderr,
      "usage: %s [--warehouse DIR] [--threads N] [--p-lo P] [--p-hi P]\n"
      "          [--no-progress] [--rest-uri URI]\n\n"
      "Builds primeparts.mtuple_k{K} for every k >= 2 present in\n"
      "primeparts.partitions: one row per distinct m-tuple, counted. Reads only\n"
      "through the catalog (sharded SourceTableReader over (p, m_k)); writes only\n"
      "through the catalog (DropTable + CommitFiles). --p-lo/--p-hi restrict the\n"
      "prime window for a quick slice test.\n\n"
      "  --rest-uri URI  IRC endpoint override (default %s); falls back to the\n"
      "                  local LMDB catalog only if unreachable.\n",
      argv0, ppc::kDefaultRestUri);
}

bool ParseI64(const char* s, int64_t* out) {
  char* end = nullptr;
  long long v = std::strtoll(s, &end, 10);
  if (end == s || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool ParseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char* n) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s requires a value\n", n); return nullptr; }
      return argv[++i];
    };
    if (a == "--warehouse") {
      const char* v = val("--warehouse"); if (!v) return false; o->warehouse = v;
    } else if (a == "--threads") {
      const char* v = val("--threads"); int64_t p = 0;
      if (!v || !ParseI64(v, &p) || p <= 0 || p > 1024) {
        std::fprintf(stderr, "invalid --threads: %s\n", v ? v : ""); return false;
      }
      o->threads = static_cast<int>(p);
    } else if (a == "--p-lo") {
      const char* v = val("--p-lo");
      if (!v || !ParseI64(v, &o->p_lo) || o->p_lo < 0) {
        std::fprintf(stderr, "invalid --p-lo: %s\n", v ? v : ""); return false;
      }
    } else if (a == "--p-hi") {
      const char* v = val("--p-hi");
      if (!v || !ParseI64(v, &o->p_hi) || o->p_hi < 0) {
        std::fprintf(stderr, "invalid --p-hi: %s\n", v ? v : ""); return false;
      }
    } else if (a == "--no-progress") {
      o->progress = false;
    } else if (a == "--rest-uri") {
      const char* v = val("--rest-uri"); if (!v) return false; o->rest_uri = v;
    } else if (a == "--help" || a == "-h") {
      Usage(argv[0]); std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false;
    }
  }
  return true;
}

struct Progress {
  std::atomic<int64_t> rows{0};
  std::atomic<int64_t> primes{0};
  std::atomic<bool> done{false};
  std::chrono::steady_clock::time_point started;
};

void ProgressLoop(Progress* pr) {
  while (!pr->done.load()) {
    const double el = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - pr->started).count();
    const int64_t rows = pr->rows.load();
    std::fprintf(stderr, "\rrows=%" PRId64 " primes=%" PRId64 " rate=%.1fM/s\x1B[K",
                 rows, pr->primes.load(), el > 0 ? rows / el / 1e6 : 0.0);
    std::fflush(stderr);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  std::fprintf(stderr, "\r");
}

using TupleMap = std::unordered_map<uint64_t, int64_t>;  // mask -> primes

int32_t ParseTagFromPath(const std::string& path, const std::string& marker,
                         int32_t fallback) {
  const size_t pos = path.rfind(marker);
  if (pos == std::string::npos) return fallback;
  size_t s = pos + marker.size(), e = s;
  while (e < path.size() && path[e] >= '0' && path[e] <= '9') ++e;
  if (e == s) return fallback;
  return static_cast<int32_t>(std::strtol(path.substr(s, e - s).c_str(), nullptr, 10));
}

// One bucket: run-detect primes over the p-range [lo, hi] and tally masks. The
// bucket is a self-contained p-range, so its whole prime set arrives here (in a
// single p-ascending stream over the bucket's files) and no prime is split.
bool ScanBucket(const fs::path& meta, int64_t lo, int64_t hi, TupleMap* tally,
                Progress* pr, std::string* error) {
  std::shared_ptr<iceberg::Expression> filter =
      iceberg::Expressions::GreaterThanOrEqual("p", iceberg::Literal::Long(lo));
  filter = iceberg::Expressions::And(
      filter, iceberg::Expressions::LessThanOrEqual("p", iceberg::Literal::Long(hi)));

  auto reader = primeparts::SourceTableReader::OpenMetadata(
      meta, {"p", "m_k"}, filter, error);
  if (!reader) return false;

  int64_t cur_p = -1;
  uint64_t run_mask = 0;
  int run_len = 0;

  auto finalize = [&]() {
    if (run_len >= ana::kMinK) (*tally)[run_mask] += 1;
    if (pr && run_len >= 1) pr->primes.fetch_add(1);
    run_mask = 0;
    run_len = 0;
  };

  std::shared_ptr<arrow::RecordBatch> batch;
  bool stop = false;
  while (!stop) {
    if (!reader->Next(&batch, error)) return false;
    if (!batch) break;  // EOF
    if (pr) pr->rows.fetch_add(batch->num_rows());
    auto pa = std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("p"));
    auto ma = std::static_pointer_cast<arrow::Int32Array>(batch->GetColumnByName("m_k"));
    const int64_t n = batch->num_rows();
    for (int64_t i = 0; i < n; ++i) {
      const int64_t p = pa->Value(i);
      if (p < lo) continue;             // below the bucket/window range
      if (p > hi) { stop = true; break; }  // past it (p streamed ascending)
      if (p != cur_p) { finalize(); cur_p = p; }
      run_mask |= (uint64_t{1} << ma->Value(i));
      ++run_len;
    }
  }
  finalize();  // last run
  return true;
}

// Build and commit primeparts.mtuple_k{K} from its (mask -> count) rows.
bool WriteTable(const std::shared_ptr<iceberg::Catalog>& catalog,
                const Options& opts, int k,
                const std::vector<std::pair<uint64_t, int64_t>>& rows,
                std::string* error) {
  const std::string table = "mtuple_k" + std::to_string(k);
  auto schema = primeparts::PresenceSchema();

  // Arrow schema mirrors PresenceSchema field order: b1..b38, shift, count.
  std::vector<std::shared_ptr<arrow::Field>> fields;
  fields.reserve(ana::kTupleWidth + 2);
  for (int j = 1; j <= ana::kTupleWidth; ++j)
    fields.push_back(arrow::field("b" + std::to_string(j), arrow::int32()));
  fields.push_back(arrow::field("shift", arrow::int32()));
  fields.push_back(arrow::field("count", arrow::int64()));
  auto arrow_schema = arrow::schema(fields);

  primeparts::WriterConfig cfg;
  cfg.output_dir = ppc::StagingDataDir(opts.warehouse, table);
  cfg.schema = schema;
  cfg.table_name = table;
  cfg.filename_prefix = table;
  cfg.partition_spec = iceberg::PartitionSpec::Unpartitioned();
  cfg.target_rows_per_file = 0;            // small table: one file
  cfg.max_row_group_rows = 1 << 20;
  auto writer = primeparts::BucketParquetWriter::Make(cfg, error);
  if (!writer) return false;

  constexpr int64_t kBatch = 1 << 16;
  const int64_t total = static_cast<int64_t>(rows.size());
  int32_t bits[ana::kTupleWidth];
  for (int64_t off = 0; off < total; off += kBatch) {
    const int64_t end = std::min(total, off + kBatch);
    std::vector<arrow::Int32Builder> bcol(ana::kTupleWidth);
    arrow::Int32Builder shift_b;
    arrow::Int64Builder count_b;
    for (int64_t i = off; i < end; ++i) {
      // Loud guard: a shape wider than kTupleWidth bits would be silently
      // truncated by MaskToColumns (the b1..bW columns). Fail instead.
      if ((ana::ShapeOf(rows[i].first) >> ana::kTupleWidth) != 0) {
        *error = "tally: tuple shape exceeds kTupleWidth (" +
                 std::to_string(ana::kTupleWidth) + "); widen PresenceSchema";
        return false;
      }
      int32_t shift = 0;
      ana::MaskToColumns(rows[i].first, bits, &shift);
      for (int j = 0; j < ana::kTupleWidth; ++j) {
        if (!bcol[j].Append(bits[j]).ok()) { *error = "tally: append b"; return false; }
      }
      if (!shift_b.Append(shift).ok() || !count_b.Append(rows[i].second).ok()) {
        *error = "tally: append shift/count"; return false;
      }
    }
    std::vector<std::shared_ptr<arrow::Array>> cols;
    cols.reserve(ana::kTupleWidth + 2);
    for (int j = 0; j < ana::kTupleWidth; ++j) {
      std::shared_ptr<arrow::Array> a;
      if (!bcol[j].Finish(&a).ok()) { *error = "tally: finish b"; return false; }
      cols.push_back(std::move(a));
    }
    std::shared_ptr<arrow::Array> sa, ca;
    if (!shift_b.Finish(&sa).ok() || !count_b.Finish(&ca).ok()) {
      *error = "tally: finish shift/count"; return false;
    }
    cols.push_back(std::move(sa));
    cols.push_back(std::move(ca));
    auto batch = arrow::RecordBatch::Make(arrow_schema, end - off, cols);
    primeparts::BucketParquetWriter::BatchStats st{};  // no p column: zeros
    if (!writer->Write(*batch, st, error)) return false;
  }

  std::vector<primeparts::WrittenFile> written;
  if (!writer->Close(&written, error)) return false;
  std::vector<std::shared_ptr<iceberg::DataFile>> files;
  for (const auto& wf : written)
    if (wf.data_file) files.push_back(wf.data_file);

  if (!ppc::DropTable(catalog, opts.warehouse, table, /*purge=*/true, error))
    return false;
  auto spec = iceberg::PartitionSpec::Unpartitioned();
  std::string meta_loc;
  if (!ppc::CommitFiles(catalog, opts.warehouse, table, schema, spec, files,
                        &meta_loc, error))
    return false;
  std::fprintf(stderr, "%s: %" PRId64 " tuples -> %s\n", table.c_str(), total,
               meta_loc.c_str());
  return true;
}

struct BucketRange { int64_t lo; int64_t hi; };

bool Run(const std::shared_ptr<iceberg::Catalog>& catalog, const Options& opts,
         std::string* error) {
  fs::path meta = ppc::TableMetadataPath(catalog, "partitions", error);
  if (meta.empty()) return false;

  // Enumerate buckets through the catalog (manifest file stats, no data decode):
  // group planned data files by (p_bucket_version, p_bucket) into p-ranges.
  auto disc = primeparts::SourceTableReader::OpenMetadata(meta, {"p"}, nullptr, error);
  if (!disc) return false;
  std::map<std::pair<int32_t, int32_t>, BucketRange> by_bucket;
  for (const auto& f : disc->source_files()) {
    const int32_t bv = ParseTagFromPath(f.path, "/p_bucket_version=", 1);
    const int32_t bk = ParseTagFromPath(f.path, "/p_bucket=", 0);
    auto key = std::make_pair(bv, bk);
    auto it = by_bucket.find(key);
    if (it == by_bucket.end()) by_bucket[key] = {f.p_min, f.p_max};
    else { it->second.lo = std::min(it->second.lo, f.p_min);
           it->second.hi = std::max(it->second.hi, f.p_max); }
  }
  disc.reset();

  // Apply the optional --p-lo/--p-hi window; drop buckets it excludes entirely.
  std::vector<BucketRange> buckets;
  for (const auto& [key, r] : by_bucket) {
    int64_t lo = opts.p_lo > 0 ? std::max(r.lo, opts.p_lo) : r.lo;
    int64_t hi = opts.p_hi > 0 ? std::min(r.hi, opts.p_hi) : r.hi;
    if (lo <= hi) buckets.push_back({lo, hi});
  }
  if (buckets.empty()) { *error = "tally: no buckets in window"; return false; }
  std::fprintf(stderr, "buckets: %zu, threads: %d\n", buckets.size(), opts.threads);

  const int nthreads = std::min<int>(std::max(1, opts.threads),
                                     static_cast<int>(buckets.size()));
  std::vector<TupleMap> maps(static_cast<size_t>(nthreads));
  Progress pr; pr.started = std::chrono::steady_clock::now();
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};
  std::mutex err_mu; std::string first_err;

  std::thread prog;
  if (opts.progress) prog = std::thread(ProgressLoop, &pr);

  std::vector<std::thread> ts;
  for (int t = 0; t < nthreads; ++t) {
    ts.emplace_back([&, t]() {
      while (!failed.load()) {
        size_t idx = next.fetch_add(1);
        if (idx >= buckets.size()) break;
        std::string e;
        if (!ScanBucket(meta, buckets[idx].lo, buckets[idx].hi,
                        &maps[static_cast<size_t>(t)],
                        opts.progress ? &pr : nullptr, &e)) {
          bool exp = false;
          if (failed.compare_exchange_strong(exp, true)) {
            std::lock_guard<std::mutex> lk(err_mu); first_err = e;
          }
          break;
        }
      }
    });
  }
  for (auto& t : ts) t.join();
  if (opts.progress) { pr.done.store(true); prog.join(); }
  if (failed.load()) { *error = first_err; return false; }

  // Merge per-thread maps, then bucket by k = popcount(mask).
  TupleMap merged;
  for (auto& m : maps) {
    for (const auto& [mask, c] : m) merged[mask] += c;
    m.clear();
  }
  std::map<int, std::vector<std::pair<uint64_t, int64_t>>> by_k;
  for (const auto& [mask, c] : merged)
    by_k[ana::Popcount(mask)].emplace_back(mask, c);

  if (by_k.empty()) { *error = "tally: no k>=2 primes in window"; return false; }
  for (auto& [k, rows] : by_k) {
    // Deterministic order: descending count, then mask.
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
              });
    if (!WriteTable(catalog, opts, k, rows, error)) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) { Usage(argv[0]); return 2; }

  std::string err;
  auto catalog = ppc::OpenCatalog(opts.warehouse, opts.rest_uri, nullptr, &err);
  if (!catalog) { std::fprintf(stderr, "OpenCatalog: %s\n", err.c_str()); return 1; }

  if (!Run(catalog, opts, &err)) {
    std::fprintf(stderr, "tally: %s\n", err.c_str()); return 1;
  }
  return 0;
}
