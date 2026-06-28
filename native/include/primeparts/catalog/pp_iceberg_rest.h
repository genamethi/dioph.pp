// primeparts/catalog/pp_iceberg_rest.h
//
// Shared iceberg catalog helpers for primeparts native tools.
//
// Two construction entry points: `MakeLocalCatalog` (the catalog of record:
// SqlCatalog over an LMDB CatalogStore) and `MakeCatalog` (a RestCatalog/IRC
// client, used to talk to an external IRC server such as pp-catalogd). Plus the
// shared table-publish + metadata-resolution helpers every tool consumes.

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

/// Connection settings for an iceberg REST catalog (IRC) client. When
/// `rest_uri` is empty, callers fall back to an in-memory catalog (on-disk
/// only) — see MakeCatalog.
struct RestOptions {
  std::string rest_uri;        // e.g. http://localhost:PORT/iceberg (pp-catalogd)
  std::string rest_name = "primeparts";
  std::string rest_warehouse;  // defaults to `warehouse` arg of MakeCatalog
  std::string rest_prefix;     // usually empty
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

/// Resolve a table's current metadata.json path through the catalog
/// (LoadTable -> metadata_file_location, file: scheme stripped). Namespace is
/// "primeparts". Empty path + *error on failure. This is the single seam every
/// reader uses to turn a table name into a metadata path — never the filesystem
/// or a raw catalog DB.
fs::path TableMetadataPath(const std::shared_ptr<iceberg::Catalog>& catalog,
                           const std::string& table, std::string* error);

/// Ensure `ns` exists in the catalog, creating it if absent. False + `*error`
/// on failure.
bool EnsureNamespace(const std::shared_ptr<iceberg::Catalog>& catalog,
                     const iceberg::Namespace& ns, std::string* error);

/// Drop table `primeparts.<table>` through the catalog. When `purge` is true,
/// implements the REST `dropTable?purgeRequested=true` contract: the table's
/// physical data + metadata are deleted. The base location is resolved through
/// the catalog (LoadTable -> location()) *before* the drop, so we delete exactly
/// what the catalog says the table owns. This function is the ONLY place in the
/// codebase that removes warehouse files — purge is a catalog operation, never
/// something application/analysis code does directly. (The deletion lives here
/// because the vendored v0.3.0 SqlCatalog FileIO does not purge on its own.)
/// Idempotent: a table the catalog does not know is treated as already dropped,
/// but a purge still reclaims any orphan files under the conventional location
/// (warehouse/primeparts/<table>) — debris from a build killed after a prior
/// drop but before CommitFiles.
bool DropTable(const std::shared_ptr<iceberg::Catalog>& catalog,
               const fs::path& warehouse, const std::string& table, bool purge,
               std::string* error);

/// Publish a table to the catalog under namespace `primeparts`.
///
/// Two paths, matching the existing tools:
///  1. An on-disk metadata.json already exists: DropTable(purge=false) then
///     RegisterTable against it — preserves snapshot history, updates the
///     catalog location pointer. No-op if the catalog already points there.
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

/// Commit a batch of already-written `DataFile`s to `primeparts.<table_name>`
/// as a single FastAppend snapshot. Unlike PublishTable (which drops+registers
/// an on-disk metadata.json), this is the incremental-append seam: it ensures
/// the namespace, **loads the table if the catalog already knows it, else
/// creates it**, then appends `files` in one snapshot and refreshes. Resume-safe
/// — repeated `generate` runs append onto the existing snapshot history.
///
/// Works against any `iceberg::Catalog`: in-process `MakeLocalCatalog` or a
/// `MakeCatalog` RestCatalog client pointed at `pp-catalogd` (the commit then
/// routes through the server's updateTable endpoint). On success sets
/// `*metadata_location`. A no-op (`files` empty) still ensures the table exists.
bool CommitFiles(const std::shared_ptr<iceberg::Catalog>& catalog,
                 const fs::path& warehouse, const std::string& table_name,
                 const std::shared_ptr<iceberg::Schema>& schema,
                 const std::shared_ptr<iceberg::PartitionSpec>& spec,
                 const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
                 std::string* metadata_location, std::string* error);

}  // namespace primeparts::catalog
