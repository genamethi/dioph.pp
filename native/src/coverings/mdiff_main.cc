// primeparts-mdiff — per-k hit-mask tables.
//
// For a chosen set of k values, derive primeparts.mdiff_k{K}: one row per prime
// with exactly K representations p = 2^m + q^n, keyed as (p, hit_mask) where
// hit_mask is the int64 bitmask of the K hit positions (bit m set iff p - 2^m
// is a prime power; m_max ~ 39 < 64). popcount(hit_mask) == K. The pairwise
// index differences d = m_a - m_b (each a Mersenne index M_d = 2^d - 1) and
// prime_rank = π(p) are pure functions of (hit_mask, p) and are NOT stored;
// HitMaskDiffs() in the Mersenne helper decodes the d-multiset on demand.
//
// Access pattern is a single sorted pass over primeparts.partitions, which is
// stored sorted by (p, m_k) and partitioned by p_bucket. A prime's
// representations are exactly its contiguous run of partition rows; the run
// length is k(p). This is run-detection on already-sorted data (NOT a
// group-by): we never hash by p. Each p_bucket is a self-contained p-range
// (a prime never straddles a bucket), so buckets are processed independently
// in parallel, and within a bucket files are read in p-order.
//
// All output is published through the catalog seam (CommitFiles) as real,
// catalog-registered Iceberg tables; replace semantics per table =
// ppc::DropTable(purge) then CommitFiles. This tool never touches the warehouse
// filesystem directly — purge is a catalog operation (pp_iceberg_rest).
//
// The Mersenne-factor cache (primeparts.mersenne_factors) is built by the
// separate primeparts-mersenne sidecar; downstream cover/congruence work joins
// the d's decoded from hit_mask (HitMaskDiffs) against that table.

#include "primeparts/writer.h"
#include "primeparts/schemas.h"
#include "primeparts/source_scan.h"
#include "primeparts/catalog/pp_iceberg_rest.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

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
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"

namespace fs = std::filesystem;
namespace ppc = primeparts::catalog;

namespace {

constexpr int64_t kReaderBatchSize = 1 << 20;
const fs::path kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";

struct Options {
  fs::path warehouse = kDefaultWarehouse;
  std::set<int> k_values;        // target k tables to (re)build
  // Default low: each worker holds a chunk buffer (chunk_rows * 16 B) for the
  // in-memory (shape, p) sort, so peak RAM ~= threads * chunk_rows * 16 B. The
  // production box is small (~15 GB); 2 * 200M * 16 B ~= 6.4 GB leaves headroom.
  int threads = 2;
  int64_t chunk_rows = 200'000'000;   // rows sorted in memory -> one rolled file
  int64_t row_group_rows = 64'000'000;  // < chunk so each row group is a shape band
  int64_t flush_rows = 1 << 20;  // arrow batch size feeding the writer
  bool progress = true;
};

void Usage(const char* argv0) {
  std::fprintf(
      stderr,
      "usage: %s [--warehouse DIR] [--k N]... [--threads N]\n"
      "          [--chunk-rows N] [--row-group-rows N] [--flush-rows N] [--no-progress]\n\n"
      "Builds primeparts.mdiff_k{K} for each --k (a single sorted pass over\n"
      "primeparts.partitions, per-bucket parallel, run-detected). Output is\n"
      "clustered by covering shape: each bucket is emitted as rolled chunk files\n"
      "of ~--chunk-rows rows, sorted in memory by (shape, p), with row groups of\n"
      "~--row-group-rows so each is a narrow shape band. Peak RAM ~=\n"
      "threads * chunk-rows * 16 B. The mersenne_factors cache is built\n"
      "separately by primeparts-mersenne.\n",
      argv0);
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
    } else if (a == "--k") {
      const char* v = val("--k"); int64_t p = 0;
      if (!v || !ParseI64(v, &p) || p < 2 || p > 60) {
        std::fprintf(stderr, "invalid --k (need 2..60): %s\n", v ? v : ""); return false;
      }
      o->k_values.insert(static_cast<int>(p));
    } else if (a == "--threads") {
      const char* v = val("--threads"); int64_t p = 0;
      if (!v || !ParseI64(v, &p) || p <= 0 || p > 1024) {
        std::fprintf(stderr, "invalid --threads: %s\n", v ? v : ""); return false;
      }
      o->threads = static_cast<int>(p);
    } else if (a == "--chunk-rows") {
      const char* v = val("--chunk-rows");
      if (!v || !ParseI64(v, &o->chunk_rows) || o->chunk_rows <= 0) {
        std::fprintf(stderr, "invalid --chunk-rows: %s\n", v ? v : ""); return false;
      }
    } else if (a == "--row-group-rows") {
      const char* v = val("--row-group-rows");
      if (!v || !ParseI64(v, &o->row_group_rows) || o->row_group_rows <= 0) {
        std::fprintf(stderr, "invalid --row-group-rows: %s\n", v ? v : ""); return false;
      }
    } else if (a == "--flush-rows") {
      const char* v = val("--flush-rows");
      if (!v || !ParseI64(v, &o->flush_rows) || o->flush_rows <= 0) {
        std::fprintf(stderr, "invalid --flush-rows: %s\n", v ? v : ""); return false;
      }
    } else if (a == "--no-progress") {
      o->progress = false;
    } else if (a == "--help" || a == "-h") {
      Usage(argv[0]); std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false;
    }
  }
  return true;
}

