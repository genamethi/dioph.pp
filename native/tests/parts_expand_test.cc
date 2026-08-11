#include "primeparts/parts_expand.h"

#include <gtest/gtest.h>

#include <map>
#include <numeric>
#include <vector>

#include "primeparts/core.h"

namespace {

using primeparts::ExpandMask;
using primeparts::ExpandMaskBatch;
using primeparts::MaskCount;
using primeparts::MaskHistogram;
using primeparts::MaskOffsets;
using primeparts::PartQ;

struct Higher {
  int32_t m;
  int32_t n;
  int64_t q;
};

struct Folded {
  std::map<int64_t, uint64_t> mask;
  std::map<int64_t, std::vector<Higher>> higher;
};

Folded Fold(const pp_batch_result& r) {
  Folded out;
  for (size_t i = 0; i < r.partition_count; ++i) {
    const int64_t p = r.partition_p[i];
    if (r.partition_n[i] == 1) {
      out.mask[p] |= uint64_t{1} << r.partition_m[i];
    } else {
      out.higher[p].push_back(
          Higher{r.partition_m[i], r.partition_n[i], r.partition_q[i]});
    }
  }
  return out;
}

int FloorLog2(int64_t v) {
  return 63 - __builtin_clzll(static_cast<uint64_t>(v));
}

void CheckRoundTrip(const pp_batch_result& r) {
  const Folded f = Fold(r);

  for (const auto& [p, mask] : f.mask) {
    EXPECT_NE(mask, 0u) << "p=" << p;
    EXPECT_EQ(mask & 1u, 0u) << "p=" << p;
    EXPECT_EQ(mask >> 63, 0u) << "p=" << p;
    EXPECT_EQ(mask >> (FloorLog2(p) + 1), 0u) << "p=" << p;
  }

  for (const auto& [p, rows] : f.higher) {
    auto it = f.mask.find(p);
    const uint64_t mask = it == f.mask.end() ? 0u : it->second;
    for (const Higher& h : rows) {
      EXPECT_EQ((mask >> h.m) & 1u, 0u) << "p=" << p << " m=" << h.m;
    }
  }

  for (size_t i = 0; i < r.prime_count; ++i) {
    const int64_t p = r.prime_p[i];
    auto mit = f.mask.find(p);
    const uint64_t mask = mit == f.mask.end() ? 0u : mit->second;
    auto hit = f.higher.find(p);
    const size_t higher_rows = hit == f.higher.end() ? 0u : hit->second.size();
    EXPECT_EQ(MaskCount(mask) + static_cast<int>(higher_rows), r.prime_k[i])
        << "p=" << p;
    if (mask == 0) {
      EXPECT_EQ(static_cast<int>(higher_rows), r.prime_k[i]) << "p=" << p;
    }
  }

  size_t at = 0;
  for (size_t i = 0; i < r.prime_count; ++i) {
    const int64_t p = r.prime_p[i];
    auto mit = f.mask.find(p);
    const uint64_t mask = mit == f.mask.end() ? 0u : mit->second;
    auto hit = f.higher.find(p);
    const std::vector<Higher> empty;
    const std::vector<Higher>& rows = hit == f.higher.end() ? empty : hit->second;

    std::vector<std::pair<int32_t, std::pair<int32_t, int64_t>>> merged;
    primeparts::ForEachPart(p, mask, [&](int32_t m, int64_t q) {
      merged.push_back({m, {1, q}});
    });
    for (const Higher& h : rows) merged.push_back({h.m, {h.n, h.q}});
    std::sort(merged.begin(), merged.end());

    for (const auto& e : merged) {
      ASSERT_LT(at, r.partition_count);
      EXPECT_EQ(r.partition_p[at], p);
      EXPECT_EQ(r.partition_m[at], e.first);
      EXPECT_EQ(r.partition_n[at], e.second.first);
      EXPECT_EQ(r.partition_q[at], e.second.second);
      ++at;
    }
  }
  EXPECT_EQ(at, r.partition_count);
}

class PartsExpandTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { ASSERT_EQ(pp_init(), PP_OK); }
  static void TearDownTestSuite() { pp_shutdown(); }
};

