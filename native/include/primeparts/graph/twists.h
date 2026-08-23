#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/graph/parts.h"

namespace primeparts::graph {

struct TwistRow {
  int64_t p = 0;
  int32_t m = 0;
  int32_t n = 0;
  int64_t q = 0;
};

class TwistTable {
 public:
  static std::unique_ptr<TwistTable> Build(int64_t hi, std::string* error);

  int64_t Hi() const { return hi_; }
  std::size_t Count() const { return rows_.size(); }
  const std::vector<TwistRow>& Rows() const { return rows_; }

  bool Has(int64_t p) const;
  std::vector<TwistRow> At(int64_t p) const;
  std::vector<TwistRow> Into(int64_t q) const;

 private:
  int64_t hi_ = 0;
  std::vector<TwistRow> rows_;
  std::vector<TwistRow> by_q_;
};

}  // namespace primeparts::graph
