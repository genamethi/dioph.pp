#pragma once

#include <cstdint>

namespace primeparts {

inline int64_t IPow(int64_t base, int32_t exp, bool* overflow) {
  int64_t r = 1;
  *overflow = false;
  for (int32_t i = 0; i < exp; ++i) {
    if (base != 0 && r > INT64_MAX / base) {
      *overflow = true;
      return 0;
    }
    r *= base;
  }
  return r;
}

}  // namespace primeparts
