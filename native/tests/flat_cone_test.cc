#include "primeparts/graph/flat_cone.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

using primeparts::graph::ComputedMask;
using primeparts::graph::ConeStats;
using primeparts::graph::FlatCone;

std::vector<int64_t> Cone(int64_t p) {
  return FlatCone(p, ComputedMask, nullptr);
}

TEST(FlatConeTest, MaskIsTheSetOfPrimeSubtractions) {
  EXPECT_EQ(ComputedMask(29), uint64_t{1} << 4);
  EXPECT_EQ(ComputedMask(13), (uint64_t{1} << 1) | (uint64_t{1} << 3));
  EXPECT_EQ(ComputedMask(11), (uint64_t{1} << 2) | (uint64_t{1} << 3));
  EXPECT_EQ(ComputedMask(3), uint64_t{0});
}

TEST(FlatConeTest, ConeOfTwentyNineByHand) {
  EXPECT_EQ(Cone(29), (std::vector<int64_t>{3, 5, 7, 11, 13, 29}));
}

TEST(FlatConeTest, ConeIsClosedDownwardUnderTheMask) {
  for (int64_t p : {29, 137, 8191, 65537}) {
    const std::vector<int64_t> cone = Cone(p);
    ASSERT_FALSE(cone.empty()) << p;
    for (int64_t v : cone) {
      uint64_t bits = ComputedMask(v);
      while (bits) {
        const int m = __builtin_ctzll(bits);
        const int64_t q = v - (int64_t{1} << m);
        EXPECT_TRUE(std::binary_search(cone.begin(), cone.end(), q))
            << "p=" << p << " v=" << v << " m=" << m;
        bits &= bits - 1;
      }
    }
  }
}

TEST(FlatConeTest, ConeContainsItsTargetAndNothingAbove) {
  const std::vector<int64_t> cone = Cone(8191);
  EXPECT_EQ(cone.back(), 8191);
  EXPECT_GE(cone.front(), 3);
}

TEST(FlatConeTest, MatchesIndependentlyMeasuredSizes) {
  ConeStats a;
  FlatCone(86573, ComputedMask, &a);
  EXPECT_EQ(a.nodes, 3717);

  ConeStats b;
  FlatCone(987391, ComputedMask, &b);
  EXPECT_EQ(b.nodes, 26791);
}

TEST(FlatConeTest, StatsCountEdgesAndFlatSources) {
  ConeStats s;
  const std::vector<int64_t> cone = FlatCone(29, ComputedMask, &s);
  EXPECT_EQ(s.nodes, static_cast<int64_t>(cone.size()));

  int64_t edges = 0;
  int64_t sources = 0;
  for (int64_t v : cone) {
    const uint64_t bits = ComputedMask(v);
    edges += __builtin_popcountll(bits);
    if (bits == 0) ++sources;
  }
  EXPECT_EQ(s.edges, edges);
  EXPECT_EQ(s.flat_sources, sources);
}

TEST(FlatConeTest, SmallInputsYieldNothing) {
  EXPECT_TRUE(Cone(2).empty());
  EXPECT_EQ(Cone(3), (std::vector<int64_t>{3}));
}

}  // namespace
