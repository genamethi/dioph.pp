#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace iceberg {
class Schema;
class SortOrder;
struct TableMetadata;
}

namespace primeparts::scan {

enum class SortOrderSupport {
  kOk,
  kInvalid,
  kUnsupported,
};

SortOrderSupport CheckSortOrder(const iceberg::Schema& schema,
                                const iceberg::SortOrder& order,
                                std::string* error);

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
