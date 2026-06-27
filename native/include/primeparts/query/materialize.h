// primeparts/query/materialize.h
//
// Publish a small in-memory integer table as an unpartitioned Iceberg
// materialized view primeparts.<name>, via the BucketParquetWriter + CommitFiles
// seams. Catalog-agnostic (in-process MakeLocalCatalog or a pp-catalogd client).
// Schema is built ad-hoc from the column names (all int64) — deliberately NOT in
// writer.h, which stays restricted to core base-table schemas.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace iceberg {
class Catalog;
}

namespace primeparts::query {

namespace fs = std::filesystem;

/// Replace-publish primeparts.<name> from a column-major int64 table:
/// `columns[j]` is the j-th column, named `col_names[j]`; every column must have
/// the same length. Replace semantics — any existing table of that name (and its
/// on-disk files) is dropped first, so re-materializing refreshes the cache.
/// Returns false + *error on failure; sets *metadata_location on success.
bool MaterializeIntColumns(
    const std::shared_ptr<iceberg::Catalog>& catalog, const fs::path& warehouse,
    const std::string& name, const std::vector<std::string>& col_names,
    const std::vector<std::vector<int64_t>>& columns,
    std::string* metadata_location, std::string* error);

}  // namespace primeparts::query
