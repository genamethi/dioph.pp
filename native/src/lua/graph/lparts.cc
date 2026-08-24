#include "lcommon.h"

#include <cstdint>
#include <string>
#include <vector>

#include "primeparts/lua/lppconv.h"

namespace {

using primeparts::graph::Edge;
using primeparts::graph::lua::EdgeTable;
using primeparts::graph::lua::SetCall;
using primeparts::graph::lua::SharedOracle;
using primeparts::lua::Fail;
using primeparts::lua::ReqInt;
using primeparts::lua::Spec;

sol::table Of(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.parts.of";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const int64_t p = ReqInt(spec, "p", kFn);

  std::vector<Edge> parts;
  std::string error;
  if (!SharedOracle().Parts(p, &parts, &error)) Fail(kFn, error);

  sol::table out = lua.create_table();
  out["p"] = p;
  out["k"] = static_cast<int64_t>(parts.size());
  out["partitions"] = EdgeTable(lua, parts);
  return out;
}

int64_t Calls() { return SharedOracle().Calls(); }

std::string Source() { return SharedOracle().Name(); }

}  // namespace

extern "C" int luaopen_graph_parts(lua_State* L) {
  sol::state_view lua(L);
  sol::table parts = lua.create_table();
  parts.set_function("of", &Of);
  parts.set_function("calls", &Calls);
  parts.set_function("source", &Source);
  SetCall(lua, parts,
          sol::make_object(lua, [](sol::table, sol::this_state ts,
                                   sol::optional<sol::table> arg) {
            return Of(ts, arg);
          }));
  parts.push();
  return 1;
}
