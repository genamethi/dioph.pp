#include "primeparts/graph/point_count.h"

#include <gtest/gtest.h>

#include <vector>
#include <numeric>
#include <cmath>

#include "primeparts/graph/chain_forms.h"
#include "primeparts/graph/words.h"
#include "primeparts/graph/as_fold.h"

namespace {

using primeparts::graph::ComputedHigher;
using primeparts::graph::ComputedMask;
using primeparts::graph::Composed;
using primeparts::graph::EnumerateWords;
using primeparts::graph::Folded;
using primeparts::graph::Moment;
using primeparts::graph::Sum;
using primeparts::graph::WordOptions;
using primeparts::graph::WordStats;
using primeparts::graph::Correlate;
using primeparts::graph::SwanAtInfinity;
using primeparts::graph::TraceSpectrum;
using primeparts::graph::Traces;
using primeparts::graph::WeilMagnitude;
using primeparts::graph::WeilSum;
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

TEST(PointCountTest, Total) {
  const GiNaC::symbol x("x");
  for (int d : {1, 2, 3, 4, 8}) {
    FieldPoints fp;
    std::string err;
    ASSERT_TRUE(FiberCounts(Composed(Chain(3, 29, {{1, 3}}), x), x, 2, d, 1, &fp,
                            &err))
        << err;
    EXPECT_EQ(fp.size(), int64_t{1} << d);
    EXPECT_EQ(TotalPoints(fp), int64_t{1} << d);
  }
}

TEST(PointCountTest, Linear) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  ASSERT_TRUE(FiberCounts(x + GiNaC::pow(TSymbol(), 3), x, 2, 5, 6, &fp, &err))
      << err;
  for (int32_t c : fp.counts) {
    EXPECT_EQ(c, 1);
  }
}

TEST(PointCountTest, Frobenius) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 2) + GiNaC::pow(TSymbol(), 2), x, 2, 6, 5,
                          &fp, &err))
      << err;
  for (int32_t c : fp.counts) {
    EXPECT_EQ(c, 1);
  }
}

TEST(PointCountTest, Cube) {
  const GiNaC::symbol x("x");
  for (int d : {2, 3, 4, 6}) {
    FieldPoints fp;
    std::string err;
    ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3) + TSymbol(), x, 2, d, 1, &fp, &err))
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

TEST(PointCountTest, ThetaZero) {
  const GiNaC::symbol x("x");
  FieldPoints at_zero;
  FieldPoints pure;
  std::string err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3) + GiNaC::pow(TSymbol(), 5), x, 2, 6, 0,
                          &at_zero, &err))
      << err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3), x, 2, 6, 1, &pure, &err)) << err;
  EXPECT_EQ(at_zero.counts, pure.counts);
}

TEST(PointCountTest, Correlation) {
  const GiNaC::symbol x("x");
  const int d = 6;
  const uint64_t theta = 3;
  const GiNaC::ex a = Composed(Chain(3, 29, {{1, 3}}), x);
  const GiNaC::ex b = Composed(Chain(5, 29, {{2, 2}}), x);

  FieldPoints fa;
  FieldPoints fb;
  std::string err;
  ASSERT_TRUE(FiberCounts(a, x, 2, d, theta, &fa, &err)) << err;
  ASSERT_TRUE(FiberCounts(b, x, 2, d, theta, &fb, &err)) << err;

  int64_t direct = 0;
  for (size_t i = 0; i < fa.counts.size(); ++i) {
    direct += static_cast<int64_t>(fa.counts[i]) * fb.counts[i];
  }
  EXPECT_EQ(Correlate(fa, fb), direct);
  EXPECT_GT(Correlate(fa, fb), 0);
}

TEST(PointCountTest, SelfCorrelation) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3) + TSymbol(), x, 2, 4, 1, &fp, &err))
      << err;
  const int64_t q = 16;
  const int64_t g = std::gcd(int64_t{3}, q - 1);
  EXPECT_EQ(Correlate(fp, fp), 1 + (q - 1) / g * g * g);
}

TEST(PointCountTest, SumOfOne) {
  const GiNaC::symbol x("x");
  const GiNaC::ex p = Composed(Chain(3, 29, {{1, 3}}), x);
  FieldPoints one;
  FieldPoints summed;
  std::string err;
  ASSERT_TRUE(FiberCounts(p, x, 2, 5, 3, &one, &err)) << err;
  ASSERT_TRUE(Sum({p}, x, 2, 5, 3, &summed, &err)) << err;
  EXPECT_EQ(one.counts, summed.counts);
}

