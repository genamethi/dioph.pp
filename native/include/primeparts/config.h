#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct lua_State;

namespace primeparts::config {

namespace fs = std::filesystem;

struct Core {
  std::string rest_uri;
  std::string ns_name;
  std::string warehouse;
};

struct Generate {
  int64_t threads = 0;
  int64_t chunk_primes = 0;
  int64_t group_chunks = 0;
  int64_t queue_depth = 0;
  int64_t primecount_threads = 0;
};

struct Catalogd {
  std::string host;
  int64_t port = 0;
  std::string scan_planning_mode;
  int64_t plan_batch = 0;
  int64_t plan_ttl = 0;
};

struct Verify {
  int64_t threads = 0;
  int64_t max_examples = 0;
};

struct Query {
  int64_t limit = 0;
};

struct Graph {
  int64_t threads = 0;
  int64_t top = 0;
  int64_t max_p = 0;
  int64_t e_level = 0;
  int64_t he_n = 0;
  int64_t ell_max = 0;
  bool sweep = false;
  bool sweep_only = false;
  bool orbits = false;
  bool materialize = false;
  bool critical = false;
  bool branches = false;
  std::string ells;
  std::string mode;
  std::string format;
};

struct Tui {
  int64_t log_limit = 0;
  int64_t default_limit = 0;
  std::string log_format;
  bool autosave = false;
};

struct Lua {
  std::string path;
  std::string cpath;
};

struct Conf {
  fs::path path;
  bool touched = false;
  bool generated = false;
  std::vector<std::string> defaulted;
  Core core;
  Generate generate;
  Catalogd catalogd;
  Verify verify;
  Query query;
  Graph graph;
  Tui tui;
  Lua lua;
};

struct Field {
  enum Kind { kStr, kI64, kBool };
  const char* section;
  const char* key;
  Kind kind;
  std::string* (*str)(Conf&);
  int64_t* (*num)(Conf&);
  bool* (*flag)(Conf&);
  const char* str_default;
  int64_t num_default;
  bool flag_default;
  const char* const* allowed;
};

extern const Field kFields[];
extern const std::size_t kFieldCount;

std::string RenderExample();

fs::path Resolve(const fs::path& requested, std::string* error);

bool Load(const fs::path& requested, Conf* out, std::string* error);

bool AppendDefaults(Conf* conf, std::string* error);

void Announce(const Conf& conf);

void SetSearchPath(lua_State* L, const Conf& conf);

}  // namespace primeparts::config
