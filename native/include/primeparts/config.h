#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace primeparts::config {

namespace fs = std::filesystem;

// Path to the shared primeparts config.lua: $XDG_CONFIG_HOME/primeparts/config.lua
// (or $HOME/.config/primeparts/config.lua), matching where the TUI reads/writes
// it. Empty when neither env var is set.
fs::path ConfigFilePath();

// Load config.lua's `config({ key = value, ... })` into a string map (values
// stringified: numbers/booleans as text). Missing file -> empty map. A lua error
// leaves the partial map and sets *error when non-null.
std::map<std::string, std::string> Load(std::string* error);

}  // namespace primeparts::config
