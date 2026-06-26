// pp-catalogd — native Iceberg REST Catalog (IRC) HTTP server entry point.
//
// Serves the local catalog of record (SqlCatalog over LMDB) over IRC /v1 routes
// so any IRC client (the iceberg-cpp RestCatalog, pyiceberg, Spark, Trino) can
// drive it. See primeparts/catalog/pp_catalogd.h.
//
// Usage:
//   pp-catalogd [--warehouse DIR] [--host H] [--port N]

#include <cstdio>
#include <cstdlib>
#include <string>

#include "primeparts/catalog/pp_catalogd.h"

namespace {

constexpr char kDefaultWarehouse[] =
    "/media/extssd/research/dioph.pp/data/ib-staging";

void Usage() {
  std::fprintf(stderr,
    "pp-catalogd — native Iceberg REST Catalog server\n\n"
    "  --warehouse DIR   warehouse root holding catalog.lmdb (default: %s)\n"
    "  --host H          bind address (default: 127.0.0.1)\n"
    "  --port N          bind port (default: 8181)\n",
    kDefaultWarehouse);
}

}  // namespace

int main(int argc, char** argv) {
  primeparts::catalog::CatalogdOptions opts;
  opts.warehouse = kDefaultWarehouse;

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
    else if (f == "-h" || f == "--help") { Usage(); return 0; }
    else { std::fprintf(stderr, "pp-catalogd: unknown flag '%s'\n", f.c_str()); Usage(); return 2; }
  }

  return primeparts::catalog::RunCatalogd(opts);
}
