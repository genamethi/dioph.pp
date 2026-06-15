// primeparts/query/query_service.h
//
// QueryService — the interactive data-query layer over the primeparts warehouse.
//
// Resolves tables through the LMDB catalog seam (MakeLocalCatalog -> LoadTable ->
// metadata.json location) and reads via SourceTableReader, so it is the first
// clean citizen of the ground-up catalog seam (not a raw-sqlite reader).
//
// Two priority queries (see markdown/arch/tui_query_design.md):
//   * ScanByK(k, limit)   — primes with k == K, early-stop at limit. Fast.
//   * LookupPrime(p) + LookupPartitions(p) — point lookup. ~seconds on the
//     current layout (file-pruning only, no row-group skip; a sparse
//     prime_rank->offset index is the future accelerator).

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace primeparts::query {

namespace fs = std::filesystem;

struct PrimeInfo {
  int64_t p = 0;
  int32_t k = 0;
  int64_t prime_rank = 0;
};

struct PartitionTuple {
  int32_t m_k = 0;
  int32_t n_k = 0;
  int64_t q_k = 0;
};

struct ScanHit {
  int64_t p = 0;
  int64_t prime_rank = 0;
};

class QueryService {
 public:
  /// Open against a warehouse root (the dir holding catalog.lmdb). Builds the
  /// local LMDB catalog. Returns nullptr + *error on failure.
  static std::unique_ptr<QueryService> Open(const fs::path& warehouse,
                                            std::string* error);
  ~QueryService();
  QueryService(const QueryService&) = delete;
  QueryService& operator=(const QueryService&) = delete;

  /// Point lookup of a prime. Returns nullopt if p is absent (not an error);
  /// sets *error only on a real failure.
  std::optional<PrimeInfo> LookupPrime(int64_t p, std::string* error);

  /// The (m_k, n_k, q_k) partitions of p (empty for k=0 primes). Sets *error on
  /// failure.
  std::vector<PartitionTuple> LookupPartitions(int64_t p, std::string* error);

  /// Up to `limit` primes with k == `k`, in p-order, early-stopping once `limit`
  /// hits are collected.
  std::vector<ScanHit> ScanByK(int32_t k, int64_t limit, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit QueryService(std::unique_ptr<Impl> impl);
};

}  // namespace primeparts::query
