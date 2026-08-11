#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace duckdb {
class Connection;
}  // namespace duckdb

namespace primeparts::graph {

// Scans (p, m_k, n_k) rows out of the given `partitions` parquet files via
// DuckDB, solves each row's q via primeparts::SolveQ, and invokes `on_edge`
// for every row where SolveQ succeeds and q falls in [min_q, max_q]
// (pass INT64_MIN/INT64_MAX to skip that check). `extra_where`, if
// non-empty, is appended as an additional SQL predicate on top of the
// implicit read_parquet(...) scan. Every row DuckDB returns counts toward
// `*rows_scanned`, including ones SolveQ rejects.
bool ScanPartitionEdges(
    duckdb::Connection* con, const std::vector<std::string>& paths,
    const std::string& extra_where, int64_t min_q, int64_t max_q,
    const std::function<void(int64_t p, int32_t m, int32_t n, int64_t q)>&
        on_edge,
    int64_t* rows_scanned, std::string* error);

}  // namespace primeparts::graph
