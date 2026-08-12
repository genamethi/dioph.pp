#include "primeparts/graph/chain_forms.h"

#include <gtest/gtest.h>

#include <vector>

#include "primeparts/graph/hermite_modl.h"

namespace {

using primeparts::graph::ChainSymbols;
using primeparts::graph::ClosesAtTwo;
using primeparts::graph::Composed;
using primeparts::graph::Block;
using primeparts::graph::Degree;
using primeparts::graph::Fold;
using primeparts::graph::Folded;
using primeparts::graph::Lift;
using primeparts::graph::EvalAtTwo;
using primeparts::graph::FiberProduct;
using primeparts::graph::Graded;
using primeparts::graph::MaskToT;
using primeparts::graph::Step;
using primeparts::graph::ToHermite;
using primeparts::graph::TSymbol;
using primeparts::graph::TToMask;
using primeparts::graph::Uncollapsed;
using primeparts::graph::Word;

Word Chain(int64_t root, int64_t target, std::vector<Step> steps) {
  Word w;
  w.root = root;
  w.target = target;
  w.steps = std::move(steps);
  return w;
}

TEST(ChainFormsTest, MaskRoundTripsThroughT) {
  for (uint64_t mask : {uint64_t{0}, uint64_t{1} << 4,
                        (uint64_t{1} << 1) | (uint64_t{1} << 4),
                        (uint64_t{1} << 63), ~uint64_t{0}}) {
    uint64_t back = 0;
    ASSERT_TRUE(TToMask(MaskToT(mask), &back)) << mask;
    EXPECT_EQ(back, mask);
  }
}

TEST(ChainFormsTest, MaskRejectsNonBinaryCoefficients) {
  uint64_t back = 0;
  EXPECT_FALSE(TToMask(2 * GiNaC::pow(TSymbol(), 3), &back));
}

TEST(ChainFormsTest, EvaluationAtTwoRecoversTheInteger) {
  EXPECT_TRUE(EvalAtTwo(MaskToT((uint64_t{1} << 1) | (uint64_t{1} << 4)))
                  .is_equal(18));
}

TEST(ChainFormsTest, DepthOneFormsOfTwentyNine) {
  const std::vector<Word> forms = {Chain(13, 29, {{4, 1}}),
                                   Chain(3, 29, {{1, 3}}),
                                   Chain(5, 29, {{2, 2}})};
  const GiNaC::symbol x("x");

  ASSERT_TRUE(Composed(forms[0], x).is_equal(x + GiNaC::pow(TSymbol(), 4)));
  ASSERT_TRUE(Composed(forms[1], x).is_equal(GiNaC::pow(x, 3) + TSymbol()));
  ASSERT_TRUE(
      Composed(forms[2], x).is_equal(GiNaC::pow(x, 2) + GiNaC::pow(TSymbol(), 2)));

  for (const Word& w : forms) {
    EXPECT_TRUE(ClosesAtTwo(w)) << w.root;
  }
}

TEST(ChainFormsTest, DepthTwoEliminationMatchesByHand) {
  const GiNaC::symbol x("x");

  const Word via11 = Chain(11, 29, {{1, 1}, {4, 1}});
  const Word via3 = Chain(3, 29, {{2, 2}, {4, 1}});
  const Word via5 = Chain(5, 29, {{3, 1}, {4, 1}});

  const GiNaC::ex t = TSymbol();
  EXPECT_TRUE(Composed(via11, x).is_equal(x + t + GiNaC::pow(t, 4)));
  EXPECT_TRUE(Composed(via3, x).is_equal(GiNaC::pow(x, 2) + GiNaC::pow(t, 2) +
                                         GiNaC::pow(t, 4)));
  EXPECT_TRUE(Composed(via5, x).is_equal(x + GiNaC::pow(t, 3) +
                                         GiNaC::pow(t, 4)));

  for (const Word& w : {via11, via3, via5}) {
    EXPECT_TRUE(ClosesAtTwo(w)) << w.root;
  }
}

TEST(ChainFormsTest, GradedIsTheEliminationFiltration) {
  const GiNaC::symbol x("x");
  const Word w = Chain(3, 29, {{2, 2}, {4, 1}});
  const std::vector<GiNaC::ex> levels = Graded(w, x);

  ASSERT_EQ(levels.size(), 3u);
  EXPECT_TRUE(levels[0].is_equal(x));
  EXPECT_TRUE(levels[1].is_equal(GiNaC::pow(x, 2) + GiNaC::pow(TSymbol(), 2)));
  EXPECT_TRUE(levels.back().is_equal(Composed(w, x)));

  EXPECT_TRUE(EvalAtTwo(levels[1]).subs(x == 3).is_equal(13));
  EXPECT_TRUE(EvalAtTwo(levels[2]).subs(x == 3).is_equal(29));
}

TEST(ChainFormsTest, UncollapsedGeneratorsVanishOnTheChain) {
  const Word w = Chain(3, 29, {{2, 2}, {4, 1}});
  const std::vector<GiNaC::symbol> y = ChainSymbols(w);
  const std::vector<GiNaC::ex> gens = Uncollapsed(w, y);

  ASSERT_EQ(y.size(), 3u);
  ASSERT_EQ(gens.size(), 2u);

  GiNaC::lst subs;
  subs.append(y[0] == 3);
  subs.append(y[1] == 13);
  subs.append(y[2] == 29);
  for (const GiNaC::ex& g : gens) {
    EXPECT_TRUE(GiNaC::expand(EvalAtTwo(g).subs(subs)).is_zero());
  }
}

TEST(ChainFormsTest, HermiteCoefficientsCarryTheExponentsInHeZero) {
  const GiNaC::symbol x("x");
  const GiNaC::ex t = TSymbol();

  const auto h11 = ToHermite(Composed(Chain(11, 29, {{1, 1}, {4, 1}}), x), x);
  EXPECT_TRUE(h11.at(0).is_equal(t + GiNaC::pow(t, 4)));
  EXPECT_TRUE(h11.at(1).is_equal(1));

  const auto h3 = ToHermite(Composed(Chain(3, 29, {{2, 2}, {4, 1}}), x), x);
  EXPECT_TRUE(GiNaC::expand(h3.at(0) - (1 + GiNaC::pow(t, 2) + GiNaC::pow(t, 4)))
                  .is_zero());
  EXPECT_TRUE(h3.at(2).is_equal(1));

  uint64_t mask = 0;
  ASSERT_TRUE(TToMask(h11.at(0), &mask));
  EXPECT_EQ(mask, (uint64_t{1} << 1) | (uint64_t{1} << 4));
}

TEST(ChainFormsTest, FiberProductVanishesOnAgreeingRoots) {
  const GiNaC::symbol y("y");
  const GiNaC::symbol yp("yp");
  const Word a = Chain(3, 29, {{1, 3}});
  const Word b = Chain(5, 29, {{2, 2}});

  const GiNaC::ex c = FiberProduct(a, b, y, yp);
  EXPECT_TRUE(GiNaC::expand(c).is_equal(GiNaC::pow(y, 3) + TSymbol() -
                                        GiNaC::pow(yp, 2) -
                                        GiNaC::pow(TSymbol(), 2)));

  GiNaC::lst at;
  at.append(y == 3);
  at.append(yp == 5);
  EXPECT_TRUE(GiNaC::expand(EvalAtTwo(c).subs(at)).is_zero());
}

TEST(ChainFormsTest, FaithfulFoldReproducesTheStepPolynomial) {
  const GiNaC::symbol x("x");
  for (const Word& w : {Chain(11, 29, {{1, 1}, {4, 1}}),
                        Chain(3, 29, {{2, 2}, {4, 1}}),
                        Chain(3, 0, {{1, 1}, {2, 1}, {3, 3}, {5, 1}, {2, 2}})}) {
    const Folded f = Fold(w, Lift::kFaithful);
    EXPECT_TRUE(GiNaC::expand(Composed(f, x) - Composed(w, x)).is_zero());
    EXPECT_EQ(Degree(f), Degree(w));
  }
}

TEST(ChainFormsTest, FoldCollapsesRunsIntoBlockConstants) {
  const Word w = Chain(3, 29, {{2, 2}, {4, 1}});
  const Folded f = Fold(w, Lift::kFaithful);
  const GiNaC::ex t = TSymbol();

  EXPECT_TRUE(f.a0.is_zero());
  ASSERT_EQ(f.blocks.size(), 1u);
  EXPECT_EQ(f.blocks[0].n, 2);
  EXPECT_TRUE(GiNaC::expand(f.blocks[0].c - (GiNaC::pow(t, 2) + GiNaC::pow(t, 4)))
                  .is_zero());
}

TEST(ChainFormsTest, LiftsAgreeAtTwoAndDifferFormallyOnRepeatedExponents) {
  const GiNaC::symbol x("x");
  const Word w = Chain(0, 0, {{3, 1}, {3, 1}, {1, 2}});

  const Folded faithful = Fold(w, Lift::kFaithful);
  const Folded canonical = Fold(w, Lift::kCanonical);

  EXPECT_TRUE(GiNaC::expand(faithful.a0 - 2 * GiNaC::pow(TSymbol(), 3)).is_zero());
  EXPECT_TRUE(GiNaC::expand(canonical.a0 - GiNaC::pow(TSymbol(), 4)).is_zero());

  EXPECT_TRUE(GiNaC::expand(EvalAtTwo(Composed(faithful, x)) -
                            EvalAtTwo(Composed(canonical, x)))
                  .is_zero());
  EXPECT_FALSE(
      GiNaC::expand(Composed(faithful, x) - Composed(canonical, x)).is_zero());
}

TEST(ChainFormsTest, CanonicalLiftKeepsConstantsBinary) {
  const Word w = Chain(0, 0, {{3, 1}, {3, 1}, {1, 2}});
  const Folded canonical = Fold(w, Lift::kCanonical);
  uint64_t mask = 0;
  ASSERT_TRUE(TToMask(canonical.a0, &mask));
  EXPECT_EQ(mask, uint64_t{1} << 4);
}

TEST(ChainFormsTest, DegreeIsTheProductOfExponents) {
  EXPECT_EQ(Degree(Chain(3, 29, {{2, 2}, {4, 1}})), 2);
  EXPECT_EQ(Degree(Chain(3, 0, {{1, 3}, {2, 2}, {5, 4}})), 24);
  EXPECT_EQ(Degree(Chain(3, 0, {})), 1);
}

}  // namespace
