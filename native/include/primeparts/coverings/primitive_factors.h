#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace primeparts {

struct PrimitiveFactor {
  uint64_t q = 0;
  int32_t exponent = 0;
};

struct MersenneFactor {
  int32_t d = 0;
  uint64_t q = 0;
  int32_t exponent = 0;
};

struct MersenneHelper {
  int32_t max_d = 0;
  std::vector<MersenneFactor> factors;
  std::unordered_map<uint64_t, int32_t> ord2_by_q;

  // Returns ord2(divisor), or 0 if not in helper.
  int32_t GetOrd2(uint64_t divisor) const;
};

MersenneHelper BuildMersenneHelper(int32_t max_d);

// Decode a hit-position bitmask (bit m set iff m is a hit position) into the
// multiset of pairwise positive index differences d = m_a - m_b (m_a > m_b),
// each a Mersenne index M_d = 2^d - 1. Emitted in the canonical order used by
// mdiff: positions sorted ascending m[0] < ... < m[K-1], then
//   for a = K-1 .. 1, for b = a-1 .. 0: emit m[a] - m[b].
// For K = popcount(mask) positions this yields C = K(K-1)/2 differences (empty
// for K < 2). The inverse of the mask packing in mdiff_main.
std::vector<int32_t> HitMaskDiffs(uint64_t mask);

// Returns a bitmask where the i-th bit is set if (p - 2^i) mod s == 0
// for any s in the backbone {3, 5, 7, 11, 13, 17}.
uint64_t GetBackboneMask(uint64_t p, int32_t max_m);

// Returns a bitmask where the i-th bit is set if (p - 2^i) mod divisor == 0.
// This leverages the periodicity d = ord2(divisor).
uint64_t GetCoverageMask(uint64_t p, uint64_t divisor, int32_t d, int32_t max_m);

bool IsBackboneCovered(uint64_t p, int32_t m);
bool IsPrimitiveFactor(uint64_t p, int32_t m, uint64_t divisor);
bool IsPrimitiveFactor(uint64_t p, int32_t m, uint64_t divisor,
                       const MersenneHelper& helper);

std::vector<PrimitiveFactor> PrimitiveFactorsForTerm(uint64_t p, int32_t m);
std::vector<PrimitiveFactor> PrimitiveFactorsForTerm(uint64_t p, int32_t m,
                                                     const MersenneHelper& helper);

}  // namespace primeparts
