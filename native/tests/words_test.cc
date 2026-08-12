#include "primeparts/graph/words.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "primeparts/graph/as_fold.h"

namespace {

using primeparts::graph::ComputedHigher;
using primeparts::graph::ComputedMask;
using primeparts::graph::Composed;
using primeparts::graph::EnumerateWords;
using primeparts::graph::EvalAtTwo;
using primeparts::graph::Folded;
using primeparts::graph::H1Dimension;
using primeparts::graph::Lift;
using primeparts::graph::SwanAtInfinity;
using primeparts::graph::WordKey;
using primeparts::graph::WordOptions;
using primeparts::graph::WordStats;

std::vector<Folded> Words(int64_t p, WordStats* stats = nullptr) {
  std::vector<Folded> out;
  WordStats local;
  std::string err;
  EXPECT_TRUE(EnumerateWords(
      p, ComputedMask, ComputedHigher, WordOptions{},
      [&](const Folded& f) { out.push_back(f); }, stats ? stats : &local, &err))
      << err;
  return out;
}

bool Closes(const Folded& f) {
  const GiNaC::symbol x("x");
  const GiNaC::ex v =
      EvalAtTwo(Composed(f, x)).subs(x == GiNaC::numeric(f.root));
  return GiNaC::expand(v).is_equal(GiNaC::numeric(f.target));
}

TEST(WordsTest, ClosesAtTwo) {
  for (int64_t p : {29, 137, 251, 8191}) {
    const std::vector<Folded> words = Words(p);
    ASSERT_FALSE(words.empty()) << p;
    for (const Folded& f : words) {
      EXPECT_TRUE(Closes(f)) << "p=" << p << " root=" << f.root;
      EXPECT_EQ(f.target, p);
    }
  }
}

TEST(WordsTest, Distinct) {
  for (int64_t p : {29, 137, 1021}) {
    std::set<std::string> keys;
    for (const Folded& f : Words(p)) {
      EXPECT_TRUE(keys.insert(WordKey(f)).second) << p;
    }
  }
}

TEST(WordsTest, Anchor) {
  const std::vector<Folded> words = Words(29);
  std::set<std::string> got;
  for (const Folded& f : words) {
    got.insert(WordKey(f));
  }
  EXPECT_EQ(words.size(), got.size());
  EXPECT_GE(words.size(), 4u);

  bool saw_pure_flat = false;
  bool saw_cube = false;
  for (const Folded& f : words) {
    if (f.blocks.empty()) saw_pure_flat = true;
    if (f.blocks.size() == 1 && f.blocks[0].n == 3) saw_cube = true;
  }
  EXPECT_TRUE(saw_pure_flat);
  EXPECT_TRUE(saw_cube);
}

TEST(WordsTest, Roots) {
  for (const Folded& f : Words(251)) {
    EXPECT_EQ(ComputedMask(f.root), uint64_t{0}) << f.root;
    EXPECT_TRUE(ComputedHigher(f.root).empty()) << f.root;
  }
}

TEST(WordsTest, Degree) {
  for (const Folded& f : Words(137)) {
    const GiNaC::symbol x("x");
    int64_t d = 1;
    for (const auto& b : f.blocks) d *= b.n;
    EXPECT_EQ(Composed(f, x).degree(x), d);
  }
}

TEST(WordsTest, Folds) {
  const GiNaC::symbol x("x");
  for (const Folded& f : Words(137)) {
    const int swan = SwanAtInfinity(Composed(f, x), x);
    EXPECT_GE(swan, 1);
    EXPECT_EQ(H1Dimension(Composed(f, x), x), swan - 1);
  }
}

TEST(WordsTest, Stats) {
  WordStats s;
  const std::vector<Folded> words = Words(1021, &s);
  EXPECT_EQ(s.words, static_cast<int64_t>(words.size()));
  EXPECT_GT(s.cone_nodes, 0);
  EXPECT_FALSE(s.capped);
}

TEST(WordsTest, Cap) {
  WordStats s;
  std::string err;
  WordOptions opt;
  opt.max_words = 2;
  int64_t seen = 0;
  ASSERT_TRUE(EnumerateWords(
      8191, ComputedMask, ComputedHigher, opt, [&](const Folded&) { ++seen; },
      &s, &err))
      << err;
  EXPECT_EQ(seen, 2);
  EXPECT_TRUE(s.capped);
}

TEST(WordsTest, FaithfulRefused) {
  WordOptions opt;
  opt.lift = Lift::kFaithful;
  WordStats s;
  std::string err;
  EXPECT_FALSE(EnumerateWords(
      29, ComputedMask, ComputedHigher, opt, [](const Folded&) {}, &s, &err));
  EXPECT_NE(err.find("faithful"), std::string::npos);
}

}  // namespace
