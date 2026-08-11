#pragma once

#include <cstdint>
#include <vector>

namespace primeparts {

inline int MaskCount(uint64_t mask) { return __builtin_popcountll(mask); }

inline int64_t PartQ(int64_t p, int32_t m) {
  return p - static_cast<int64_t>(uint64_t{1} << m);
}

template <typename Fn>
inline void ForEachPart(int64_t p, uint64_t mask, Fn&& fn) {
  while (mask) {
    const int32_t m = static_cast<int32_t>(__builtin_ctzll(mask));
    fn(m, PartQ(p, m));
    mask &= mask - 1;
  }
}

inline int ExpandMask(int64_t p, uint64_t mask, int32_t* m_out, int64_t* q_out) {
  int n = 0;
  ForEachPart(p, mask, [&](int32_t m, int64_t q) {
    m_out[n] = m;
    q_out[n] = q;
    ++n;
  });
  return n;
}

void MaskOffsets(const uint64_t* masks, int64_t n, int64_t* offsets);

void MaskHistogram(const uint64_t* masks, int64_t n, int64_t* hist);

void ExpandMasks(const int64_t* p, const uint64_t* masks, int64_t n,
                 const int64_t* offsets, int32_t* m_values, int64_t* q_values,
                 int64_t* p_flat);

struct ExpandedParts {
  std::vector<int64_t> offsets;
  std::vector<int32_t> m_values;
  std::vector<int64_t> q_values;
};

ExpandedParts ExpandMaskBatch(const int64_t* p, const uint64_t* masks, int64_t n);

namespace detail {

bool HaveAvx2();

void MaskOffsetsScalar(const uint64_t* masks, int64_t n, int64_t* offsets);
void MaskOffsetsAvx2(const uint64_t* masks, int64_t n, int64_t* offsets);

void MaskHistogramScalar(const uint64_t* masks, int64_t n, int64_t* hist);

void SubPow2Scalar(const int64_t* p_flat, const int32_t* m, int64_t n,
                   int64_t* q);
void SubPow2Avx2(const int64_t* p_flat, const int32_t* m, int64_t n,
                 int64_t* q);

}  // namespace detail

}  // namespace primeparts
