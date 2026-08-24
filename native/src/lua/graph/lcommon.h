#pragma once

#include <vector>

#include <sol/sol.hpp>

extern "C" {
#include <lua.h>
}

#include "primeparts/lua/graph/bigint.h"
#include "primeparts/lua/graph/parts.h"
#include "primeparts/lua/graph/poly.h"
#include "primeparts/lua/graph/twists.h"

namespace primeparts::graph::lua {

Oracle& SharedOracle();

sol::object PushBig(sol::state_view lua, const BigInt& v);
sol::table CoeffTable(sol::state_view lua, const std::vector<Coeff>& coeffs);
sol::table EdgeTable(sol::state_view lua, const std::vector<Edge>& edges);

sol::table Submodule(lua_State* L, const char* name, lua_CFunction opener);
void SetCall(sol::state_view lua, sol::table target, const sol::object& fn);

}  // namespace primeparts::graph::lua

extern "C" {
int luaopen_graph_parts(lua_State* L);
int luaopen_graph_poly(lua_State* L);
int luaopen_graph_expr(lua_State* L);
int luaopen_graph_twists(lua_State* L);
int luaopen_graph_walk(lua_State* L);
}
