// primeparts table schemas — the concrete Iceberg schema definitions for the
// staging warehouse and derived tables. Kept out of writer.{h,cc} so the writer
// stays a general, schema-agnostic parquet primitive (it takes a schema via
// WriterConfig; it does not define any). Anything that creates a primeparts
// table includes this header for the field-id-stable schema and the p_bucket
// partition spec.

#pragma once

#include <memory>
#include <string>

namespace iceberg {
class Schema;
class PartitionSpec;
class SortOrder;
}

namespace primeparts {

// Staging-warehouse schemas. All fields required; field IDs contiguous:
//   primes:     p=1, k=2, prime_rank=3, p_bucket_version=4, p_bucket=5
//   partitions: p=1, m_k=2, n_k=3, q_k=4, prime_rank=5,
//               p_bucket_version=6, p_bucket=7
// prime_rank is the prime-counting function π(p) — the first row in primes
// (smallest present prime, p=3) carries prime_rank=2 because π(2)=1 and p=2 is
// intentionally absent. The backfill pass materializes the column in place
// before the staging metadata is published.
std::shared_ptr<iceberg::Schema> PrimesSchema();
std::shared_ptr<iceberg::Schema> PartitionsSchema();

// Identity partition spec over (p_bucket_version, p_bucket) for a schema that
// carries those columns. Returns nullptr + *error if either field is missing.
std::shared_ptr<iceberg::PartitionSpec> BucketPartitionSpec(
    const iceberg::Schema& schema, std::string* error);

std::shared_ptr<iceberg::SortOrder> PAscendingSortOrder(
    const iceberg::Schema& schema, std::string* error);

}  // namespace primeparts
