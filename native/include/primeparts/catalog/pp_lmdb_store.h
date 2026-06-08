// primeparts/catalog/pp_lmdb_store.h
//
// LMDB-backed iceberg::sql::CatalogStore for the native IRC catalog.
//
// The Iceberg catalog *logic* lives in upstream iceberg::sql::SqlCatalog; this
// supplies the persistence backend through the driver-agnostic CatalogStore
// interface ("bring your own store"). The catalog's transactional pointer
// registry — (namespace, table) -> metadata_location with optimistic
// compare-and-set, plus namespace properties — lives in a single LMDB
// environment (one env per catalog). No SQL / sqlpp23 dependency.
//
// LMDB layout (one env, two named sub-DBs):
//   tables  : key = ns '\0' name        -> value = metadata_loc '\0' previous_loc
//   nsprops : key = ns '\0' propkey      -> value = <presence-byte> [value bytes]
// A namespace "exists" iff it has >=1 nsprops row (SqlCatalog inserts a sentinel
// "exists" property on create), or it owns at least one table.

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "iceberg/catalog/sql/catalog_store.h"
#include "iceberg/result.h"

namespace primeparts::catalog {

/// Construct an LMDB-backed CatalogStore for iceberg::sql::SqlCatalog::Make().
///
/// \param path           LMDB environment directory (created if absent).
/// \param catalog_name   Informational; the env is dedicated to one catalog.
/// \param map_size_bytes LMDB maximum map size (virtual address space, not
///                       preallocated). Default 512 MiB — ample for a catalog.
///
/// The environment is opened lazily by Initialize(), which SqlCatalog::Make
/// calls before first use.
iceberg::Result<std::shared_ptr<iceberg::sql::CatalogStore>> MakeLmdbCatalogStore(
    std::filesystem::path path, std::string catalog_name,
    std::size_t map_size_bytes = (std::size_t{512} << 20));

}  // namespace primeparts::catalog
