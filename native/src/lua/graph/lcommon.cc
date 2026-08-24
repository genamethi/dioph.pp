#include "lcommon.h"

#include <memory>

extern "C" {
#include <lauxlib.h>
}

namespace primeparts::graph::lua {

Oracle& SharedOracle() {
  static std::unique_ptr<Oracle> oracle =
      MakeCachedOracle(MakeDynamicOracle(), 1 << 20);
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

sol::table EdgeTable(sol::state_view lua, const std::vector<Edge>& edges) {
  sol::table out = lua.create_table(static_cast<int>(edges.size()), 0);
  int idx = 1;
  for (const Edge& e : edges) {
    sol::table row = lua.create_table(0, 4);
    row["p"] = e.p;
    row["m"] = e.m;
    row["n"] = e.n;
    row["q"] = e.q;
    out[idx++] = row;
  }
  return out;
}

sol::table Submodule(lua_State* L, const char* name, lua_CFunction opener) {
  luaL_requiref(L, name, opener, 0);
  sol::table out = sol::stack::get<sol::table>(L, -1);
  lua_pop(L, 1);
  return out;
}

void SetCall(sol::state_view lua, sol::table target, const sol::object& fn) {
  sol::table meta = lua.create_table(0, 1);
  meta[sol::meta_function::call] = fn;
  target[sol::metatable_key] = meta;
}

}  // namespace primeparts::graph::lua
