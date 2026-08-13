#include "primeparts/core.h"

#include <gtest/gtest.h>

namespace {

class CoreTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { ASSERT_EQ(pp_init(), PP_OK); }
  static void TearDownTestSuite() { pp_shutdown(); }
};

TEST_F(CoreTest, PrimeHelpers) {
  EXPECT_EQ(pp_prime_pi(7), 4);
  EXPECT_EQ(pp_nth_prime(10), 29);
  EXPECT_EQ(pp_previous_prime(10), 7);
}

TEST_F(CoreTest, PrimePower) {
  uint64_t base = 0;
  int32_t exponent = 0;
  ASSERT_EQ(pp_is_prime_power_u64(64, &base, &exponent), PP_OK);
  EXPECT_EQ(base, 2u);
  EXPECT_EQ(exponent, 6);
  ASSERT_EQ(pp_is_prime_power_u64(81, &base, &exponent), PP_OK);
  EXPECT_EQ(base, 3u);
  EXPECT_EQ(exponent, 4);
  ASSERT_EQ(pp_is_prime_power_u64(12, &base, &exponent), PP_OK);
  EXPECT_EQ(exponent, 0);
}

TEST_F(CoreTest, ProcessRankBatch) {
  const int64_t expected_p[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29};
  const int32_t expected_k[] = {0, 0, 1, 2, 3, 3, 2, 3, 2, 3};

  pp_batch_result result;
  pp_batch_result_init(&result);
  ASSERT_EQ(pp_process_rank_batch(1, 10, &result), PP_OK);
  EXPECT_EQ(result.processed_count, 10u);
  EXPECT_EQ(result.prime_count, 10u);

  int32_t total_k = 0;
  int reps = 0;
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(result.prime_p[i], expected_p[i]);
    EXPECT_EQ(result.prime_k[i], expected_k[i]);
    total_k += result.prime_k[i];
    reps += __builtin_popcountll(result.prime_flat_mask[i]);
  }
  reps += static_cast<int>(result.higher_count);
  EXPECT_EQ(total_k, 19);
  EXPECT_EQ(reps, 19);

  auto mask_of = [&](int64_t p) -> uint64_t {
    for (size_t i = 0; i < result.prime_count; ++i)
      if (result.prime_p[i] == p) return result.prime_flat_mask[i];
    return 0;
  };
  EXPECT_EQ(mask_of(5), uint64_t{1} << 1);
  EXPECT_EQ(mask_of(7), (uint64_t{1} << 1) | (uint64_t{1} << 2));
  EXPECT_EQ(mask_of(11), (uint64_t{1} << 2) | (uint64_t{1} << 3));
  EXPECT_EQ(mask_of(29), uint64_t{1} << 4);

  auto has_higher = [&](int64_t p, int32_t m, int32_t n, int64_t q) {
    for (size_t i = 0; i < result.higher_count; ++i)
      if (result.higher_p[i] == p && result.higher_m[i] == m &&
          result.higher_n[i] == n && result.higher_q[i] == q)
        return true;
    return false;
  };
  EXPECT_TRUE(has_higher(11, 1, 2, 3));

  pp_batch_result_clear(&result);
}

TEST_F(CoreTest, CountRankBatch) {
  pp_count_result counts;
  ASSERT_EQ(pp_count_rank_batch(1, 10, &counts), PP_OK);
  EXPECT_EQ(counts.processed_count, 10u);
  EXPECT_EQ(counts.partition_count, 19u);
}

}  // namespace