TEST_F(PartsExpandTest, EmptyMaskExpandsEmpty) {
  int32_t m[64];
  int64_t q[64];
  EXPECT_EQ(ExpandMask(101, 0, m, q), 0);
  EXPECT_EQ(MaskCount(0), 0);
}

TEST_F(PartsExpandTest, BitZeroGivesPMinusOne) {
  int32_t m[64];
  int64_t q[64];
  ASSERT_EQ(ExpandMask(101, 1, m, q), 1);
  EXPECT_EQ(m[0], 0);
  EXPECT_EQ(q[0], 100);
}

TEST_F(PartsExpandTest, BitSixtyThreeAvoidsSignedShiftUb) {
  int32_t m[64];
  int64_t q[64];
  const uint64_t mask = uint64_t{1} << 63;
  ASSERT_EQ(ExpandMask(0, mask, m, q), 1);
  EXPECT_EQ(m[0], 63);
  EXPECT_EQ(q[0], INT64_MIN);
  EXPECT_EQ(PartQ(0, 63), INT64_MIN);
}

TEST_F(PartsExpandTest, FullMaskYieldsAllBitsAscending) {
  int32_t m[64];
  int64_t q[64];
  ASSERT_EQ(ExpandMask(12345, ~uint64_t{0}, m, q), 64);
  for (int i = 0; i < 64; ++i) EXPECT_EQ(m[i], i);
  for (int i = 1; i < 64; ++i) EXPECT_LT(m[i - 1], m[i]);
}

TEST_F(PartsExpandTest, OffsetsAreExclusiveScanOfPopcounts) {
  std::vector<uint64_t> masks{0, 1, 0b1010, ~uint64_t{0}, 0b100, 0};
  std::vector<int64_t> got(masks.size() + 1);
  MaskOffsets(masks.data(), static_cast<int64_t>(masks.size()), got.data());

  std::vector<int64_t> want(masks.size() + 1, 0);
  for (size_t i = 0; i < masks.size(); ++i) {
    want[i + 1] = want[i] + MaskCount(masks[i]);
  }
  EXPECT_EQ(got, want);
  EXPECT_EQ(got.back(), 1 + 2 + 64 + 1);
}

TEST_F(PartsExpandTest, HistogramCountsEachBitPosition) {
  std::vector<uint64_t> masks{0b1010, 0b0010, uint64_t{1} << 63};
  int64_t hist[64] = {0};
  MaskHistogram(masks.data(), static_cast<int64_t>(masks.size()), hist);

  int64_t want[64] = {0};
  for (uint64_t mask : masks) {
    for (int b = 0; b < 64; ++b) {
      if ((mask >> b) & 1u) ++want[b];
    }
  }
  for (int b = 0; b < 64; ++b) EXPECT_EQ(hist[b], want[b]) << "bit " << b;
}

TEST_F(PartsExpandTest, BatchExpansionMatchesPerRowExpansion) {
  std::vector<int64_t> p{11, 29, 101, 7};
  std::vector<uint64_t> masks{0b1100, uint64_t{1} << 4, 0, 0b110};
  const auto got = ExpandMaskBatch(p.data(), masks.data(),
                                   static_cast<int64_t>(p.size()));

  int64_t at = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    int32_t m[64];
    int64_t q[64];
    const int n = ExpandMask(p[i], masks[i], m, q);
    EXPECT_EQ(got.offsets[i + 1] - got.offsets[i], n);
    for (int j = 0; j < n; ++j) {
      EXPECT_EQ(got.m_values[at], m[j]);
      EXPECT_EQ(got.q_values[at], q[j]);
      ++at;
    }
  }
  EXPECT_EQ(at, got.offsets.back());
}

