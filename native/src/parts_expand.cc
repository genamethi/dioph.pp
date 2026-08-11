#include "primeparts/parts_expand.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define PP_X86 1
#endif

namespace primeparts {
namespace detail {

bool HaveAvx2() {
#if defined(PP_X86)
  static const bool ok = __builtin_cpu_supports("avx2");
  return ok;
#else
  return false;
#endif
}

void MaskOffsetsScalar(const uint64_t* masks, int64_t n, int64_t* offsets) {
  int64_t run = 0;
  offsets[0] = 0;
  for (int64_t i = 0; i < n; ++i) {
    run += __builtin_popcountll(masks[i]);
    offsets[i + 1] = run;
  }
}

void MaskHistogramScalar(const uint64_t* masks, int64_t n, int64_t* hist) {
  for (int64_t i = 0; i < n; ++i) {
    uint64_t mask = masks[i];
    while (mask) {
      ++hist[__builtin_ctzll(mask)];
      mask &= mask - 1;
    }
  }
}

void SubPow2Scalar(const int64_t* p_flat, const int32_t* m, int64_t n,
                   int64_t* q) {
  for (int64_t i = 0; i < n; ++i) q[i] = PartQ(p_flat[i], m[i]);
}

#if defined(PP_X86)

[[gnu::target("avx2")]] static inline __m256i PopcountEpi64(__m256i v) {
  const __m256i lut =
      _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 0, 1, 1,
                       2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
  const __m256i lo_mask = _mm256_set1_epi8(0x0f);
  const __m256i lo = _mm256_and_si256(v, lo_mask);
  const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), lo_mask);
  const __m256i cnt = _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo),
                                      _mm256_shuffle_epi8(lut, hi));
  return _mm256_sad_epu8(cnt, _mm256_setzero_si256());
}

[[gnu::target("avx2")]] void MaskOffsetsAvx2(const uint64_t* masks, int64_t n,
                                             int64_t* offsets) {
  int64_t run = 0;
  offsets[0] = 0;
  int64_t i = 0;
  alignas(32) int64_t lane[4];
  for (; i + 4 <= n; i += 4) {
    const __m256i v =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(masks + i));
    _mm256_store_si256(reinterpret_cast<__m256i*>(lane), PopcountEpi64(v));
    run += lane[0];
    offsets[i + 1] = run;
    run += lane[1];
    offsets[i + 2] = run;
    run += lane[2];
    offsets[i + 3] = run;
    run += lane[3];
    offsets[i + 4] = run;
  }
  for (; i < n; ++i) {
    run += __builtin_popcountll(masks[i]);
    offsets[i + 1] = run;
  }
}

[[gnu::target("avx2")]] void SubPow2Avx2(const int64_t* p_flat, const int32_t* m,
                                         int64_t n, int64_t* q) {
  const __m256i one = _mm256_set1_epi64x(1);
  int64_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m128i m32 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(m + i));
    const __m256i pow = _mm256_sllv_epi64(one, _mm256_cvtepi32_epi64(m32));
    const __m256i pv =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p_flat + i));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(q + i),
                        _mm256_sub_epi64(pv, pow));
  }
  for (; i < n; ++i) q[i] = PartQ(p_flat[i], m[i]);
}

#else

void MaskOffsetsAvx2(const uint64_t* masks, int64_t n, int64_t* offsets) {
  MaskOffsetsScalar(masks, n, offsets);
}

void SubPow2Avx2(const int64_t* p_flat, const int32_t* m, int64_t n,
                 int64_t* q) {
  SubPow2Scalar(p_flat, m, n, q);
}

#endif

}  // namespace detail

void MaskOffsets(const uint64_t* masks, int64_t n, int64_t* offsets) {
  static const auto fn =
      detail::HaveAvx2() ? &detail::MaskOffsetsAvx2 : &detail::MaskOffsetsScalar;
  fn(masks, n, offsets);
}

void MaskHistogram(const uint64_t* masks, int64_t n, int64_t* hist) {
  detail::MaskHistogramScalar(masks, n, hist);
}

void ExpandMasks(const int64_t* p, const uint64_t* masks, int64_t n,
                 const int64_t* offsets, int32_t* m_values, int64_t* q_values,
                 int64_t* p_flat) {
  std::vector<int64_t> scratch;
  if (p_flat == nullptr) {
    scratch.resize(static_cast<size_t>(offsets[n]));
    p_flat = scratch.data();
  }
  for (int64_t i = 0; i < n; ++i) {
    int64_t at = offsets[i];
    const int64_t pv = p[i];
    uint64_t mask = masks[i];
    while (mask) {
      m_values[at] = static_cast<int32_t>(__builtin_ctzll(mask));
      p_flat[at] = pv;
      mask &= mask - 1;
      ++at;
    }
  }
  static const auto fn =
      detail::HaveAvx2() ? &detail::SubPow2Avx2 : &detail::SubPow2Scalar;
  fn(p_flat, m_values, offsets[n], q_values);
}

ExpandedParts ExpandMaskBatch(const int64_t* p, const uint64_t* masks,
                              int64_t n) {
  ExpandedParts out;
  out.offsets.resize(static_cast<size_t>(n) + 1);
  MaskOffsets(masks, n, out.offsets.data());
  const int64_t total = out.offsets[static_cast<size_t>(n)];
  out.m_values.resize(static_cast<size_t>(total));
  out.q_values.resize(static_cast<size_t>(total));
  ExpandMasks(p, masks, n, out.offsets.data(), out.m_values.data(),
              out.q_values.data(), nullptr);
  return out;
}

}  // namespace primeparts