int32_t ParseTagFromPath(const std::string& path, const std::string& marker,
                         int32_t fallback) {
  const size_t pos = path.rfind(marker);
  if (pos == std::string::npos) return fallback;
  size_t s = pos + marker.size(), e = s;
  while (e < path.size() && path[e] >= '0' && path[e] <= '9') ++e;
  if (e == s) return fallback;
  return static_cast<int32_t>(std::strtol(path.substr(s, e - s).c_str(), nullptr, 10));
}

template <typename Builder>
std::shared_ptr<arrow::Array> Finish(Builder* b) {
  std::shared_ptr<arrow::Array> out;
  auto st = b->Finish(&out);
  if (!st.ok()) throw std::runtime_error(st.ToString());
  return out;
}

std::unique_ptr<parquet::arrow::FileReader> OpenParquet(const std::string& path,
                                                        std::string* error) {
  auto file_r = arrow::io::ReadableFile::Open(path);
  if (!file_r.ok()) { *error = "open " + path + ": " + file_r.status().ToString(); return nullptr; }
  parquet::ArrowReaderProperties ap;
  ap.set_use_threads(false);   // parallelism comes from per-bucket workers
  ap.set_pre_buffer(true);
  ap.set_batch_size(kReaderBatchSize);
  parquet::arrow::FileReaderBuilder builder;
  auto st = builder.Open(file_r.ValueOrDie(), parquet::ReaderProperties());
  if (!st.ok()) { *error = "open builder " + path + ": " + st.ToString(); return nullptr; }
  builder.properties(ap);
  std::unique_ptr<parquet::arrow::FileReader> reader;
  st = builder.Build(&reader);
  if (!st.ok()) { *error = "build reader " + path + ": " + st.ToString(); return nullptr; }
  return reader;
}

// shape = hit_mask anchored so its lowest set bit is at position 0 — the
// translation-invariant covering family (mask >> ctz(mask)). hit_mask > 0 for
// k >= 2, so ctz is well-defined.
inline int64_t ShapeOf(int64_t mask) {
  return static_cast<int64_t>(static_cast<uint64_t>(mask) >>
                              __builtin_ctzll(static_cast<uint64_t>(mask)));
}

// Per-(bucket, k) output accumulator. Each prime's hit positions are packed into
// an int64 hit_mask; rows are buffered up to chunk_rows, sorted in memory by
// (shape, p), and emitted as one rolled parquet file per chunk. Row groups
// (writer max_row_group_rows < chunk_rows) are therefore narrow shape bands, so a
// per-family scan prunes by the shape min/max stat. The k-vector and pairwise
// differences are recoverable from hit_mask (HitMaskDiffs); prime_rank=π(p) is
// dropped. Peak memory per accumulator = chunk_rows * 16 B (the buffer).
struct KAccum {
  int k = 0;
  int64_t chunk_rows = 0;
  int64_t flush_rows = 0;
  std::shared_ptr<iceberg::Schema> schema;
  std::unique_ptr<primeparts::BucketParquetWriter> writer;
  std::vector<std::pair<int64_t, int64_t>> buf;  // (p, hit_mask)

