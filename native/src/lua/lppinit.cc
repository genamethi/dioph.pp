#include "primeparts/lua/lppinit.h"

#include <string>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

#include "primeparts/config.h"
#include "primeparts/lua/lconf.h"
#include "primeparts/lua/lgraph.h"
#include "primeparts/lua/lnt.h"
#include "primeparts/lua/lppconv.h"
#include "primeparts/lua/lquery.h"
#include "primeparts/lua/lrestclient.h"

namespace {

std::string g_config_path;

const luaL_Reg kPpLibs[] = {
    {"conf", luaopen_conf},
    {"nt", luaopen_nt},
    {"query", luaopen_query},
    {"irc", luaopen_irc},
    {"graph", luaopen_graph},
    {nullptr, nullptr},
};

std::string LoadConf(lua_State *L) {
  primeparts::config::Conf conf;
  std::string error;
  if (!primeparts::config::Load(g_config_path, &conf, &error)) {
    return error.empty() ? "load failed" : error;
  }
  primeparts::config::Announce(conf);
  primeparts::config::SetSearchPath(L, conf);
  primeparts::lua::SetConf(std::move(conf));
  return {};
}

}  // namespace

extern "C" void pp_set_config_path(const char *path) {
  g_config_path = path != nullptr ? path : "";
}

extern "C" void pp_openlibs(lua_State *L) {
  luaL_openselectedlibs(L, ~0, 0);

  bool failed = false;
  {
    const std::string error = LoadConf(L);
    if (!error.empty()) {
      lua_pushfstring(L, "pp config: %s", error.c_str());
      failed = true;
    }
  }
  if (failed) lua_error(L);

  for (const luaL_Reg *lib = kPpLibs; lib->name != nullptr; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);
    lua_pop(L, 1);
  }
}
