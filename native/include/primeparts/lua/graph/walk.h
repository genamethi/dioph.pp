#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "primeparts/lua/graph/expr.h"
#include "primeparts/lua/graph/parts.h"
#include "primeparts/lua/graph/twists.h"
#include "primeparts/lua/graph/word.h"

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
};

bool Reach(Oracle& oracle, int64_t p, int32_t depth, int64_t max_nodes,
           const TwistTable* twists, Frontier* out, std::string* error);

struct Chain {
  std::vector<Edge> edges;
  int64_t terminal = 0;
  int32_t twist_count = 0;
  int64_t degree = 1;
};

Word WordOf(const Chain& chain);

struct ChainSet {
  std::vector<Chain> chains;
  int64_t oracle_calls = 0;
};

struct ChainOpts {
  int32_t depth = 8;
  bool sources_only = false;
  std::vector<int64_t> targets;
};

bool Chains(Oracle& oracle, int64_t p, const ChainOpts& opts, ChainSet* out,
            std::string* error);

Expr ChainForm(const std::vector<Edge>& edges, const std::string& symbol);

struct SkelOpts {
  std::vector<int32_t> skeleton;
  int64_t hi = 0;
  std::vector<int64_t> roots;
};

struct SkelHit {
  Word word;
  int64_t p = 0;
};

struct SkelSet {
  std::vector<SkelHit> hits;
  std::vector<int64_t> roots;
  std::vector<int64_t> bounds;
  int64_t nodes = 0;
};

bool Skeleton(const SkelOpts& opts, SkelSet* out, std::string* error);

}  // namespace primeparts::graph
