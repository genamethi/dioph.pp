#include "primeparts/config.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
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
  f << RenderExample();
  if (!f) {
    if (error) *error = "write failed: " + path.string();
    return false;
  }
  return true;
}

const char kValidator[] = R"LUA(
local text = ...
local lpeg = require("lpeg")
local P, R, S, Cp = lpeg.P, lpeg.R, lpeg.S, lpeg.Cp

local space = S(" \t\r\n")
local comment = P("--") * (P(1) - P("\n"))^0
local ws = (space + comment)^0
local alpha = R("az", "AZ") + P("_")
local digit = R("09")
local name = alpha * (alpha + digit)^0
local number = S("+-")^-1 * digit^1 * (P(".") * digit^0)^-1
local dquote = P('"') * (P("\\") * P(1) + (1 - S('"\\')))^0 * P('"')
local squote = P("'") * (P("\\") * P(1) + (1 - S("'\\")))^0 * P("'")
local scalar = dquote + squote + P("true") + P("false") + number

local function commas(item)
  return (item * ws * (P(",") * ws * item * ws)^0 * (P(",") * ws)^-1)^-1
end

local field = name * ws * P("=") * ws * scalar
local section = name * ws * P("=") * ws * P("{") * ws * commas(field) * P("}")
local conftable = P("conf") * ws * P("=") * ws * P("{") * ws *
                  commas(section) * P("}")
local touched = P("touched") * ws * P("=") * ws * (P("true") + P("false"))
local guard = P("conf.") * name * ws * P("=") * ws * P("conf.") * name * ws *
              P("or") * ws * P("{") * ws * P("}")
local flat = P("conf.") * name * P(".") * name * ws * P("=") * ws * scalar
local stmt = conftable + touched + guard + flat
local grammar = ws * (stmt * ws)^0 * Cp()

local stop = grammar:match(text)
if stop and stop > #text then return true end
local line = 1
for _ in text:sub(1, stop or 1):gmatch("\n") do line = line + 1 end
return nil, "line " .. line .. ": not a permitted config construct"
)LUA";

std::vector<fs::path> LuaRoots() {
  const fs::path dir = ExeDir();
  if (dir.empty()) return {};
  return {dir / ".." / "share" / "pp" / "lua", dir / "lua"};
}

void SetRocksPath(lua_State* L) {
  std::string cpath;
  for (const fs::path& root : LuaRoots()) {
    if (!cpath.empty()) cpath += ";";
    cpath += (root / "rocks" / "lib" / "lua" /
              (LUA_VERSION_MAJOR "." LUA_VERSION_MINOR) / "?.so")
                 .string();
  }
  if (cpath.empty()) return;
  lua_getglobal(L, "package");
  lua_pushstring(L, cpath.c_str());
  lua_setfield(L, -2, "cpath");
  lua_pop(L, 1);
}

bool Validate(lua_State* L, const std::string& text, const std::string& file,
              std::string* error) {
  if (luaL_loadbuffer(L, kValidator, sizeof(kValidator) - 1, "=config-check") !=
      LUA_OK) {
    if (error != nullptr) *error = std::string("config check: ") +
                                   lua_tostring(L, -1);
    return false;
  }
  lua_pushlstring(L, text.data(), text.size());
  if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
    if (error != nullptr)
      *error = std::string("config check: ") + lua_tostring(L, -1);
    return false;
  }
  const bool ok = lua_toboolean(L, -2) != 0;
  if (!ok && error != nullptr) {
    const char* m = lua_tostring(L, -1);
    *error = file + ": " + (m != nullptr ? m : "rejected by config check");
  }
  lua_pop(L, 2);
  return ok;
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
};

bool Allowed(const Field& f, const std::string& value) {
  if (f.allowed == nullptr) return true;
  for (const char* const* p = f.allowed; *p != nullptr; ++p)
    if (value == *p) return true;
  return false;
}

std::string AllowedList(const Field& f) {
  std::string s;
  for (const char* const* p = f.allowed; *p != nullptr; ++p) {
    if (!s.empty()) s += "|";
    s += *p;
  }
  return s;
}

