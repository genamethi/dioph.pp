#include "primeparts/lua/lgraph.h"

#include <sol/sol.hpp>

#include "graph/lcommon.h"

extern "C" int luaopen_graph(lua_State *L) {
  using primeparts::graph::lua::Submodule;

  sol::state_view lua(L);
  sol::table graph = lua.create_table();
  graph["parts"] = Submodule(L, "graph.parts", &luaopen_graph_parts);
  graph["poly"] = Submodule(L, "graph.poly", &luaopen_graph_poly);
  graph["expr"] = Submodule(L, "graph.expr", &luaopen_graph_expr);
  graph["twists"] = Submodule(L, "graph.twists", &luaopen_graph_twists);
  graph["walk"] = Submodule(L, "graph.walk", &luaopen_graph_walk);
  graph.push();
  return 1;
}
