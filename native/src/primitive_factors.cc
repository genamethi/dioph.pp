#include "primeparts/primitive_factors.h"

#include <algorithm>

#include <flint/ulong_extras.h>

namespace primeparts {

namespace {

constexpr uint64_t kBackbone[] = {3, 5, 7, 11, 13, 17};
constexpr int32_t kBackboneOrd2[] = {2, 4, 3, 10, 12, 8};

uint64_t Pow2Mod(int32_t exp, uint64_t mod) {
  if (mod == 1) return 0;
  uint64_t out = 1 % mod;
  uint64_t base = 2 % mod;
  int32_t e = exp;
  while (e > 0) {
    if ((e & 1) != 0) {
      out = static_cast<uint64_t>((static_cast<__uint128_t>(out) * base) % mod);
    }
    base = static_cast<uint64_t>((static_cast<__uint128_t>(base) * base) % mod);
    e >>= 1;
  }
  return out;
}

bool IsBackbone(uint64_t s) {
  for (uint64_t b : kBackbone) {
    if (s == b) return true;
  }
  return false;
}

void FactorU64(uint64_t n, n_factor_t* factors) {
  n_factor_init(factors);
  n_factor(factors, static_cast<ulong>(n), 1);
}

}  // namespace

int32_t MersenneHelper::GetOrd2(uint64_t divisor) const {
  auto it = ord2_by_q.find(divisor);
  if (it != ord2_by_q.end()) return it->second;
  return 0;
}

MersenneHelper BuildMersenneHelper(int32_t max_d) {
  MersenneHelper helper;
  helper.max_d = max_d;
  if (max_d <= 0 || max_d >= 64) return helper;

  for (int32_t d = 1; d <= max_d; ++d) {
    const uint64_t n = (uint64_t{1} << d) - 1;
    if (n < 2) continue;
    n_factor_t factors;
    FactorU64(n, &factors);
    for (int i = 0; i < factors.num; ++i) {
      const uint64_t q = static_cast<uint64_t>(factors.p[i]);
      helper.factors.push_back({d, q, static_cast<int32_t>(factors.exp[i])});
      auto [it, inserted] = helper.ord2_by_q.emplace(q, d);
      if (!inserted && d < it->second) it->second = d;
    }
  }
  std::sort(helper.factors.begin(), helper.factors.end(),
            [](const MersenneFactor& a, const MersenneFactor& b) {
              if (a.d != b.d) return a.d < b.d;
              return a.q < b.q;
            });
  return helper;
}

uint64_t GetCoverageMask(uint64_t p, uint64_t divisor, int32_t d, int32_t max_m) {
  if (d <= 0 || divisor < 2) return 0;
  uint64_t mask = 0;
  const uint64_t p_mod = p % divisor;
  
  // Find the first m such that 2^m == p (mod divisor).
  // This is the "start" of our arithmetic progression.
  int32_t r = -1;
  uint64_t pow2 = 1 % divisor;
  for (int32_t m = 0; m < d; ++m) {
    if (pow2 == p_mod) {
      r = m;
      break;
    }
    pow2 = (pow2 * 2) % divisor;
  }

  if (r == -1) return 0; // p is not in the subgroup <2> mod divisor.

  // Propagate the coverage: m = r, r+d, r+2d...
  // Bits are 1-indexed for m, so bit i corresponds to m=i.
  for (int32_t m = (r == 0 ? d : r); m <= max_m; m += d) {
    if (m > 0 && m <= 64) {
      mask |= (uint64_t{1} << (m - 1));
    }
  }
  return mask;
}

uint64_t GetBackboneMask(uint64_t p, int32_t max_m) {
  uint64_t mask = 0;
  for (size_t i = 0; i < 6; ++i) {
    mask |= GetCoverageMask(p, kBackbone[i], kBackboneOrd2[i], max_m);
  }
  return mask;
}

bool IsBackboneCovered(uint64_t p, int32_t m) {
  if (m <= 0) return false;
  for (uint64_t s : kBackbone) {
    if ((p % s) == Pow2Mod(m, s)) return true;
  }
  return false;
}

bool IsPrimitiveFactor(uint64_t p, int32_t m, uint64_t s) {
  if (m <= 0 || s < 2) return false;
  if ((p % s) != Pow2Mod(m, s)) return false;
  for (int32_t j = 1; j < m; ++j) {
    if ((p % s) == Pow2Mod(j, s)) return false;
  }
  return true;
}

bool IsPrimitiveFactor(uint64_t p, int32_t m, uint64_t s,
                       const MersenneHelper& helper) {
  if (m <= 0 || s < 2) return false;
  if ((p % s) != Pow2Mod(m, s)) return false;
  auto it = helper.ord2_by_q.find(s);
  if (it == helper.ord2_by_q.end()) return true;
  return it->second >= m;
}

std::vector<PrimitiveFactor> PrimitiveFactorsForTerm(uint64_t p, int32_t m) {
  MersenneHelper helper = BuildMersenneHelper(m);
  return PrimitiveFactorsForTerm(p, m, helper);
}

std::vector<PrimitiveFactor> PrimitiveFactorsForTerm(uint64_t p, int32_t m,
                                                     const MersenneHelper& helper) {
  std::vector<PrimitiveFactor> out;
  if (m <= 0 || m >= 64) return out;
  const uint64_t power = uint64_t{1} << m;
  if (p <= power) return out;

  const uint64_t n = p - power;
  n_factor_t factors;
  FactorU64(n, &factors);
  for (int i = 0; i < factors.num; ++i) {
    const uint64_t s = static_cast<uint64_t>(factors.p[i]);
    if (IsBackbone(s)) continue;
    if (!IsPrimitiveFactor(p, m, s, helper)) continue;
    out.push_back({s, static_cast<int32_t>(factors.exp[i])});
  }
  std::sort(out.begin(), out.end(),
            [](const PrimitiveFactor& a, const PrimitiveFactor& b) {
              return a.q < b.q;
            });
  return out;
}

}  // namespace primeparts
