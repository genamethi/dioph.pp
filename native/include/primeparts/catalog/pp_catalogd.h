#pragma once

#include <cstddef>
#include <string>

namespace primeparts::catalog {

struct CatalogdOptions {
  std::string warehouse;
  std::string host;
  int port = 0;
  size_t plan_batch_tasks = 0;
  int plan_ttl_seconds = 0;
  std::string scan_planning_mode;
};

int RunCatalogd(const CatalogdOptions &opts);

} // namespace primeparts::catalog
