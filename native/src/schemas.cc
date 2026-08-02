#include "primeparts/schemas.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iceberg/partition_field.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/sort_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/transform.h"
#include "iceberg/type.h"

namespace primeparts {

namespace {

int32_t FieldIdByName(const iceberg::Schema& schema, std::string_view name) {
  for (const auto& field : schema.fields()) {
    if (field.name() == name) return field.field_id();
  }
  return -1;
}

}  // namespace

std::shared_ptr<iceberg::Schema> PrimesSchema() {
  return std::make_shared<iceberg::Schema>(
      std::vector<iceberg::SchemaField>{
          iceberg::SchemaField::MakeRequired(1, "p",                iceberg::int64()),
          iceberg::SchemaField::MakeRequired(2, "k",                iceberg::int32()),
          iceberg::SchemaField::MakeRequired(3, "prime_rank",       iceberg::int64()),
          iceberg::SchemaField::MakeRequired(4, "p_bucket_version", iceberg::int32()),
          iceberg::SchemaField::MakeRequired(5, "p_bucket",         iceberg::int32()),
      },
      0);
}

std::shared_ptr<iceberg::Schema> PartitionsSchema() {
  return std::make_shared<iceberg::Schema>(
      std::vector<iceberg::SchemaField>{
          iceberg::SchemaField::MakeRequired(1, "p",                iceberg::int64()),
          iceberg::SchemaField::MakeRequired(2, "m_k",              iceberg::int32()),
          iceberg::SchemaField::MakeRequired(3, "n_k",              iceberg::int32()),
          iceberg::SchemaField::MakeRequired(5, "prime_rank",       iceberg::int64()),
          iceberg::SchemaField::MakeRequired(6, "p_bucket_version", iceberg::int32()),
          iceberg::SchemaField::MakeRequired(7, "p_bucket",         iceberg::int32()),
      },
      0);
}

std::shared_ptr<iceberg::PartitionSpec> BucketPartitionSpec(
    const iceberg::Schema& schema, std::string* error) {
  const int32_t bucket_version_id = FieldIdByName(schema, "p_bucket_version");
  const int32_t bucket_id = FieldIdByName(schema, "p_bucket");
  if (bucket_version_id < 0 || bucket_id < 0) {
    if (error) *error = "schema is missing p_bucket_version or p_bucket";
    return nullptr;
  }
  auto spec_result = iceberg::PartitionSpec::Make(
      schema, iceberg::PartitionSpec::kInitialSpecId,
      {iceberg::PartitionField(bucket_version_id, 1000, "p_bucket_version",
                               iceberg::Transform::Identity()),
       iceberg::PartitionField(bucket_id, 1001, "p_bucket",
                               iceberg::Transform::Identity())},
      false);
  if (!spec_result.has_value()) {
    if (error) *error = spec_result.error().message;
    return nullptr;
  }
  return std::shared_ptr<iceberg::PartitionSpec>(std::move(spec_result.value()));
}

std::shared_ptr<iceberg::SortOrder> AscendingSortOrder(
    const iceberg::Schema& schema, const std::vector<std::string>& fields,
    std::string* error) {
  std::vector<iceberg::SortField> sort_fields;
  sort_fields.reserve(fields.size());
  for (const auto& name : fields) {
    const int32_t field_id = FieldIdByName(schema, name);
    if (field_id < 0) {
      if (error) *error = "schema is missing " + name;
      return nullptr;
    }
    sort_fields.emplace_back(field_id, iceberg::Transform::Identity(),
                             iceberg::SortDirection::kAscending,
                             iceberg::NullOrder::kFirst);
  }
  auto order_result = iceberg::SortOrder::Make(
      schema, iceberg::SortOrder::kInitialSortOrderId, std::move(sort_fields));
  if (!order_result.has_value()) {
    if (error) *error = order_result.error().message;
    return nullptr;
  }
  return std::shared_ptr<iceberg::SortOrder>(std::move(order_result.value()));
}

}  // namespace primeparts
