// primeparts/catalog/pp_iceberg_rest.h
//
// Shared iceberg REST (IRC) catalog helpers for primeparts native tools.
//
// This consolidates the RestCatalog construction + table-publish logic that was
// duplicated across drop_bucket_cols_main.cc, backfill_prime_rank_main.cc, and
// covering_sieve_main.cc. The IRC commit path writes the HMS table row that
// exposes a native-written iceberg table to any IRC client.
//
// NOTE on Hive engine visibility: an IRC RegisterTable / FastAppend commit is
// NOT by itself adopted by the running Hive engine for a *snapshot-advancing*
// change — that requires a separate HMS sync (ALTER TABLE ... SET
// TBLPROPERTIES('metadata_location'=...)). That step is the Hive side and lives
// in scripts/hive_register.sh, invoked as a subprocess (see pp-catalog). This
// header is the iceberg-cpp/IRC side only.

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/file_io.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/table_identifier.h"
#include "iceberg/type_fwd.h"

namespace primeparts::catalog {

namespace fs = std::filesystem;

/// Connection settings for an iceberg REST catalog (IRC). When `rest_uri` is
/// empty, callers should fall back to an in-memory catalog (on-disk only,
/// invisible to Hive) — see MakeCatalog.
struct RestOptions {
  std::string rest_uri;        // e.g. http://192.168.1.202:9090/iceberg
  std::string rest_name = "primeparts";
  std::string rest_warehouse;  // defaults to `warehouse` arg of MakeCatalog
  std::string rest_prefix;     // usually empty for this HMS deployment
};

/// A FileIO backed by the local filesystem (arrow). Used by the in-memory
/// catalog fallback and by metadata reads.
std::shared_ptr<iceberg::FileIO> LocalIO();

/// Return the lexicographically-greatest `*.metadata.json` under `metadata_dir`
/// (iceberg metadata files are zero-padded monotonic, so name order == version
/// order). Empty path on error, with `*error` set when non-null.
fs::path LatestMetadataJson(const fs::path& metadata_dir, std::string* error);

/// Build a catalog. With a non-empty `opts.rest_uri`, returns a RestCatalog
/// (registers arrow/avro/parquet readers as a side effect) and sets
/// `*mode="rest"`. Otherwise returns an InMemoryCatalog rooted at `warehouse`
/// and sets `*mode="in-memory"`. Returns nullptr with `*error` set on failure.
std::shared_ptr<iceberg::Catalog> MakeCatalog(const RestOptions& opts,
                                              const fs::path& warehouse,
                                              std::string* mode,
                                              std::string* error);

/// Build the local catalog of record: an in-process `iceberg::sql::SqlCatalog`
/// backed by an LMDB `CatalogStore` at `<warehouse>/catalog.lmdb`. This is the
/// ground-up replacement for the (removed) HMS REST servlet and the ephemeral
/// InMemoryCatalog — persistent, single-writer, IRC-spec-compatible. Registers
/// arrow/avro/parquet factories. Returns nullptr + `*error` on failure.
std::shared_ptr<iceberg::Catalog> MakeLocalCatalog(const fs::path& warehouse,
                                                   std::string* error);

/// Ensure `ns` exists in the catalog, creating it if absent. False + `*error`
/// on failure.
bool EnsureNamespace(const std::shared_ptr<iceberg::Catalog>& catalog,
                     const iceberg::Namespace& ns, std::string* error);

/// Publish a table to the catalog under namespace `primeparts`.
///
/// Two paths, matching the existing tools:
///  1. An on-disk metadata.json already exists: DropTable(purge=false) then
///     RegisterTable against it — preserves snapshot history, stamps the HMS
///     location pointer. No-op if the catalog already points there.
///  2. No metadata.json: CreateTable + (if `files` non-empty) FastAppend,
///     writing a fresh metadata.json. Used by the in-memory bootstrap path.
///
/// On success sets `*metadata_location` to the published location. Warehouse
/// layout is `<warehouse>/primeparts/<table_name>/...`.
bool PublishTable(const std::shared_ptr<iceberg::Catalog>& catalog,
                  const fs::path& warehouse, const std::string& table_name,
                  const std::shared_ptr<iceberg::Schema>& schema,
                  const std::shared_ptr<iceberg::PartitionSpec>& spec,
                  const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
                  std::string* metadata_location, std::string* error);

}  // namespace primeparts::catalog
