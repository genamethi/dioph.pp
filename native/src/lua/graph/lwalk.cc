#include "lcommon.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "primeparts/lua/graph/expr.h"
#include "primeparts/lua/graph/walk.h"
#include "primeparts/lua/lppconv.h"

namespace {

using primeparts::graph::Block;
using primeparts::graph::Chain;
using primeparts::graph::ChainOpts;
using primeparts::graph::ChainSet;
using primeparts::graph::Expr;
using primeparts::graph::Frontier;
using primeparts::graph::TwistTable;
using primeparts::graph::Word;
using primeparts::graph::lua::PartTable;
using primeparts::graph::lua::SharedOracle;
using primeparts::graph::lua::Submodule;
using primeparts::lua::BoolOr;
using primeparts::lua::Fail;
using primeparts::lua::IntOr;
using primeparts::lua::ReqInt;
using primeparts::lua::Spec;
using primeparts::lua::StrOr;

constexpr int64_t kDefaultDepth = 8;
constexpr int64_t kDefaultLimit = 1000;
constexpr int64_t kDefaultMaxNodes = 1000000;

std::vector<int64_t> IdArray(const sol::table& t, const char* key) {
  std::vector<int64_t> out;
  sol::optional<sol::table> arr = t[key];
  if (!arr) return out;
  for (std::size_t i = 1; i <= arr->size(); ++i) {
    if (sol::optional<int64_t> v = (*arr)[i]) {
      out.push_back(*v);
      continue;
    }
    if (sol::optional<sol::table> row = (*arr)[i]) {
      if (sol::optional<int64_t> v = (*row)["p"]) out.push_back(*v);
      else if (sol::optional<int64_t> v2 = (*row)["terminal"]) out.push_back(*v2);
    }
  }
  return out;
}

sol::table WordTable(sol::state_view lua, const Word& w) {
  sol::table out = lua.create_table(0, 7);
  out["a0"] = w.a0;
  out["terminal"] = w.terminal;
  out["degree"] = w.Degree();
  out["grade"] = w.Grade();
  out["odd_degree"] = w.OddDegree();
  sol::table blocks = lua.create_table(static_cast<int>(w.blocks.size()), 0);
  sol::table skel = lua.create_table(static_cast<int>(w.blocks.size()), 0);
  int idx = 1;
  for (const Block& b : w.blocks) {
    sol::table row = lua.create_table(0, 2);
    row["n"] = b.n;
    row["c"] = b.c;
    blocks[idx] = row;
    skel[idx] = b.n;
    idx++;
  }
  out["blocks"] = blocks;
  out["skeleton"] = skel;
  return out;
}

sol::table Reach(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.walk.reach";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const int64_t p = ReqInt(spec, "p", kFn);
  const auto depth = static_cast<int32_t>(IntOr(spec, "depth", kDefaultDepth));
  const int64_t max_nodes = IntOr(spec, "max_nodes", kDefaultMaxNodes);

  const TwistTable* twists = nullptr;
  sol::optional<std::shared_ptr<TwistTable>> tw = spec["twists"];
  if (tw && *tw) twists = tw->get();

  Frontier frontier;
  std::string error;
  if (!primeparts::graph::Reach(SharedOracle(), p, depth, max_nodes, twists,
                                &frontier, &error)) {
    Fail(kFn, error);
  }

  sol::table out = lua.create_table();
  sol::table levels = lua.create_table(static_cast<int>(frontier.levels.size()), 0);
  int li = 1;
  for (const auto& level : frontier.levels) {
    sol::table nodes = lua.create_table(static_cast<int>(level.nodes.size()), 0);
    int ni = 1;
    for (const int64_t node : level.nodes) nodes[ni++] = node;
    sol::table row = lua.create_table(0, 3);
    row["depth"] = level.depth;
    row["count"] = static_cast<int64_t>(level.nodes.size());
    row["nodes"] = nodes;
    levels[li++] = row;
  }
  out["levels"] = levels;

  sol::table hits = lua.create_table(static_cast<int>(frontier.twist_hits.size()), 0);
  int hi = 1;
  for (const auto& hit : frontier.twist_hits) {
    sol::table row = lua.create_table(0, 2);
    row["p"] = hit.p;
    row["depth"] = hit.depth;
    hits[hi++] = row;
  }
  out["twists"] = hits;
  out["nodes"] = frontier.node_count;
  out["calls"] = frontier.oracle_calls;
  out["depth"] = frontier.reached_depth;
  out["complete"] = frontier.complete;
  return out;
}

void CollectChains(const sol::table& spec, const char* fn, int64_t* p,
                   ChainSet* set, std::string* symbol) {
  *p = ReqInt(spec, "p", fn);
  ChainOpts opts;
  opts.depth = static_cast<int32_t>(IntOr(spec, "depth", kDefaultDepth));
  opts.limit = IntOr(spec, "limit", kDefaultLimit);
  opts.sources_only = BoolOr(spec, "sources", false);
  opts.targets = IdArray(spec, "targets");
  *symbol = StrOr(spec, "symbol", "x");
  std::string error;
  if (!primeparts::graph::Chains(SharedOracle(), *p, opts, set, &error)) {
    Fail(fn, error);
  }
}

sol::table Chains(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.walk.chains";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  int64_t p = 0;
  ChainSet set;
  std::string symbol;
  CollectChains(spec, kFn, &p, &set, &symbol);

  sol::table out = lua.create_table(static_cast<int>(set.chains.size()), 0);
  int idx = 1;
  for (const Chain& chain : set.chains) {
    sol::table row = lua.create_table(0, 5);
    row["terminal"] = chain.terminal;
    row["length"] = static_cast<int64_t>(chain.edges.size());
    row["twists"] = chain.twist_count;
    row["degree"] = chain.degree;
    row["edges"] = PartTable(lua, chain.edges);
    out[idx++] = row;
  }
  sol::table meta = lua.create_table();
  meta["truncated"] = set.truncated;
  meta["calls"] = set.oracle_calls;
  out["meta"] = meta;
  return out;
}

sol::table Forms(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.walk.forms";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  int64_t p = 0;
  ChainSet set;
  std::string symbol;
  CollectChains(spec, kFn, &p, &set, &symbol);

  const bool distinct = BoolOr(spec, "distinct", true);
  std::vector<std::pair<int64_t, std::string>> seen;
  std::vector<int64_t> seen_count;
  sol::table out = lua.create_table(static_cast<int>(set.chains.size()), 0);
  int idx = 1;
  for (const Chain& chain : set.chains) {
    Expr form = primeparts::graph::ChainForm(chain.edges, symbol);
    const Word word = primeparts::graph::WordOf(chain);
    const std::string key = word.Key();
    if (distinct) {
      bool dup = false;
      for (std::size_t s = 0; s < seen.size(); ++s) {
        if (seen[s].second == key) {
          seen_count[s]++;
          dup = true;
          break;
        }
      }
      if (dup) continue;
      seen.emplace_back(chain.terminal, key);
      seen_count.push_back(1);
    }
    sol::table row = lua.create_table(0, 8);
    row["terminal"] = chain.terminal;
    row["length"] = static_cast<int64_t>(chain.edges.size());
    row["twists"] = chain.twist_count;
    row["degree"] = chain.degree;
    row["edges"] = PartTable(lua, chain.edges);
    row["word"] = WordTable(lua, word);
    row["expr"] = form;
    row["exact"] = form.Subs(symbol, chain.terminal).EqualsInt(p);
    row["paths"] = 1;
    out[idx++] = row;
  }
  if (distinct) {
    for (std::size_t s = 0; s < seen_count.size(); ++s) {
      sol::optional<sol::table> row = out[static_cast<int>(s) + 1];
      if (row) (*row)["paths"] = seen_count[s];
    }
  }
  sol::table meta = lua.create_table();
  meta["truncated"] = set.truncated;
  meta["calls"] = set.oracle_calls;
  meta["symbol"] = symbol;
  out["meta"] = meta;
  return out;
}

}  // namespace

extern "C" int luaopen_graph_walk(lua_State* L) {
  Submodule(L, "graph.expr", &luaopen_graph_expr);
  Submodule(L, "graph.twists", &luaopen_graph_twists);

  sol::state_view lua(L);
  sol::table walk = lua.create_table();
  walk.set_function("reach", &Reach);
  walk.set_function("chains", &Chains);
  walk.set_function("forms", &Forms);
  walk.push();
  return 1;
}
