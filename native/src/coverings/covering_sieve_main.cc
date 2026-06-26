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

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/pp_row_delta.h"
#include "primeparts/common/thread_pool.h"
#include "primeparts/common/uri.h"

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>

#include "iceberg/catalog.h"
#include "iceberg/data/position_delete_writer.h"
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
#include <getopt.h>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ppc = primeparts::catalog;

namespace {

constexpr char kDefaultRestUri[] = "http://192.168.1.202:9090/iceberg";
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
  std::string rest_uri = kDefaultRestUri;
  std::string table = kDefaultTable;
  uint64_t apply = 0;   // the modulus to add this pass (required)
  bool have_apply = false;
  int threads = 0;      // 0 => hardware_concurrency()
  int show_top = kDefaultTop;  // triage rows to show (0 => triage off)
  std::string metric = "expected";  // triage scoring: expected|bestclass|hybrid
  bool report = false;  // dump campaign arc from snapshot history and exit
  bool classify = false;  // structural pass: backbone residual gap distribution
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s --apply S [options]\n"
    "  Add odd-prime modulus S to the covering on primeparts.<table>, delete\n"
    "  the primes it pushes to fully covered, commit a snapshot (IRC), and rank\n"
    "  the next modulus by best-class elimination over the residual bitmasks.\n"
    "  --apply S            modulus to add this pass (odd prime, known ord_s(2))\n"
    "  --table NAME         delete-target table (default %s)\n"
    "  --rest-uri URI       IRC endpoint (default %s)\n"
    "  --warehouse DIR      warehouse root (default %s)\n"
    "  --threads N          parallel read-shard threads (default: hw concurrency)\n"
    "  --top N              triage rows to display (default %d; 0 = no triage)\n"
    "  --metric M           triage scoring: expected (default) | bestclass | hybrid\n"
    "                         bestclass = max_c popcount (structure-aware, no weight)\n"
    "                         hybrid    = bestclass x d/(q-1) (realization-weighted)\n"
    "                         expected  = total_open/(q-1) (realized, ~1/(q-1))\n"
    "  --report             print the campaign arc from snapshot history, exit\n"
    "  --classify           structural pass: backbone {3,5,7,11,13,17} residual\n"
    "                         gap-count distribution over the table, then exit\n",
    argv0, kDefaultTable, kDefaultRestUri, kDefaultWarehouse, kDefaultTop);
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
      default: Usage(argv[0]); return 2;
    }
  }
  if (opts.metric != "hybrid" && opts.metric != "bestclass" &&
      opts.metric != "expected") {
    std::fprintf(stderr, "error: --metric must be hybrid|bestclass|expected\n");
    return 2;
  }
  if (!opts.report && !opts.classify && !opts.have_apply) {
    Usage(argv[0]);
    return 2;
  }

  // --- Load the table via the local LMDB catalog (read + RowDelta commits) --
  std::string err;
  auto catalog = ppc::MakeLocalCatalog(opts.warehouse, &err);
  if (!catalog) {
    std::fprintf(stderr, "error: MakeLocalCatalog: %s\n", err.c_str());
    return 1;
  }
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
