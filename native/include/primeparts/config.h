#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

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
  std::string mode;
  std::string format;
};

struct Tui {
  int64_t log_limit = 0;
};

struct Conf {
  fs::path path;
  bool touched = false;
  bool generated = false;
  Core core;
  Generate generate;
  Catalogd catalogd;
  Verify verify;
  Query query;
  Graph graph;
  Tui tui;
};

extern const char kExampleConfig[];

fs::path Resolve(const fs::path& requested, std::string* error);

bool Load(const fs::path& requested, Conf* out, std::string* error);

void Announce(const Conf& conf);

}  // namespace primeparts::config
