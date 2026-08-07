#include "primeparts/config.h"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <unistd.h>

#include <lua.hpp>

namespace primeparts::config {

namespace {

fs::path ExeDir() {
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  return fs::path(buf).parent_path();
}

fs::path XdgConfig() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return fs::path(xdg) / "primeparts" / "config.lua";
  return {};
}

fs::path HomeConfig() {
  if (const char* home = std::getenv("HOME"); home && *home)
    return fs::path(home) / ".config" / "primeparts" / "config.lua";
  return {};
}

bool Exists(const fs::path& p) {
  if (p.empty()) return false;
  std::error_code ec;
  return fs::exists(p, ec);
}

bool WriteExample(const fs::path& path, std::string* error) {
  std::error_code ec;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot create " + path.string();
    return false;
  }
  f << kExampleConfig;
  if (!f) {
    if (error) *error = "write failed: " + path.string();
    return false;
  }
  return true;
}

std::string ExpandTilde(const std::string& value) {
  if (value.empty() || value[0] != '~') return value;
  const char* home = std::getenv("HOME");
  if (!home || !*home) return value;
  if (value.size() == 1) return home;
  if (value[1] != '/') return value;
  return std::string(home) + value.substr(1);
}

struct Reader {
  lua_State* L;
  const std::string& file;
  std::string* error;
  bool ok = true;

  bool Section(const char* name) {
    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      Fail(name, nullptr, "section missing");
      return false;
    }
    return true;
  }

  void EndSection() { lua_pop(L, 1); }

  void Fail(const char* section, const char* key, const char* what) {
    if (!ok) return;
    ok = false;
    if (error == nullptr) return;
    *error = file + ": conf." + section;
    if (key != nullptr) {
      *error += ".";
      *error += key;
    }
    *error += ": ";
    *error += what;
  }

  void Str(const char* section, const char* key, std::string* out) {
    lua_getfield(L, -1, key);
    if (lua_type(L, -1) != LUA_TSTRING) {
      Fail(section, key, lua_isnil(L, -1) ? "missing" : "expected a string");
    } else {
      *out = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
  }

  void Bool(const char* section, const char* key, bool* out) {
    lua_getfield(L, -1, key);
    if (!lua_isboolean(L, -1)) {
      Fail(section, key, lua_isnil(L, -1) ? "missing" : "expected a boolean");
    } else {
      *out = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
  }

  void I64(const char* section, const char* key, int64_t* out) {
    lua_getfield(L, -1, key);
    if (!lua_isinteger(L, -1)) {
      Fail(section, key, lua_isnil(L, -1) ? "missing" : "expected an integer");
    } else {
      *out = static_cast<int64_t>(lua_tointeger(L, -1));
    }
    lua_pop(L, 1);
  }
};

bool ReadConf(lua_State* L, const std::string& file, Conf* out,
              std::string* error) {
  Reader r{L, file, error};

  if (r.Section("core")) {
    r.Str("core", "rest_uri", &out->core.rest_uri);
    r.Str("core", "namespace", &out->core.ns_name);
    r.Str("core", "warehouse", &out->core.warehouse);
    out->core.warehouse = ExpandTilde(out->core.warehouse);
    r.EndSection();
  }
  if (r.Section("generate")) {
    r.I64("generate", "threads", &out->generate.threads);
    r.I64("generate", "chunk_primes", &out->generate.chunk_primes);
    r.EndSection();
  }
  if (r.Section("catalogd")) {
    r.Str("catalogd", "host", &out->catalogd.host);
    r.I64("catalogd", "port", &out->catalogd.port);
    r.Str("catalogd", "scan_planning_mode", &out->catalogd.scan_planning_mode);
    r.I64("catalogd", "plan_batch", &out->catalogd.plan_batch);
    r.I64("catalogd", "plan_ttl", &out->catalogd.plan_ttl);
    r.EndSection();
  }
  if (r.Section("verify")) {
    r.I64("verify", "threads", &out->verify.threads);
    r.I64("verify", "max_examples", &out->verify.max_examples);
    r.EndSection();
  }
  if (r.Section("query")) {
    r.I64("query", "limit", &out->query.limit);
    r.EndSection();
  }
  if (r.Section("graph")) {
    r.I64("graph", "threads", &out->graph.threads);
    r.I64("graph", "top", &out->graph.top);
    r.I64("graph", "max_p", &out->graph.max_p);
    r.Str("graph", "mode", &out->graph.mode);
    r.Str("graph", "format", &out->graph.format);
    r.EndSection();
  }
  if (r.Section("tui")) {
    r.I64("tui", "log_limit", &out->tui.log_limit);
    r.I64("tui", "default_limit", &out->tui.default_limit);
    r.Str("tui", "log_format", &out->tui.log_format);
    r.Bool("tui", "autosave", &out->tui.autosave);
    r.EndSection();
  }
  if (r.ok && out->catalogd.scan_planning_mode != "server" &&
      out->catalogd.scan_planning_mode != "client") {
    r.Fail("catalogd", "scan_planning_mode", "expected \"server\" or \"client\"");
  }
  if (r.ok && out->graph.mode != "basis" && out->graph.mode != "hasse" &&
      out->graph.mode != "compose" && out->graph.mode != "edges" &&
      out->graph.mode != "roots" && out->graph.mode != "paths" &&
      out->graph.mode != "spectrum") {
    r.Fail("graph", "mode",
           "expected basis|hasse|compose|edges|roots|paths|spectrum");
  }
  if (r.ok && out->graph.format != "text" && out->graph.format != "dot" &&
      out->graph.format != "json") {
    r.Fail("graph", "format", "expected text|dot|json");
  }
  if (r.ok && out->tui.log_format != "flat" && out->tui.log_format != "json") {
    r.Fail("tui", "log_format", "expected flat|json");
  }
  return r.ok;
}

}  // namespace

