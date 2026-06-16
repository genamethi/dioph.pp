// primeparts/tui/lua_presets.cc — see header.

#include "primeparts/tui/lua_presets.h"

#include <fstream>
#include <system_error>
#include <utility>

#include <lua.hpp>  // liblua 5.5 (extern "C" wrapper)

namespace primeparts::tui {

using primeparts::query::QueryField;

struct LuaState {
  lua_State* L = nullptr;
  std::vector<QueryPreset>* sink = nullptr;  // set per Load()
};

namespace {

// Lua: query("id", { desc=, kind=, fields={k=0,...}, accepts={"k",...}, target= })
int query_cfn(lua_State* L) {
  auto* st = static_cast<LuaState*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* id = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  QueryPreset p;
  p.id = id;

  auto get_str = [&](const char* key, std::string& out) {
    lua_getfield(L, 2, key);
    if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
    lua_pop(L, 1);
  };
  get_str("desc", p.desc);
  get_str("kind", p.kind);
  get_str("target", p.target);

  // fields = { name = value, ... } (a map; values are integers)
  lua_getfield(L, 2, "fields");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {  // key at -2, value at -1
      if (lua_type(L, -2) == LUA_TSTRING) {
        QueryField f;
        f.name = lua_tostring(L, -2);
        f.value = static_cast<int64_t>(lua_tointeger(L, -1));
        p.fields.push_back(std::move(f));
      }
      lua_pop(L, 1);  // pop value; keep key for next
    }
  }
  lua_pop(L, 1);  // pop fields

  // accepts = { "k", ... } (an array of strings)
  lua_getfield(L, 2, "accepts");
  if (lua_istable(L, -1)) {
    const lua_Integer n = luaL_len(L, -1);
    for (lua_Integer i = 1; i <= n; ++i) {
      lua_geti(L, -1, i);
      if (lua_isstring(L, -1)) p.accepts.emplace_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);  // pop accepts

  if (st != nullptr && st->sink != nullptr) st->sink->push_back(std::move(p));
  return 0;
}

std::string lua_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') o.push_back('\\');
    if (c == '\n') { o += "\\n"; continue; }
    o.push_back(c);
  }
  return o;
}

}  // namespace

LuaPresets::LuaPresets() : st_(std::make_unique<LuaState>()) {
  st_->L = luaL_newstate();
  luaL_openlibs(st_->L);
  lua_pushlightuserdata(st_->L, st_.get());
  lua_pushcclosure(st_->L, query_cfn, 1);  // query() carries our State* upvalue
  lua_setglobal(st_->L, "query");
}

LuaPresets::~LuaPresets() {
  if (st_->L != nullptr) lua_close(st_->L);
}

std::vector<QueryPreset> LuaPresets::Load(const fs::path& file,
                                          std::vector<std::string>* errors) {
  std::vector<QueryPreset> out;
  st_->sink = &out;
  if (luaL_dofile(st_->L, file.string().c_str()) != LUA_OK) {
    if (errors != nullptr) {
      const char* msg = lua_tostring(st_->L, -1);
      errors->emplace_back(msg != nullptr ? msg : "lua error");
    }
    lua_pop(st_->L, 1);
  }
  st_->sink = nullptr;
  return out;
}

std::string LuaPresets::Serialize(const QueryPreset& p) {
  std::string s = "query(\"" + p.id + "\", {\n";
  s += "  desc = \"" + lua_escape(p.desc) + "\",\n";
  s += "  kind = \"" + p.kind + "\",\n";
  s += "  fields = { ";
  for (size_t i = 0; i < p.fields.size(); ++i) {
    s += p.fields[i].name + " = " + std::to_string(p.fields[i].value);
    if (i + 1 < p.fields.size()) s += ", ";
  }
  s += " },\n";
  s += "  accepts = { ";
  for (size_t i = 0; i < p.accepts.size(); ++i) {
    s += "\"" + p.accepts[i] + "\"";
    if (i + 1 < p.accepts.size()) s += ", ";
  }
  s += " },\n";
  s += "  target = \"" + p.target + "\",\n";
  s += "})\n";
  return s;
}

bool LuaPresets::Save(const QueryPreset& p, const fs::path& file,
                      std::string* error) {
  std::error_code ec;
  if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
  std::ofstream f(file, std::ios::app);
  if (!f) {
    if (error) *error = "cannot open " + file.string();
    return false;
  }
  f << Serialize(p);
  if (!f) {
    if (error) *error = "write failed: " + file.string();
    return false;
  }
  return true;
}

bool LuaPresets::SaveAll(const std::vector<QueryPreset>& ps, const fs::path& file,
                         std::string* error) {
  std::error_code ec;
  if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
  std::ofstream f(file, std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot open " + file.string();
    return false;
  }
  f << "-- primeparts query presets (managed by the TUI; safe to hand-edit)\n\n";
  for (const auto& p : ps) f << Serialize(p);
  if (!f) {
    if (error) *error = "write failed: " + file.string();
    return false;
  }
  return true;
}

}  // namespace primeparts::tui
