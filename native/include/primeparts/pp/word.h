#pragma once

#include <ginac/ginac.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "primeparts/pp/base.h"
#include "primeparts/pp/cone.h"

namespace primeparts::pp {

struct Step {
  int32_t m = 0;
  int32_t n = 1;
};

struct Route {
  int64_t root = 0;
  int64_t target = 0;
  std::vector<Step> steps;
};

struct Block {
  int32_t n = 2;
  GiNaC::ex c;
};

struct Word {
  int64_t root = 0;
  int64_t target = 0;
  GiNaC::ex a0;
  std::vector<Block> blocks;
};

enum class Lift {
  kFaithful,
  kCanonical,
};

Word Fold(const Route& r, Lift lift);

std::vector<int32_t> Skeleton(const Word& w);

std::vector<GiNaC::ex> Levels(const Word& w, const GiNaC::symbol& x);

GiNaC::ex Composed(const Word& w, const GiNaC::symbol& x);

bool Closes(const Word& w);

int64_t Degree(const Word& w);

int64_t Degree(const Route& r);

std::string Key(const Word& w);

struct WordOptions {
  Lift lift = Lift::kCanonical;
  int64_t max_words = 1000000;
  int max_blocks = 16;
};

struct WordStats {
  int64_t words = 0;
  int64_t cone_nodes = 0;
  int64_t ascents = 0;
  bool capped = false;
};

bool Enumerate(int64_t p, const MaskFn& mask, const HigherFn& higher,
               const WordOptions& options,
               const std::function<void(const Word&)>& emit, WordStats* stats,
               std::string* error);

}  // namespace primeparts::pp
