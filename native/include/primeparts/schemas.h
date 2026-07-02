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
std::shared_ptr<iceberg::Schema> BoundariesSchema();

// Hit-mask schema for primeparts.mdiff_k{K}: one row per prime with exactly
// k(p)==K representations. The K sorted hit positions are packed into a single
// int64 bitmask (bit m set iff m is a hit position; m_max ~ 39 < 64, so the
// representation is k-agnostic). Field ids contiguous:
//   p=1                int64
//   hit_mask=2         int64   (OR of (1<<m) over the run; popcount == K)
// hit_mask is the SOLE stored truth. Everything else is derived and NOT stored:
// the pairwise index differences d = m_a - m_b (each a Mersenne index
// M_d = 2^d - 1) decode on demand via HitMaskDiffs(); the translation-invariant
// shape (hit_mask >> ctz) and the anchor phase (m_min mod period, the coset) are
// pure functions of hit_mask; prime_rank = π(p) is recomputable. The table is
// UNPARTITIONED and physically ordered by p — derived groupings (gap d, coset)
// belong in views, not columns. The `k` argument is validated (>= 2) but the
// physical schema is identical for every k. Returns nullptr + *error on k < 2.
std::shared_ptr<iceberg::Schema> MdiffSchema(int k, std::string* error);

// primeparts.mersenne_factors: full factorization of each 2^d-1 over a working
// d-range, with primitivity precomputed. Unpartitioned. Field ids contiguous:
//   d=1 int32, prime=2 int64, exponent=3 int32, ord2=4 int32, is_primitive=5 int32
// ord2 = ord_2(prime) (the least index e with prime | 2^e-1). is_primitive
// (0/1) = (ord2 == d): prime is a primitive factor of 2^d-1 iff d is that least
// index. Stored so downstream never rescans earlier d-rows to decide primitivity.
std::shared_ptr<iceberg::Schema> MersenneFactorsSchema();

// primeparts.covering_primary / primeparts.covering_bleed_min: a covering system
// over the difference values x = p - 2^m, one row per congruence: x is covered
// when x ≡ residue (mod modulus). Unpartitioned. Field ids:
//   modulus=1 int64, residue=2 int64
std::shared_ptr<iceberg::Schema> CoveringSchema();

// Identity partition spec over (p_bucket_version, p_bucket) for a schema that
// carries those columns. Returns nullptr + *error if either field is missing.
std::shared_ptr<iceberg::PartitionSpec> BucketPartitionSpec(
    const iceberg::Schema& schema, std::string* error);

}  // namespace primeparts
