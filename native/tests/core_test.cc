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
  EXPECT_EQ(result.partition_count, 19u);

  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(result.prime_p[i], expected_p[i]);
    EXPECT_EQ(result.prime_k[i], expected_k[i]);
  }

  auto check_partition = [&](size_t i, int64_t p, int32_t m, int32_t n,
                             int64_t q) {
    ASSERT_LT(i, result.partition_count);
    EXPECT_EQ(result.partition_p[i], p);
    EXPECT_EQ(result.partition_m[i], m);
    EXPECT_EQ(result.partition_n[i], n);
    EXPECT_EQ(result.partition_q[i], q);
  };
  check_partition(0, 5, 1, 1, 3);
  check_partition(1, 7, 1, 1, 5);
  check_partition(2, 7, 2, 1, 3);
  check_partition(3, 11, 1, 2, 3);
  check_partition(4, 11, 2, 1, 7);
  check_partition(5, 11, 3, 1, 3);
  check_partition(18, 29, 4, 1, 13);

  pp_batch_result_clear(&result);
}

TEST_F(CoreTest, CountRankBatch) {
  pp_count_result counts;
  ASSERT_EQ(pp_count_rank_batch(1, 10, &counts), PP_OK);
  EXPECT_EQ(counts.processed_count, 10u);
  EXPECT_EQ(counts.partition_count, 19u);
}

}  // namespace
