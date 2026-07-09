#include "primeparts/config.h"

#include <cstdlib>
#include <system_error>

#include <lua.hpp>  // liblua 5.5 (extern "C" wrapper)

namespace primeparts::config {

namespace {

// Lua: config({ k = v, ... }) -> collect into the map carried as an upvalue.
int config_cfn(lua_State* L) {
  auto* sink = static_cast<std::map<std::string, std::string>*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  luaL_checktype(L, 1, LUA_TTABLE);
  if (sink == nullptr) return 0;
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {  // table at index 1; key -2, value -1
    if (lua_type(L, -2) == LUA_TSTRING) {
      std::string key = lua_tostring(L, -2);
      std::string val;
      switch (lua_type(L, -1)) {
        case LUA_TBOOLEAN: val = lua_toboolean(L, -1) ? "true" : "false"; break;
        case LUA_TNUMBER:  val = std::to_string((long long)lua_tointeger(L, -1)); break;
        case LUA_TSTRING:  val = lua_tostring(L, -1); break;
        default: break;
      }
      (*sink)[key] = val;
    }
    lua_pop(L, 1);
  }
  return 0;
}

}  // namespace

fs::path ConfigFilePath() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return fs::path(xdg) / "primeparts" / "config.lua";
  if (const char* home = std::getenv("HOME"); home && *home)
    return fs::path(home) / ".config" / "primeparts" / "config.lua";
  return {};
}

std::map<std::string, std::string> Load(std::string* error) {
  std::map<std::string, std::string> kv;
  fs::path path = ConfigFilePath();
  std::error_code ec;
  if (path.empty() || !fs::exists(path, ec)) return kv;

  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  lua_pushlightuserdata(L, &kv);
  lua_pushcclosure(L, config_cfn, 1);
  lua_setglobal(L, "config");
  if (luaL_dofile(L, path.string().c_str()) != LUA_OK) {
    if (error != nullptr) {
      const char* m = lua_tostring(L, -1);
      *error = m != nullptr ? m : "lua error";
    }
  }
  lua_close(L);
  return kv;
}

}  // namespace primeparts::config
