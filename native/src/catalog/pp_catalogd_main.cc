#include <cstdio>
#include <cstdlib>
#include <string>

#include "primeparts/catalog/pp_catalogd.h"

#include "primeparts/config.h"

namespace {

constexpr char kDefaultWarehouse[] =
    "./data/ib-staging";

void Usage() {
  std::fprintf(stderr,
    "pp-catalogd — native Iceberg REST Catalog server\n\n"
    "  --warehouse DIR   warehouse root holding catalog.lmdb (default: %s)\n"
    "  --host H          bind address (default: 127.0.0.1)\n"
    "  --port N          bind port (default: 8181)\n"
    "  --plan-batch N    file scan tasks per planning batch (default: 64)\n"
    "  --plan-ttl N      seconds an idle plan-id is retained (default: 300)\n"
    "  --scan-planning-mode server|client   advertised planning mode\n"
    "                    (default: server)\n",
    kDefaultWarehouse);
}

}  // namespace

int main(int argc, char** argv) {
  primeparts::catalog::CatalogdOptions opts;
  std::string cfg_err;
  auto cfg = primeparts::config::Load(&cfg_err);
  if (auto it = cfg.find("warehouse"); it != cfg.end() && !it->second.empty()) {
    opts.warehouse = it->second;
  } else {
    opts.warehouse = kDefaultWarehouse;
  }

  for (int i = 1; i < argc; ++i) {
    std::string f = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "pp-catalogd: %s needs an argument\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (f == "--warehouse") opts.warehouse = next("--warehouse");
    else if (f == "--host") opts.host = next("--host");
    else if (f == "--port") opts.port = std::atoi(next("--port").c_str());
    else if (f == "--plan-batch")
      opts.plan_batch_tasks = static_cast<size_t>(
          std::strtoul(next("--plan-batch").c_str(), nullptr, 10));
    else if (f == "--plan-ttl")
      opts.plan_ttl_seconds = std::atoi(next("--plan-ttl").c_str());
    else if (f == "--scan-planning-mode") {
      opts.scan_planning_mode = next("--scan-planning-mode");
      if (opts.scan_planning_mode != "server" &&
          opts.scan_planning_mode != "client") {
        std::fprintf(stderr,
                     "pp-catalogd: --scan-planning-mode must be 'server' or "
                     "'client', got '%s'\n",
                     opts.scan_planning_mode.c_str());
        return 2;
      }
    }
    else if (f == "-h" || f == "--help") { Usage(); return 0; }
    else { std::fprintf(stderr, "pp-catalogd: unknown flag '%s'\n", f.c_str()); Usage(); return 2; }
  }

  return primeparts::catalog::RunCatalogd(opts);
}
