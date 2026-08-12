#include "primeparts/graph/as_fold.h"

#include <gtest/gtest.h>

#include <vector>

#include "primeparts/graph/chain_forms.h"

namespace {

using primeparts::graph::ArtinSchreierReduce;
using primeparts::graph::Composed;
using primeparts::graph::EvalAtTwo;
using primeparts::graph::H1Dimension;
using primeparts::graph::IsSquareModTwo;
using primeparts::graph::ReduceModTwo;
using primeparts::graph::SqrtModTwo;
using primeparts::graph::Step;
using primeparts::graph::SwanAtInfinity;
using primeparts::graph::TSymbol;
using primeparts::graph::Word;

int OddPart(int n) {
  while ((n & 1) == 0) n >>= 1;
  return n;
}

Word Chain(std::vector<Step> steps) {
  Word w;
  w.steps = std::move(steps);
  return w;
}

TEST(AsFoldTest, Squares) {
  const GiNaC::ex t = TSymbol();
  EXPECT_TRUE(IsSquareModTwo(GiNaC::pow(t, 4)));
  EXPECT_TRUE(IsSquareModTwo(1 + GiNaC::pow(t, 2) + GiNaC::pow(t, 6)));
  EXPECT_FALSE(IsSquareModTwo(GiNaC::pow(t, 3)));
  EXPECT_FALSE(IsSquareModTwo(t + GiNaC::pow(t, 2)));
  EXPECT_TRUE(IsSquareModTwo(2 * GiNaC::pow(t, 3)));
}

TEST(AsFoldTest, Sqrt) {
  const GiNaC::ex t = TSymbol();
  EXPECT_TRUE(GiNaC::expand(SqrtModTwo(GiNaC::pow(t, 4)) - GiNaC::pow(t, 2))
                  .is_zero());
  EXPECT_TRUE(
      GiNaC::expand(SqrtModTwo(1 + GiNaC::pow(t, 6)) - (1 + GiNaC::pow(t, 3)))
          .is_zero());
}

TEST(AsFoldTest, ModTwo) {
  const GiNaC::ex t = TSymbol();
  EXPECT_TRUE(ReduceModTwo(2 * GiNaC::pow(t, 3)).is_zero());
  EXPECT_TRUE(GiNaC::expand(ReduceModTwo(3 * GiNaC::pow(t, 3)) -
                            GiNaC::pow(t, 3))
                  .is_zero());
}

TEST(AsFoldTest, DepthOne) {
  const GiNaC::symbol x("x");
  for (int n = 2; n <= 25; ++n) {
    for (int m : {1, 2, 3, 7, 12}) {
      const GiNaC::ex p = Composed(Chain({{m, n}}), x);
      EXPECT_EQ(SwanAtInfinity(p, x), OddPart(n)) << "n=" << n << " m=" << m;
    }
  }
}

TEST(AsFoldTest, TwoBlocks) {
  const GiNaC::symbol x("x");
  const GiNaC::ex odd = Composed(Chain({{1, 2}, {1, 3}}), x);
  const GiNaC::ex even = Composed(Chain({{2, 2}, {1, 3}}), x);

  EXPECT_EQ(SwanAtInfinity(odd, x), 4);
  EXPECT_EQ(SwanAtInfinity(even, x), 3);
  EXPECT_EQ(H1Dimension(odd, x), 3);
  EXPECT_EQ(H1Dimension(even, x), 2);
}

TEST(AsFoldTest, EvaluatedBase) {
  const GiNaC::symbol x("x");
  const Word w = Chain({{1, 2}, {1, 3}});
  const GiNaC::ex formal = Composed(w, x);
  const GiNaC::ex evaluated = EvalAtTwo(formal);

  EXPECT_EQ(SwanAtInfinity(formal, x), 4);
  EXPECT_EQ(SwanAtInfinity(evaluated, x), OddPart(6));
}

TEST(AsFoldTest, Idempotent) {
  const GiNaC::symbol x("x");
  for (const Word& w : {Chain({{1, 2}, {1, 3}}), Chain({{3, 4}, {2, 5}}),
                        Chain({{1, 1}, {2, 6}, {5, 3}})}) {
    const GiNaC::ex once = ArtinSchreierReduce(Composed(w, x), x);
    const GiNaC::ex twice = ArtinSchreierReduce(once, x);
    EXPECT_TRUE(GiNaC::expand(once - twice).is_zero());
  }
}

TEST(AsFoldTest, PowerOfTwoDegree) {
  const GiNaC::symbol x("x");
  for (int k = 1; k <= 4; ++k) {
    const int n = 1 << k;
    const GiNaC::ex p = EvalAtTwo(Composed(Chain({{3, n}}), x));
    EXPECT_EQ(SwanAtInfinity(p, x), 1) << n;
    EXPECT_EQ(H1Dimension(p, x), 0) << n;
  }
}

}  // namespace
