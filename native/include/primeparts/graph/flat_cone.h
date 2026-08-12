#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace primeparts::graph {

using MaskFn = std::function<uint64_t(int64_t)>;

uint64_t ComputedMask(int64_t p);

struct ConeStats {
  int64_t nodes = 0;
  int64_t edges = 0;
  int64_t flat_sources = 0;
};

std::vector<int64_t> FlatCone(int64_t p, const MaskFn& mask, ConeStats* stats);

}  // namespace primeparts::graph
