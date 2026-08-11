#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace duckdb {
class Connection;
}  // namespace duckdb

namespace primeparts::graph {

struct PartsPaths {
  std::vector<std::string> flat;
  std::vector<std::string> higher;

  bool empty() const { return flat.empty() && higher.empty(); }
};

struct EdgeFilter {
  int64_t min_p = 0;
  int64_t max_p = 0;
  int32_t min_n = 1;
  int64_t min_q = INT64_MIN;
  int64_t max_q = INT64_MAX;
  const std::vector<int64_t>* p_in = nullptr;
};

struct ScanCounts {
  int64_t flat_rows = 0;
  int64_t higher_rows = 0;
  int64_t edges = 0;
};

// Expands flat_parts hit masks and streams higher_parts rows as edges,
// invoking `on_edge` for every (p, m, n, q) whose q falls in
// [filter.min_q, filter.max_q]. min_n > 1 skips flat_parts entirely, since
// every flat row is n = 1. Neither side needs SolveQ: flat gives q = p - 2^m
// and higher stores q_k.
bool ScanPartitionEdges(
    duckdb::Connection* con, const PartsPaths& paths, const EdgeFilter& filter,
    const std::function<void(int64_t p, int32_t m, int32_t n, int64_t q)>&
        on_edge,
    ScanCounts* counts, std::string* error);

}  // namespace primeparts::graph