void ApplyDefault(const Field& f, Conf* out) {
  switch (f.kind) {
    case Field::kStr: *f.str(*out) = f.str_default; break;
    case Field::kI64: *f.num(*out) = f.num_default; break;
    case Field::kBool: *f.flag(*out) = f.flag_default; break;
  }
}

bool ReadField(Reader& r, const Field& f, Conf* out) {
  lua_State* L = r.L;
  lua_getfield(L, -1, f.key);
  const bool absent = lua_isnil(L, -1);
  if (absent) {
    lua_pop(L, 1);
    return false;
  }
  switch (f.kind) {
    case Field::kStr:
      if (lua_type(L, -1) != LUA_TSTRING)
        r.Fail(f.section, f.key, "expected a string");
      else
        *f.str(*out) = lua_tostring(L, -1);
      break;
    case Field::kI64:
      if (!lua_isinteger(L, -1))
        r.Fail(f.section, f.key, "expected an integer");
      else
        *f.num(*out) = static_cast<int64_t>(lua_tointeger(L, -1));
      break;
    case Field::kBool:
      if (!lua_isboolean(L, -1))
        r.Fail(f.section, f.key, "expected a boolean");
      else
        *f.flag(*out) = lua_toboolean(L, -1) != 0;
      break;
  }
  lua_pop(L, 1);
  return true;
}

bool ReadConf(lua_State* L, const std::string& file, Conf* out,
              std::string* error) {
  Reader r{L, file, error};
  const char* section = nullptr;
  bool open = false;

  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const Field& f = kFields[i];
    if (section == nullptr || std::strcmp(section, f.section) != 0) {
      if (open) r.EndSection();
      section = f.section;
      open = r.Section(f.section);
    }
    ApplyDefault(f, out);
    const bool present = open && ReadField(r, f, out);
    if (!r.ok) break;
    if (!present)
      out->defaulted.push_back(std::string(f.section) + "." + f.key);
    else if (f.kind == Field::kStr && !Allowed(f, *f.str(*out)))
      r.Fail(f.section, f.key, ("expected " + AllowedList(f)).c_str());
    if (!r.ok) break;
  }
  if (open) r.EndSection();

  out->core.warehouse = ExpandTilde(out->core.warehouse);
  return r.ok;
}

const char* const kPlanModes[] = {"server", "client", nullptr};
const char* const kGraphModes[] = {"basis",  "hasse", "compose",  "edges",
                                   "roots",  "paths", "spectrum", nullptr};
const char* const kGraphFormats[] = {"text", "dot", "json", nullptr};
const char* const kLogFormats[] = {"flat", "json", nullptr};

}  // namespace

#define PP_STR(sec, mem, key, def, allow)                                    \
  {#sec, key, Field::kStr, [](Conf& c) { return &c.sec.mem; }, nullptr,      \
   nullptr, def, 0, false, allow},
#define PP_I64(sec, mem, key, def)                                           \
  {#sec, key, Field::kI64, nullptr, [](Conf& c) { return &c.sec.mem; },      \
   nullptr, nullptr, def, false, nullptr},
#define PP_BOOL(sec, mem, key, def)                                          \
  {#sec, key, Field::kBool, nullptr, nullptr,                                \
   [](Conf& c) { return &c.sec.mem; }, nullptr, 0, def, nullptr},

