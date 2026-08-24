#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/lua/lnt.h"

namespace primeparts::graph {

using Edge = nt::Edge;

class Oracle {
 public:
  virtual ~Oracle() = default;
  virtual bool Parts(int64_t p, std::vector<Edge>* out, std::string* error) = 0;
  virtual const char* Name() const = 0;
  virtual int64_t Calls() const = 0;
};

bool DynamicParts(int64_t p, std::vector<Edge>* out, std::string* error);

std::unique_ptr<Oracle> MakeDynamicOracle();

std::unique_ptr<Oracle> MakeCachedOracle(std::unique_ptr<Oracle> inner,
                                         std::size_t capacity);

}  // namespace primeparts::graph
