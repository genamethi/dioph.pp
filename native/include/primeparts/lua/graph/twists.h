#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/lua/graph/parts.h"

namespace primeparts::graph {

class TwistTable {
 public:
  static std::unique_ptr<TwistTable> Build(int64_t hi, std::string* error);
  static std::unique_ptr<TwistTable> FromRows(std::vector<Edge> rows,
                                              int64_t hi, std::string* error);

  const char* Origin() const { return origin_; }

  int64_t Hi() const { return hi_; }
  std::size_t Count() const { return rows_.size(); }
  const std::vector<Edge>& Rows() const { return rows_; }

  bool Has(int64_t p) const;
  std::vector<Edge> At(int64_t p) const;
  std::vector<Edge> Into(int64_t q) const;

 private:
  int64_t hi_ = 0;
  const char* origin_ = "enumerated";
  std::vector<Edge> rows_;
  std::vector<Edge> by_q_;

  void Index();
};

}  // namespace primeparts::graph
