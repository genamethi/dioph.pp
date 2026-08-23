#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "primeparts/graph/expr.h"
#include "primeparts/graph/parts.h"
#include "primeparts/graph/twists.h"

namespace primeparts::graph {

struct Level {
  int32_t depth = 0;
  std::vector<int64_t> nodes;
};

struct TwistHit {
  int64_t p = 0;
  int32_t depth = 0;
};

struct Frontier {
  std::vector<Level> levels;
  std::vector<TwistHit> twist_hits;
  int64_t node_count = 0;
  int64_t oracle_calls = 0;
  int32_t reached_depth = 0;
  bool complete = false;
};

bool Reach(Oracle& oracle, int64_t p, int32_t depth, int64_t max_nodes,
           const TwistTable* twists, Frontier* out, std::string* error);

struct Chain {
  std::vector<Part> edges;
  int64_t terminal = 0;
  int32_t twist_count = 0;
  int64_t degree = 1;
};

struct ChainSet {
  std::vector<Chain> chains;
  bool truncated = false;
  int64_t oracle_calls = 0;
};

bool Chains(Oracle& oracle, int64_t p, int32_t depth, int64_t limit,
            bool sources_only, ChainSet* out, std::string* error);

Expr ChainForm(const std::vector<Part>& edges, const std::string& symbol);

}  // namespace primeparts::graph