  bool Init(const fs::path& warehouse, int32_t bv, int32_t bucket,
            const Options& opts, std::string* error) {
    chunk_rows = opts.chunk_rows;
    flush_rows = opts.flush_rows;
    schema = primeparts::MdiffSchema(k, error);
    if (!schema) return false;
    primeparts::WriterConfig cfg;
    const std::string table = "mdiff_k" + std::to_string(k);
    cfg.output_dir = primeparts::BucketDataDir(warehouse, table, bv, bucket);
    cfg.schema = schema;
    cfg.table_name = table;
    cfg.filename_prefix = table;
    cfg.delta_columns = {"p"};
    cfg.bucket_version = bv;
    cfg.bucket = bucket;
    cfg.starting_file_seq = 0;
    cfg.target_rows_per_file = chunk_rows;        // one rolled file per chunk
    cfg.max_row_group_rows = opts.row_group_rows;  // shape-band row groups
    writer = primeparts::BucketParquetWriter::Make(cfg, error);
    if (!writer) return false;
    buf.reserve(static_cast<size_t>(chunk_rows));
    return true;
  }

  // m must be sorted ascending, size == k; positions in [1, 63].
  bool Append(int64_t p, const std::vector<int32_t>& m, std::string* error) {
    int64_t mask = 0;
    for (int32_t pos : m) mask |= (int64_t{1} << pos);
    buf.emplace_back(p, mask);
    if (static_cast<int64_t>(buf.size()) >= chunk_rows) return FlushChunk(error);
    return true;
  }

  // Sort the buffered chunk by (shape, p) and write it as one rolled file.
  bool FlushChunk(std::string* error) {
    if (buf.empty()) return true;
    std::sort(buf.begin(), buf.end(),
              [](const std::pair<int64_t, int64_t>& a,
                 const std::pair<int64_t, int64_t>& b) {
                const int64_t sa = ShapeOf(a.second), sb = ShapeOf(b.second);
                if (sa != sb) return sa < sb;
                return a.first < b.first;
              });
    auto arrow_schema = arrow::schema({arrow::field("p", arrow::int64()),
                                       arrow::field("hit_mask", arrow::int64()),
                                       arrow::field("shape", arrow::int64())});
    const size_t n = buf.size();
    for (size_t off = 0; off < n; off += static_cast<size_t>(flush_rows)) {
      const size_t end = std::min(n, off + static_cast<size_t>(flush_rows));
      arrow::Int64Builder p_b, mask_b, shape_b;
      int64_t pmin = buf[off].first, pmax = buf[off].first;
      for (size_t i = off; i < end; ++i) {
        const int64_t p = buf[i].first, mask = buf[i].second;
        pmin = std::min(pmin, p); pmax = std::max(pmax, p);
        if (!p_b.Append(p).ok() || !mask_b.Append(mask).ok() ||
            !shape_b.Append(ShapeOf(mask)).ok()) {
          *error = "mdiff: append failed"; return false;
        }
      }
      auto batch = arrow::RecordBatch::Make(
          arrow_schema, static_cast<int64_t>(end - off),
          {Finish(&p_b), Finish(&mask_b), Finish(&shape_b)});
      primeparts::BucketParquetWriter::BatchStats st{};
      st.p_min = pmin; st.p_max = pmax; st.rank_min = 0; st.rank_max = 0;
      if (!writer->Write(*batch, st, error)) return false;
    }
    buf.clear();
    return true;
  }

  bool Close(std::vector<std::shared_ptr<iceberg::DataFile>>* out,
             std::string* error) {
    if (!FlushChunk(error)) return false;
    std::vector<primeparts::WrittenFile> written;
    if (!writer->Close(&written, error)) return false;
    for (const auto& wf : written)
      if (wf.data_file) out->push_back(wf.data_file);
    return true;
  }
};

struct Progress {
  std::atomic<int64_t> rows{0};       // partition rows scanned
  std::atomic<int64_t> emitted{0};    // mdiff rows emitted (all k)
  std::atomic<int64_t> buckets{0};
  std::atomic<bool> done{false};
  std::chrono::steady_clock::time_point started;
};

