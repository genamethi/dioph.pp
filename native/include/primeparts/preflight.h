// Preflight assumption checks for the rewriter (Pass 0).
//
// Runs four read-only checks against the source Iceberg warehouse before
// any writes happen. Refuses to proceed if any check fails. Each check
// is independent — failures are collected so the user gets one shot to
// see everything that's wrong, not fail-fast.
//
// 1. No file p-range overlap in funbuns.primes and funbuns.partitions
//    (manifest-only).
// 2. Sort-order metadata matches: primes (p ASC), partitions (p ASC, m_k ASC).
// 3. In-file sort spot-check: 1 file per bucket per table, batches
//    p-ascending (and (p, m_k) for partitions).
// 4. k-sum from primes == total record_count from partitions.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace primeparts {

namespace fs = std::filesystem;

struct PreflightCheck {
  std::string name;
  bool passed = false;
  std::string detail;  // Empty on pass; failure description on fail.
};

struct PreflightReport {
  std::vector<PreflightCheck> checks;

  bool all_passed() const {
    for (const auto& c : checks)
      if (!c.passed) return false;
    return true;
  }

  // Serialize to JSON for the report log.
  std::string ToJson() const;
};

// Runs all four checks. `sqlite_path` is the source catalog DB
// (read-only). `ns` / `primes_tbl` / `partitions_tbl` identify the
// source tables (in production: "funbuns", "primes", "decompositions").
// Always returns a populated report — even on fatal internal errors,
// those show up as failed checks. Never throws.
PreflightReport RunPreflight(const fs::path& sqlite_path,
                             std::string_view ns,
                             std::string_view primes_tbl,
                             std::string_view partitions_tbl);

}  // namespace primeparts
