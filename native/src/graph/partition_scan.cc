#include "primeparts/graph/partition_scan.h"

#include <duckdb.hpp>

#include "primeparts/partition_math.h"

namespace primeparts::graph {

namespace {

std::string QuoteList(const std::vector<std::string>& paths) {
  std::string s = "[";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i) s += ", ";
    s += "'" + paths[i] + "'";
  }
  s += "]";
  return s;
}

}  // namespace

bool ScanPartitionEdges(
    duckdb::Connection* con, const std::vector<std::string>& paths,
    const std::string& extra_where, int64_t min_q, int64_t max_q,
    const std::function<void(int64_t p, int32_t m, int32_t n, int64_t q)>&
        on_edge,
    int64_t* rows_scanned, std::string* error) {
  if (paths.empty()) return true;

  std::string sql =
      "SELECT p, m_k, n_k FROM read_parquet(" + QuoteList(paths) + ")";
  if (!extra_where.empty()) sql += " WHERE " + extra_where;

  auto result = con->Query(sql);
  if (result->HasError()) {
    *error = result->GetError();
    return false;
  }
  for (auto& chunk : *result) {
    const int64_t p = chunk.GetValue<int64_t>(0);
    const int32_t m = chunk.GetValue<int32_t>(1);
    const int32_t n = chunk.GetValue<int32_t>(2);
    if (rows_scanned) ++*rows_scanned;
    int64_t q = 0;
    if (!primeparts::SolveQ(p, m, n, &q)) continue;
    if (q < min_q || q > max_q) continue;
    on_edge(p, m, n, q);
  }
  return true;
}

}  // namespace primeparts::graph
