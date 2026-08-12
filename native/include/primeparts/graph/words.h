#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "primeparts/graph/chain_forms.h"
#include "primeparts/graph/flat_cone.h"

namespace primeparts::graph {

struct HigherEdge {
  int32_t m = 0;
  int32_t n = 2;
  int64_t q = 0;
};

using HigherFn = std::function<std::vector<HigherEdge>(int64_t)>;

std::vector<HigherEdge> ComputedHigher(int64_t p);

struct WordOptions {
  Lift lift = Lift::kCanonical;
  int64_t max_words = 1000000;
  int max_blocks = 16;
};

struct WordStats {
  int64_t words = 0;
  int64_t cone_nodes = 0;
  int64_t higher_edges = 0;
  bool capped = false;
};

bool EnumerateWords(int64_t p, const MaskFn& mask, const HigherFn& higher,
                    const WordOptions& options,
                    const std::function<void(const Folded&)>& emit,
                    WordStats* stats, std::string* error);

std::string WordKey(const Folded& f);

}  // namespace primeparts::graph
