// primeparts-covering-sieve — the k=0 covering-system filter.
//
// Rewritten 2026-05-31. The old design (fs-glob input, two string-labelled
// covered_/uncovered_passN partitions, InMemoryCatalog) is gone. New model,
// proven green by the delete spike + `pp-catalog --mor-verify`:
//
//   The uncovered set is the LIVE (post-delete) rows of a `primes_k0` copy
//   (`primes_k0_sieve`) under merge-on-read position deletes. One pass adds ONE
//   odd-prime modulus to the covering, deletes the primes it pushes to *fully
//   covered*, and advances a snapshot via IRC. The surviving set is the object
//   of interest: primes whose obstruction no Mersenne-factor covering reaches
//   (the local->global residue). First hole = MIN(p) of the live set is kept as
//   a diagnostic only.
//
// Coverage / obstruction (lab-notes 2026-03-30): a k0 prime is obstructed when
// the ord(2,q) progressions cover every position m in [1, floor(log2 p)] — each
// r = p - 2^m has >= 2 distinct prime factors. Position m is covered by modulus
// s iff s | (p - 2^m) iff 2^m == p (mod s). DEGENERATE case r = 1 (only p=3) is
// satisfied trivially. Every odd prime divides M_d = 2^d-1 for d = ord_s(2), so
// the useful covering moduli are the small-order (primitive) Mersenne factors —
// already tabulated in mersenne_reference.parquet / MersenneHelper.
//
// Kernel:
//   * VECTORIZED — per modulus s, pattern table pat_s[r] = coverage bitmask for
//     any prime with p == r (mod s); hot loop is Barrett-mod -> gather -> OR.
//   * MULTITHREADED READ — N independent delete-aware SourceTableReader SHARDS,
//     each decoding a disjoint ~1/N subset of files in parallel.
//   * TRIAGE — the scan tallies {residual bitmask -> count}; afterward, for each
//     Mersenne order d we score the BEST single residue class mod d against that
//     tally (a modulus of order d tiles exactly one class), and rank the orders.
//     We do NOT test candidate moduli per prime, and we add ONE modulus per pass.

#include "primeparts/coverings/primitive_factors.h"
#include "primeparts/source_scan.h"
#include "primeparts/writer.h"
#include "primeparts/schemas.h"

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/pp_row_delta.h"
#include "primeparts/common/thread_pool.h"
#include "primeparts/common/uri.h"

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>

#include "iceberg/catalog.h"
#include "iceberg/data/position_delete_writer.h"
#include "iceberg/expression/literal.h"
#include "iceberg/file_format.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/snapshot.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ppc = primeparts::catalog;

namespace {

constexpr char kDefaultWarehouse[] =
    "/media/extssd/research/dioph.pp/data/ib-staging";
constexpr char kDefaultTable[] = "primes_k0_sieve";
constexpr char kModulusKey[] = "covering.modulus";

// Coverage bitmask positions m run 1..kMaxPos (bit m-1). primes_k0 max p is
// ~5.6e11 < 2^39, so max_m <= 39; 62 is a safe ceiling for any int64 p.
constexpr int kMaxPos = 62;
constexpr int kDefaultTop = 5;  // triage rows to display

struct Options {
  std::string warehouse = kDefaultWarehouse;
  std::string rest_uri;  // empty => OpenCatalog resolves env or kDefaultRestUri
  std::string table = kDefaultTable;
  uint64_t apply = 0;   // the modulus to add this pass (required)
  bool have_apply = false;
  int threads = 0;      // 0 => hardware_concurrency()
  int show_top = kDefaultTop;  // triage rows to show (0 => triage off)
  std::string metric = "expected";  // triage scoring: expected|bestclass|hybrid
  bool report = false;  // dump campaign arc from snapshot history and exit
  bool classify = false;  // structural pass: backbone residual gap distribution
  bool overgen = false;   // over-generate the covering_system + covering_log tables
  int64_t limit = 0;      // overgen: scan only the first N primes (0 = all)
  bool method3 = false;   // build the minimal distinct covering system, then exit
  int min_modulus = 3;    // method3: smallest modulus allowed (2 => even class ok)
  int period_cap = 2520;  // method3: covering period bound (and max single modulus)
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s --apply S [options]\n"
    "  Add odd-prime modulus S to the covering on primeparts.<table>, delete\n"
    "  the primes it pushes to fully covered, commit a snapshot (IRC), and rank\n"
    "  the next modulus by best-class elimination over the residual bitmasks.\n"
    "  --apply S            modulus to add this pass (odd prime, known ord_s(2))\n"
    "  --table NAME         delete-target table (default %s)\n"
    "  --rest-uri URI       IRC endpoint override (default %s)\n"
    "  --warehouse DIR      warehouse root (default %s)\n"
    "  --threads N          parallel read-shard threads (default: hw concurrency)\n"
    "  --top N              triage rows to display (default %d; 0 = no triage)\n"
    "  --metric M           triage scoring: expected (default) | bestclass | hybrid\n"
    "                         bestclass = max_c popcount (structure-aware, no weight)\n"
    "                         hybrid    = bestclass x d/(q-1) (realization-weighted)\n"
    "                         expected  = total_open/(q-1) (realized, ~1/(q-1))\n"
    "  --report             print the campaign arc from snapshot history, exit\n"
    "  --classify           structural pass: backbone {3,5,7,11,13,17} residual\n"
    "                         gap-count distribution over the table, then exit\n"
    "  --overgen            generate the over-generated covering system as catalog\n"
    "                         tables: covering_system (l, k=floor(p_max/l)) and\n"
    "                         covering_log (l, k, p, a) for every (prime, modulus),\n"
    "                         published through the catalog seam, then exit\n"
    "  --limit N            overgen: use only the first N primes (0 = all)\n"
    "  --method3            build the minimal DISTINCT covering system of\n"
    "                         congruences (doc algorithm: <2>-dlog phase recovery,\n"
    "                         fold to distinct moduli, prune to minimal), then\n"
    "                         report realized recall over primes_k0, and exit\n"
    "  --min-modulus N      method3: smallest modulus allowed (default 3; 2 also\n"
    "                         permits the even-position class m == 0 (mod 2))\n"
    "  --period-cap N       method3: covering period / max single modulus (def 2520)\n",
    argv0, kDefaultTable, ppc::kDefaultRestUri, kDefaultWarehouse, kDefaultTop);
}

int FloorLog2(uint64_t x) {
  if (x == 0) return 0;
  return 63 - __builtin_clzll(x);
}

using primeparts::common::StripFileScheme;

// Barrett reduction by a fixed small modulus s (avoids hardware divide in the
// hot loop; mu = floor(2^64 / s), one 64x64->128 multiply-high + correction).
struct FastMod {
  uint64_t s = 1;
  uint64_t mu = 0;
  FastMod() = default;
  explicit FastMod(uint64_t s_)
      : s(s_), mu(static_cast<uint64_t>(((static_cast<unsigned __int128>(1) << 64)) / s_)) {}
  inline uint64_t mod(uint64_t p) const {
    uint64_t q = static_cast<uint64_t>((static_cast<unsigned __int128>(p) * mu) >> 64);
    uint64_t r = p - q * s;
    while (r >= s) r -= s;  // 0-1 corrections
    return r;
  }
};

// A covering modulus with its precomputed pattern table. pat[r] (r in [0,s)) is
// the 64-bit coverage bitmask for any prime with p == r (mod s): bit (m-1) set
// iff 2^m == r (mod s), for m = 1..kMaxPos. Residues not in <2> get pat[r] = 0.
struct CovModulus {
  uint64_t s = 0;
  FastMod fm;
  std::vector<uint64_t> pat;
};

CovModulus BuildCov(uint64_t s) {
  CovModulus c;
  c.s = s;
  c.fm = FastMod(s);
  c.pat.assign(s, 0);
  uint64_t cur = 1 % s;  // 2^0 mod s
  for (int m = 1; m <= kMaxPos; ++m) {
    cur = (cur * 2) % s;  // 2^m mod s
    c.pat[cur] |= (uint64_t{1} << (m - 1));
  }
  return c;
}

