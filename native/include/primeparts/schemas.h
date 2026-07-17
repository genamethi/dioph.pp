
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace iceberg {
class Schema;
class PartitionSpec;
class SortOrder;
}

namespace primeparts {

std::shared_ptr<iceberg::Schema> PrimesSchema();
std::shared_ptr<iceberg::Schema> PartitionsSchema();

std::shared_ptr<iceberg::PartitionSpec> BucketPartitionSpec(
    const iceberg::Schema& schema, std::string* error);

std::shared_ptr<iceberg::SortOrder> AscendingSortOrder(
    const iceberg::Schema& schema, const std::vector<std::string>& fields,
    std::string* error);

}  // namespace primeparts
