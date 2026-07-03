#include "primeparts/analysis/tuples.h"

#include <cmath>
#include <numeric>

namespace primeparts::analysis {

std::vector<int> MaskToPositions(uint64_t mask) {
  std::vector<int> out;
  out.reserve(Popcount(mask));
  while (mask != 0) {
    out.push_back(__builtin_ctzll(mask));
    mask &= mask - 1;  // clear lowest set bit
  }
  return out;  // already ascending (ctz yields the lowest bit each step)
}

uint64_t PositionsToMask(const std::vector<int>& positions) {
  uint64_t mask = 0;
  for (int p : positions) mask |= (uint64_t{1} << p);
  return mask;
}

void MaskToColumns(uint64_t mask, int32_t* bits, int32_t* shift) {
  const int s = ShiftOf(mask);
  const uint64_t shape = mask >> s;
  for (int j = 0; j < kTupleWidth; ++j) {
    bits[j] = static_cast<int32_t>((shape >> j) & 1u);
  }
  *shift = static_cast<int32_t>(s);
}

uint64_t ColumnsToMask(const int32_t* bits, int32_t shift) {
  uint64_t shape = 0;
  for (int j = 0; j < kTupleWidth; ++j) {
    if (bits[j]) shape |= (uint64_t{1} << j);
  }
  return shape << shift;
}

bool IsExactAP(const std::vector<int>& positions) {
  if (positions.size() <= 2) return true;
  const int d = positions[1] - positions[0];
  for (size_t i = 2; i < positions.size(); ++i) {
    if (positions[i] - positions[i - 1] != d) return false;
  }
  return true;
}

int GcdDiffs(const std::vector<int>& positions) {
  if (positions.size() < 2) return 0;
  int g = 0;
  for (size_t i = 1; i < positions.size(); ++i) {
    g = std::gcd(g, positions[i] - positions[i - 1]);
  }
  return g;
}

int BestStep(const std::vector<int>& positions) {
  const size_t k = positions.size();
  if (k < 2) return 0;
  const int span = positions.back() - positions.front();
  const int denom = static_cast<int>(k) - 1;
  // round-to-nearest integer step
  return (2 * span + denom) / (2 * denom);
}

int NearAPDeviation(const std::vector<int>& positions) {
  const size_t k = positions.size();
  if (k < 2) return -1;
  const int d = BestStep(positions);
  int worst = 0;
  for (size_t i = 0; i < k; ++i) {
    const int predicted = positions.front() + static_cast<int>(i) * d;
    const int dev = std::abs(positions[i] - predicted);
    if (dev > worst) worst = dev;
  }
  return worst;
}

double StdResidual(int64_t count, double expected) {
  if (expected <= 0.0) return 0.0;
  return (static_cast<double>(count) - expected) / std::sqrt(expected);
}

double UniformResidual(int64_t count, int64_t total, int64_t n_distinct) {
  if (n_distinct <= 0 || total <= 0) return 0.0;
  return StdResidual(count, static_cast<double>(total) / static_cast<double>(n_distinct));
}

double IndependenceResidual(int64_t count, int64_t total,
                            const std::vector<int>& positions,
                            const std::vector<double>& marginals) {
  if (total <= 0) return 0.0;
  double prod = static_cast<double>(total);
  for (int p : positions) {
    const double f = (p >= 0 && p < static_cast<int>(marginals.size())) ? marginals[p] : 0.0;
    prod *= f;
  }
  return StdResidual(count, prod);
}

}  // namespace primeparts::analysis
