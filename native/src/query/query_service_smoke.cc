// Smoke for QueryService against the live LMDB-cataloged warehouse.
// Usage: query-service-smoke <warehouse_root>   (default: ib-staging)

#include "primeparts/query/query_service.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using primeparts::query::QueryService;

namespace {
double secs_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main(int argc, char** argv) {
  const std::string wh =
      argc >= 2 ? argv[1] : "/media/extssd/research/dioph.pp/data/ib-staging";

  std::string err;
  auto qs = QueryService::Open(wh, &err);
  if (!qs) {
    std::fprintf(stderr, "[!] Open: %s\n", err.c_str());
    return 1;
  }
  std::printf("[ok] QueryService open on %s (via catalog.lmdb)\n", wh.c_str());

  int failures = 0;

  // 1) k-scan: k=0, LIMIT 10, unbounded p (the priority query) — near-instant.
  {
    auto t0 = std::chrono::steady_clock::now();
    auto hits = qs->ScanByK(0, /*p_lo=*/0, /*p_hi=*/0, /*limit=*/10, &err);
    double dt = secs_since(t0);
    std::printf("\n[k-scan] k=0 LIMIT 10 -> %zu hits in %.3fs\n", hits.size(), dt);
    for (auto& h : hits) std::printf("    p=%lld  rank=%lld\n", (long long)h.p, (long long)h.prime_rank);
    if (hits.size() != 10) { std::printf("    [!] expected 10\n"); ++failures; }
  }

  // 1b) k-scan with a p-window pushdown: k=0 in p in [1e9, 2e9], LIMIT 5.
  {
    auto t0 = std::chrono::steady_clock::now();
    auto hits = qs->ScanByK(0, 1000000000LL, 2000000000LL, 5, &err);
    double dt = secs_since(t0);
    std::printf("\n[k-scan] k=0 p in [1e9,2e9] LIMIT 5 -> %zu hits in %.3fs\n",
                hits.size(), dt);
    bool in_range = true;
    for (auto& h : hits) {
      std::printf("    p=%lld  rank=%lld\n", (long long)h.p, (long long)h.prime_rank);
      if (h.p < 1000000000LL || h.p > 2000000000LL) in_range = false;
    }
    if (hits.size() != 5 || !in_range) {
      std::printf("    [!] expected 5 hits all within [1e9,2e9]\n"); ++failures;
    }
  }

  // 2) point lookup: p=11 -> k=3, rank=5; partitions = 3 tuples.
  {
    auto t0 = std::chrono::steady_clock::now();
    auto pi = qs->LookupPrime(11, &err);
    double dt = secs_since(t0);
    if (pi) {
      std::printf("\n[lookup] p=11 -> k=%d rank=%lld in %.3fs\n", pi->k,
                  (long long)pi->prime_rank, dt);
      if (pi->k != 3 || pi->prime_rank != 5) { std::printf("    [!] expected k=3 rank=5\n"); ++failures; }
    } else {
      std::printf("\n[lookup] p=11 NOT FOUND%s%s\n", err.empty() ? "" : ": ", err.c_str());
      ++failures;
    }
    auto parts = qs->LookupPartitions(11, &err);
    std::printf("[lookup] p=11 partitions (%zu):\n", parts.size());
    for (auto& pt : parts)
      std::printf("    m=%d n=%d q=%lld\n", pt.m_k, pt.n_k, (long long)pt.q_k);
    if (parts.size() != 3) { std::printf("    [!] expected 3 partitions (k=3)\n"); ++failures; }
  }

  // 3) a non-prime even number -> absent.
  {
    auto pi = qs->LookupPrime(12, &err);
    std::printf("\n[lookup] p=12 (not prime) -> %s\n", pi ? "FOUND (BUG)" : "absent (ok)");
    if (pi) ++failures;
  }

  // 4) cooperative cancel: an unbounded k=16 scan is sparse (no k=16 in the
  // first ~900M rows) so it would run a long time. Cancel after a beat and
  // require a prompt return — this is the machinery the TUI's worker uses.
  {
    std::atomic<bool> cancel{false};
    primeparts::query::ScanControl ctl;
    ctl.cancel = &cancel;
    int64_t last_scanned = 0;
    ctl.progress = [&](int64_t sc, int64_t) { last_scanned = sc; };
    auto t0 = std::chrono::steady_clock::now();
    std::thread th([&] {
      std::string e;
      qs->ScanByK(16, 0, 0, 10, &e, ctl);  // sparse -> long-running
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    cancel.store(true);
    th.join();
    double dt = secs_since(t0);
    std::printf("\n[cancel] k=16 unbounded cancelled after 0.4s -> returned in "
                "%.2fs (scanned ~%lld rows)\n", dt, (long long)last_scanned);
    if (dt > 5.0) { std::printf("    [!] cancel too slow (>5s)\n"); ++failures; }
  }

  std::printf("\n== query-service-smoke %s (%d failures) ==\n",
              failures == 0 ? "PASS" : "FAIL", failures);
  return failures == 0 ? 0 : 1;
}
