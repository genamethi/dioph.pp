#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace iceberg {
struct TableMetadata;
}

namespace primeparts::scan {

struct TableReadTraits {
  struct SortKey {
    int32_t field_id = -1;
    std::string name;
    bool ascending = true;
  };

  std::vector<SortKey> sort_keys;
  std::unordered_map<std::string, std::string> properties;

  bool sorted() const { return !sort_keys.empty(); }

  static bool FromMetadata(const iceberg::TableMetadata& metadata,
                           TableReadTraits* out, std::string* error);
};

}  // namespace primeparts::scan