const Field kFields[] = {
    PP_STR(core, rest_uri, "rest_uri", "http://127.0.0.1:8181", nullptr)
    PP_STR(core, ns_name, "namespace", "primeparts", nullptr)
    PP_STR(core, warehouse, "warehouse", "~/.local/share/pp-data", nullptr)

    PP_I64(generate, threads, "threads", 0)
    PP_I64(generate, chunk_primes, "chunk_primes", 500000)
    PP_I64(generate, group_chunks, "group_chunks", 0)
    PP_I64(generate, queue_depth, "queue_depth", 6)
    PP_I64(generate, primecount_threads, "primecount_threads", 0)

    PP_STR(catalogd, host, "host", "127.0.0.1", nullptr)
    PP_I64(catalogd, port, "port", 8181)
    PP_STR(catalogd, scan_planning_mode, "scan_planning_mode", "server",
           kPlanModes)
    PP_I64(catalogd, plan_batch, "plan_batch", 64)
    PP_I64(catalogd, plan_ttl, "plan_ttl", 300)

    PP_I64(verify, threads, "threads", 0)
    PP_I64(verify, max_examples, "max_examples", 20)

    PP_I64(query, limit, "limit", 10)

    PP_I64(graph, threads, "threads", 8)
    PP_I64(graph, top, "top", 20)
    PP_I64(graph, max_p, "max_p", 100000)
    PP_I64(graph, e_level, "e", 1)
    PP_I64(graph, he_n, "he_n", 0)
    PP_I64(graph, ell_max, "ell_max", 0)
    PP_BOOL(graph, sweep, "sweep", false)
    PP_BOOL(graph, sweep_only, "sweep_only", false)
    PP_BOOL(graph, orbits, "orbits", false)
    PP_BOOL(graph, materialize, "materialize", false)
    PP_BOOL(graph, critical, "critical", false)
    PP_BOOL(graph, branches, "branches", false)
    PP_STR(graph, ells, "ells", "", nullptr)
    PP_STR(graph, mode, "mode", "basis", kGraphModes)
    PP_STR(graph, format, "format", "text", kGraphFormats)

    PP_I64(tui, log_limit, "log_limit", 200)
    PP_I64(tui, default_limit, "default_limit", 10)
    PP_STR(tui, log_format, "log_format", "flat", kLogFormats)
    PP_BOOL(tui, autosave, "autosave", false)
};

#undef PP_STR
#undef PP_I64
#undef PP_BOOL

const std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

std::string RenderExample() {
  Conf d;
  for (std::size_t i = 0; i < kFieldCount; ++i) ApplyDefault(kFields[i], &d);

  std::ostringstream o;
  o << "--touched = true\n\nconf = {\n";
  std::size_t i = 0;
  while (i < kFieldCount) {
    std::size_t j = i, width = 0;
    while (j < kFieldCount &&
           std::strcmp(kFields[j].section, kFields[i].section) == 0) {
      width = std::max(width, std::strlen(kFields[j].key));
      ++j;
    }
    if (i != 0) o << "\n";
    o << "  " << kFields[i].section << " = {\n";
    for (std::size_t f = i; f < j; ++f) {
      const Field& fd = kFields[f];
      o << "    " << fd.key
        << std::string(width - std::strlen(fd.key), ' ') << " = ";
      switch (fd.kind) {
        case Field::kStr: o << "\"" << *fd.str(d) << "\""; break;
        case Field::kI64: o << *fd.num(d); break;
        case Field::kBool: o << (*fd.flag(d) ? "true" : "false"); break;
      }
      o << ",\n";
    }
    o << "  },\n";
    i = j;
  }
  o << "}\n";
  return o.str();
}

namespace {

const Field* FindField(const std::string& path) {
  const std::size_t dot = path.find('.');
  if (dot == std::string::npos) return nullptr;
  const std::string section = path.substr(0, dot);
  const std::string key = path.substr(dot + 1);
  for (std::size_t i = 0; i < kFieldCount; ++i)
    if (section == kFields[i].section && key == kFields[i].key)
      return &kFields[i];
  return nullptr;
}

std::string DefaultLiteral(const Field& f) {
  switch (f.kind) {
    case Field::kStr: return std::string("\"") + f.str_default + "\"";
    case Field::kI64: return std::to_string(f.num_default);
    case Field::kBool: return f.flag_default ? "true" : "false";
  }
  return "nil";
}

}  // namespace

