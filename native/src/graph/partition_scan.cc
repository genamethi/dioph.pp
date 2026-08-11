#include "primeparts/graph/partition_scan.h"

#include <duckdb.hpp>

#include "primeparts/parts_expand.h"

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

std::string RangeWhere(const EdgeFilter& filter) {
  std::string w;
  if (filter.min_p > 0) w = "p >= " + std::to_string(filter.min_p);
  if (filter.max_p > 0) {
    if (!w.empty()) w += " AND ";
    w += "p <= " + std::to_string(filter.max_p);
  }
  return w;
}

bool RegisterWanted(duckdb::Connection* con, const std::vector<int64_t>& want,
                    std::string* error) {
  auto drop = con->Query("DROP TABLE IF EXISTS pp_want");
  if (drop->HasError()) {
    *error = drop->GetError();
    return false;
  }
  auto make = con->Query("CREATE TEMP TABLE pp_want(p BIGINT)");
  if (make->HasError()) {
    *error = make->GetError();
    return false;
  }
  duckdb::Appender appender(*con, "pp_want");
  for (const int64_t p : want) {
    appender.BeginRow();
    appender.Append<int64_t>(p);
    appender.EndRow();
  }
  appender.Close();
  return true;
}

}  // namespace

bool ScanPartitionEdges(
    duckdb::Connection* con, const PartsPaths& paths, const EdgeFilter& filter,
    const std::function<void(int64_t p, int32_t m, int32_t n, int64_t q)>&
        on_edge,
    ScanCounts* counts, std::string* error) {
  if (paths.empty()) return true;

  const bool use_set = filter.p_in != nullptr && !filter.p_in->empty();
  if (use_set && !RegisterWanted(con, *filter.p_in, error)) return false;

  const std::string range = RangeWhere(filter);
  auto build = [&](const std::string& cols, const std::vector<std::string>& fs,
                   const std::string& extra) {
    std::string sql = "SELECT " + cols + " FROM read_parquet(" + QuoteList(fs) +
                      ") AS t";
    if (use_set) sql += " SEMI JOIN pp_want AS w ON w.p = t.p";
    std::string w = range;
    if (!extra.empty()) {
      if (!w.empty()) w += " AND ";
      w += extra;
    }
    if (!w.empty()) sql += " WHERE " + w;
    return sql;
  };

  if (filter.min_n <= 1 && !paths.flat.empty()) {
    auto result = con->Query(build("t.p, t.hit_mask", paths.flat, ""));
    if (result->HasError()) {
      *error = result->GetError();
      return false;
    }
    for (auto& row : *result) {
      const int64_t p = row.GetValue<int64_t>(0);
      const uint64_t mask = static_cast<uint64_t>(row.GetValue<int64_t>(1));
      if (counts) ++counts->flat_rows;
      ForEachPart(p, mask, [&](int32_t m, int64_t q) {
        if (q < filter.min_q || q > filter.max_q) return;
        if (counts) ++counts->edges;
        on_edge(p, m, 1, q);
      });
    }
  }

  if (!paths.higher.empty()) {
    std::string extra;
    if (filter.min_n > 1) extra = "t.n_k >= " + std::to_string(filter.min_n);
    auto result =
        con->Query(build("t.p, t.m_k, t.n_k, t.q_k", paths.higher, extra));
    if (result->HasError()) {
      *error = result->GetError();
      return false;
    }
    for (auto& row : *result) {
      const int64_t p = row.GetValue<int64_t>(0);
      const int32_t m = row.GetValue<int32_t>(1);
      const int32_t n = row.GetValue<int32_t>(2);
      const int64_t q = row.GetValue<int64_t>(3);
      if (counts) ++counts->higher_rows;
      if (q < filter.min_q || q > filter.max_q) continue;
      if (counts) ++counts->edges;
      on_edge(p, m, n, q);
    }
  }

  return true;
}

}  // namespace primeparts::graph