void ProgressLoop(Progress* pr, size_t total_buckets) {
  while (!pr->done.load()) {
    const double el = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - pr->started).count();
    const int64_t rows = pr->rows.load();
    std::fprintf(stderr,
                 "\rbuckets=%" PRId64 "/%zu rows=%" PRId64 " emitted=%" PRId64
                 " rate=%.1fM/s\x1B[K",
                 pr->buckets.load(), total_buckets, rows, pr->emitted.load(),
                 el > 0 ? rows / el / 1e6 : 0.0);
    std::fflush(stderr);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  std::fprintf(stderr, "\r");
}

// Process one bucket: run-detect over its partition files (p-ordered) and route
// each prime's run to the accumulator for its k (if targeted).
bool ProcessBucket(const std::vector<std::string>& files, int32_t bv,
                   int32_t bucket, const Options& opts,
                   std::map<int, std::vector<std::shared_ptr<iceberg::DataFile>>>* sink,
                   std::mutex* sink_mu, Progress* pr, std::string* error) {
  std::map<int, KAccum> accums;
  for (int k : opts.k_values) {
    KAccum a; a.k = k;
    if (!a.Init(opts.warehouse, bv, bucket, opts, error)) return false;
    accums.emplace(k, std::move(a));
  }

  int64_t cur_p = -1;
  std::vector<int32_t> run_m;
  run_m.reserve(8);

  auto finalize = [&](std::string* err) -> bool {
    if (cur_p < 0) return true;
    const int k = static_cast<int>(run_m.size());
    auto it = accums.find(k);
    if (it == accums.end()) return true;  // not a targeted k
    std::sort(run_m.begin(), run_m.end());  // defensive; already (p,m_k)-sorted
    // Append buffers into the chunk and rolls/sorts/flushes a file when full.
    if (!it->second.Append(cur_p, run_m, err)) return false;
    if (pr) pr->emitted.fetch_add(1);
    return true;
  };

  for (const auto& path : files) {
    auto reader = OpenParquet(path, error);
    if (!reader) return false;
    auto md = reader->parquet_reader()->metadata();
    auto* sd = md->schema();
    const int ip = sd->ColumnIndex("p");
    const int im = sd->ColumnIndex("m_k");
    if (ip < 0 || im < 0) {
      *error = "partitions file missing p/m_k: " + path; return false;
    }
    std::vector<int> rgs(static_cast<size_t>(md->num_row_groups()));
    for (int rg = 0; rg < md->num_row_groups(); ++rg) rgs[static_cast<size_t>(rg)] = rg;
    auto rbr_r = reader->GetRecordBatchReader(rgs, {ip, im});
    if (!rbr_r.ok()) { *error = "rbr " + path + ": " + rbr_r.status().ToString(); return false; }
    auto rbr = std::move(rbr_r).ValueOrDie();
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      auto st = rbr->ReadNext(&batch);
      if (!st.ok()) { *error = "ReadNext " + path + ": " + st.ToString(); return false; }
      if (!batch) break;
      if (pr) pr->rows.fetch_add(batch->num_rows());
      auto pa = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
      auto ma = std::static_pointer_cast<arrow::Int32Array>(batch->column(1));
      const int64_t n = batch->num_rows();
      for (int64_t i = 0; i < n; ++i) {
        const int64_t p = pa->Value(i);
        if (p != cur_p) {
          if (!finalize(error)) return false;
          cur_p = p; run_m.clear();
        }
        run_m.push_back(ma->Value(i));
      }
    }
  }
  if (!finalize(error)) return false;

  {
    std::lock_guard<std::mutex> lk(*sink_mu);
    for (auto& [k, a] : accums) {
      if (!a.Close(&(*sink)[k], error)) return false;
    }
  }
  if (pr) pr->buckets.fetch_add(1);
  return true;
}

