#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/catalog/pp_iceberg_rest.h"

namespace iceberg {
class Catalog;
class Schema;
class PartitionSpec;
struct DataFile;
} // namespace iceberg

namespace iceberg::sql {
class CatalogStore;
}

namespace primeparts::catalog {

namespace fs = std::filesystem;

struct TableCommitSpec {
  std::string table_name;
  std::shared_ptr<iceberg::Schema> schema;
  std::shared_ptr<iceberg::PartitionSpec> spec;
  TableDeclaration declare;
  std::vector<std::shared_ptr<iceberg::DataFile>> files;
};

bool CommitFilesAtomic(const std::shared_ptr<iceberg::Catalog> &catalog,
                       const std::shared_ptr<iceberg::sql::CatalogStore> &store,
                       const std::string &rest_uri,
                       const iceberg::Namespace &ns, const fs::path &warehouse,
                       std::vector<TableCommitSpec> &specs, std::string *error);

bool AssembleTransactionBody(const std::shared_ptr<iceberg::Catalog> &catalog,
                             const iceberg::Namespace &ns,
                             const fs::path &warehouse,
                             std::vector<TableCommitSpec> &specs,
                             std::string *body_json, std::string *error);

} // namespace primeparts::catalog
