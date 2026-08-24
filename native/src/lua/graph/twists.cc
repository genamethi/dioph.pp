#include "primeparts/lua/graph/twists.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <primesieve.h>

#include "primeparts/lua/lnt.h"

namespace primeparts::graph {

namespace {

bool LessByP(const Edge& a, int64_t p) { return a.p < p; }

}  // namespace

std::unique_ptr<TwistTable> TwistTable::Build(int64_t hi, std::string* error) {
  if (hi < 9) {
    *error = "hi must be at least 9";
    return nullptr;
  }
  auto table = std::unique_ptr<TwistTable>(new TwistTable());
  table->hi_ = hi;
  const uint64_t bound = static_cast<uint64_t>(hi);

  std::vector<Edge> edges;
  primesieve_iterator it;
  primesieve_init(&it);
  for (;;) {
    const uint64_t q = primesieve_next_prime(&it);
    if (q > bound / q) break;
    nt::InvOf(q, bound, &edges);
    for (const Edge& e : edges) {
      if (e.n >= 2) table->rows_.push_back(e);
    }
  }
  primesieve_free_iterator(&it);

  table->Index();
  return table;
}

std::unique_ptr<TwistTable> TwistTable::FromRows(std::vector<Edge> rows,
                                                 int64_t hi,
                                                 std::string* error) {
  for (const Edge& r : rows) {
    if (r.n < 2) {
      *error = "row for p=" + std::to_string(r.p) + " has n=" +
               std::to_string(r.n) + "; a twist needs n >= 2";
      return nullptr;
    }
  }
  auto table = std::unique_ptr<TwistTable>(new TwistTable());
  table->hi_ = hi;
  table->origin_ = "catalog";
  table->rows_ = std::move(rows);
  table->Index();
  return table;
}

void TwistTable::Index() {
  std::sort(rows_.begin(), rows_.end(), [](const Edge& a, const Edge& b) {
    if (a.p != b.p) return a.p < b.p;
    return a.m < b.m;
  });
  by_q_.resize(rows_.size());
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    by_q_[i] = static_cast<uint32_t>(i);
  }
  std::sort(by_q_.begin(), by_q_.end(), [this](uint32_t a, uint32_t b) {
    if (rows_[a].q != rows_[b].q) return rows_[a].q < rows_[b].q;
    return rows_[a].p < rows_[b].p;
  });
}

bool TwistTable::Has(int64_t p) const {
  const auto it = std::lower_bound(rows_.begin(), rows_.end(), p, LessByP);
  return it != rows_.end() && it->p == p;
}

std::vector<Edge> TwistTable::At(int64_t p) const {
  std::vector<Edge> out;
  auto it = std::lower_bound(rows_.begin(), rows_.end(), p, LessByP);
  for (; it != rows_.end() && it->p == p; ++it) out.push_back(*it);
  return out;
}

std::vector<Edge> TwistTable::Into(int64_t q) const {
  std::vector<Edge> out;
  auto it = std::lower_bound(
      by_q_.begin(), by_q_.end(), q,
      [this](uint32_t a, int64_t v) { return rows_[a].q < v; });
  for (; it != by_q_.end() && rows_[*it].q == q; ++it) out.push_back(rows_[*it]);
  return out;
}

}  // namespace primeparts::graph
