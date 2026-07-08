// primeparts/catalog/pp_commit.h
//
// Atomic multi-table commit — the client side of the IRC wire seam. The client
// writes ALL file data (parquet + manifests, via iceberg-cpp's FastAppend/Apply)
// and assembles the per-table (requirements, updates); only the catalog
// head-pointer CAS crosses the seam. Two transports over one assembly path: a
// daemon POST to /v1/transactions/commit, or an in-process store transaction.
//
// This TU compiles against iceberg-cpp's internal serde (-Ivendor/iceberg-cpp/src),
// like pp_catalogd.cc, so pp owns the commit request end to end instead of going
// through RestCatalog's opaque single-table commit path.

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace iceberg {
class Catalog;
class Schema;
class PartitionSpec;
struct DataFile;
}

namespace iceberg::sql {
class CatalogStore;
}

namespace primeparts::catalog {

namespace fs = std::filesystem;

/// One table's contribution to an atomic commit: the staged data files to append
/// under `primeparts.<table_name>`. `files` are mutated in place — each
/// `file_path` is rewritten to its committed in-warehouse location.
struct TableCommitSpec {
  std::string table_name;
  std::shared_ptr<iceberg::Schema> schema;
  std::shared_ptr<iceberg::PartitionSpec> spec;
  std::vector<std::shared_ptr<iceberg::DataFile>> files;
};

/// Commit every spec's staged files as ONE atomic transaction. Per table:
/// ensure/create it, move staged files into the committed tree, then FastAppend
/// + Apply() (writes the manifests + manifest list and returns the new snapshot,
/// without committing). The resulting (requirements, updates) for all tables are
/// then committed atomically through exactly one transport:
///   * in-process — when `store` is non-null: `store->RunInTransaction` wrapping
///     one `catalog->UpdateTable` per table (any failure aborts the whole txn).
///   * daemon — otherwise: POST `{table-changes:[...]}` to
///     `rest_uri`/v1/transactions/commit.
/// Returns false + `*error` on any assembly or commit failure. Specs with no
/// files still ensure the table exists but contribute no snapshot.
bool CommitFilesAtomic(const std::shared_ptr<iceberg::Catalog>& catalog,
                       const std::shared_ptr<iceberg::sql::CatalogStore>& store,
                       const std::string& rest_uri, const fs::path& warehouse,
                       std::vector<TableCommitSpec>& specs, std::string* error);

/// Assemble the `{table-changes:[...]}` commit body for `specs` (ensure/create,
/// move staged files, FastAppend + Apply) and return it serialized in
/// `*body_json`, WITHOUT committing. This is the daemon transport's request; also
/// used by tests to exercise the server's transaction route directly (e.g. by
/// tampering a requirement to prove atomic rollback). False + `*error` on failure.
bool AssembleTransactionBody(const std::shared_ptr<iceberg::Catalog>& catalog,
                             const fs::path& warehouse,
                             std::vector<TableCommitSpec>& specs,
                             std::string* body_json, std::string* error);

}  // namespace primeparts::catalog
