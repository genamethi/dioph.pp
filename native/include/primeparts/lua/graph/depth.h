#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "primeparts/lua/graph/parts.h"

namespace primeparts::graph {

class Depth {
 public:
  static constexpr int32_t kComplete = INT32_MAX;

  static Depth Of(int64_t root);
  static Depth From(int64_t root, std::vector<int64_t> coeffs);

  Depth Shift() const;
  Depth Add(const Depth& other) const;
  Depth Cut(int32_t d) const;

  int32_t Known() const { return known_; }
  bool Complete() const { return known_ == kComplete; }

  std::vector<int64_t> Roots() const;
  const std::vector<int64_t>* At(int64_t root) const;

  int32_t Min(int64_t root) const;
  int32_t Max() const;
  int64_t Count() const;
  int64_t Cells() const;

 private:
  std::vector<std::pair<int64_t, std::vector<int64_t>>> by_root_;
  int32_t known_ = kComplete;
};

bool DepthOf(Oracle& oracle, int64_t p, int32_t depth, int64_t max_cells,
             Depth* out, std::string* error);

}  // namespace primeparts::graph
