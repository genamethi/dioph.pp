#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace iceberg {
class Catalog;
struct Namespace;
}

namespace primeparts::query {

namespace fs = std::filesystem;

enum class ColumnType { kLong, kInt, kString };

struct MaterializeColumn {
  std::string name;
  ColumnType type = ColumnType::kLong;
  std::vector<int64_t> ints;
  std::vector<std::string> strings;
  bool no_stats = false;
};

struct MaterializeOptions {
  std::vector<std::string> sort_keys;
  std::unordered_map<std::string, std::string> properties;
  int64_t target_file_bytes = 0;
};

bool MaterializeColumns(const std::shared_ptr<iceberg::Catalog>& catalog,
                        const iceberg::Namespace& ns, const fs::path& warehouse,
                        const std::string& name,
                        const std::vector<MaterializeColumn>& columns,
                        const MaterializeOptions& options,
                        std::string* metadata_location, std::string* error);

bool MaterializeIntColumns(
    const std::shared_ptr<iceberg::Catalog>& catalog,
    const iceberg::Namespace& ns, const fs::path& warehouse,
    const std::string& name, const std::vector<std::string>& col_names,
    const std::vector<std::vector<int64_t>>& columns,
    std::string* metadata_location, std::string* error);

}  // namespace primeparts::query
