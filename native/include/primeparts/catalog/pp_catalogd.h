// primeparts/catalog/pp_catalogd.h
//
// pp-catalogd — the native Iceberg REST Catalog (IRC) HTTP server.
// native/vendor/iceberg-refs/rest-catalog-open-api.yaml.
//
// Request/response JSON serde is reused verbatim from iceberg-cpp's (internal)
// serializers — see pp_catalogd.cc — so the wire contract stays byte-compatible
// with the IRC specification.

#pragma once

#include <string>

namespace primeparts::catalog {

struct CatalogdOptions {
  std::string warehouse; // on-disk warehouse root; catalog.lmdb lives under it
  std::string host = "127.0.0.1";
  int port = 8181; // conventional IRC port
};

// Returns 0 on clean shutdown
int RunCatalogd(const CatalogdOptions &opts);

} // namespace primeparts::catalog
