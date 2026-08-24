#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "primeparts/lua/graph/depth.h"
#include "primeparts/lua/graph/expr.h"
#include "primeparts/lua/graph/parts.h"

namespace primeparts::graph {

struct Block {
  int32_t n = 0;
  int64_t c = 0;
};

struct Word {
  int64_t a0 = 0;
  std::vector<Block> blocks;
  int64_t terminal = 0;

  int64_t Degree() const;
  int32_t Grade() const;
  int64_t OddDegree() const;
  std::string Key() const;

  Word Step(int32_t m, int32_t n) const;
};

bool operator<(const Word& a, const Word& b);

Expr WordForm(const Word& word, const std::string& symbol);

class Words {
 public:
  static Words Of(int64_t root);

  Words Step(int32_t m, int32_t n) const;
  Words Add(const Words& other) const;
  Words Cut(int32_t d) const;

  int32_t Known() const { return known_; }
  bool Complete() const { return known_ == Depth::kComplete; }

  int64_t Count() const;
  int64_t Size() const { return static_cast<int64_t>(by_word_.size()); }
  int64_t Cells() const;
  Depth Grade() const;

  const std::map<Word, std::vector<int64_t>>& All() const { return by_word_; }

 private:
  std::map<Word, std::vector<int64_t>> by_word_;
  int32_t known_ = Depth::kComplete;
};

struct WordOpts {
  int32_t depth = -1;
  int64_t max_cells = 0;
  std::vector<int64_t> targets;
};

bool WordsOf(Oracle& oracle, int64_t p, const WordOpts& opts, Words* out,
             std::string* error);

}  // namespace primeparts::graph
