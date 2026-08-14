#include "primeparts/pp/cone.h"

#include <flint/ulong_extras.h>

#include <algorithm>
#include <unordered_set>

#include "primeparts/core.h"
#include "primeparts/parts_expand.h"

namespace primeparts::pp {
namespace {

int TopExponent(int64_t p) {
  return 63 - __builtin_clzll(static_cast<uint64_t>(p));
}

}  // namespace

uint64_t ComputedMask(int64_t p) {
  if (p < 3) {
    return 0;
  }
  const int max_m = TopExponent(p);
  uint64_t mask = 0;
  for (int m = 1; m <= max_m; ++m) {
    const int64_t q = primeparts::PartQ(p, m);
    if (q >= 2 && n_is_prime(static_cast<ulong>(q))) {
      mask |= uint64_t{1} << m;
    }
  }
  return mask;
}

std::vector<Ascent> ComputedHigher(int64_t p) {
  std::vector<Ascent> out;
  if (p < 3) {
    return out;
  }
  const int max_m = TopExponent(p);
  for (int m = 1; m <= max_m; ++m) {
    const int64_t w = primeparts::PartQ(p, m);
    if (w < 2) {
      continue;
    }
    uint64_t base = 0;
    int32_t exponent = 0;
    if (pp_is_prime_power_u64(static_cast<uint64_t>(w), &base, &exponent) !=
        PP_OK) {
      continue;
    }
    if (exponent < 2) {
      continue;
    }
    Ascent a;
    a.m = static_cast<int32_t>(m);
    a.n = exponent;
    a.q = static_cast<int64_t>(base);
    out.push_back(a);
  }
  return out;
}

std::vector<int64_t> Cone(int64_t p, const MaskFn& mask, ConeStats* stats) {
  std::vector<int64_t> out;
  if (p < 3) {
    if (stats != nullptr) {
      *stats = ConeStats{};
    }
    return out;
  }

  std::unordered_set<int64_t> seen;
  std::vector<int64_t> frontier{p};
  seen.insert(p);
  int64_t edges = 0;
  int64_t sources = 0;

  while (!frontier.empty()) {
    const int64_t v = frontier.back();
    frontier.pop_back();
    out.push_back(v);

    const uint64_t bits = mask(v);
    if (bits == 0) {
      ++sources;
      continue;
    }
    primeparts::ForEachPart(v, bits, [&](int32_t, int64_t q) {
      ++edges;
      if (seen.insert(q).second) {
        frontier.push_back(q);
      }
    });
  }

  std::sort(out.begin(), out.end());
  if (stats != nullptr) {
    stats->nodes = static_cast<int64_t>(out.size());
    stats->edges = edges;
    stats->sources = sources;
  }
  return out;
}

}  // namespace primeparts::pp
