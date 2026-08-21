#include "primeparts/lua/lconf.h"

#include <cstddef>
#include <string>

#include <sol/sol.hpp>

#include "primeparts/config.h"
#include "primeparts/lua/lppconv.h"

namespace {

sol::table Section(sol::state_view lua, sol::table root, const char* name) {
  sol::optional<sol::table> existing = root[name];
  if (existing) return *existing;
  sol::table created = lua.create_table();
  root[name] = created;
  return created;
}

}  // namespace

extern "C" int luaopen_conf(lua_State *L) {
  sol::state_view lua(L);
  sol::table conf = lua.create_table();

  primeparts::config::Conf& c =
      const_cast<primeparts::config::Conf&>(primeparts::lua::Conf());

  conf["path"] = c.path.string();
  conf["touched"] = c.touched;
  conf["generated"] = c.generated;

  sol::table defaulted = lua.create_table(static_cast<int>(c.defaulted.size()), 0);
  int idx = 1;
  for (const std::string& name : c.defaulted) defaulted[idx++] = name;
  conf["defaulted"] = defaulted;

  for (std::size_t i = 0; i < primeparts::config::kFieldCount; ++i) {
    const primeparts::config::Field& f = primeparts::config::kFields[i];
    sol::table section = Section(lua, conf, f.section);
    switch (f.kind) {
      case primeparts::config::Field::kStr:
        section[f.key] = *f.str(c);
        break;
      case primeparts::config::Field::kI64:
        section[f.key] = *f.num(c);
        break;
      case primeparts::config::Field::kBool:
        section[f.key] = *f.flag(c);
        break;
    }
  }

  conf.push();
  return 1;
}