// Read the applied-moduli set from the snapshot-summary history (each sieve
// pass stamps covering.modulus). Returns them in snapshot order.
std::vector<uint64_t> AppliedModuli(const std::shared_ptr<iceberg::Table>& tbl) {
  std::vector<uint64_t> out;
  for (const auto& snap : tbl->snapshots()) {
    auto it = snap->summary.find(kModulusKey);
    if (it == snap->summary.end()) continue;
    out.push_back(std::stoull(it->second));
  }
  return out;
}

// Dump the campaign arc from the snapshot-summary history. Each sieve pass
// stamps covering.* into its snapshot, so the snapshots ARE the per-pass log
// (the sieve_passes record) — single source of truth, no separate table.
int ReportArc(const std::shared_ptr<iceberg::Table>& tbl) {
  std::printf("%-5s %-8s %-4s %-13s %-16s %-11s %s\n", "pass", "modulus", "ord",
              "deleted", "survivors", "first-hole", "open-positions");
  for (const auto& snap : tbl->snapshots()) {
    const auto& s = snap->summary;
    if (s.find(kModulusKey) == s.end()) continue;  // skip the clone append
    auto get = [&](const char* k) -> const char* {
      auto it = s.find(k);
      return it == s.end() ? "-" : it->second.c_str();
    };
    std::printf("%-5s %-8s %-4s %-13s %-16s %-11s %s\n",
                get("covering.pass_index"), get(kModulusKey), get("covering.ord"),
                get("covering.newly_deleted"), get("covering.live_count"),
                get("covering.first_hole"), get("covering.open_positions"));
  }
  return 0;
}

// Per-shard sift accumulator. Triage tallies the residual (uncovered-position)
// bitmask of each survivor — there are few distinct patterns (residual coverage
// is periodic mod the applied moduli), so this map stays small.
struct SiftAccum {
  std::vector<std::pair<std::string, int64_t>> deletes;  // (data file, pos)
  int64_t live_in = 0;
  int64_t newly_deleted = 0;
  int64_t live_out = 0;
  uint64_t min_survivor = std::numeric_limits<uint64_t>::max();
  std::unordered_map<uint64_t, int64_t> hist;  // residual bitmask -> count
};

// Vectorized sift of one batch's contiguous (p, _pos) buffers. With triage on,
// it computes the full residual and tallies it; otherwise it uses the skip
// optimization (a prime the new modulus can't touch stays live).
void SiftBatch(const int64_t* p, const int64_t* pos, int64_t n,
               const std::string& file, const CovModulus& newc,
               const std::vector<CovModulus>& prior, bool do_triage,
               SiftAccum* acc) {
  for (int64_t i = 0; i < n; ++i) {
    const uint64_t pp = static_cast<uint64_t>(p[i]);
    ++acc->live_in;
    const int max_m = FloorLog2(pp);
    bool covered = false;
    uint64_t full = 0, cov = 0;
    if (max_m > 0) {
      full = (uint64_t{1} << max_m) - 1;  // positions 1..max_m
      // Trivial obstruction: p = 2^max_m + 1 => r = 1 (unit, not a prime power)
      // => top position satisfied with no modulus (only p=3 among k0 primes).
      const uint64_t triv =
          (pp == (uint64_t{1} << max_m) + 1) ? (uint64_t{1} << (max_m - 1)) : 0;
      const uint64_t cov_new = (newc.pat[newc.fm.mod(pp)] & full) | triv;
      if (do_triage) {
        cov = cov_new;
        for (const auto& c : prior) cov |= c.pat[c.fm.mod(pp)];
        cov &= full;
        covered = (cov == full);
      } else if (cov_new != 0) {
        cov = cov_new;
        for (const auto& c : prior) cov |= c.pat[c.fm.mod(pp)];
        covered = ((cov & full) == full);
      }
    }
    if (covered) {
      acc->deletes.emplace_back(file, pos[i]);
      ++acc->newly_deleted;
    } else {
      ++acc->live_out;
      if (pp < acc->min_survivor) acc->min_survivor = pp;
      if (do_triage && max_m > 0) acc->hist[full & ~cov]++;  // residual bitmask
    }
  }
}

// --classify: characterize the structure. For each prime, count how many
// positions the FIXED backbone {3,5,7,11,13,17} leaves uncovered (the residual
// gap). g=0 => fully explained by the backbone (unconditionally obstructed via
// those six); g>0 => the prime's obstruction needs larger moduli (the residue).
// Reproduces the lab-notes mod-255255 gap distribution, natively, no deletion.
struct ClassifyAccum {
  int64_t total = 0;
  std::array<int64_t, kMaxPos + 2> gap{};  // gap[g] = #primes with g open positions
};

void ClassifyBatch(const int64_t* p, int64_t n,
                   const std::vector<CovModulus>& backbone, ClassifyAccum* acc) {
  for (int64_t i = 0; i < n; ++i) {
    const uint64_t pp = static_cast<uint64_t>(p[i]);
    ++acc->total;
    const int max_m = FloorLog2(pp);
    if (max_m <= 0) continue;
    const uint64_t full = (uint64_t{1} << max_m) - 1;
    uint64_t cov = 0;
    for (const auto& c : backbone) cov |= c.pat[c.fm.mod(pp)];
    int g = __builtin_popcountll(full & ~cov);
    if (g >= static_cast<int>(acc->gap.size()))
      g = static_cast<int>(acc->gap.size()) - 1;
    ++acc->gap[g];
  }
}