TEST_F(PartsExpandTest, Avx2AgreesWithScalarOnRandomMasks) {
  if (!primeparts::detail::HaveAvx2()) GTEST_SKIP() << "no avx2";

  std::vector<uint64_t> masks(10000);
  uint64_t s = 0x9e3779b97f4a7c15ull;
  for (uint64_t& v : masks) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    v = s;
  }

  std::vector<int64_t> a(masks.size() + 1), b(masks.size() + 1);
  primeparts::detail::MaskOffsetsScalar(
      masks.data(), static_cast<int64_t>(masks.size()), a.data());
  primeparts::detail::MaskOffsetsAvx2(
      masks.data(), static_cast<int64_t>(masks.size()), b.data());
  EXPECT_EQ(a, b);

  const int64_t total = a.back();
  std::vector<int64_t> p_flat(static_cast<size_t>(total));
  std::vector<int32_t> m(static_cast<size_t>(total));
  int64_t at = 0;
  for (size_t i = 0; i < masks.size(); ++i) {
    uint64_t mask = masks[i];
    while (mask) {
      p_flat[at] = static_cast<int64_t>(i) * 7919 + 3;
      m[at] = static_cast<int32_t>(__builtin_ctzll(mask));
      mask &= mask - 1;
      ++at;
    }
  }
  std::vector<int64_t> qa(static_cast<size_t>(total));
  std::vector<int64_t> qb(static_cast<size_t>(total));
  primeparts::detail::SubPow2Scalar(p_flat.data(), m.data(), total, qa.data());
  primeparts::detail::SubPow2Avx2(p_flat.data(), m.data(), total, qb.data());
  EXPECT_EQ(qa, qb);
}

TEST_F(PartsExpandTest, FoldMatchesKnownTuples) {
  pp_batch_result r;
  pp_batch_result_init(&r);
  ASSERT_EQ(pp_process_rank_batch(1, 10, &r), PP_OK);
  const Folded f = Fold(r);

  EXPECT_EQ(f.mask.at(11), 0b1100u);
  ASSERT_EQ(f.higher.at(11).size(), 1u);
  EXPECT_EQ(f.higher.at(11)[0].m, 1);
  EXPECT_EQ(f.higher.at(11)[0].n, 2);
  EXPECT_EQ(f.higher.at(11)[0].q, 3);
  EXPECT_EQ(MaskCount(f.mask.at(11)) + 1, 3);

  EXPECT_EQ(f.mask.at(29), uint64_t{1} << 4);
  ASSERT_EQ(f.higher.at(29).size(), 2u);
  EXPECT_EQ(MaskCount(f.mask.at(29)) + 2, 3);

  CheckRoundTrip(r);
  pp_batch_result_clear(&r);
}

TEST_F(PartsExpandTest, RoundTripsAcrossTheInt32Boundary) {
  const int64_t rank = pp_prime_pi(int64_t{1} << 31);
  ASSERT_GT(rank, 5000);

  pp_batch_result r;
  pp_batch_result_init(&r);
  ASSERT_EQ(pp_process_rank_batch(rank - 5000, 10000, &r), PP_OK);
  ASSERT_GT(r.prime_count, 0u);

  bool saw_big_p = false;
  for (size_t i = 0; i < r.prime_count; ++i) {
    if (r.prime_p[i] > INT32_MAX) saw_big_p = true;
  }
  bool saw_big_m = false;
  for (size_t i = 0; i < r.partition_count; ++i) {
    if (r.partition_m[i] > 30) saw_big_m = true;
  }
  EXPECT_TRUE(saw_big_p);
  EXPECT_TRUE(saw_big_m);

  CheckRoundTrip(r);
  pp_batch_result_clear(&r);
}

TEST_F(PartsExpandTest, RoundTripsOnLargeInt64Primes) {
  std::vector<uint64_t> primes;
  for (int m = 50; m <= 61; ++m) {
    const uint64_t base = uint64_t{1} << m;
    for (uint64_t d = 1; d < 4000; d += 2) {
      uint64_t base_out = 0;
      int32_t exp_out = 0;
      const uint64_t cand = base + d;
      if (pp_is_prime_power_u64(cand, &base_out, &exp_out) == PP_OK &&
          exp_out == 1) {
        primes.push_back(cand);
        break;
      }
    }
  }
  ASSERT_GE(primes.size(), 8u);
  std::sort(primes.begin(), primes.end());

  pp_batch_result r;
  pp_batch_result_init(&r);
  ASSERT_EQ(pp_process_prime_array(primes.data(), primes.size(), &r), PP_OK);
  ASSERT_EQ(r.prime_count, primes.size());

  bool saw_high_m = false;
  for (size_t i = 0; i < r.partition_count; ++i) {
    if (r.partition_m[i] >= 50) saw_high_m = true;
  }
  EXPECT_TRUE(saw_high_m);

  CheckRoundTrip(r);
  pp_batch_result_clear(&r);
}

}  // namespace
