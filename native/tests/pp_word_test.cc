#include "primeparts/pp/word.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace {

using primeparts::pp::Base;
using primeparts::pp::Closes;
using primeparts::pp::Composed;
using primeparts::pp::ComputedHigher;
using primeparts::pp::ComputedMask;
using primeparts::pp::Degree;
using primeparts::pp::Enumerate;
using primeparts::pp::Evaluate;
using primeparts::pp::Fold;
using primeparts::pp::FromMask;
using primeparts::pp::Key;
using primeparts::pp::Levels;
using primeparts::pp::Lift;
using primeparts::pp::Route;
using primeparts::pp::Skeleton;
using primeparts::pp::Step;
using primeparts::pp::ToMask;
using primeparts::pp::Word;
using primeparts::pp::WordOptions;
using primeparts::pp::WordStats;

std::vector<Word> Words(int64_t p, WordStats* stats = nullptr) {
  std::vector<Word> out;
  WordStats local;
  std::string err;
  EXPECT_TRUE(Enumerate(
      p, ComputedMask, ComputedHigher, WordOptions{},
      [&](const Word& w) { out.push_back(w); }, stats ? stats : &local, &err))
      << err;
  return out;
}

TEST(PpBaseTest, Roundtrip) {
  for (uint64_t m : {uint64_t{0}, uint64_t{1}, uint64_t{0b101101},
                     uint64_t{1} << 63, ~uint64_t{0}}) {
    uint64_t back = 0;
    ASSERT_TRUE(ToMask(FromMask(m), &back)) << m;
    EXPECT_EQ(back, m);
  }
}

TEST(PpBaseTest, Evaluation) {
  for (uint64_t m :
       {uint64_t{0}, uint64_t{1}, uint64_t{0b101101}, uint64_t{0xdeadbeef}}) {
    EXPECT_TRUE(Evaluate(FromMask(m)).is_equal(GiNaC::numeric(m))) << m;
  }
}

TEST(PpBaseTest, Rejects) {
  uint64_t back = 0;
  EXPECT_FALSE(ToMask(2 * GiNaC::pow(Base(), 3), &back));
  EXPECT_FALSE(ToMask(GiNaC::pow(Base(), 64), &back));
}

TEST(PpWordTest, Fold) {
  Route r;
  r.root = 3;
  r.target = 31;
  r.steps = {Step{1, 3}, Step{1, 1}};

  const Word w = Fold(r, Lift::kCanonical);
  EXPECT_EQ(Skeleton(w), (std::vector<int32_t>{3}));
  EXPECT_EQ(Degree(w), 3);
  EXPECT_EQ(Degree(r), 3);
  EXPECT_TRUE(Closes(w));
}

TEST(PpWordTest, Lifts) {
  Route r;
  r.root = 3;
  r.target = 51;
  r.steps = {Step{1, 1}, Step{1, 1}, Step{1, 2}};

  const Word canonical = Fold(r, Lift::kCanonical);
  const Word faithful = Fold(r, Lift::kFaithful);
  EXPECT_EQ(Skeleton(canonical), Skeleton(faithful));
  EXPECT_FALSE(canonical.a0.is_equal(faithful.a0));
  EXPECT_TRUE(Evaluate(canonical.a0).is_equal(Evaluate(faithful.a0)));
  EXPECT_TRUE(Closes(canonical));
  EXPECT_TRUE(Closes(faithful));
}

TEST(PpWordTest, Closure) {
  for (int64_t p : {29, 137, 251, 8191}) {
    const std::vector<Word> words = Words(p);
    ASSERT_FALSE(words.empty()) << p;
    for (const Word& w : words) {
      EXPECT_TRUE(Closes(w)) << "p=" << p << " root=" << w.root;
      EXPECT_EQ(w.target, p);
    }
  }
}

TEST(PpWordTest, Distinct) {
  for (int64_t p : {29, 137, 1021}) {
    std::set<std::string> keys;
    for (const Word& w : Words(p)) {
      EXPECT_TRUE(keys.insert(Key(w)).second) << p;
    }
  }
}

TEST(PpWordTest, Anchor) {
  const std::vector<Word> words = Words(29);
  EXPECT_GE(words.size(), 4u);

  bool flat = false;
  bool cube = false;
  for (const Word& w : words) {
    if (w.blocks.empty()) flat = true;
    if (Skeleton(w) == std::vector<int32_t>{3}) cube = true;
  }
  EXPECT_TRUE(flat);
  EXPECT_TRUE(cube);
}

TEST(PpWordTest, Roots) {
  for (const Word& w : Words(251)) {
    EXPECT_EQ(ComputedMask(w.root), uint64_t{0}) << w.root;
    EXPECT_TRUE(ComputedHigher(w.root).empty()) << w.root;
  }
}

TEST(PpWordTest, Degree) {
  const GiNaC::symbol x("x");
  for (const Word& w : Words(137)) {
    EXPECT_EQ(Composed(w, x).degree(x), Degree(w));
    EXPECT_EQ(Levels(w, x).size(), w.blocks.size() + 1);
  }
}

TEST(PpWordTest, Stats) {
  WordStats s;
  const std::vector<Word> words = Words(1021, &s);
  EXPECT_EQ(s.words, static_cast<int64_t>(words.size()));
  EXPECT_GT(s.cone_nodes, 0);
  EXPECT_FALSE(s.capped);
}

TEST(PpWordTest, Cap) {
  WordStats s;
  std::string err;
  WordOptions opt;
  opt.max_words = 2;
  int64_t seen = 0;
  ASSERT_TRUE(Enumerate(
      8191, ComputedMask, ComputedHigher, opt, [&](const Word&) { ++seen; }, &s,
      &err))
      << err;
  EXPECT_EQ(seen, 2);
  EXPECT_TRUE(s.capped);
}

TEST(PpWordTest, FaithfulRefused) {
  WordOptions opt;
  opt.lift = Lift::kFaithful;
  WordStats s;
  std::string err;
  EXPECT_FALSE(Enumerate(
      29, ComputedMask, ComputedHigher, opt, [](const Word&) {}, &s, &err));
  EXPECT_NE(err.find("faithful"), std::string::npos);
}

}  // namespace
