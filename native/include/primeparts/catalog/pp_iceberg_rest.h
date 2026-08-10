#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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

inline constexpr char kCatalogName[] = "primeparts";

iceberg::Namespace ResolveNamespace(const std::string& name);

std::string NamespaceUrlPath(const iceberg::Namespace& ns);

struct RestOptions {
  std::string rest_uri;
  std::string rest_name = kCatalogName;
  std::string rest_warehouse;
  std::string rest_prefix;
};

std::shared_ptr<iceberg::FileIO> LocalIO();

fs::path StagingDataDir(const fs::path& warehouse, const iceberg::Namespace& ns,
                        const std::string& table_name);

std::shared_ptr<iceberg::Catalog> MakeCatalog(const RestOptions& opts,
                                              const fs::path& warehouse,
                                              std::string* mode,
                                              std::string* error);

struct LocalCatalog {
  std::shared_ptr<iceberg::Catalog> catalog;
  std::shared_ptr<iceberg::sql::CatalogStore> store;
};

LocalCatalog MakeLocalCatalogWithStore(const fs::path& warehouse,
                                       std::string* error);

std::shared_ptr<iceberg::Catalog> MakeLocalCatalog(const fs::path& warehouse,
                                                   std::string* error);

std::shared_ptr<iceberg::Catalog> OpenCatalog(const fs::path& warehouse,
                                              const std::string& rest_uri,
                                              std::string* mode, std::string* error);

bool RestServerReachable(const std::string& rest_uri);

std::string AdvertisedWarehouse(const std::string& rest_uri);

enum class FieldBoundState {
  kTableAbsent,
  kNoSnapshot,
  kSnapshotNoBound,
  kPresent,
};

std::string_view FieldBoundStateName(FieldBoundState state);

bool FetchFieldUpperBound(const std::string& rest_uri,
                          const iceberg::Namespace& ns,
                          const std::string& table, const std::string& field,
                          int64_t* out, bool* present, std::string* error);

bool FetchFieldBound(const std::string& rest_uri, const iceberg::Namespace& ns,
                     const std::string& table, const std::string& field,
                     int64_t* out, FieldBoundState* state, std::string* error);

fs::path TableMetadataPath(const std::shared_ptr<iceberg::Catalog>& catalog,
                           const iceberg::Namespace& ns,
                           const std::string& table, std::string* error);

bool EnsureNamespace(const std::shared_ptr<iceberg::Catalog>& catalog,
                     const iceberg::Namespace& ns, std::string* error);

bool DropTable(const std::shared_ptr<iceberg::Catalog>& catalog,
               const iceberg::Namespace& ns, const fs::path& warehouse,
               const std::string& table, bool purge, std::string* error);

struct TableDeclaration {
  std::shared_ptr<iceberg::SortOrder> sort_order;
  std::unordered_map<std::string, std::string> properties;
};

std::shared_ptr<iceberg::Table> EnsureTable(
    const std::shared_ptr<iceberg::Catalog>& catalog,
    const iceberg::Namespace& ns, const fs::path& warehouse,
    const std::string& table_name,
    const std::shared_ptr<iceberg::Schema>& schema,
    const std::shared_ptr<iceberg::PartitionSpec>& spec,
    const TableDeclaration& declare, std::string* error);

bool MoveStagedFilesInto(
    const std::shared_ptr<iceberg::Table>& table, const iceberg::Namespace& ns,
    const fs::path& warehouse, const std::string& table_name,
    const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
    std::string* error);

bool CommitFiles(const std::shared_ptr<iceberg::Catalog>& catalog,
                 const iceberg::Namespace& ns, const fs::path& warehouse,
                 const std::string& table_name,
                 const std::shared_ptr<iceberg::Schema>& schema,
                 const std::shared_ptr<iceberg::PartitionSpec>& spec,
                 const TableDeclaration& declare,
                 const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
                 std::string* metadata_location, std::string* error);

}  // namespace primeparts::catalog
