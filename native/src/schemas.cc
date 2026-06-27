#include "primeparts/schemas.h"

#include <string_view>
#include <utility>
#include <vector>

#include "iceberg/partition_field.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
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

// All fields required. The staging warehouse is the post-backfill target —
// prime_rank is materialized either inline by the rewriter or by a separate
// backfill pass before metadata publish, and downstream readers can rely on it
// being non-null. Field ids are contiguous.
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
          iceberg::SchemaField::MakeRequired(4, "q_k",              iceberg::int64()),
          iceberg::SchemaField::MakeRequired(5, "prime_rank",       iceberg::int64()),
          iceberg::SchemaField::MakeRequired(6, "p_bucket_version", iceberg::int32()),
          iceberg::SchemaField::MakeRequired(7, "p_bucket",         iceberg::int32()),
      },
      0);
}

std::shared_ptr<iceberg::Schema> MdiffSchema(int k, std::string* error) {
  if (k < 2) {
    if (error) *error = "MdiffSchema: k must be >= 2";
    return nullptr;
  }
  // k-agnostic: K hit positions packed into one int64 bitmask. The d-vector and
  // prime_rank are derived, not stored. popcount(hit_mask) recovers k.
  return std::make_shared<iceberg::Schema>(
      std::vector<iceberg::SchemaField>{
          iceberg::SchemaField::MakeRequired(1, "p",                iceberg::int64()),
          iceberg::SchemaField::MakeRequired(2, "hit_mask",         iceberg::int64()),
          iceberg::SchemaField::MakeRequired(3, "p_bucket_version", iceberg::int32()),
          iceberg::SchemaField::MakeRequired(4, "p_bucket",         iceberg::int32()),
      },
      0);
}

std::shared_ptr<iceberg::Schema> BoundariesSchema() {
  return std::make_shared<iceberg::Schema>(
      std::vector<iceberg::SchemaField>{
          iceberg::SchemaField::MakeRequired(1, "p_bucket_version", iceberg::int32()),
          iceberg::SchemaField::MakeRequired(2, "p_bucket",         iceberg::int32()),
          iceberg::SchemaField::MakeRequired(3, "p_min",            iceberg::int64()),
          iceberg::SchemaField::MakeRequired(4, "rank_min",         iceberg::int64()),
      },
      0);
}

std::shared_ptr<iceberg::Schema> MersenneFactorsSchema() {
  return std::make_shared<iceberg::Schema>(
      std::vector<iceberg::SchemaField>{
          iceberg::SchemaField::MakeRequired(1, "d",            iceberg::int32()),
          iceberg::SchemaField::MakeRequired(2, "prime",        iceberg::int64()),
          iceberg::SchemaField::MakeRequired(3, "exponent",     iceberg::int32()),
          iceberg::SchemaField::MakeRequired(4, "ord2",         iceberg::int32()),
          iceberg::SchemaField::MakeRequired(5, "is_primitive", iceberg::int32()),
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
      /*allow_missing_fields=*/false);
  if (!spec_result.has_value()) {
    if (error) *error = spec_result.error().message;
    return nullptr;
  }
  return std::shared_ptr<iceberg::PartitionSpec>(std::move(spec_result.value()));
}

}  // namespace primeparts
