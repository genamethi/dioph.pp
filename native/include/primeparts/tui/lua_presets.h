#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/config.h"
#include "primeparts/query/query_preset.h"

namespace primeparts::tui {

namespace fs = std::filesystem;
using primeparts::query::QueryPreset;

struct LuaState;

class LuaPresets {
 public:
  LuaPresets();
  ~LuaPresets();
  LuaPresets(const LuaPresets&) = delete;
  LuaPresets& operator=(const LuaPresets&) = delete;

  void SetSearchPath(const primeparts::config::Conf& conf);

  std::vector<QueryPreset> Load(const fs::path& file,
                                std::vector<std::string>* errors);

  static std::string Serialize(const QueryPreset& p);

  static bool Save(const QueryPreset& p, const fs::path& file, std::string* error);

  static bool SaveAll(const std::vector<QueryPreset>& ps, const fs::path& file,
                      std::string* error);

 private:
  std::unique_ptr<LuaState> st_;
};

}  // namespace primeparts::tui
