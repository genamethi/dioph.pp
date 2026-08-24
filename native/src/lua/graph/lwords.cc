#include "lcommon.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "primeparts/lua/graph/word.h"
#include "primeparts/lua/lppconv.h"

namespace {

using primeparts::graph::Block;
using primeparts::graph::Depth;
using primeparts::graph::Expr;
using primeparts::graph::Word;
using primeparts::graph::Words;
using primeparts::graph::lua::SetCall;
using primeparts::graph::lua::SharedOracle;
using primeparts::lua::Fail;
using primeparts::lua::IntOr;
using primeparts::lua::ReqInt;
using primeparts::lua::Spec;
using primeparts::lua::StrOr;

constexpr int64_t kDefaultCeiling = 64000000;

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

Words Of(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.words.of";
  const sol::table spec = Spec(ts, arg);
  const int64_t p = ReqInt(spec, "p", kFn);

  primeparts::graph::WordOpts opts;
  opts.depth = static_cast<int32_t>(IntOr(spec, "depth", -1));
  opts.max_cells = IntOr(spec, "max_cells", kDefaultCeiling);
  sol::optional<sol::table> arr = spec["targets"];
  if (arr) {
    for (std::size_t i = 1; i <= arr->size(); ++i) {
      if (sol::optional<int64_t> v = (*arr)[i]) {
        opts.targets.push_back(*v);
        continue;
      }
      if (sol::optional<sol::table> row = (*arr)[i]) {
        if (sol::optional<int64_t> v = (*row)["p"]) opts.targets.push_back(*v);
      }
    }
  }

  Words out;
  std::string error;
  if (!primeparts::graph::WordsOf(SharedOracle(), p, opts, &out, &error)) {
    Fail(kFn, error);
  }
  return out;
}

sol::table Rows(const Words& self, sol::optional<std::string> symbol,
                sol::optional<std::string> base, sol::this_state ts) {
  sol::state_view lua(ts);
  const std::string sym = symbol.value_or("x");
  const std::string bas = base.value_or("");
  sol::table out = lua.create_table(static_cast<int>(self.Size()), 0);
  int idx = 1;
  for (const auto& [word, counts] : self.All()) {
    sol::table row = lua.create_table(0, 6);
    row["word"] = WordTable(lua, word);
    row["terminal"] = word.terminal;
    row["degree"] = word.Degree();
    row["expr"] = primeparts::graph::WordForm(word, sym, bas);
    int64_t paths = 0;
    sol::table by = lua.create_table(static_cast<int>(counts.size()), 0);
    for (std::size_t d = 0; d < counts.size(); ++d) {
      paths += counts[d];
      by[static_cast<int>(d) + 1] = counts[d];
    }
    row["paths"] = paths;
    row["by_depth"] = by;
    out[idx++] = row;
  }
  return out;
}

void Bind(sol::table& words) {
  words.new_usertype<Words>(
      "Words", sol::no_constructor,
      "known", [](const Words& self) {
        return self.Complete() ? int64_t{-1} : static_cast<int64_t>(self.Known());
      },
      "complete", &Words::Complete,
      "count", &Words::Count,
      "size", &Words::Size,
      "cells", &Words::Cells,
      "grade", &Words::Grade,
      "cut", &Words::Cut,
      "step", &Words::Step,
      "rows", &Rows,
      sol::meta_function::addition, &Words::Add);
}

}  // namespace

extern "C" int luaopen_graph_words(lua_State* L) {
  primeparts::graph::lua::Submodule(L, "graph.expr", &luaopen_graph_expr);
  primeparts::graph::lua::Submodule(L, "graph.depth", &luaopen_graph_depth);

  sol::state_view lua(L);
  sol::table words = lua.create_table();
  words.set_function("of", &Of);
  words.set_function("root", &Words::Of);
  Bind(words);
  SetCall(lua, words,
          sol::make_object(lua, [](sol::table, sol::this_state ts,
                                   sol::optional<sol::table> arg) {
            return Of(ts, arg);
          }));
  words.push();
  return 1;
}
