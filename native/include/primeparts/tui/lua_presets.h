// primeparts/tui/lua_presets.h
//
// The TUI's embedded-Lua facility for query presets: parse scripts/lua/*.lua
// (which call query("id", {spec})), serialize a preset back to Lua text, and
// append-save it. Owns a lua_State (liblua 5.5). The reader/query layer
// (QueryService::ValidatePreset) is the validator; this is just (de)serialize.

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/query/query_preset.h"

namespace primeparts::tui {

namespace fs = std::filesystem;
using primeparts::query::QueryPreset;

struct LuaState;  // opaque (holds lua_State*), defined in the .cc

class LuaPresets {
 public:
  LuaPresets();
  ~LuaPresets();
  LuaPresets(const LuaPresets&) = delete;
  LuaPresets& operator=(const LuaPresets&) = delete;

  // Run a Lua file that calls query("id", {spec}) N times; return the collected
  // presets. Lua / parse errors are appended to `*errors` (the file may still
  // yield the presets defined before the error).
  std::vector<QueryPreset> Load(const fs::path& file,
                                std::vector<std::string>* errors);

  // Lua-syntax text for a preset, round-trippable with Load.
  static std::string Serialize(const QueryPreset& p);

  // Append Serialize(p) to `file` (creating parent dirs / the file). The caller
  // is responsible for validating first. false + *error on an IO failure.
  static bool Save(const QueryPreset& p, const fs::path& file, std::string* error);

 private:
  std::unique_ptr<LuaState> st_;
};

}  // namespace primeparts::tui