fs::path Resolve(const fs::path& requested, std::string* error) {
  if (!requested.empty()) {
    if (!Exists(requested)) {
      if (error) *error = "no such config: " + requested.string();
      return {};
    }
    return requested;
  }
  if (Exists("config.lua")) return "config.lua";
  if (const fs::path xdg = XdgConfig(); Exists(xdg)) return xdg;
  if (const fs::path home = HomeConfig(); Exists(home)) return home;

  const fs::path dir = ExeDir();
  if (dir.empty()) {
    if (error) *error = "cannot locate the running binary to seed a config";
    return {};
  }
  const fs::path seeded = dir / "config.lua";
  if (!Exists(seeded) && !WriteExample(seeded, error)) return {};
  return seeded;
}

bool Load(const fs::path& requested, Conf* out, std::string* error) {
  *out = Conf{};
  const bool seeding = requested.empty() && !Exists("config.lua") &&
                       !Exists(XdgConfig()) && !Exists(HomeConfig());
  out->path = Resolve(requested, error);
  if (out->path.empty()) return false;
  out->generated = seeding;

  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  const std::string file = out->path.string();
  if (luaL_dofile(L, file.c_str()) != LUA_OK) {
    if (error != nullptr) {
      const char* m = lua_tostring(L, -1);
      *error = m != nullptr ? m : (file + ": lua error");
    }
    lua_close(L);
    return false;
  }

  lua_getglobal(L, "touched");
  out->touched = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);

  if (!lua_istable(L, -1)) {
    lua_pop(L, lua_gettop(L));
    lua_getglobal(L, "conf");
  }
  if (!lua_istable(L, -1)) {
    if (error != nullptr) *error = file + ": no conf table";
    lua_close(L);
    return false;
  }

  const bool ok = ReadConf(L, file, out, error);
  lua_close(L);
  return ok;
}

void Announce(const Conf& conf) {
  if (conf.touched) return;
  std::fprintf(stderr, "%s: defaults in use; edit it and uncomment touched\n",
               conf.path.string().c_str());
}

}  // namespace primeparts::config