TEST(PointCountTest, Moments) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 3) + TSymbol(), x, 2, 6, 5, &fp, &err))
      << err;
  EXPECT_EQ(Moment(fp, 1), int64_t{64});
  EXPECT_EQ(Moment(fp, 2), Correlate(fp, fp));
}

TEST(PointCountTest, CoproductSecondMoment) {
  const GiNaC::symbol x("x");
  const int d = 6;
  const uint64_t theta = 3;
  std::vector<Folded> words;
  WordStats stats;
  std::string err;
  ASSERT_TRUE(EnumerateWords(
      29, ComputedMask, ComputedHigher, WordOptions{},
      [&](const Folded& f) { words.push_back(f); }, &stats, &err))
      << err;
  ASSERT_GE(words.size(), 4u);

  std::vector<GiNaC::ex> polys;
  for (const Folded& f : words) {
    polys.push_back(Composed(f, x));
  }

  FieldPoints total;
  ASSERT_TRUE(Sum(polys, x, 2, d, theta, &total, &err)) << err;
  EXPECT_EQ(Moment(total, 1), static_cast<int64_t>(polys.size()) * (1 << d));

  int64_t pairwise = 0;
  for (const GiNaC::ex& a : polys) {
    for (const GiNaC::ex& b : polys) {
      FieldPoints fa;
      FieldPoints fb;
      ASSERT_TRUE(FiberCounts(a, x, 2, d, theta, &fa, &err)) << err;
      ASSERT_TRUE(FiberCounts(b, x, 2, d, theta, &fb, &err)) << err;
      pairwise += Correlate(fa, fb);
    }
  }
  EXPECT_EQ(Moment(total, 2), pairwise);
  EXPECT_GT(Moment(total, 2), Moment(total, 1));
}

TEST(PointCountTest, OddCharacteristic) {
  const GiNaC::symbol x("x");
  std::string err;
  for (int r : {3, 5, 7}) {
    FieldPoints fp;
    ASSERT_TRUE(FiberCounts(x + GiNaC::pow(TSymbol(), 3), x, r, 2, 1, &fp, &err))
        << err;
    EXPECT_EQ(fp.size(), int64_t{r} * r);
    for (int32_t c : fp.counts) {
      EXPECT_EQ(c, 1) << r;
    }
  }
}

TEST(PointCountTest, ShiftSurvivesAnOddBase) {
  const GiNaC::symbol x("x");
  std::string err;
  FieldPoints shifted;
  FieldPoints pure;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 2) + GiNaC::pow(TSymbol(), 3), x, 5, 2,
                          1, &shifted, &err))
      << err;
  ASSERT_TRUE(FiberCounts(GiNaC::pow(x, 2), x, 5, 2, 1, &pure, &err)) << err;
  EXPECT_NE(shifted.counts, pure.counts);
}

TEST(PointCountTest, TraceCountsPartitionTheField) {
  const GiNaC::symbol x("x");
  std::string err;
  for (int r : {2, 3, 5}) {
    TraceSpectrum s;
    ASSERT_TRUE(Traces(GiNaC::pow(x, 3) + TSymbol(), x, r, 2, 1, &s, &err))
        << err;
    int64_t total = 0;
    for (int64_t c : s.counts) total += c;
    EXPECT_EQ(total, int64_t{r} * r) << r;
    EXPECT_EQ(static_cast<int>(s.counts.size()), r);
  }
}

TEST(PointCountTest, WeilBound) {
  const GiNaC::symbol x("x");
  std::string err;
  for (int d : {4, 6, 8}) {
    const GiNaC::ex poly = Composed(Chain(3, 0, {{1, 2}, {1, 3}}), x);
    TraceSpectrum s;
    ASSERT_TRUE(Traces(poly, x, 2, d, 3, &s, &err)) << err;
    const int64_t sum = WeilSum(s);
    const int swan = SwanAtInfinity(poly, x);
    const double bound = (swan - 1) * std::sqrt(static_cast<double>(1 << d));
    EXPECT_LE(std::abs(static_cast<double>(sum)), bound + 1e-9)
        << "d=" << d << " sum=" << sum << " swan=" << swan;
    EXPECT_NEAR(WeilMagnitude(s), std::abs(static_cast<double>(sum)), 1e-9);
  }
}

TEST(PointCountTest, Rejects) {
  const GiNaC::symbol x("x");
  FieldPoints fp;
  std::string err;
  EXPECT_FALSE(FiberCounts(x, x, 2, 0, 0, &fp, &err));
  EXPECT_FALSE(FiberCounts(x, x, 2, 25, 0, &fp, &err));
  EXPECT_FALSE(FiberCounts(x, x, 2, 3, 8, &fp, &err));
}

}  // namespace
