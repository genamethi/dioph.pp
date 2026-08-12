#include "primeparts/graph/point_count.h"

#include <gtest/gtest.h>

#include <vector>
#include <numeric>

#include "primeparts/graph/chain_forms.h"

namespace {

using primeparts::graph::Composed;
using primeparts::graph::Correlate;
using primeparts::graph::FieldPoints;
using primeparts::graph::FiberCounts;
using primeparts::graph::Step;
using primeparts::graph::TotalPoints;
using primeparts::graph::TSymbol;
using primeparts::graph::Word;

Word Chain(int64_t root, int64_t target, std::vector<Step> steps) {
  Word w;
  w.root = root;
  w.target = target;
  w.steps = std::move(steps);
  return w;
}

TEST(PointCountTest, CountsSumToTheFieldSize) {
  const GiNaC::symbol x("x");
  for (int d : {1, 2, 3, 4, 8}) {
    FieldPoints fp;
    std::string err;
    ASSERT_TRUE(FiberCounts(Composed(Chain(3, 29, {{1, 3}}), x), x, d, 1, &fp,
                            &err))
        << err;
    EXPECT_EQ(fp.size(), int64_t{1} << d);
    EXPECT_EQ(TotalPoints(fp), int64_t{1} << d);
  }
}

TEST(PointCountTest, LinearMapIsABijection) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  ASSERT_TRUE(FiberCounts(x + GiNaC::pow(TSymbol(), 3), x, 5, 6, &fp, &err))
      << err;
  for (int32_t c : fp.counts) {
    EXPECT_EQ(c, 1);
  }
}

TEST(PointCountTest, SquaringIsFrobeniusHenceAlsoABijection) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 2) + GiNaC::pow(TSymbol(), 2), x, 6, 5,
                          &fp, &err))
      << err;
  for (int32_t c : fp.counts) {
    EXPECT_EQ(c, 1);
  }
}

TEST(PointCountTest, CubeFibersMatchTheGcdWithFieldOrder) {
  const GiNaC::symbol x("x");
  for (int d : {2, 3, 4, 6}) {
    FieldPoints fp;
    std::string err;
    ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3) + TSymbol(), x, d, 1, &fp, &err))
        << err;
    const int64_t q = int64_t{1} << d;
    const int64_t g = std::gcd(int64_t{3}, q - 1);
    int64_t nonzero = 0;
    for (int32_t c : fp.counts) {
      if (c != 0) ++nonzero;
      EXPECT_TRUE(c == 0 || c == static_cast<int32_t>(g) || c == 1) << d;
    }
    EXPECT_EQ(nonzero, (q - 1) / g + 1) << d;
  }
}

TEST(PointCountTest, ThetaZeroIsTheDegenerateFiber) {
  const GiNaC::symbol x("x");
  FieldPoints at_zero;
  FieldPoints pure;
  std::string err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3) + GiNaC::pow(TSymbol(), 5), x, 6, 0,
                          &at_zero, &err))
      << err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3), x, 6, 1, &pure, &err)) << err;
  EXPECT_EQ(at_zero.counts, pure.counts);
}

TEST(PointCountTest, CorrelationMatchesDirectPairCount) {
  const GiNaC::symbol x("x");
  const int d = 6;
  const uint64_t theta = 3;
  const GiNaC::ex a = Composed(Chain(3, 29, {{1, 3}}), x);
  const GiNaC::ex b = Composed(Chain(5, 29, {{2, 2}}), x);

  FieldPoints fa;
  FieldPoints fb;
  std::string err;
  ASSERT_TRUE(FiberCounts(a, x, d, theta, &fa, &err)) << err;
  ASSERT_TRUE(FiberCounts(b, x, d, theta, &fb, &err)) << err;

  int64_t direct = 0;
  for (size_t i = 0; i < fa.counts.size(); ++i) {
    direct += static_cast<int64_t>(fa.counts[i]) * fb.counts[i];
  }
  EXPECT_EQ(Correlate(fa, fb), direct);
  EXPECT_GT(Correlate(fa, fb), 0);
}

TEST(PointCountTest, CorrelationOfAChainWithItselfCountsCollisions) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3) + TSymbol(), x, 4, 1, &fp, &err))
      << err;
  const int64_t q = 16;
  const int64_t g = std::gcd(int64_t{3}, q - 1);
  EXPECT_EQ(Correlate(fp, fp), 1 + (q - 1) / g * g * g);
}

TEST(PointCountTest, RejectsOutOfRangeArguments) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  EXPECT_FALSE(FiberCounts(x, x, 0, 0, &fp, &err));
  EXPECT_FALSE(FiberCounts(x, x, 25, 0, &fp, &err));
  EXPECT_FALSE(FiberCounts(x, x, 3, 8, &fp, &err));
}

}  // namespace