int ClassifyRun(const std::shared_ptr<iceberg::Table>& tbl, const Options& opts) {
  static const uint64_t kBackbone[] = {3, 5, 7, 11, 13, 17};
  std::vector<CovModulus> backbone;
  for (uint64_t s : kBackbone) backbone.push_back(BuildCov(s));

  int n_workers = opts.threads > 0
                      ? opts.threads
                      : static_cast<int>(std::thread::hardware_concurrency());
  if (n_workers < 1) n_workers = 1;
  (void)primeparts::common::SetupArrowThreadPools(n_workers);

  std::string err;
  const std::string meta_path = StripFileScheme(tbl->metadata_file_location());
  int64_t total_records = 0;
  {
    auto probe = primeparts::SourceTableReader::OpenMetadata(meta_path, {"p"},
                                                             nullptr, &err);
    if (!probe) {
      std::fprintf(stderr, "error: open reader: %s\n", err.c_str());
      return 1;
    }
    total_records = probe->total_records();
  }

  std::vector<ClassifyAccum> accums(n_workers);
  std::vector<std::string> werr(n_workers);
  std::atomic<int64_t> scanned{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> done{false};
  auto t0 = std::chrono::steady_clock::now();

  auto worker = [&](int shard) {
    std::string e;
    auto reader = primeparts::SourceTableReader::OpenMetadata(
        meta_path, {"p"}, nullptr, &e, shard, n_workers);
    if (!reader) { werr[shard] = "open: " + e; failed.store(true); return; }
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      if (!reader->Next(&batch, &e)) {
        werr[shard] = "read: " + e;
        failed.store(true);
        return;
      }
      if (!batch) break;
      auto p_arr = std::static_pointer_cast<arrow::Int64Array>(
          batch->GetColumnByName("p"));
      const int64_t nn = batch->num_rows();
      ClassifyBatch(p_arr->raw_values(), nn, backbone, &accums[shard]);
      scanned.fetch_add(nn, std::memory_order_relaxed);
    }
  };
  std::vector<std::thread> workers;
  workers.reserve(n_workers);
  for (int i = 0; i < n_workers; ++i) workers.emplace_back(worker, i);
  std::thread monitor([&] {
    while (!done.load(std::memory_order_relaxed)) {
      int64_t s = scanned.load(std::memory_order_relaxed);
      double pct = total_records > 0 ? 100.0 * s / total_records : 0.0;
      std::fprintf(stderr, "\r  classifying: %lld / %lld (%.1f%%)   ",
                   static_cast<long long>(s),
                   static_cast<long long>(total_records), pct);
      std::fflush(stderr);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::fprintf(stderr, "\r%60s\r", "");
    std::fflush(stderr);
  });
  for (auto& t : workers) t.join();
  done.store(true, std::memory_order_relaxed);
  monitor.join();
  if (failed.load()) {
    for (int i = 0; i < n_workers; ++i)
      if (!werr[i].empty()) {
        std::fprintf(stderr, "error: shard %d: %s\n", i, werr[i].c_str());
        break;
      }
    return 1;
  }

  ClassifyAccum tot;
  for (auto& a : accums) {
    tot.total += a.total;
    for (size_t g = 0; g < tot.gap.size(); ++g) tot.gap[g] += a.gap[g];
  }
  auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();

  std::printf("== classify: backbone {3,5,7,11,13,17} over primeparts.%s ==\n",
              opts.table.c_str());
  std::printf("  scanned %lld primes (%.1fs, %.0f Mrow/s)\n",
              static_cast<long long>(tot.total), secs,
              secs > 0 ? tot.total / secs / 1e6 : 0.0);
  std::printf("  residual gap-count (positions per prime the backbone leaves open):\n");
  for (size_t g = 0; g < tot.gap.size(); ++g) {
    if (tot.gap[g] == 0) continue;
    std::printf("    %2zu gaps: %-14lld (%.3f%%)%s\n", g,
                static_cast<long long>(tot.gap[g]),
                tot.total > 0 ? 100.0 * tot.gap[g] / tot.total : 0.0,
                g == 0 ? "   <- fully explained by the backbone" : "");
  }
  return 0;
}

// --overgen: generate the over-generated covering system, dumb and exhaustive.
// No filtering, no k=0 distinction, no statistics -- just the arithmetic
// progressions l*k + a encoded two ways and published as catalog tables:
//
//   covering_system : one row per primitive Mersenne modulus l (read from the
//                     mersenne_factors table, is_primitive=1 -> the distinct
//                     moduli): (l, k) with k = floor(p_max/l), so the family
//                     l*j + a (0 <= a < l, 0 <= j <= k) tiles [0, p_max].
//   covering_log    : one row per (l, p): (l, k, p, a) with p = l*k + a,
//                     k = floor(p/l), a = p mod l -- the Euclidean decomposition
//                     of every prime against every modulus, sorted by (l, p),
//                     delta-packed on the monotone columns (l, k, p).
//
// Both publish through the catalog seam (DropTable purge + CommitFiles), so
// re-runs replace cleanly and the tables are reproducible for the agent and the
// verifier. The reducing pass consumes covering_log; this stage makes no
// decisions about coverage.
int OvergenRun(const std::shared_ptr<iceberg::Catalog>& catalog,
               const std::shared_ptr<iceberg::Table>& primes_tbl,
               const Options& opts) {
  std::string err;

  // Primitive factors (is_primitive=1) from primeparts.mersenne_factors -- the
  // distinct covering moduli l, sorted ascending.
  std::vector<int64_t> ells;
  {
    iceberg::TableIdentifier mf_id{.ns = iceberg::Namespace{{"primeparts"}},
                                   .name = "mersenne_factors"};
    auto mf = catalog->LoadTable(mf_id);
    if (!mf.has_value()) {
      std::fprintf(stderr, "error: LoadTable mersenne_factors: %s\n",
                   mf.error().message.c_str());
      return 1;
    }
    const std::string meta =
        StripFileScheme(mf.value()->metadata_file_location());
    auto reader = primeparts::SourceTableReader::OpenMetadata(
        meta, {"prime", "is_primitive"}, nullptr, &err);
    if (!reader) {
      std::fprintf(stderr, "error: open mersenne_factors: %s\n", err.c_str());
      return 1;
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      if (!reader->Next(&batch, &err)) {
        std::fprintf(stderr, "error: read mersenne_factors: %s\n", err.c_str());
        return 1;
      }
      if (!batch) break;
      auto pr = std::static_pointer_cast<arrow::Int64Array>(
          batch->GetColumnByName("prime"));
      auto ip = std::static_pointer_cast<arrow::Int32Array>(
          batch->GetColumnByName("is_primitive"));
      const int64_t n = batch->num_rows();
      for (int64_t i = 0; i < n; ++i)
        if (ip->Value(i) == 1) ells.push_back(pr->Value(i));
    }
  }
  std::sort(ells.begin(), ells.end());
  ells.erase(std::unique(ells.begin(), ells.end()), ells.end());
  if (ells.empty()) {
    std::fprintf(stderr, "error: mersenne_factors has no primitive factors\n");
    return 1;
  }

  // Every prime p from the source table (the k=0 set as-is), sorted ascending.
  std::vector<int64_t> primes;
  {
    const std::string meta =
        StripFileScheme(primes_tbl->metadata_file_location());
    auto reader =
        primeparts::SourceTableReader::OpenMetadata(meta, {"p"}, nullptr, &err);
    if (!reader) {
      std::fprintf(stderr, "error: open primes: %s\n", err.c_str());
      return 1;
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    bool stop = false;
    while (!stop) {
      if (!reader->Next(&batch, &err)) {
        std::fprintf(stderr, "error: read primes: %s\n", err.c_str());
        return 1;
      }
      if (!batch) break;
      auto pa = std::static_pointer_cast<arrow::Int64Array>(
          batch->GetColumnByName("p"));
      const int64_t* p = pa->raw_values();
      const int64_t n = batch->num_rows();
      for (int64_t i = 0; i < n; ++i) {
        if (opts.limit > 0 && static_cast<int64_t>(primes.size()) >= opts.limit) {
          stop = true;
          break;
        }
        primes.push_back(p[i]);
      }
    }
  }
  std::sort(primes.begin(), primes.end());
  const int64_t NP = static_cast<int64_t>(primes.size());
  if (NP == 0) {
    std::fprintf(stderr, "error: source table has no primes\n");
    return 1;
  }
  const int64_t p_max = primes.back();

  std::printf("== cov-sieve --overgen: generate covering tables ==\n");
  std::printf("  source   : primeparts.%s\n", opts.table.c_str());
  std::printf("  moduli   : %zu primitive factors (l in [%lld, %lld])\n",
              ells.size(), (long long)ells.front(), (long long)ells.back());
  std::printf("  primes   : %lld  (p_max = %lld)\n", (long long)NP,
              (long long)p_max);
  std::printf("  log rows : %lld  (moduli x primes)\n",
              (long long)(static_cast<int64_t>(ells.size()) * NP));

  // Open a replace-mode writer for primeparts.<name> (DropTable purge, then a
  // fresh staging writer). Returns nullptr + sets err on failure.
  auto open_writer =
      [&](const std::string& name,
          const std::shared_ptr<iceberg::Schema>& schema,
          const std::vector<std::string>& delta_cols, int64_t target_rows,
          int64_t max_rg_rows) -> std::unique_ptr<primeparts::BucketParquetWriter> {
    if (!ppc::DropTable(catalog, opts.warehouse, name, /*purge=*/true, &err))
      return nullptr;
    primeparts::WriterConfig cfg;
    cfg.output_dir = ppc::StagingDataDir(opts.warehouse, name);
    cfg.schema = schema;
    cfg.table_name = name;
    cfg.filename_prefix = name;
    cfg.partition_spec = iceberg::PartitionSpec::Unpartitioned();
    cfg.partition_values = std::make_shared<iceberg::PartitionValues>(
        std::vector<iceberg::Literal>{});
    cfg.simple_filename = true;
    cfg.delta_columns = delta_cols;
    cfg.target_rows_per_file = target_rows;
    if (max_rg_rows > 0) cfg.max_row_group_rows = max_rg_rows;
    return primeparts::BucketParquetWriter::Make(cfg, &err);
  };
  auto commit = [&](const std::string& name,
                    const std::shared_ptr<iceberg::Schema>& schema,
                    std::vector<primeparts::WrittenFile>& written) -> bool {
    std::vector<std::shared_ptr<iceberg::DataFile>> dfs;
    for (auto& wf : written)
      if (wf.data_file) dfs.push_back(wf.data_file);
    std::string meta;
    if (!ppc::CommitFiles(catalog, opts.warehouse, name, schema,
                          iceberg::PartitionSpec::Unpartitioned(), dfs, &meta,
                          &err))
      return false;
    std::printf("  committed: primeparts.%s (%zu files) -> %s\n", name.c_str(),
                written.size(), meta.c_str());
    return true;
  };

  // covering_system: (l, floor(p_max/l)) -- one row per modulus.
  {
    auto schema = primeparts::CoveringSystemSchema();
    auto writer = open_writer("covering_system", schema, {"l", "k"}, 0, 0);
    if (!writer) {
      std::fprintf(stderr, "error: covering_system writer: %s\n", err.c_str());
      return 1;
    }
    arrow::Int64Builder lb, kb;
    for (int64_t l : ells)
      if (!lb.Append(l).ok() || !kb.Append(p_max / l).ok()) {
        std::fprintf(stderr, "error: covering_system append\n");
        return 1;
      }
    std::shared_ptr<arrow::Array> la, ka;
    if (!lb.Finish(&la).ok() || !kb.Finish(&ka).ok()) {
      std::fprintf(stderr, "error: covering_system finish\n");
      return 1;
    }
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("l", arrow::int64()),
                       arrow::field("k", arrow::int64())}),
        static_cast<int64_t>(ells.size()), {la, ka});
    primeparts::BucketParquetWriter::BatchStats st{
        .p_min = ells.front(), .p_max = ells.back(), .rank_min = 0, .rank_max = 0};
    if (!writer->Write(*batch, st, &err)) {
      std::fprintf(stderr, "error: covering_system write: %s\n", err.c_str());
      return 1;
    }
    std::vector<primeparts::WrittenFile> written;
    if (!writer->Close(&written, &err)) {
      std::fprintf(stderr, "error: covering_system close: %s\n", err.c_str());
      return 1;
    }
    if (!commit("covering_system", schema, written)) {
      std::fprintf(stderr, "error: commit covering_system: %s\n", err.c_str());
      return 1;
    }
  }

  // covering_log: (l, floor(p/l), p, p mod l) for every (l, p), in (l, p) order.
  {
    auto schema = primeparts::CoveringLogSchema();
    auto writer = open_writer("covering_log", schema, {"l", "k", "p"},
                              /*target_rows=*/64'000'000,
                              /*max_rg_rows=*/8'000'000);
    if (!writer) {
      std::fprintf(stderr, "error: covering_log writer: %s\n", err.c_str());
      return 1;
    }
    auto arrow_schema = arrow::schema({arrow::field("l", arrow::int64()),
                                       arrow::field("k", arrow::int64()),
                                       arrow::field("p", arrow::int64()),
                                       arrow::field("a", arrow::int64())});
    const int64_t BATCH = 1'000'000;
    arrow::Int64Builder lb, kb, pb, ab;
    auto reserve = [&]() {
      (void)lb.Reserve(BATCH);
      (void)kb.Reserve(BATCH);
      (void)pb.Reserve(BATCH);
      (void)ab.Reserve(BATCH);
    };
    int64_t cur = 0, total = 0, bp_min = 0, bp_max = 0;
    auto flush = [&]() -> bool {
      if (cur == 0) return true;
      std::shared_ptr<arrow::Array> la, ka, pa, aa;
      if (!lb.Finish(&la).ok() || !kb.Finish(&ka).ok() ||
          !pb.Finish(&pa).ok() || !ab.Finish(&aa).ok()) {
        err = "covering_log: builder finish";
        return false;
      }
      auto batch = arrow::RecordBatch::Make(arrow_schema, cur, {la, ka, pa, aa});
      primeparts::BucketParquetWriter::BatchStats st{
          .p_min = bp_min, .p_max = bp_max, .rank_min = 0, .rank_max = 0};
      if (!writer->Write(*batch, st, &err)) return false;
      total += cur;
      cur = 0;
      reserve();
      return true;
    };
    reserve();
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t l : ells) {
      if (!flush()) {  // close any partial batch at the modulus boundary
        std::fprintf(stderr, "error: covering_log write: %s\n", err.c_str());
        return 1;
      }
      for (int64_t j = 0; j < NP; ++j) {
        const int64_t p = primes[j];
        const int64_t k = p / l;
        const int64_t a = p - k * l;  // p mod l
        if (cur == 0) bp_min = p;
        bp_max = p;
        lb.UnsafeAppend(l);
        kb.UnsafeAppend(k);
        pb.UnsafeAppend(p);
        ab.UnsafeAppend(a);
        if (++cur >= BATCH && !flush()) {
          std::fprintf(stderr, "error: covering_log write: %s\n", err.c_str());
          return 1;
        }
      }
    }
    if (!flush()) {
      std::fprintf(stderr, "error: covering_log write: %s\n", err.c_str());
      return 1;
    }
    std::vector<primeparts::WrittenFile> written;
    if (!writer->Close(&written, &err)) {
      std::fprintf(stderr, "error: covering_log close: %s\n", err.c_str());
      return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    if (!commit("covering_log", schema, written)) {
      std::fprintf(stderr, "error: commit covering_log: %s\n", err.c_str());
      return 1;
    }
    std::printf("  covering_log: %lld rows in %.1fs\n", (long long)total,
                std::chrono::duration<double>(t1 - t0).count());
  }
  return 0;
}

