#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace iceberg {
class Catalog;
struct Namespace;
}

namespace primeparts::query {

namespace fs = std::filesystem;

bool MaterializeIntColumns(
    const std::shared_ptr<iceberg::Catalog>& catalog,
    const iceberg::Namespace& ns, const fs::path& warehouse,
    const std::string& name, const std::vector<std::string>& col_names,
    const std::vector<std::vector<int64_t>>& columns,
    std::string* metadata_location, std::string* error);

}  // namespace primeparts::query
