#pragma once

#include <cstdint>
#include <vector>

namespace primeparts::graph {

struct HermiteRootsReport {
  uint64_t ell = 0;
  int n = 0;
  std::vector<uint64_t> core_roots;
  std::vector<uint64_t> roots;
  bool criterion_match = false;
};

HermiteRootsReport HermiteRootsModL(int n, uint64_t ell);

uint64_t HermiteEvalModL(int n, uint64_t value, uint64_t ell);

std::vector<uint64_t> PrimitiveMersenneFactors(int d);

}  // namespace primeparts::graph
