#include "lcommon.h"

#include <cstdint>
#include <string>
#include <vector>

#include "primeparts/lua/graph/depth.h"
#include "primeparts/lua/lppconv.h"

namespace {

using primeparts::graph::Depth;
using primeparts::graph::lua::SetCall;
using primeparts::graph::lua::SharedOracle;
using primeparts::lua::Fail;
using primeparts::lua::IntOr;
using primeparts::lua::ReqInt;
using primeparts::lua::Spec;

constexpr int64_t kDefaultCeiling = 64000000;

sol::table Ints(sol::state_view lua, const std::vector<int64_t>& v) {
  sol::table out = lua.create_table(static_cast<int>(v.size()), 0);
  int idx = 1;
  for (const int64_t x : v) out[idx++] = x;
  return out;
}

Depth Of(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.depth.of";
  const sol::table spec = Spec(ts, arg);
  const int64_t p = ReqInt(spec, "p", kFn);
  const auto depth = static_cast<int32_t>(IntOr(spec, "depth", -1));
  const int64_t ceiling = IntOr(spec, "max_cells", kDefaultCeiling);

  Depth out;
  std::string error;
  if (!primeparts::graph::DepthOf(SharedOracle(), p, depth, ceiling, &out,
                                  &error)) {
    Fail(kFn, error);
  }
  return out;
}

void Bind(sol::table& depth) {
  depth.new_usertype<Depth>(
      "Depth", sol::no_constructor,
      "known", [](const Depth& self) {
        return self.Complete() ? int64_t{-1} : static_cast<int64_t>(self.Known());
      },
      "complete", &Depth::Complete,
      "roots", [](const Depth& self, sol::this_state ts) {
        return Ints(sol::state_view(ts), self.Roots());
      },
      "at", [](const Depth& self, int64_t root, sol::this_state ts) {
        sol::state_view lua(ts);
        const std::vector<int64_t>* c = self.At(root);
        if (c == nullptr) return sol::make_object(lua, sol::lua_nil);
        return sol::make_object(lua, Ints(lua, *c));
      },
      "min", &Depth::Min,
      "max", &Depth::Max,
      "count", &Depth::Count,
      "cells", &Depth::Cells,
      "cut", &Depth::Cut,
      "shift", &Depth::Shift,
      sol::meta_function::addition, &Depth::Add);
}

}  // namespace

extern "C" int luaopen_graph_depth(lua_State* L) {
  sol::state_view lua(L);
  sol::table depth = lua.create_table();
  depth.set_function("of", &Of);
  depth.set_function("root", &Depth::Of);
  Bind(depth);
  SetCall(lua, depth,
          sol::make_object(lua, [](sol::table, sol::this_state ts,
                                   sol::optional<sol::table> arg) {
            return Of(ts, arg);
          }));
  depth.push();
  return 1;
}