bool BuildMdiff(const std::shared_ptr<iceberg::Catalog>& catalog,
                const Options& opts, std::string* error) {
  // Enumerate partitions data files through the catalog (metadata, not globbing).
  fs::path meta = ppc::TableMetadataPath(catalog, "partitions", error);
  if (meta.empty()) return false;
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      meta, {"p", "m_k"}, nullptr, error);
  if (!reader) return false;
  auto files = reader->source_files();
  reader.reset();
  if (files.empty()) { *error = "partitions: no data files planned"; return false; }

  // Group files by (bucket_version, bucket); keep p-order within each bucket
  // (source_files() is already sorted by p_min).
  struct Bucket { int32_t bv; int32_t bucket; std::vector<std::string> files; };
  std::map<std::pair<int32_t, int32_t>, std::vector<std::string>> by_bucket;
  for (const auto& f : files) {
    const int32_t bv = ParseTagFromPath(f.path, "/p_bucket_version=", 1);
    const int32_t bk = ParseTagFromPath(f.path, "/p_bucket=", 0);
    by_bucket[{bv, bk}].push_back(f.path);
  }
  std::vector<Bucket> buckets;
  buckets.reserve(by_bucket.size());
  for (auto& [key, fl] : by_bucket)
    buckets.push_back({key.first, key.second, std::move(fl)});

  // Replace semantics: drop (purge) each target table through the catalog seam
  // BEFORE writing fresh per-bucket files. ppc::DropTable owns all file removal.
  for (int k : opts.k_values)
    if (!ppc::DropTable(catalog, "mdiff_k" + std::to_string(k), /*purge=*/true,
                        error))
      return false;

  std::map<int, std::vector<std::shared_ptr<iceberg::DataFile>>> sink;
  std::mutex sink_mu;
  Progress pr; pr.started = std::chrono::steady_clock::now();
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};
  std::mutex err_mu; std::string first_err;

  auto worker = [&]() {
    while (!failed.load()) {
      size_t idx = next.fetch_add(1);
      if (idx >= buckets.size()) break;
      const auto& b = buckets[idx];
      std::string e;
      if (!ProcessBucket(b.files, b.bv, b.bucket, opts, &sink, &sink_mu,
                         opts.progress ? &pr : nullptr, &e)) {
        bool exp = false;
        if (failed.compare_exchange_strong(exp, true)) {
          std::lock_guard<std::mutex> lk(err_mu); first_err = e;
        }
        break;
      }
    }
  };

  std::thread prog;
  if (opts.progress) prog = std::thread(ProgressLoop, &pr, buckets.size());
  std::vector<std::thread> ts;
  const int n = std::min<int>(opts.threads, static_cast<int>(buckets.size()));
  for (int i = 0; i < n; ++i) ts.emplace_back(worker);
  for (auto& t : ts) t.join();
  if (opts.progress) { pr.done.store(true); prog.join(); }

  if (failed.load()) { *error = first_err; return false; }

  // Publish each table in one FastAppend snapshot over its per-bucket files.
  for (int k : opts.k_values) {
    const std::string table = "mdiff_k" + std::to_string(k);
    std::string e;
    auto schema = primeparts::MdiffSchema(k, &e);
    if (!schema) { *error = e; return false; }
    auto spec = primeparts::BucketPartitionSpec(*schema, &e);
    if (!spec) { *error = "partition spec for " + table + ": " + e; return false; }
    std::string meta_loc;
    if (!ppc::CommitFiles(catalog, opts.warehouse, table, schema, spec, sink[k],
                          &meta_loc, error))
      return false;
    int64_t rows = 0;
    for (const auto& df : sink[k]) rows += df->record_count;
    std::fprintf(stderr, "%s: %" PRId64 " rows in %zu files -> %s\n", table.c_str(),
                 rows, sink[k].size(), meta_loc.c_str());
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) { Usage(argv[0]); return 2; }
  if (opts.k_values.empty()) {
    std::fprintf(stderr, "nothing to do: pass --k N (>=2)\n");
    Usage(argv[0]);
    return 2;
  }

  std::string err;
  auto catalog = ppc::MakeLocalCatalog(opts.warehouse, &err);
  if (!catalog) { std::fprintf(stderr, "MakeLocalCatalog: %s\n", err.c_str()); return 1; }

  if (!BuildMdiff(catalog, opts, &err)) {
    std::fprintf(stderr, "build mdiff: %s\n", err.c_str()); return 1;
  }
  return 0;
}
