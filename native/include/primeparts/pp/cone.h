#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace primeparts::pp {

using MaskFn = std::function<uint64_t(int64_t)>;

struct Ascent {
  int32_t m = 0;
  int32_t n = 2;
  int64_t q = 0;
};

using HigherFn = std::function<std::vector<Ascent>(int64_t)>;

uint64_t ComputedMask(int64_t p);

std::vector<Ascent> ComputedHigher(int64_t p);

struct ConeStats {
  int64_t nodes = 0;
  int64_t edges = 0;
  int64_t sources = 0;
};

std::vector<int64_t> Cone(int64_t p, const MaskFn& mask, ConeStats* stats);

}  // namespace primeparts::pp
