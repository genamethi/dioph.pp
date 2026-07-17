#include "primeparts/scan/table_traits.h"

#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/sort_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_properties.h"
#include "iceberg/transform.h"

namespace primeparts::scan {

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

  std::vector<SortKey> keys;
  for (const auto& sf : order->fields()) {
    if (!sf.transform() ||
        sf.transform()->transform_type() != iceberg::TransformType::kIdentity) {
      if (error) {
        *error = "NotImplemented: declared sort order field " +
                 std::to_string(sf.source_id()) + " uses transform '" +
                 (sf.transform() ? sf.transform()->ToString()
                                 : std::string("null")) +
                 "'; TableReadTraits resolves identity transforms only — "
                 "resolving this order requires applying the transform to "
                 "source values when ordering tasks and slicing batches";
      }
      return false;
    }
    SortKey key;
    key.field_id = sf.source_id();
    key.ascending = sf.direction() == iceberg::SortDirection::kAscending;
    for (const auto& f : schema->fields()) {
      if (f.field_id() == sf.source_id()) {
        key.name = std::string(f.name());
        break;
      }
    }
    if (key.name.empty()) {
      if (error) {
        *error = "sort order references unknown field id " +
                 std::to_string(sf.source_id());
      }
      return false;
    }
    keys.push_back(std::move(key));
  }
  out->sort_keys = std::move(keys);
  return true;
}

}  // namespace primeparts::scan
