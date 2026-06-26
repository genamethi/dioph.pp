// primeparts/catalog/pp_catalogd.h
//
// pp-catalogd — the native Iceberg REST Catalog (IRC) HTTP server. A thin
// cpp-httplib router over the local catalog of record (`MakeLocalCatalog` =
// SqlCatalog over the LMDB store). The engine does the real work (validate
// requirements, apply updates, write metadata.json, CAS the head pointer); this
// layer is a JSON<->Catalog adapter whose routes derive from the vendored spec
// native/vendor/iceberg-refs/rest-catalog-open-api.yaml.
//
// Request/response JSON serde is reused verbatim from iceberg-cpp's (internal)
// serializers — see pp_catalogd.cc — so the wire contract stays byte-compatible
// with the iceberg-cpp RestCatalog client (and pyiceberg/Spark/Trino).

#pragma once

#include <string>

namespace primeparts::catalog {

struct CatalogdOptions {
  std::string warehouse;       // on-disk warehouse root; catalog.lmdb lives under it
  std::string host = "127.0.0.1";
  int port = 8181;             // conventional IRC port
};

// Open the local catalog at `opts.warehouse`, bind `host:port`, and serve IRC
// routes until SIGINT/SIGTERM. Returns 0 on clean shutdown, non-zero on
// engine-open or bind failure.
int RunCatalogd(const CatalogdOptions& opts);

}  // namespace primeparts::catalog
