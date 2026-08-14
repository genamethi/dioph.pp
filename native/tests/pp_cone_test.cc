#include "primeparts/pp/cone.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

using primeparts::pp::ComputedHigher;
using primeparts::pp::ComputedMask;
using primeparts::pp::Cone;
using primeparts::pp::ConeStats;

std::vector<int64_t> Ancestors(int64_t p) {
  return Cone(p, ComputedMask, nullptr);
}

TEST(PpConeTest, Mask) {
  EXPECT_EQ(ComputedMask(29), uint64_t{1} << 4);
  EXPECT_EQ(ComputedMask(13), (uint64_t{1} << 1) | (uint64_t{1} << 3));
  EXPECT_EQ(ComputedMask(11), (uint64_t{1} << 2) | (uint64_t{1} << 3));
  EXPECT_EQ(ComputedMask(3), uint64_t{0});
}

TEST(PpConeTest, Higher) {
  const std::vector<primeparts::pp::Ascent> a = ComputedHigher(11);
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].m, 1);
  EXPECT_EQ(a[0].n, 2);
  EXPECT_EQ(a[0].q, 3);

  const std::vector<primeparts::pp::Ascent> b = ComputedHigher(29);
  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(b[0].m, 1);
  EXPECT_EQ(b[0].n, 3);
  EXPECT_EQ(b[0].q, 3);
  EXPECT_EQ(b[1].m, 2);
  EXPECT_EQ(b[1].n, 2);
  EXPECT_EQ(b[1].q, 5);
}

TEST(PpConeTest, Anchor) {
  EXPECT_EQ(Ancestors(29), (std::vector<int64_t>{3, 5, 7, 11, 13, 29}));
}

TEST(PpConeTest, Closure) {
  for (int64_t p : {29, 137, 8191, 65537}) {
    const std::vector<int64_t> cone = Ancestors(p);
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

TEST(PpConeTest, Bounds) {
  const std::vector<int64_t> cone = Ancestors(8191);
  EXPECT_EQ(cone.back(), 8191);
  EXPECT_GE(cone.front(), 3);
}

TEST(PpConeTest, Sizes) {
  ConeStats a;
  Cone(86573, ComputedMask, &a);
  EXPECT_EQ(a.nodes, 3717);

  ConeStats b;
  Cone(987391, ComputedMask, &b);
  EXPECT_EQ(b.nodes, 26791);
}

TEST(PpConeTest, Stats) {
  ConeStats s;
  const std::vector<int64_t> cone = Cone(29, ComputedMask, &s);
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

TEST(PpConeTest, Degenerate) {
  EXPECT_TRUE(Ancestors(2).empty());
  EXPECT_EQ(Ancestors(3), (std::vector<int64_t>{3}));
  EXPECT_TRUE(ComputedHigher(2).empty());
}

}  // namespace
