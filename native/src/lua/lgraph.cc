#include "primeparts/lua/lgraph.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "primeparts/graph/expr.h"
#include "primeparts/graph/parts.h"
#include "primeparts/graph/poly.h"
#include "primeparts/graph/twists.h"
#include "primeparts/graph/walk.h"
#include "primeparts/lua/lppconv.h"

namespace {

using primeparts::graph::BigInt;
using primeparts::graph::Chain;
using primeparts::graph::ChainSet;
using primeparts::graph::Coeff;
using primeparts::graph::Expr;
using primeparts::graph::Frontier;
using primeparts::graph::Oracle;
using primeparts::graph::Part;
using primeparts::graph::Poly;
using primeparts::graph::TwistRow;
using primeparts::graph::TwistTable;
using primeparts::lua::BoolOr;
using primeparts::lua::Fail;
using primeparts::lua::IntOr;
using primeparts::lua::ReqInt;
using primeparts::lua::Spec;
using primeparts::lua::StrOr;

constexpr int64_t kDefaultDepth = 8;
constexpr int64_t kDefaultLimit = 1000;
constexpr int64_t kDefaultMaxNodes = 1000000;

Oracle& SharedOracle() {
  static std::unique_ptr<Oracle> oracle =
      primeparts::graph::MakeCachedOracle(primeparts::graph::MakeDynamicOracle(),
                                          1 << 20);
  return *oracle;
}

sol::object PushBig(sol::state_view lua, const BigInt& v) {
  if (v.fits) return sol::make_object(lua, v.value);
  return sol::make_object(lua, v.text);
}

sol::table CoeffTable(sol::state_view lua, const std::vector<Coeff>& coeffs) {
  sol::table out = lua.create_table(static_cast<int>(coeffs.size()), 0);
  int idx = 1;
  for (const Coeff& c : coeffs) {
    sol::table row = lua.create_table(0, 2);
    row["i"] = c.index;
    row["c"] = PushBig(lua, c.value);
    out[idx++] = row;
  }
  return out;
}


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

sol::table WordTable(sol::state_view lua, const primeparts::graph::Word& w) {
  sol::table out = lua.create_table(0, 6);
  out["a0"] = w.a0;
  out["terminal"] = w.terminal;
  out["degree"] = w.Degree();
  out["grade"] = w.Grade();
  out["odd_degree"] = w.OddDegree();
  sol::table blocks = lua.create_table(static_cast<int>(w.blocks.size()), 0);
  sol::table skel = lua.create_table(static_cast<int>(w.blocks.size()), 0);
  int idx = 1;
  for (const primeparts::graph::Block& b : w.blocks) {
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

sol::table PartTable(sol::state_view lua, const std::vector<Part>& parts) {
  sol::table out = lua.create_table(static_cast<int>(parts.size()), 0);
  int idx = 1;
  for (const Part& p : parts) {
    sol::table row = lua.create_table(0, 3);
    row["m"] = p.m;
    row["n"] = p.n;
    row["q"] = p.q;
    out[idx++] = row;
  }
  return out;
}

sol::table Parts(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.parts";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const int64_t p = ReqInt(spec, "p", kFn);

  std::vector<Part> parts;
  std::string error;
  if (!SharedOracle().Parts(p, &parts, &error)) Fail(kFn, error);

  sol::table out = lua.create_table();
  out["p"] = p;
  out["k"] = static_cast<int64_t>(parts.size());
  out["partitions"] = PartTable(lua, parts);
  return out;
}

sol::table Reach(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.reach";
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

bool CollectChains(const sol::table& spec, const char* fn, int64_t* p,
                   ChainSet* set, std::string* symbol) {
  *p = ReqInt(spec, "p", fn);
  primeparts::graph::ChainOpts opts;
  opts.depth = static_cast<int32_t>(IntOr(spec, "depth", kDefaultDepth));
  opts.limit = IntOr(spec, "limit", kDefaultLimit);
  opts.sources_only = BoolOr(spec, "sources", false);
  opts.targets = IdArray(spec, "targets");
  *symbol = StrOr(spec, "symbol", "x");
  std::string error;
  if (!primeparts::graph::Chains(SharedOracle(), *p, opts, set, &error)) {
    Fail(fn, error);
  }
  return true;
}

sol::table Chains(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.chains";
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
  static const char kFn[] = "graph.forms";
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
    const primeparts::graph::Word word = primeparts::graph::WordOf(chain);
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
    sol::table row = lua.create_table(0, 6);
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

std::shared_ptr<TwistTable> Twists(sol::this_state ts,
                                   sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.twists";
  const sol::table spec = Spec(ts, arg);
  std::string error;

  sol::optional<sol::table> rows = spec["rows"];
  if (rows) {
    std::vector<primeparts::graph::TwistRow> parsed;
    int64_t hi = IntOr(spec, "hi", 0);
    for (std::size_t i = 1; i <= rows->size(); ++i) {
      sol::optional<sol::table> row = (*rows)[i];
      if (!row) Fail(kFn, "rows[" + std::to_string(i) + "] is not a table");
      const int64_t rp = (*row)["p"].get_or(int64_t{0});
      const int64_t rn = (*row)["n"].get_or(int64_t{0});
      if (rn < 2) continue;
      parsed.push_back(primeparts::graph::TwistRow{
          .p = rp,
          .m = static_cast<int32_t>((*row)["m"].get_or(int64_t{0})),
          .n = static_cast<int32_t>(rn),
          .q = (*row)["q"].get_or(int64_t{0})});
      if (rp > hi) hi = rp;
    }
    std::unique_ptr<TwistTable> table =
        TwistTable::FromRows(std::move(parsed), hi, &error);
    if (!table) Fail(kFn, error);
    return std::shared_ptr<TwistTable>(table.release());
  }

  const int64_t hi = ReqInt(spec, "hi", kFn);
  std::unique_ptr<TwistTable> table = TwistTable::Build(hi, &error);
  if (!table) Fail(kFn, error);
  return std::shared_ptr<TwistTable>(table.release());
}

sol::table TwistRows(sol::state_view lua, const std::vector<TwistRow>& rows) {
  sol::table out = lua.create_table(static_cast<int>(rows.size()), 0);
  int idx = 1;
  for (const TwistRow& r : rows) {
    sol::table row = lua.create_table(0, 4);
    row["p"] = r.p;
    row["m"] = r.m;
    row["n"] = r.n;
    row["q"] = r.q;
    out[idx++] = row;
  }
  return out;
}

void BindPoly(sol::table& graph) {
  graph.new_usertype<Poly>(
      "Poly", sol::no_constructor,
      "degree", &Poly::Degree,
      "text", &Poly::Text,
      "mono", [](const Poly& self, sol::this_state ts) {
        return CoeffTable(sol::state_view(ts), self.Monomial());
      },
      "he", [](const Poly& self, sol::this_state ts) {
        return CoeffTable(sol::state_view(ts), self.Hermite());
      },
      "eval", [](const Poly& self, int64_t x, sol::this_state ts) {
        return PushBig(sol::state_view(ts), self.Eval(x));
      },
      "mod", [](const Poly& self, const std::string& n) {
        std::string error;
        Poly out = self.ModCoeffs(n, &error);
        if (!error.empty()) Fail("Poly:mod", error);
        return out;
      },
      sol::meta_function::addition, &Poly::Add,
      sol::meta_function::subtraction, &Poly::Sub,
      sol::meta_function::multiplication, &Poly::Mul,
      sol::meta_function::equal_to, &Poly::Equals,
      sol::meta_function::to_string, &Poly::Text);
}

void BindExpr(sol::table& graph) {
  graph.new_usertype<Expr>(
      "Expr", sol::no_constructor,
      "text", &Expr::Text,
      "expand", &Expr::Expand,
      "symbols", [](const Expr& self, sol::this_state ts) {
        sol::state_view lua(ts);
        const std::vector<std::string> names = self.Symbols();
        sol::table out = lua.create_table(static_cast<int>(names.size()), 0);
        int idx = 1;
        for (const std::string& n : names) out[idx++] = n;
        return out;
      },
      "degree", &Expr::Degree,
      "subs", [](const Expr& self, const std::string& name, int64_t v) {
        return self.Subs(name, v);
      },
      "value", [](const Expr& self, sol::this_state ts) {
        std::string error;
        BigInt v = self.Value(&error);
        if (!error.empty()) Fail("Expr:value", error);
        return PushBig(sol::state_view(ts), v);
      },
      "poly", [](const Expr& self, const std::string& name) {
        Poly out;
        std::string error;
        if (!self.ToPoly(name, &out, &error)) Fail("Expr:poly", error);
        return out;
      },
      "he", [](const Expr& self, const std::string& name, sol::this_state ts) {
        Poly p;
        std::string error;
        if (!self.ToPoly(name, &p, &error)) Fail("Expr:he", error);
        return CoeffTable(sol::state_view(ts), p.Hermite());
      },
      sol::meta_function::addition, &Expr::Add,
      sol::meta_function::subtraction, &Expr::Sub,
      sol::meta_function::multiplication, &Expr::Mul,
      sol::meta_function::power_of, &Expr::Pow,
      sol::meta_function::to_string, &Expr::Text);
}

void BindTwists(sol::table& graph) {
  graph.new_usertype<TwistTable>(
      "TwistTable", sol::no_constructor,
      "hi", &TwistTable::Hi,
      "origin", [](const TwistTable& self) { return std::string(self.Origin()); },
      "count", [](const TwistTable& self) {
        return static_cast<int64_t>(self.Count());
      },
      "has", &TwistTable::Has,
      "at", [](const TwistTable& self, int64_t p, sol::this_state ts) {
        return TwistRows(sol::state_view(ts), self.At(p));
      },
      "into", [](const TwistTable& self, int64_t q, sol::this_state ts) {
        return TwistRows(sol::state_view(ts), self.Into(q));
      },
      "rows", [](const TwistTable& self, sol::optional<int64_t> limit,
                 sol::this_state ts) {
        const std::vector<TwistRow>& all = self.Rows();
        const std::size_t n =
            limit && *limit > 0
                ? std::min(all.size(), static_cast<std::size_t>(*limit))
                : all.size();
        return TwistRows(sol::state_view(ts),
                         std::vector<TwistRow>(all.begin(), all.begin() + n));
      });
}

}  // namespace

extern "C" int luaopen_graph(lua_State *L) {
  sol::state_view lua(L);
  sol::table graph = lua.create_table();
  graph.set_function("parts", &Parts);
  graph.set_function("reach", &Reach);
  graph.set_function("chains", &Chains);
  graph.set_function("forms", &Forms);
  graph.set_function("twists", &Twists);
  graph.set_function("sym", &Expr::Symbol);
  graph.set_function("int", &Expr::Int);
  graph.set_function("pow2", &Expr::PowerOfTwo);
  graph.set_function("he", &Poly::He);
  BindPoly(graph);
  BindExpr(graph);
  BindTwists(graph);
  graph.push();
  return 1;
}
