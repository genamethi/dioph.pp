#pragma once

#include <cstdint>
#include <string>

namespace primeparts::graph {

struct BigInt {
  bool fits = true;
  int64_t value = 0;
  std::string text;
};

}  // namespace primeparts::graph
