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

namespace iceberg::sql {
class CatalogStore;
}

namespace primeparts::catalog {

namespace fs = std::filesystem;

/// Default IRC endpoint (bare scheme://host:port, no /v1 suffix), matching
/// pp-catalogd's default bind (127.0.0.1:8181). OpenCatalog resolves to this when
/// neither an explicit URI nor PRIMEPARTS_REST_URI is given; --rest-uri / the
/// env var only OVERRIDE the URI. Keep in sync with pp_catalogd.h's `port`.
inline constexpr char kDefaultRestUri[] = "http://127.0.0.1:8181";

/// Connection settings for an iceberg REST catalog (IRC) client. `rest_uri` is
/// required — see MakeCatalog.
struct RestOptions {
  std::string rest_uri;        // e.g. http://localhost:PORT/iceberg (pp-catalogd)
  std::string rest_name = "primeparts";
  std::string rest_warehouse;  // defaults to `warehouse` arg of MakeCatalog
  std::string rest_prefix;     // usually empty
};

/// A FileIO backed by the local filesystem (arrow). Used by the local catalog
/// engine and by metadata reads.
std::shared_ptr<iceberg::FileIO> LocalIO();

/// Staging directory a writer emits parquet into BEFORE it is committed. It lives
/// OUTSIDE the warehouse table tree (a sibling of the warehouse, under a
/// `.pp-staging/<warehouse-name>/<table>` path on the same filesystem so the
/// commit-move is an atomic rename). This is the catalog seam: client code never
/// computes a path under the warehouse table dir — it writes to staging, then
/// `CommitFiles` asks the catalog for the table's location and MOVES the staged
/// files there before registering. A build killed before commit can therefore
/// only ever leave staging debris; the committed table dirs never hold a file
/// that didn't arrive via a successful commit. Staging debris is uncommitted and
/// safe to delete (CommitFiles prunes the emptied staging dir on success).
fs::path StagingDataDir(const fs::path& warehouse, const std::string& table_name);

/// Return the lexicographically-greatest `*.metadata.json` under `metadata_dir`
/// (iceberg metadata files are zero-padded monotonic, so name order == version
/// order). Empty path on error, with `*error` set when non-null.
fs::path LatestMetadataJson(const fs::path& metadata_dir, std::string* error);

/// Build a RestCatalog client for `opts.rest_uri` (registers arrow/avro/parquet
/// readers as a side effect) and set `*mode="rest"`. `rest_uri` is required.
/// Returns nullptr with `*error` set on failure.
std::shared_ptr<iceberg::Catalog> MakeCatalog(const RestOptions& opts,
                                              const fs::path& warehouse,
                                              std::string* mode,
                                              std::string* error);

/// The catalog of record plus the LMDB store instance behind it. The store is
/// the SAME shared instance the SqlCatalog uses, so `store->RunInTransaction`
/// wraps the write txn that the catalog's `UpdateTable` calls join — the basis
/// for atomic multi-table commit through catalogd.
struct LocalCatalog {
  std::shared_ptr<iceberg::Catalog> catalog;
  std::shared_ptr<iceberg::sql::CatalogStore> store;
};

/// Build the local catalog of record: an in-process `iceberg::sql::SqlCatalog`
/// backed by an LMDB `CatalogStore` at `<warehouse>/catalog.lmdb`. Registers
/// arrow/avro/parquet factories. `.catalog` is null + `*error` set on failure.
LocalCatalog MakeLocalCatalogWithStore(const fs::path& warehouse,
                                       std::string* error);

/// Convenience: `MakeLocalCatalogWithStore(...).catalog` for callers that don't
/// need the store handle.
std::shared_ptr<iceberg::Catalog> MakeLocalCatalog(const fs::path& warehouse,
                                                   std::string* error);

/// Unified catalog entry point — catalogd over REST is the ONLY pathway; the
/// LMDB state is touched exclusively by the daemon. This is the single seam
/// every tool uses to obtain a catalog.
///
/// URI resolution: `rest_uri` if non-empty, else `PRIMEPARTS_REST_URI`, else the
/// compiled-in `kDefaultRestUri` — the arg/env only OVERRIDE the endpoint. If the
/// resolved server answers `GET /v1/config`, returns a RestCatalog client
/// (`*mode="rest"`). Otherwise returns nullptr + `*error`: tools do not run
/// without catalogd.
std::shared_ptr<iceberg::Catalog> OpenCatalog(const fs::path& warehouse,
                                              const std::string& rest_uri,
                                              std::string* mode, std::string* error);

/// True if an IRC server at `rest_uri` (scheme://host:port, no context path)
/// answers `GET /v1/config` within a short timeout. Used by OpenCatalog to fail
/// fast; exposed for tools that want to report reachability.
bool RestServerReachable(const std::string& rest_uri);

/// Ask pp-catalogd at `rest_uri` for the committed upper bound of `field` on
/// namespace `ns` / `table` (GET .../field-upper-bound?field=NAME). On success
/// returns true: `*present` is false when the table has no snapshot (fresh) or
/// the field carries no bound, otherwise `*out` holds the max. False + `*error`
/// on an unreachable server, non-200, or unparseable response. Used by generate
/// to resume from the frontier (field="prime_rank").
bool FetchFieldUpperBound(const std::string& rest_uri, const std::string& ns,
                          const std::string& table, const std::string& field,
                          int64_t* out, bool* present, std::string* error);

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

/// Load `primeparts.<table_name>` if the catalog knows it, else CreateTable it
/// fresh (zstd/level-3 write props, ensures the namespace). Null + `*error` on
/// failure. Shared by CommitFiles and the atomic multi-table commit.
std::shared_ptr<iceberg::Table> EnsureTable(
    const std::shared_ptr<iceberg::Catalog>& catalog, const fs::path& warehouse,
    const std::string& table_name,
    const std::shared_ptr<iceberg::Schema>& schema,
    const std::shared_ptr<iceberg::PartitionSpec>& spec, std::string* error);

/// Move each staged `DataFile` (written under StagingDataDir) into the table's
/// committed `<location>/data/` tree, preserving the writer's staging sub-layout,
/// and rewrite each `file_path` in place to the committed path. Prunes the
/// emptied staging tree. False + `*error` on failure.
bool MoveStagedFilesInto(
    const std::shared_ptr<iceberg::Table>& table, const fs::path& warehouse,
    const std::string& table_name,
    const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
    std::string* error);

/// Commit a batch of `DataFile`s written to a staging dir (see StagingDataDir)
/// to `primeparts.<table_name>` as a single FastAppend snapshot. This is the
/// catalog seam and the ONLY place client-written data files enter the warehouse:
/// it ensures the namespace, **loads the table if the catalog already knows it,
/// else creates it**, then MOVES each staged file into `<table.location()>/data/`
/// (the catalog-chosen destination), rewrites each `DataFile`'s `file_path` to
/// the moved location, appends them in one snapshot, and refreshes. The move
/// happens before the append so the manifest only ever records committed paths.
/// Resume-safe — repeated `generate` runs append onto the existing snapshot
/// history. `files`' pointees are mutated in place (their `file_path` is rewritten
/// to the committed location).
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