// ============================ Method 3 ======================================
// covering-method3 — the minimal DISTINCT covering system of congruences, built
// the way the lab spec lays it out (NOT the pat[]/divisibility sieve above).
// Vocabulary, fixed precisely so the two gates never blur:
//
//   * congruence C = (r mod d): the set { m : m == r (mod d) }. A primitive
//     Mersenne factor l with d = ord_l(2) is the GENERATOR of the <2> discrete-
//     log action in F_l^*: a prime p is in l's orbit iff (p mod l) in <2>, and
//     then its phase r = dlog_2(p mod l) gives the class m == r (mod d). The
//     factor generates the action; it is never a divisibility test on p - 2^m.
//   * covering system of Z: a finite { C_i } whose union is all of Z.
//   * DISTINCT : the moduli d_i are pairwise different — each modulus value used
//                at most once, so at a modulus we keep ONE phase, not several.
//   * MINIMAL  : no proper subset is still a covering system — drop any single
//                C_i and some integer (position) is left uncovered.
//     DISTINCT and MINIMAL are independent gates; the report verifies each.
//   * realizable modulus: d = lcm of orders of a set of primitive factors. One
//     factor (atomic) realizes its own order; a FOLD realizes the lcm of several
//     factors' orders (composite congruence pinned by CRT) — this is how the
//     distinct moduli grow past the handful of small atomic orders.

static uint64_t Gcd(uint64_t a, uint64_t b) {
  while (b) { uint64_t t = a % b; a = b; b = t; }
  return a;
}
static uint64_t Lcm(uint64_t a, uint64_t b) {
  return (a && b) ? a / Gcd(a, b) * b : 0;
}

// Physical (not logical) core count from /proc/cpuinfo distinct
// (physical id, core id) pairs; 0 if topology is unavailable.
int PhysicalCores() {
  std::ifstream f("/proc/cpuinfo");
  if (!f) return 0;
  std::set<std::pair<int, int>> cores;
  std::string line;
  int phys = -1, core = -1;
  auto field = [](const std::string& s) -> int {
    auto c = s.find(':');
    return c == std::string::npos ? -1 : std::atoi(s.c_str() + c + 1);
  };
  while (std::getline(f, line)) {
    if (line.rfind("physical id", 0) == 0) phys = field(line);
    else if (line.rfind("core id", 0) == 0) core = field(line);
    else if (line.empty()) {
      if (phys >= 0 && core >= 0) cores.insert({phys, core});
      phys = core = -1;
    }
  }
  if (phys >= 0 && core >= 0) cores.insert({phys, core});
  return static_cast<int>(cores.size());
}

struct M3Atomic { int64_t l; uint32_t d; };  // primitive factor + its order

// Primitive Mersenne factors (is_primitive=1) as atomic (l, ord_l(2)).
bool LoadAtomics(const std::shared_ptr<iceberg::Catalog>& catalog,
                 std::vector<M3Atomic>* out, std::string* err) {
  iceberg::TableIdentifier id{.ns = iceberg::Namespace{{"primeparts"}},
                              .name = "mersenne_factors"};
  auto mf = catalog->LoadTable(id);
  if (!mf.has_value()) {
    *err = "LoadTable mersenne_factors: " + mf.error().message;
    return false;
  }
  const std::string meta = StripFileScheme(mf.value()->metadata_file_location());
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      meta, {"prime", "ord2", "is_primitive"}, nullptr, err);
  if (!reader) return false;
  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (!reader->Next(&batch, err)) return false;
    if (!batch) break;
    auto pr = std::static_pointer_cast<arrow::Int64Array>(
        batch->GetColumnByName("prime"));
    auto od = std::static_pointer_cast<arrow::Int32Array>(
        batch->GetColumnByName("ord2"));
    auto ip = std::static_pointer_cast<arrow::Int32Array>(
        batch->GetColumnByName("is_primitive"));
    const int64_t n = batch->num_rows();
    for (int64_t i = 0; i < n; ++i)
      if (ip->Value(i) == 1 && od->Value(i) >= 1)
        out->push_back({pr->Value(i), static_cast<uint32_t>(od->Value(i))});
  }
  return true;
}

