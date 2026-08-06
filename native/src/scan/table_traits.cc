#include "primeparts/scan/table_traits.h"

#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/sort_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_properties.h"
#include "iceberg/transform.h"

namespace primeparts::scan {

namespace {

const iceberg::SchemaField* FieldById(const iceberg::Schema& schema,
                                      int32_t field_id) {
  for (const auto& f : schema.fields()) {
    if (f.field_id() == field_id) return &f;
  }
  return nullptr;
}

}  // namespace

SortOrderSupport CheckSortOrder(const iceberg::Schema& schema,
                                const iceberg::SortOrder& order,
                                std::string* error) {
  if (order.is_unsorted()) return SortOrderSupport::kOk;
  for (const auto& sf : order.fields()) {
    if (!sf.transform() ||
        sf.transform()->transform_type() != iceberg::TransformType::kIdentity) {
      if (error) {
        *error = "NotImplemented: declared sort order field " +
                 std::to_string(sf.source_id()) + " uses transform '" +
                 (sf.transform() ? sf.transform()->ToString()
                                 : std::string("null")) +
                 "'; sort orders resolve identity transforms only — "
                 "resolving this order requires applying the transform to "
                 "source values when ordering tasks and slicing batches";
      }
      return SortOrderSupport::kUnsupported;
    }
    if (!FieldById(schema, sf.source_id())) {
      if (error) {
        *error = "sort order references unknown field id " +
                 std::to_string(sf.source_id());
      }
      return SortOrderSupport::kInvalid;
    }
  }
  return SortOrderSupport::kOk;
}

bool TableReadTraits::FromMetadata(const iceberg::TableMetadata& metadata,
                                   TableReadTraits* out, std::string* error) {
  *out = TableReadTraits{};
  out->properties = metadata.properties.configs();

  auto order_r = metadata.SortOrder();
  if (!order_r.has_value()) {
    if (error) *error = "TableMetadata::SortOrder: " + order_r.error().message;
    return false;
  }
  const auto& order = order_r.value();
  if (!order || order->is_unsorted()) return true;

  auto schema_r = metadata.Schema();
  if (!schema_r.has_value()) {
    if (error) *error = "TableMetadata::Schema: " + schema_r.error().message;
    return false;
  }
  const auto& schema = schema_r.value();

  if (CheckSortOrder(*schema, *order, error) != SortOrderSupport::kOk)
    return false;

  std::vector<SortKey> keys;
  for (const auto& sf : order->fields()) {
    SortKey key;
    key.field_id = sf.source_id();
    key.ascending = sf.direction() == iceberg::SortDirection::kAscending;
    key.name = std::string(FieldById(*schema, sf.source_id())->name());
    keys.push_back(std::move(key));
  }
  out->sort_keys = std::move(keys);
  return true;
}

}  // namespace primeparts::scan
