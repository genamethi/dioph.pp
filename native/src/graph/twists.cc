#include "primeparts/graph/twists.h"

#include <algorithm>

#include <flint/ulong_extras.h>
#include <primesieve.h>

#pragma GCC diagnostic ignored "-Wpedantic"

namespace primeparts::graph {

namespace {

bool LessByP(const TwistRow& a, int64_t p) { return a.p < p; }

}  // namespace

std::unique_ptr<TwistTable> TwistTable::Build(int64_t hi, std::string* error) {
  if (hi < 9) {
    *error = "hi must be at least 9";
    return nullptr;
  }
  auto table = std::unique_ptr<TwistTable>(new TwistTable());
  table->hi_ = hi;
  const auto bound = static_cast<unsigned __int128>(hi);

  primesieve_iterator it;
  primesieve_init(&it);
  for (;;) {
    const uint64_t q = primesieve_next_prime(&it);
    if (static_cast<unsigned __int128>(q) * q > bound) break;
    auto power = static_cast<unsigned __int128>(q) * q;
    for (int n = 2;; ++n) {
      if (power + 2 > bound) break;
      const auto qn = static_cast<uint64_t>(power);
      for (int m = 1; m < 63; ++m) {
        const auto p = static_cast<unsigned __int128>(qn) +
                       (static_cast<unsigned __int128>(1) << m);
        if (p > bound) break;
        if (n_is_prime(static_cast<uint64_t>(p)) != 0) {
          table->rows_.push_back(TwistRow{.p = static_cast<int64_t>(p),
                                          .m = m,
                                          .n = n,
                                          .q = static_cast<int64_t>(q)});
        }
      }
      if (power > bound / q) break;
      power *= q;
    }
  }
  primesieve_free_iterator(&it);

  std::sort(table->rows_.begin(), table->rows_.end(),
            [](const TwistRow& a, const TwistRow& b) {
              if (a.p != b.p) return a.p < b.p;
              return a.m < b.m;
            });
  return table;
}

bool TwistTable::Has(int64_t p) const {
  const auto it = std::lower_bound(rows_.begin(), rows_.end(), p, LessByP);
  return it != rows_.end() && it->p == p;
}

std::vector<TwistRow> TwistTable::At(int64_t p) const {
  std::vector<TwistRow> out;
  auto it = std::lower_bound(rows_.begin(), rows_.end(), p, LessByP);
  for (; it != rows_.end() && it->p == p; ++it) out.push_back(*it);
  return out;
}

std::vector<TwistRow> TwistTable::Into(int64_t q) const {
  std::vector<TwistRow> out;
  for (const TwistRow& r : rows_) {
    if (r.q == q) out.push_back(r);
  }
  return out;
}

}  // namespace primeparts::graph