struct M3Cong { uint32_t d, r; std::vector<int64_t> factors; };

// Per-factor realization table for the recall scan: dlog (residue -> phase) and
// the position bitmask of each phase class.
struct M3Realize {
  int64_t l; uint32_t d;
  std::vector<int32_t> dlog;     // dlog[a] = r with 2^r == a (mod l); -1 off-orbit
  std::vector<uint64_t> classm;  // classm[r] = positions m in [1,62], m == r (mod d)
};

struct M3ScanAccum {
  int64_t total = 0, no_hole = 0;
  std::array<int64_t, 64> gap{};  // gap[g] = #primes with g uncovered positions
};

int Method3Run(const std::shared_ptr<iceberg::Catalog>& catalog,
               const Options& opts) {
  std::string err;
  std::vector<M3Atomic> atoms;
  if (!LoadAtomics(catalog, &atoms, &err)) {
    std::fprintf(stderr, "error: load atomics: %s\n", err.c_str());
    return 1;
  }
  if (atoms.empty()) {
    std::fprintf(stderr, "error: no primitive factors\n");
    return 1;
  }

  // order -> smallest realizing factor; order -> all factors (collisions).
  std::map<uint32_t, int64_t> realizer;
  std::map<uint32_t, std::vector<int64_t>> by_order;
  std::map<int64_t, uint32_t> ord_of;
  for (const auto& a : atoms) {
    by_order[a.d].push_back(a.l);
    ord_of[a.l] = a.d;
    auto it = realizer.find(a.d);
    if (it == realizer.end() || a.l < it->second) realizer[a.d] = a.l;
  }
  for (auto& kv : by_order) std::sort(kv.second.begin(), kv.second.end());

  const uint32_t PCAP =
      static_cast<uint32_t>(opts.period_cap > 1 ? opts.period_cap : 2520);
  const uint32_t MINMOD =
      static_cast<uint32_t>(opts.min_modulus > 1 ? opts.min_modulus : 2);

  // Realizable moduli = lcm-closure of the atomic orders, restricted to divisors
  // of PCAP. gen[v] = a minimal set of atomic orders whose lcm is v (size > 1 is
  // a FOLD). Closure converges: finitely many divisors of PCAP.
  std::map<uint32_t, std::vector<uint32_t>> gen;
  for (const auto& kv : realizer)
    if (PCAP % kv.first == 0) gen[kv.first] = {kv.first};
  for (bool changed = true; changed;) {
    changed = false;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> adds;
    for (const auto& g : gen)
      for (const auto& dk : realizer) {
        uint64_t nv = Lcm(g.first, dk.first);
        if (nv == 0 || nv > PCAP || PCAP % nv != 0) continue;
        std::vector<uint32_t> ng = g.second;
        if (std::find(ng.begin(), ng.end(), dk.first) == ng.end())
          ng.push_back(dk.first);
        std::sort(ng.begin(), ng.end());
        auto it = gen.find(static_cast<uint32_t>(nv));
        if (it == gen.end() || ng.size() < it->second.size())
          adds.emplace_back(static_cast<uint32_t>(nv), ng);
      }
    for (auto& pr : adds) {
      auto it = gen.find(pr.first);
      if (it == gen.end() || pr.second.size() < it->second.size()) {
        gen[pr.first] = pr.second;
        changed = true;
      }
    }
  }

  std::vector<uint32_t> cand;
  for (const auto& g : gen)
    if (g.first >= MINMOD) cand.push_back(g.first);
  std::sort(cand.begin(), cand.end());

  // ---- greedy covering-system construction over one period [0, PCAP) --------
  // All candidate moduli divide PCAP, so coverage is PCAP-periodic and "covers
  // Z" == "covers [0, PCAP)". Each step adds the (modulus, phase) covering the
  // most still-open residues; smallest modulus breaks ties (the density 1/d
  // heuristic, used only to order the greedy). DISTINCT is enforced: a modulus
  // already used is skipped. A phase that covers nothing new is exactly the
  // strict-subset case (e.g. 1 mod 6 under 1 mod 3) and is never chosen.
  std::vector<M3Cong> S;
  std::vector<char> covered(PCAP, 0);
  std::set<uint32_t> used;
  uint64_t open = PCAP;
  while (open > 0) {
    uint32_t best_d = 0, best_r = 0;
    uint64_t best_gain = 0;
    for (uint32_t d : cand) {
      if (used.count(d)) continue;
      for (uint32_t r = 0; r < d; ++r) {
        uint64_t gain = 0;
        for (uint32_t m = r; m < PCAP; m += d)
          if (!covered[m]) ++gain;
        if (gain > best_gain || (gain > 0 && gain == best_gain && d < best_d)) {
          best_gain = gain; best_d = d; best_r = r;
        }
      }
    }
    if (best_gain == 0) break;  // cannot extend within PCAP
    for (uint32_t m = best_r; m < PCAP; m += best_d) covered[m] = 1;
    std::vector<int64_t> facs;
    for (uint32_t o : gen[best_d]) facs.push_back(realizer[o]);
    S.push_back({best_d, best_r, facs});
    used.insert(best_d);
    open = 0;
    for (uint32_t m = 0; m < PCAP; ++m)
      if (!covered[m]) ++open;
  }
  const uint64_t build_holes = open;

  // ---- MINIMAL: drop any congruence whose removal still leaves [0, PCAP) fully
  // covered, to fixpoint. Only meaningful once the system is complete. ---------
  if (build_holes == 0) {
    for (bool pruned = true; pruned;) {
      pruned = false;
      for (size_t i = 0; i < S.size(); ++i) {
        std::vector<char> c2(PCAP, 0);
        for (size_t j = 0; j < S.size(); ++j) {
          if (j == i) continue;
          for (uint32_t m = S[j].r; m < PCAP; m += S[j].d) c2[m] = 1;
        }
        bool full = true;
        for (uint32_t m = 0; m < PCAP && full; ++m)
          if (!c2[m]) full = false;
        if (full) { S.erase(S.begin() + i); pruned = true; break; }
      }
    }
  }

  // period, density sum, distinctness, and an explicit minimality re-check.
  uint64_t period = 1;
  double sum_inv = 0;
  bool distinct = true;
  std::set<uint32_t> seen;
  for (const auto& c : S) {
    period = Lcm(period, c.d);
    sum_inv += 1.0 / c.d;
    if (!seen.insert(c.d).second) distinct = false;
  }
  if (period == 0) period = 1;
  bool minimal = (build_holes == 0);
  for (size_t i = 0; i < S.size() && minimal; ++i) {
    std::vector<char> c2(period, 0);
    for (size_t j = 0; j < S.size(); ++j) {
      if (j == i) continue;
      for (uint64_t m = S[j].r; m < period; m += S[j].d) c2[m] = 1;
    }
    bool full = true;
    for (uint64_t m = 0; m < period && full; ++m)
      if (!c2[m]) full = false;
    if (full) minimal = false;  // i was redundant => not minimal
  }

  // ---------------------------- report --------------------------------------
  std::printf("== covering-method3: minimal distinct covering system ==\n");
  std::printf("  atomic orders d=ord_l(2) (primitive factors, smallest l):\n   ");
  for (const auto& kv : realizer)
    std::printf(" %u(l=%lld)", kv.first, static_cast<long long>(kv.second));
  std::printf("\n  order collisions (same modulus -> DROP or FOLD for distinctness):\n");
  bool any_coll = false;
  for (const auto& kv : by_order)
    if (kv.second.size() > 1) {
      any_coll = true;
      std::printf("    d=%u: {", kv.first);
      for (size_t i = 0; i < kv.second.size(); ++i)
        std::printf("%s%lld", i ? "," : "", static_cast<long long>(kv.second[i]));
      std::printf("}\n");
    }
  if (!any_coll) std::printf("    (none among atomic orders)\n");
  std::printf("  params: min_modulus=%u  period_cap=%u  realizable moduli=%zu\n",
              MINMOD, PCAP, cand.size());

  std::vector<M3Cong> disp = S;
  std::sort(disp.begin(), disp.end(),
            [](const M3Cong& a, const M3Cong& b) { return a.d < b.d; });
  std::printf("\n  covering system  (m == r (mod d), realized via <2>-dlog of l):\n");
  for (const auto& c : disp) {
    std::printf("    m == %2u (mod %-4u)  l=", c.r, c.d);
    for (size_t i = 0; i < c.factors.size(); ++i)
      std::printf("%s%lld", i ? "*" : "", static_cast<long long>(c.factors[i]));
    if (c.factors.size() > 1) std::printf("  [FOLD]");
    std::printf("\n");
  }
  std::printf("\n  congruences: %zu   Sum 1/d = %.4f   period (lcm) = %llu\n",
              S.size(), sum_inv, static_cast<unsigned long long>(period));
  std::printf("  DISTINCT (moduli pairwise different): %s\n",
              distinct ? "yes" : "NO");
  std::printf("  MINIMAL  (no proper subset covers)  : %s\n",
              minimal ? "yes" : "NO");
  if (build_holes > 0) {
    std::printf("  INCOMPLETE: %llu of %u residues uncovered within period_cap — "
                "raise --period-cap or lower --min-modulus.\n",
                static_cast<unsigned long long>(build_holes), PCAP);
    return 1;
  }

  // -------- realized recall on the full k=0 set (primes_k0, no k>0) ----------
  // For each k=0 prime p and each factor l the system uses, recover p's OWN
  // phase r = dlog_l(p mod l) and mark positions m == r (mod ord_l(2)). A prime
  // is "explained" iff every position 1..floor(log2 p) is marked (no hole).
  // This is a measurement on k=0 ONLY — k>0 stays sealed until the system is
  // frozen. It is the realized recall of the factor SET (an upper bound on the
  // folded system's recall, since a fold covers a subset of the atomic classes).
  std::set<int64_t> facset;
  for (const auto& c : S)
    for (int64_t l : c.factors) facset.insert(l);
  std::vector<M3Realize> rz;
  for (int64_t l : facset) {
    const uint32_t d = ord_of[l];
    M3Realize v;
    v.l = l; v.d = d;
    v.dlog.assign(static_cast<size_t>(l), -1);
    v.classm.assign(d, 0);
    uint64_t x = 1 % static_cast<uint64_t>(l);
    for (uint32_t r = 0; r < d; ++r) {
      v.dlog[x] = static_cast<int32_t>(r);
      x = (x * 2) % static_cast<uint64_t>(l);
    }
    for (uint32_t m = 1; m <= 62; ++m)
      v.classm[m % d] |= (uint64_t{1} << (m - 1));
    rz.push_back(std::move(v));
  }

  iceberg::TableIdentifier pk_id{.ns = iceberg::Namespace{{"primeparts"}},
                                 .name = "primes_k0"};
  auto pk = catalog->LoadTable(pk_id);
  if (!pk.has_value()) {
    std::fprintf(stderr, "error: LoadTable primes_k0: %s\n",
                 pk.error().message.c_str());
    return 1;
  }
  const std::string meta_path =
      StripFileScheme(pk.value()->metadata_file_location());

  const int phys = PhysicalCores();
  int n_workers = opts.threads > 0
                      ? opts.threads
                      : (phys > 0 ? phys
                                  : static_cast<int>(std::thread::hardware_concurrency()));
  if (n_workers < 1) n_workers = 1;
  (void)primeparts::common::SetupArrowThreadPools(n_workers);

  int64_t total_records = 0;
  {
    auto probe = primeparts::SourceTableReader::OpenMetadata(meta_path, {"p"},
                                                             nullptr, &err);
    if (!probe) {
      std::fprintf(stderr, "error: open primes_k0: %s\n", err.c_str());
      return 1;
    }
    total_records = probe->total_records();
  }

  std::vector<M3ScanAccum> accums(n_workers);
  std::vector<std::string> werr(n_workers);
  std::atomic<int64_t> scanned{0};
  std::atomic<bool> failed{false}, done{false};

  auto worker = [&](int shard) {
    std::string e;
    auto reader = primeparts::SourceTableReader::OpenMetadata(
        meta_path, {"p"}, nullptr, &e, shard, n_workers);
    if (!reader) { werr[shard] = "open: " + e; failed.store(true); return; }
    M3ScanAccum& acc = accums[shard];
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      if (!reader->Next(&batch, &e)) {
        werr[shard] = "read: " + e; failed.store(true); return;
      }
      if (!batch) break;
      auto pa = std::static_pointer_cast<arrow::Int64Array>(
          batch->GetColumnByName("p"));
      const int64_t* p = pa->raw_values();
      const int64_t n = batch->num_rows();
      for (int64_t i = 0; i < n; ++i) {
        const uint64_t pp = static_cast<uint64_t>(p[i]);
        ++acc.total;
        const int max_m = FloorLog2(pp);
        if (max_m <= 0) { ++acc.gap[0]; ++acc.no_hole; continue; }
        const uint64_t full = (uint64_t{1} << max_m) - 1;
        uint64_t cov = 0;
        for (const auto& v : rz) {
          const int32_t r = v.dlog[pp % static_cast<uint64_t>(v.l)];
          if (r >= 0) cov |= v.classm[r];
        }
        int g = __builtin_popcountll(full & ~cov);
        if (g >= 64) g = 63;
        ++acc.gap[g];
        if (g == 0) ++acc.no_hole;
      }
      scanned.fetch_add(n, std::memory_order_relaxed);
    }
  };

  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> ws;
  ws.reserve(n_workers);
  for (int i = 0; i < n_workers; ++i) ws.emplace_back(worker, i);
  std::thread monitor([&] {
    while (!done.load(std::memory_order_relaxed)) {
      int64_t s = scanned.load(std::memory_order_relaxed);
      double pct = total_records > 0 ? 100.0 * s / total_records : 0.0;
      std::fprintf(stderr, "\r  recall scan: %lld / %lld (%.1f%%)   ",
                   static_cast<long long>(s),
                   static_cast<long long>(total_records), pct);
      std::fflush(stderr);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::fprintf(stderr, "\r%60s\r", "");
    std::fflush(stderr);
  });
  for (auto& t : ws) t.join();
  done.store(true, std::memory_order_relaxed);
  monitor.join();
  if (failed.load()) {
    for (int i = 0; i < n_workers; ++i)
      if (!werr[i].empty()) {
        std::fprintf(stderr, "error: shard %d: %s\n", i, werr[i].c_str());
        break;
      }
    return 1;
  }

  M3ScanAccum tot;
  for (const auto& a : accums) {
    tot.total += a.total;
    tot.no_hole += a.no_hole;
    for (size_t g = 0; g < tot.gap.size(); ++g) tot.gap[g] += a.gap[g];
  }
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("\n  realized recall on primes_k0 (k>0 sealed), %d phys cores, %.1fs:\n",
              n_workers, secs);
  std::printf("    factor set: ");
  for (int64_t l : facset) std::printf("%lld ", static_cast<long long>(l));
  std::printf("\n    scanned %lld   no-hole (every position explained) %lld (%.3f%%)\n",
              static_cast<long long>(tot.total),
              static_cast<long long>(tot.no_hole),
              tot.total ? 100.0 * tot.no_hole / tot.total : 0.0);
  std::printf("    uncovered-position histogram (g = positions left open per prime):\n");
  for (size_t g = 0; g < tot.gap.size(); ++g)
    if (tot.gap[g])
      std::printf("      g=%2zu: %-12lld (%.3f%%)%s\n", g,
                  static_cast<long long>(tot.gap[g]),
                  tot.total ? 100.0 * tot.gap[g] / tot.total : 0.0,
                  g == 0 ? "  <- explained" : "");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  static struct option long_opts[] = {
      {"apply", required_argument, nullptr, 'a'},
      {"table", required_argument, nullptr, 't'},
      {"rest-uri", required_argument, nullptr, 'r'},
      {"warehouse", required_argument, nullptr, 'w'},
      {"threads", required_argument, nullptr, 'j'},
      {"top", required_argument, nullptr, 'k'},
      {"metric", required_argument, nullptr, 'm'},
      {"report", no_argument, nullptr, 'R'},
      {"classify", no_argument, nullptr, 'C'},
      {"overgen", no_argument, nullptr, 'O'},
      {"limit", required_argument, nullptr, 'L'},
      {"method3", no_argument, nullptr, '3'},
      {"min-modulus", required_argument, nullptr, 'M'},
      {"period-cap", required_argument, nullptr, 'P'},
      {nullptr, 0, nullptr, 0}};
  int o;
  while ((o = getopt_long(argc, argv, "a:t:r:w:j:k:m:", long_opts, nullptr)) != -1) {
    switch (o) {
      case 'a': opts.apply = std::stoull(optarg); opts.have_apply = true; break;
      case 't': opts.table = optarg; break;
      case 'r': opts.rest_uri = optarg; break;
      case 'w': opts.warehouse = optarg; break;
      case 'j': opts.threads = std::atoi(optarg); break;
      case 'k': opts.show_top = std::atoi(optarg); break;
      case 'm': opts.metric = optarg; break;
      case 'R': opts.report = true; break;
      case 'C': opts.classify = true; break;
      case 'O': opts.overgen = true; break;
      case 'L': opts.limit = std::strtoll(optarg, nullptr, 10); break;
      case '3': opts.method3 = true; break;
      case 'M': opts.min_modulus = std::atoi(optarg); break;
      case 'P': opts.period_cap = std::atoi(optarg); break;
      default: Usage(argv[0]); return 2;
    }
  }
  if (opts.metric != "hybrid" && opts.metric != "bestclass" &&
      opts.metric != "expected") {
    std::fprintf(stderr, "error: --metric must be hybrid|bestclass|expected\n");
    return 2;
  }
  if (!opts.report && !opts.classify && !opts.overgen && !opts.method3 &&
      !opts.have_apply) {
    Usage(argv[0]);
    return 2;
  }

  // --- Load the table via the catalog (read + RowDelta commits). REST-default
  // (--rest-uri / PRIMEPARTS_REST_URI) with transparent local LMDB fallback. ----
  std::string err;
  auto catalog = ppc::OpenCatalog(opts.warehouse, opts.rest_uri, nullptr, &err);
  if (!catalog) {
    std::fprintf(stderr, "error: OpenCatalog: %s\n", err.c_str());
    return 1;
  }
  // Method 3 loads its own tables (mersenne_factors + primes_k0); dispatch
  // before the --table load so it does not depend on the sieve clone existing.
  if (opts.method3) return Method3Run(catalog, opts);

  iceberg::TableIdentifier ident{.ns = iceberg::Namespace{{"primeparts"}},
                                 .name = opts.table};
  auto loaded = catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    std::fprintf(stderr, "error: LoadTable primeparts.%s: %s\n",
                 opts.table.c_str(), loaded.error().message.c_str());
    return 1;
  }
  auto tbl = loaded.value();

  if (opts.report) return ReportArc(tbl);
  if (opts.classify) return ClassifyRun(tbl, opts);
  if (opts.overgen) return OvergenRun(catalog, tbl, opts);

  primeparts::MersenneHelper helper = primeparts::BuildMersenneHelper(64);
  const int32_t ord_new = helper.GetOrd2(opts.apply);
  if (ord_new == 0) {
    std::fprintf(stderr, "error: modulus %llu has no known ord_s(2) up to d=64 "
                 "(must be an odd prime with 2 invertible mod s)\n",
                 static_cast<unsigned long long>(opts.apply));
    return 1;
  }

  // --- Applied moduli (from snapshot history) + this pass ------------------
  std::vector<uint64_t> applied = AppliedModuli(tbl);
  if (std::find(applied.begin(), applied.end(), opts.apply) != applied.end()) {
    std::fprintf(stderr, "error: modulus %llu already applied (1:1 per snapshot)\n",
                 static_cast<unsigned long long>(opts.apply));
    return 1;
  }
  std::vector<CovModulus> prior;
  prior.reserve(applied.size());
  for (uint64_t s : applied) prior.push_back(BuildCov(s));
  const CovModulus newc = BuildCov(opts.apply);
  const int32_t pass_index = static_cast<int32_t>(applied.size()) + 1;
  const bool do_triage = opts.show_top > 0;

  int n_workers = opts.threads > 0
                      ? opts.threads
                      : static_cast<int>(std::thread::hardware_concurrency());
  if (n_workers < 1) n_workers = 1;
  (void)primeparts::common::SetupArrowThreadPools(n_workers);

  std::printf("== covering-sieve pass %d ==\n", pass_index);
  std::printf("  table   : primeparts.%s\n", opts.table.c_str());
  std::printf("  applied : ");
  for (size_t i = 0; i < applied.size(); ++i)
    std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(applied[i]));
  std::printf("%s\n", applied.empty() ? "(none)" : "");
  std::printf("  adding  : %llu (ord_s(2)=%d)\n",
              static_cast<unsigned long long>(opts.apply), ord_new);
  std::printf("  shards  : %d   metric: %s\n", n_workers, opts.metric.c_str());

  const std::string meta_path = StripFileScheme(tbl->metadata_file_location());

  // Whole-snapshot row count as the progress denominator (metadata-only open).
  int64_t total_records = 0;
  {
    auto probe = primeparts::SourceTableReader::OpenMetadata(
        meta_path, {"p"}, /*filter=*/nullptr, &err);
    if (!probe) {
      std::fprintf(stderr, "error: open reader: %s\n", err.c_str());
      return 1;
    }
    total_records = probe->total_records();
  }

  // --- Sift the live set: N independent delete-aware shard readers ----------
  std::vector<SiftAccum> accums(n_workers);
  std::vector<std::string> worker_err(n_workers);
  std::atomic<int64_t> scanned{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> done{false};
  auto t0 = std::chrono::steady_clock::now();

  auto worker = [&](int shard) {
    SiftAccum& acc = accums[shard];
    std::string e;
    auto reader = primeparts::SourceTableReader::OpenMetadata(
        meta_path, {"p", "_pos"}, /*filter=*/nullptr, &e, shard, n_workers);
    if (!reader) {
      worker_err[shard] = "open: " + e;
      failed.store(true);
      return;
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      if (!reader->Next(&batch, &e)) {
        worker_err[shard] = "read: " + e;
        failed.store(true);
        return;
      }
      if (!batch) break;  // shard EOF
      auto p_arr = std::static_pointer_cast<arrow::Int64Array>(
          batch->GetColumnByName("p"));
      auto pos_arr = std::static_pointer_cast<arrow::Int64Array>(
          batch->GetColumnByName("_pos"));
      const std::string& file = reader->current_data_file_path();
      const int64_t n = batch->num_rows();
      SiftBatch(p_arr->raw_values(), pos_arr->raw_values(), n, file, newc, prior,
                do_triage, &acc);
      scanned.fetch_add(n, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(n_workers);
  for (int i = 0; i < n_workers; ++i) workers.emplace_back(worker, i);

  std::thread monitor([&] {
    while (!done.load(std::memory_order_relaxed)) {
      int64_t s = scanned.load(std::memory_order_relaxed);
      double pct = total_records > 0 ? 100.0 * s / total_records : 0.0;
      std::fprintf(stderr, "\r  scanning: %lld / %lld (%.1f%%)   ",
                   static_cast<long long>(s),
                   static_cast<long long>(total_records), pct);
      std::fflush(stderr);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::fprintf(stderr, "\r%60s\r", "");
    std::fflush(stderr);
  });

  for (auto& t : workers) t.join();
  done.store(true, std::memory_order_relaxed);
  monitor.join();

  if (failed.load()) {
    for (int i = 0; i < n_workers; ++i) {
      if (!worker_err[i].empty()) {
        std::fprintf(stderr, "error: shard %d: %s\n", i, worker_err[i].c_str());
        break;
      }
    }
    return 1;
  }

  // --- Merge per-shard accumulators ----------------------------------------
  std::vector<std::pair<std::string, int64_t>> deletes;
  int64_t live_in = 0, newly_deleted = 0, live_out = 0;
  uint64_t min_survivor = std::numeric_limits<uint64_t>::max();
  std::unordered_map<uint64_t, int64_t> hist;
  for (auto& a : accums) {
    live_in += a.live_in;
    newly_deleted += a.newly_deleted;
    live_out += a.live_out;
    if (a.min_survivor < min_survivor) min_survivor = a.min_survivor;
    for (const auto& [r, c] : a.hist) hist[r] += c;
    if (!a.deletes.empty()) {
      deletes.insert(deletes.end(), std::make_move_iterator(a.deletes.begin()),
                     std::make_move_iterator(a.deletes.end()));
    }
  }
  const int64_t first_hole =
      (min_survivor == std::numeric_limits<uint64_t>::max())
          ? 0
          : static_cast<int64_t>(min_survivor);
  int64_t total_open = 0;
  for (const auto& [r, c] : hist)
    total_open += static_cast<int64_t>(__builtin_popcountll(r)) * c;
  auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();

  std::printf("  scanned : %lld live  ->  deleted %lld, survivors %lld  (%.1fs, %.0f Mrow/s)\n",
              static_cast<long long>(live_in),
              static_cast<long long>(newly_deleted),
              static_cast<long long>(live_out), secs,
              secs > 0 ? live_in / secs / 1e6 : 0.0);
  std::printf("  first hole (MIN p of survivors, diagnostic): %lld\n",
              static_cast<long long>(first_hole));

  // Always commit a snapshot recording this modulus — even a 0-delete pass:
  // coverage is incremental (no single modulus fully covers any prime with
  // max_m>=2), so an early modulus that deletes nothing still contributes
  // coverage that later moduli complete, and MUST be recorded as applied.
  auto rd_r = ppc::RowDelta::Make(tbl);
  if (!rd_r.has_value()) {
    std::fprintf(stderr, "error: RowDelta::Make: %s\n", rd_r.error().message.c_str());
    return 1;
  }
  auto rd = rd_r.value();

  // --- Write the position-delete file (only if there are deletions) --------
  if (newly_deleted > 0) {
    // One delete file for the pass; entries sorted by (file_path, pos) per spec.
    std::sort(deletes.begin(), deletes.end());
    auto schema_r = tbl->schema();
    auto spec_r = tbl->spec();
    if (!schema_r.has_value() || !spec_r.has_value()) {
      std::fprintf(stderr, "error: schema/spec unavailable\n");
      return 1;
    }
    const std::string del_path = StripFileScheme(tbl->location()) + "/data/sieve-del-pass" +
                                 std::to_string(pass_index) + "-mod" +
                                 std::to_string(opts.apply) + ".parquet";
    // The shallow clone has no data/ dir of its own (its data files are
    // referenced in primes_k0/data/); our delete files live under the sieve
    // table, so ensure that directory exists before writing.
    std::error_code mkec;
    std::filesystem::create_directories(
        std::filesystem::path(del_path).parent_path(), mkec);
    iceberg::PositionDeleteWriterOptions wopts{
        .path = del_path,
        .schema = schema_r.value(),
        .spec = spec_r.value(),
        .partition = iceberg::PartitionValues{},
        .format = iceberg::FileFormatType::kParquet,
        .io = tbl->io(),
        .flush_threshold = 10000,
        .properties = {{"write.parquet.compression-codec", "zstd"}},
    };
    auto writer_r = iceberg::PositionDeleteWriter::Make(wopts);
    if (!writer_r.has_value()) {
      std::fprintf(stderr, "error: PositionDeleteWriter: %s\n",
                   writer_r.error().message.c_str());
      return 1;
    }
    auto writer = std::move(writer_r.value());
    for (const auto& [f, pos] : deletes) {
      if (auto s = writer->WriteDelete(f, pos); !s.has_value()) {
        std::fprintf(stderr, "error: WriteDelete: %s\n", s.error().message.c_str());
        return 1;
      }
    }
    if (auto s = writer->Close(); !s.has_value()) {
      std::fprintf(stderr, "error: writer Close: %s\n", s.error().message.c_str());
      return 1;
    }
    auto meta_r = writer->Metadata();
    if (!meta_r.has_value() || meta_r.value().data_files.empty()) {
      std::fprintf(stderr, "error: writer Metadata empty\n");
      return 1;
    }
    for (const auto& df : meta_r.value().data_files) rd->AddDeleteFile(df);
  }

  // --- Commit via RowDelta (IRC), stamping covering.* into the summary -----
  rd->Set(kModulusKey, std::to_string(opts.apply))
     .Set("covering.ord", std::to_string(ord_new))
     .Set("covering.pass_index", std::to_string(pass_index))
     .Set("covering.first_hole", std::to_string(first_hole))
     .Set("covering.live_count", std::to_string(live_out))
     .Set("covering.newly_deleted", std::to_string(newly_deleted))
     .Set("covering.open_positions", std::to_string(total_open));
  if (auto s = rd->Commit(); !s.has_value()) {
    std::fprintf(stderr, "error: RowDelta Commit: %s\n", s.error().message.c_str());
    return 1;
  }

  std::printf("  committed pass %d: deleted %lld, %lld uncovered remain, first hole %lld\n",
              pass_index, static_cast<long long>(newly_deleted),
              static_cast<long long>(live_out),
              static_cast<long long>(first_hole));

  // --- Triage: best-class structural elimination per Mersenne order --------
  // A modulus of order d tiles exactly ONE residue class of positions mod d;
  // its best-case elimination over the live set is, per residual pattern R,
  // max_c popcount(R & {positions m == c (mod d)}). Rank orders by that sum.
  // Candidate moduli are the primitive Mersenne factors (helper.ord2_by_q),
  // never tested per prime — we only read the {R -> count} tally against them.
  if (do_triage) {
    if (!hist.empty() && total_open > 0) {
      auto is_applied = [&](uint64_t q) {
        if (q == opts.apply) return true;
        return std::find(applied.begin(), applied.end(), q) != applied.end();
      };
      // Best-class elimination per Mersenne order d (structure-aware,
      // data-dependent), memoized: a modulus of order d tiles ONE class mod d,
      // best case max_c popcount(R & {positions m == c mod d}) over the tally.
      std::map<int32_t, int64_t> bc;
      auto best_class = [&](int32_t d) -> int64_t {
        auto it = bc.find(d);
        if (it != bc.end()) return it->second;
        std::vector<uint64_t> cm(d, 0);  // cm[c] = positions m == c (mod d)
        for (int m = 1; m <= kMaxPos; ++m) cm[m % d] |= (uint64_t{1} << (m - 1));
        int64_t sd = 0;
        for (const auto& [r, c] : hist) {
          int best = 0;
          for (int cc = 0; cc < d; ++cc) {
            int pc = __builtin_popcountll(r & cm[cc]);
            if (pc > best) best = pc;
          }
          sd += static_cast<int64_t>(best) * c;
        }
        bc[d] = sd;
        return sd;
      };
      // Candidate primes (primitive Mersenne factors) scored structure-aware
      // but WEIGHTED by realization probability d/(q-1) = fraction of residues
      // in <2> mod q. This keeps best-class's residual-awareness while killing
      // large-period / large-q artifacts (e.g. 257 only realizes 16/256).
      struct Cand { double score; double prealize; uint64_t q; int32_t d; };
      std::vector<Cand> cands;
      for (const auto& [q, d] : helper.ord2_by_q) {
        if (d < 2 || d > kMaxPos || q < 3) continue;
        if (is_applied(q)) continue;
        const double prealize =
            static_cast<double>(d) / static_cast<double>(q - 1);
        double score;
        if (opts.metric == "bestclass") {
          score = static_cast<double>(best_class(d));
        } else if (opts.metric == "expected") {
          // realized expected reduction ~ total_open/(q-1) (structure-blind).
          score = static_cast<double>(total_open) / static_cast<double>(q - 1);
        } else {  // hybrid
          score = static_cast<double>(best_class(d)) * prealize;
        }
        cands.push_back({score, prealize, q, d});
      }
      std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.q < b.q;
      });
      std::printf("\n  triage over %lld survivors (%lld open positions) — next "
                  "modulus [metric=%s]:\n",
                  static_cast<long long>(live_out),
                  static_cast<long long>(total_open), opts.metric.c_str());
      const int top = std::min<int>(opts.show_top, static_cast<int>(cands.size()));
      for (int k = 0; k < top; ++k) {
        const double pct =
            100.0 * cands[k].score / static_cast<double>(total_open);
        std::printf("    s=%-6llu (M_%d, realize %.0f%%):  %.2f%%%s\n",
                    static_cast<unsigned long long>(cands[k].q), cands[k].d,
                    100.0 * cands[k].prealize, pct,
                    k == 0 ? "   <- suggested next" : "");
      }
    }
  }
  return 0;
}
