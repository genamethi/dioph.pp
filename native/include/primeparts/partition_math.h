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

inline int64_t IRoot(int64_t value, int32_t n) {
  if (value < 0) return -1;
  if (n <= 0) return -1;
  if (n == 1) return value;
  int64_t lo = 0;
  int64_t hi = 1;
  while (true) {
    bool ov = false;
    const int64_t t = IPow(hi, n, &ov);
    if (ov || t >= value) break;
    hi *= 2;
  }
  while (lo <= hi) {
    const int64_t mid = lo + (hi - lo) / 2;
    bool ov = false;
    const int64_t t = IPow(mid, n, &ov);
    if (ov || t > value) {
      hi = mid - 1;
    } else if (t < value) {
      lo = mid + 1;
    } else {
      return mid;
    }
  }
  return -1;
}

inline bool SolveQ(int64_t p, int32_t m, int32_t n, int64_t* q) {
  bool ov = false;
  const int64_t two_m = IPow(2, m, &ov);
  if (ov || two_m >= p) return false;
  const int64_t rem = p - two_m;
  const int64_t root = IRoot(rem, n);
  if (root < 2) return false;
  *q = root;
  return true;
}

}  // namespace primeparts