bool AppendDefaults(Conf* conf, std::string* error) {
  if (conf->defaulted.empty()) return true;

  std::vector<std::string> lines;
  {
    std::ifstream in(conf->path);
    if (!in) {
      if (error) *error = "cannot read " + conf->path.string();
      return false;
    }
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }

  std::size_t at = lines.size();
  for (std::size_t i = lines.size(); i-- > 0;) {
    const std::string& l = lines[i];
    const std::size_t b = l.find_first_not_of(" \t");
    if (b == std::string::npos) continue;
    if (l.compare(b, 11, "return conf") == 0) {
      at = i;
      break;
    }
  }

  std::vector<std::string> add;
  std::string section;
  for (const std::string& path : conf->defaulted) {
    const Field* f = FindField(path);
    if (f == nullptr) continue;
    if (section != f->section) {
      section = f->section;
      add.push_back("");
      add.push_back("conf." + section + " = conf." + section + " or {}");
    }
    add.push_back("conf." + std::string(f->section) + "." + f->key + " = " +
                  DefaultLiteral(*f));
  }
  if (add.empty()) return true;

  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), add.begin(),
               add.end());

  std::ofstream out(conf->path, std::ios::trunc);
  if (!out) {
    if (error) *error = "cannot write " + conf->path.string();
    return false;
  }
  for (const std::string& l : lines) out << l << "\n";
  if (!out) {
    if (error) *error = "write failed: " + conf->path.string();
    return false;
  }
  return true;
}

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

  const std::vector<fs::path> roots = LuaRoots();
  if (roots.empty()) {
    if (error) *error = "cannot locate the running binary to seed a config";
    return {};
  }
  for (const fs::path& root : roots)
    if (const fs::path p = root / "pp" / "config.lua"; Exists(p)) return p;

  const fs::path seeded = roots.back() / "pp" / "config.lua";
  if (!WriteExample(seeded, error)) return {};
  return seeded;
}

bool Load(const fs::path& requested, Conf* out, std::string* error) {
  *out = Conf{};
  bool seeding = requested.empty() && !Exists("config.lua") &&
                 !Exists(XdgConfig()) && !Exists(HomeConfig());
  if (seeding)
    for (const fs::path& root : LuaRoots())
      if (Exists(root / "pp" / "config.lua")) seeding = false;
  out->path = Resolve(requested, error);
  if (out->path.empty()) return false;
  out->generated = seeding;

  const std::string file = out->path.string();
  std::string text;
  {
    std::ifstream in(out->path, std::ios::binary);
    if (!in) {
      if (error != nullptr) *error = "cannot read " + file;
      return false;
    }
    text.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
  }

  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  SetRocksPath(L);

  if (!Validate(L, text, file, error)) {
    lua_close(L);
    return false;
  }

  if (luaL_loadbuffer(L, text.data(), text.size(), file.c_str()) != LUA_OK ||
      lua_pcall(L, 0, 0, 0) != LUA_OK) {
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

  lua_getglobal(L, "conf");
  if (!lua_istable(L, -1)) {
    if (error != nullptr) *error = file + ": no conf table";
    lua_close(L);
    return false;
  }

  const bool ok = ReadConf(L, file, out, error);
  lua_close(L);
  if (!ok) return false;

  if (!out->generated && !out->defaulted.empty()) {
    std::string append_error;
    if (!AppendDefaults(out, &append_error))
      std::fprintf(stderr, "%s\n", append_error.c_str());
  }
  return true;
}

void Announce(const Conf& conf) {
  if (!conf.generated && !conf.defaulted.empty()) {
    std::fprintf(stderr, "%s: added %zu missing key%s at defaults:",
                 conf.path.string().c_str(), conf.defaulted.size(),
                 conf.defaulted.size() == 1 ? "" : "s");
    for (const std::string& k : conf.defaulted)
      std::fprintf(stderr, " %s", k.c_str());
    std::fprintf(stderr, "\n");
  }
  if (conf.touched) return;
  std::fprintf(stderr, "%s: defaults in use; edit it and uncomment touched\n",
               conf.path.string().c_str());
}

}  // namespace primeparts::config
