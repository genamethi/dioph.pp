#include "primeparts/graph/ramification.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "primeparts/graph/chain_forms.h"
#include "primeparts/graph/point_count.h"

namespace {

using primeparts::graph::BranchLocus;
using primeparts::graph::Composed;
using primeparts::graph::FiberCounts;
using primeparts::graph::FieldPoints;
using primeparts::graph::Local;
using primeparts::graph::LocalAt;
using primeparts::graph::Step;
using primeparts::graph::TSymbol;
using primeparts::graph::Word;

Word Chain(std::vector<Step> steps) {
  Word w;
  w.steps = std::move(steps);
  return w;
}

int64_t Sum(const Local& l) {
  int64_t total = 0;
  for (const auto& place : l.places) total += place.e * place.f;
  return total;
}

TEST(RamificationTest, Identity) {
  const GiNaC::symbol x("x");
  std::string err;
  for (int p : {2, 3, 5}) {
    for (uint64_t c = 0; c < 9 && c < static_cast<uint64_t>(p) * p; ++c) {
      Local l;
      ASSERT_TRUE(LocalAt(Composed(Chain({{1, 3}}), x), x, p, 2, 1, c, &l, &err))
          << err;
      EXPECT_EQ(Sum(l), l.degree) << "p=" << p << " c=" << c;
      EXPECT_EQ(l.degree, 3);
    }
  }
}

TEST(RamificationTest, Cube) {
  const GiNaC::symbol x("x");
  std::string err;
  std::vector<uint64_t> locus;
  ASSERT_TRUE(BranchLocus(GiNaC::pow(x, 3) + TSymbol(), x, 7, 1, 1, &locus, &err))
      << err;
  ASSERT_EQ(locus.size(), 1u);
  EXPECT_EQ(locus[0], 1u);

  Local l;
  ASSERT_TRUE(LocalAt(GiNaC::pow(x, 3) + TSymbol(), x, 7, 1, 1, 1, &l, &err))
      << err;
  ASSERT_EQ(l.places.size(), 1u);
  EXPECT_EQ(l.places[0].e, 3);
  EXPECT_EQ(l.places[0].f, 1);
  EXPECT_TRUE(l.ramified);
  EXPECT_TRUE(l.tame);
}

TEST(RamificationTest, Wild) {
  const GiNaC::symbol x("x");
  std::string err;
  Local l;
  ASSERT_TRUE(LocalAt(GiNaC::pow(x, 4) + x, x, 3, 1, 1, 0, &l, &err)) << err;
  EXPECT_TRUE(l.separable);
  EXPECT_TRUE(l.ramified);
  EXPECT_FALSE(l.tame);
  ASSERT_EQ(l.places.size(), 2u);
}

TEST(RamificationTest, Inseparable) {
  const GiNaC::symbol x("x");
  std::string err;
  Local l;
  ASSERT_TRUE(LocalAt(GiNaC::pow(x, 2) + TSymbol(), x, 2, 3, 1, 0, &l, &err))
      << err;
  EXPECT_FALSE(l.separable);
  EXPECT_FALSE(l.ramified);
  ASSERT_EQ(l.places.size(), 1u);
  EXPECT_EQ(l.places[0].e, 2);

  Local sep;
  ASSERT_TRUE(LocalAt(GiNaC::pow(x, 3) + TSymbol(), x, 2, 3, 1, 0, &sep, &err))
      << err;
  EXPECT_TRUE(sep.separable);
}

TEST(RamificationTest, Counts) {
  const GiNaC::symbol x("x");
  std::string err;
  const GiNaC::ex poly = GiNaC::pow(x, 3) + TSymbol();
  FieldPoints fp;
  ASSERT_TRUE(FiberCounts(poly, x, 7, 1, 1, &fp, &err)) << err;

  int64_t small_fibers = 0;
  for (int32_t c : fp.counts) {
    if (c < 3) ++small_fibers;
  }
  std::vector<uint64_t> locus;
  ASSERT_TRUE(BranchLocus(poly, x, 7, 1, 1, &locus, &err)) << err;
  EXPECT_GT(small_fibers, static_cast<int64_t>(locus.size()));
}

TEST(RamificationTest, TwoBlocks) {
  const GiNaC::symbol x("x");
  std::string err;
  const GiNaC::ex poly = Composed(Chain({{1, 2}, {1, 3}}), x);
  for (uint64_t c = 0; c < 8; ++c) {
    Local l;
    ASSERT_TRUE(LocalAt(poly, x, 2, 3, 3, c, &l, &err)) << err;
    EXPECT_EQ(l.degree, 6);
    EXPECT_EQ(Sum(l), 6) << c;
  }
}

TEST(RamificationTest, Rejects) {
  const GiNaC::symbol x("x");
  std::string err;
  Local l;
  EXPECT_FALSE(LocalAt(x, x, 2, 0, 0, 0, &l, &err));
  EXPECT_FALSE(LocalAt(x, x, 2, 3, 99, 0, &l, &err));
}

}  // namespace
