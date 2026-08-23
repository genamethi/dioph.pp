#include <cstdio>
#include <cstdlib>
#include <string>

#include "primeparts/catalog/pp_catalogd.h"

#include "primeparts/config.h"

namespace {

void Usage() {
  std::fprintf(stderr,
    "pp-catalogd — native Iceberg REST Catalog server\n\n"
    "  -c, --config PATH config file (default: ./config.lua, then\n"
    "                    $XDG_CONFIG_HOME/primeparts/config.lua, then\n"
    "                    ~/.config/primeparts/config.lua, else seeded next\n"
    "                    to this binary)\n"
    "  --warehouse DIR   warehouse root holding catalog.lmdb\n"
    "                    (default: conf.core.warehouse)\n"
    "  --host H          bind address (default: conf.catalogd.host)\n"
    "  --port N          bind port (default: conf.catalogd.port)\n"
    "  --plan-batch N    file scan tasks per planning batch\n"
    "                    (default: conf.catalogd.plan_batch)\n"
    "  --plan-ttl N      seconds an idle plan-id is retained\n"
    "                    (default: conf.catalogd.plan_ttl)\n"
    "  --scan-planning-mode server|client   advertised planning mode\n"
    "                    (default: conf.catalogd.scan_planning_mode)\n");
}

}  // namespace

int main(int argc, char** argv) {
  primeparts::catalog::CatalogdOptions opts;
  std::string config_path;
  std::string warehouse;
  std::string host;
  std::string mode;
  int port = -1;
  long plan_batch = -1;
  int plan_ttl = -1;

  for (int i = 1; i < argc; ++i) {
    std::string f = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "pp-catalogd: %s needs an argument\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (f == "--config" || f == "-c") config_path = next(f.c_str());
    else if (f == "--warehouse") warehouse = next("--warehouse");
    else if (f == "--host") host = next("--host");
    else if (f == "--port") port = std::atoi(next("--port").c_str());
    else if (f == "--plan-batch")
      plan_batch = std::strtol(next("--plan-batch").c_str(), nullptr, 10);
    else if (f == "--plan-ttl")
      plan_ttl = std::atoi(next("--plan-ttl").c_str());
    else if (f == "--scan-planning-mode") {
      mode = next("--scan-planning-mode");
      if (mode != "server" && mode != "client") {
        std::fprintf(stderr,
                     "pp-catalogd: --scan-planning-mode must be 'server' or "
                     "'client', got '%s'\n",
                     mode.c_str());
        return 2;
      }
    }
    else if (f == "-h" || f == "--help") { Usage(); return 0; }
    else { std::fprintf(stderr, "pp-catalogd: unknown flag '%s'\n", f.c_str()); Usage(); return 2; }
  }

  std::string cfg_err;
  primeparts::config::Conf conf;
  if (!primeparts::config::Load(config_path, &conf, &cfg_err)) {
    std::fprintf(stderr, "%s\n", cfg_err.c_str());
    return 2;
  }
  primeparts::config::Announce(conf);

  opts.warehouse = warehouse.empty() ? conf.core.warehouse : warehouse;
  opts.host = host.empty() ? conf.catalogd.host : host;
  opts.port = port < 0 ? static_cast<int>(conf.catalogd.port) : port;
  opts.plan_batch_tasks = static_cast<size_t>(
      plan_batch < 0 ? conf.catalogd.plan_batch : plan_batch);
  opts.plan_ttl_seconds =
      plan_ttl < 0 ? static_cast<int>(conf.catalogd.plan_ttl) : plan_ttl;
  opts.scan_planning_mode =
      mode.empty() ? conf.catalogd.scan_planning_mode : mode;

  return primeparts::catalog::RunCatalogd(opts);
}
