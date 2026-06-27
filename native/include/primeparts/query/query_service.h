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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "primeparts/query/query_preset.h"

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

/// One bucket of a GroupCount histogram: `value` of the grouped column and the
/// number of rows carrying it. Int32 columns are widened to int64.
struct GroupCountRow {
  int64_t value = 0;
  int64_t count = 0;
};

/// Result of ReadTable: the column names and row-major int64 values (one int64
/// per column per row; int32 columns are widened).
struct TableRows {
  std::vector<std::string> cols;
  std::vector<std::vector<int64_t>> rows;
};

/// Warehouse-status facts for one table, read from the catalog metadata and the
/// current snapshot summary (rows / files / size — zero scan). `max_p` is the
/// max of the "p" column's manifest upper bounds (a manifest aggregate, only
/// computed on request and only for tables with a "p" column); -1 if absent.
struct TableExtent {
  std::string table;
  bool ok = false;
  int64_t row_count = -1;    // summary total-records
  int64_t data_files = -1;   // summary total-data-files
  int64_t file_bytes = -1;   // summary total-files-size
  int64_t snapshots = 0;     // number of snapshots in history
  int64_t snapshot_id = -1;  // current snapshot id (-1 if none)
  int64_t sequence = -1;     // current snapshot sequence number
  int64_t max_p = -1;        // max upper-bound of column "p" (-1 if no p column)
};

/// Cooperative control for a (possibly long) scan. `cancel` is checked once per
/// batch — set it from another thread to stop promptly. `progress` is invoked
/// periodically with (rows_scanned, rows_total) for a progress indicator. Both
/// optional; a default-constructed control runs to completion silently.
struct ScanControl {
  std::atomic<bool>* cancel = nullptr;
  std::function<void(int64_t scanned, int64_t total)> progress;
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
  std::optional<PrimeInfo> LookupPrime(int64_t p, std::string* error,
                                       const ScanControl& ctl = {});

  /// The (m_k, n_k, q_k) partitions of p (empty for k=0 primes). Sets *error on
  /// failure.
  std::vector<PartitionTuple> LookupPartitions(int64_t p, std::string* error,
                                               const ScanControl& ctl = {});

  /// Up to `limit` primes with k == `k`, scanned within the p-window
  /// [p_lo, p_hi] (a pushdown predicate on p; <= 0 means open on that end),
  /// in p-order, early-stopping once `limit` hits are collected. The p-window
  /// is load-bearing for sparse high-k values (no max_k stat to prune on).
  std::vector<ScanHit> ScanByK(int32_t k, int64_t p_lo, int64_t p_hi,
                               int64_t limit, std::string* error,
                               const ScanControl& ctl = {});

  /// General group-by-value count: scans `table`, tallies how many rows carry
  /// each distinct value of integer column `column`, and returns the histogram
  /// ordered by value. The optional p-window [p_lo, p_hi] (<= 0 = open) is an
  /// iceberg file-pruning predicate (both base tables carry `p`). `threads`
  /// drives sharded parallel readers (SourceTableReader sharding); <= 0 means a
  /// sensible default. Intended for LOW-cardinality columns (k, m_k, n_k, …) —
  /// grouping by a high-cardinality column (p) would build a huge map.
  ///
  /// This is the one reusable aggregation primitive: e.g. the per-k histogram is
  /// `GroupCount("primes", "k", 0, 0, 0, &err)`. Empty + *error on failure.
  std::vector<GroupCountRow> GroupCount(const std::string& table,
                                        const std::string& column,
                                        int64_t p_lo, int64_t p_hi, int threads,
                                        std::string* error,
                                        const ScanControl& ctl = {});

  /// Cache a small in-memory integer result as the unpartitioned MV
  /// primeparts.<name> (replace semantics). `columns` is column-major,
  /// `col_names[j]` names `columns[j]`; all int64. Sets *metadata_location.
  /// The complement of ReadTable — together they are the MV cache lifecycle.
  bool Materialize(const std::string& name,
                   const std::vector<std::string>& col_names,
                   const std::vector<std::vector<int64_t>>& columns,
                   std::string* metadata_location, std::string* error);

  /// Read up to `limit` rows (<= 0 = all) of integer columns from
  /// primeparts.<table>. Empty `cols` = every int32/int64 schema column (int32
  /// widened to int64). Empty result + *error on failure.
  TableRows ReadTable(const std::string& table,
                      const std::vector<std::string>& cols, int64_t limit,
                      std::string* error);

  /// Distinct schema field names across the base tables (primes + partitions),
  /// read from the catalog schema and cached. The authority for preset
  /// validation — this is the "reader/catalog verifies the query" seam.
  const std::vector<std::string>& SchemaFields();

  /// Validate a query preset: non-empty id, known `kind`, and `accepts`/`target`
  /// referencing real schema fields (`target` must also be a declared field).
  /// Returns false + a human-readable `*error` on the first violation.
  bool ValidatePreset(const QueryPreset& p, std::string* error);

  /// Table names registered under the primeparts namespace, sorted. The Status
  /// screen's row source. Empty + *error on catalog failure.
  std::vector<std::string> ListTables(std::string* error);

  /// Warehouse status for one table. Summary facts (rows/files/size/snapshots)
  /// are read from the current snapshot — zero scan. With `with_max_p`, also
  /// aggregates the "p" column's manifest upper bounds (a manifest read, not a
  /// data scan; cancellable via `ctl`). Sets *error only on a real failure.
  TableExtent Extent(const std::string& table, bool with_max_p,
                     std::string* error, const ScanControl& ctl = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit QueryService(std::unique_ptr<Impl> impl);
};

}  // namespace primeparts::query
