#include "primeparts/graph/characters.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using primeparts::graph::Cyclotomic;
using primeparts::graph::GaussSum;
using primeparts::graph::JacobiSum;
using primeparts::graph::Magnitude;
using primeparts::graph::Total;

int64_t Order(int p, int d) {
  int64_t q = 1;
  for (int i = 0; i < d; ++i) q *= p;
  return q;
}

TEST(CharactersTest, GaussTermCount) {
  std::string err;
  for (int p : {2, 3, 5}) {
    for (int d : {1, 2}) {
      Cyclotomic g;
      ASSERT_TRUE(GaussSum(p, d, 1, &g, &err)) << err;
      EXPECT_EQ(Total(g), Order(p, d) - 1);
    }
  }
}

TEST(CharactersTest, GaussModulus) {
  std::string err;
  for (int p : {3, 5, 7}) {
    for (int d : {1, 2}) {
      const int64_t q = Order(p, d);
      for (int64_t a : {int64_t{1}, int64_t{2}}) {
        if (a % (q - 1) == 0) continue;
        Cyclotomic g;
        ASSERT_TRUE(GaussSum(p, d, a, &g, &err)) << err;
        EXPECT_NEAR(Magnitude(g), std::sqrt(static_cast<double>(q)), 1e-6)
            << "p=" << p << " d=" << d << " a=" << a;
      }
    }
  }
}

TEST(CharactersTest, TrivialCharacterGivesMinusOne) {
  std::string err;
  Cyclotomic g;
  ASSERT_TRUE(GaussSum(5, 1, 0, &g, &err)) << err;
  EXPECT_NEAR(Magnitude(g), 1.0, 1e-9);
}

TEST(CharactersTest, JacobiTermCount) {
  std::string err;
  for (int p : {3, 5, 7}) {
    Cyclotomic j;
    ASSERT_TRUE(JacobiSum(p, 1, 1, 1, &j, &err)) << err;
    EXPECT_EQ(Total(j), Order(p, 1) - 2);
  }
}

TEST(CharactersTest, JacobiModulus) {
  std::string err;
  for (int p : {7, 11, 13}) {
    const int64_t q = Order(p, 1);
    Cyclotomic j;
    ASSERT_TRUE(JacobiSum(p, 1, 1, 1, &j, &err)) << err;
    EXPECT_NEAR(Magnitude(j), std::sqrt(static_cast<double>(q)), 1e-6) << p;
  }
}

TEST(CharactersTest, JacobiFromGauss) {
  std::string err;
  const int p = 13;
  Cyclotomic ga;
  Cyclotomic gb;
  Cyclotomic gab;
  Cyclotomic j;
  ASSERT_TRUE(GaussSum(p, 1, 1, &ga, &err)) << err;
  ASSERT_TRUE(GaussSum(p, 1, 2, &gb, &err)) << err;
  ASSERT_TRUE(GaussSum(p, 1, 3, &gab, &err)) << err;
  ASSERT_TRUE(JacobiSum(p, 1, 1, 2, &j, &err)) << err;

  EXPECT_NEAR(Magnitude(j), Magnitude(ga) * Magnitude(gb) / Magnitude(gab),
              1e-6);
}

TEST(CharactersTest, Rejects) {
  std::string err;
  Cyclotomic g;
  EXPECT_FALSE(GaussSum(1, 1, 1, &g, &err));
  EXPECT_FALSE(GaussSum(2, 0, 1, &g, &err));
  EXPECT_FALSE(GaussSum(2, 40, 1, &g, &err));
}

}  // namespace
