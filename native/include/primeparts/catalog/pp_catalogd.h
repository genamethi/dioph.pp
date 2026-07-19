#pragma once

#include <cstddef>
#include <string>

namespace primeparts::catalog {

struct CatalogdOptions {
  std::string warehouse;
  std::string host = "127.0.0.1";
  int port = 8181;
  size_t plan_batch_tasks = 64;
  int plan_ttl_seconds = 300;
  std::string scan_planning_mode = "server";
};

int RunCatalogd(const CatalogdOptions &opts);

} // namespace primeparts::catalog
