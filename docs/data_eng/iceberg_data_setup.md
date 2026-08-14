# Iceberg data setup

Schema and warehouse-layout reference for the `primeparts` Iceberg warehouse. For
the catalog of record (native LMDB-backed `SqlCatalog`, fronted by `pp-catalogd`)
see `irc_catalog_design.md`. Bucket-layout rationale, BPR calibration, and
`prime_rank` materialization: `log_bucket_repartition_spec.md`.

## Warehouse location

```
/media/extssd/research/dioph.pp/data/ib-staging/
└── primeparts/
    ├── primes/{data,metadata}/
    └── partitions/{data,metadata}/
```

The catalog of record is a native LMDB-backed `SqlCatalog`
(`<warehouse>/catalog.lmdb`), reached in-process via `MakeLocalCatalog` or over
HTTP through `pp-catalogd`; it holds each table's current `metadata_location`
pointer. Base table data stays as Parquet + `metadata/*.metadata.json` on the
filesystem.

`MIN(p)` is 3 — p=2 is intentionally absent (see prime_rank semantics). Size
memory / cardinality estimates by running `SELECT MIN, MAX, COUNT(*)` against the
current snapshot.

## Tables

Two first-class tables under namespace `primeparts`. (`primes_k0`, the `k=0`
subset, is a derived base table the covering sieve consumes.)

| Table | Partition spec |
|---|---|
| `primeparts.primes` | `identity(p_bucket_version, p_bucket)` |
| `primeparts.partitions` | `identity(p_bucket_version, p_bucket)` |

Every prime `p` appears in `primes` exactly once. Obstructed primes (`k=0`, no
partition) are present with `k=0` and have no rows in `partitions`. The
cross-table invariant is `sum(primes.k) == rows(partitions)`.

## Schemas

Source of truth: `native/src/schemas.cc` (`PrimesSchema`, `PartitionsSchema`). All
fields required; field IDs contiguous.

**`primeparts.primes`**

| field_id | name | type | notes |
|---|---|---|---|
| 1 | `p` | Long | the prime |
| 2 | `k` | Int | # partitions; 0 = obstructed |
| 3 | `prime_rank` | Long | π(p) |
| 4 | `p_bucket_version` | Int | partition coord |
| 5 | `p_bucket` | Int | partition coord |

**`primeparts.partitions`**

| field_id | name | type | notes |
|---|---|---|---|
| 1 | `p` | Long | prime |
| 2 | `m_k` | Int | exponent on 2, ≥ 1 |
| 3 | `n_k` | Int | exponent on q, ≥ 1 |
| 4 | `q_k` | Long | odd prime base |
| 5 | `prime_rank` | Long | π(p), replicated per row from `primes` |
| 6 | `p_bucket_version` | Int | partition coord |
| 7 | `p_bucket` | Int | partition coord |

### Physical vs iceberg schema

The iceberg schemas above are the catalog contract. The identity-partition source
fields `p_bucket_version` and `p_bucket` are omitted from the physical parquet
schema — their values live only in each DataFile's manifest partition tuple, and
iceberg readers synthesize the columns at read time. On-disk parquet:

- `primes`: `p, k, prime_rank` — 3 columns.
- `partitions`: `p, m_k, n_k, q_k, prime_rank` — 5 columns.

`IcebergToArrowSchemaWithFieldIds(..., partition_spec)` (`native/src/writer.cc`)
strips identity-transform partition source IDs from the arrow schema before file
creation.

### prime_rank semantics

`prime_rank` is the prime-counting function π(p): the count of primes ≤ p. `p=2`
(π=1) is intentionally absent, so the first present row carries `p=3,
prime_rank=2`. Each subsequent row in p-sorted order increments `prime_rank` by
one.

### Sort order

- `primes`: `p ASC`
- `partitions`: `(p ASC, m_k ASC)`

Declared on the table; the writer enforces it (input arrives p-sorted).

### Encoding policy

`native/src/writer.cc` + the per-table `delta_columns` passed by the producer:

- `p`, `prime_rank`, `q_k`: `DELTA_BINARY_PACKED + zstd-3`, dictionary off.
  Monotone (or segment-monotone, for `q_k` under `(p, q_k)` order) → delta beats
  dictionary.
- `m_k`, `n_k`: dictionary + zstd-3 (default). Range-constrained and non-monotone,
  so dictionary wins; `n_k` is near-free (mostly `n_k=1`).
- Row groups: ~4 per file.

## Bucketing

Data is bucketed on `(p_bucket_version, p_bucket)` for read/analysis pruning, not
write parallelism. `AlignedBucketWriter` (`native/src/aligned_writer.cc`) shapes
the layout byte-driven:

- **Row group = snap unit.** Closes at the first `p`-boundary at-or-after a byte
  target. No `p` straddles a row group.
- **`primes` is the size reference.** One row per atom, so a byte-arithmetic cut
  lands on a `p`-boundary directly; `primes` files land ≈ 1 GiB. `partitions`
  follows the identical cut sequence (binary search on `p` to the cut value), so
  the two tables' row groups cover identical p-spans — anti-join alignment holds
  by construction. `partitions` row groups come out larger (more rows/atom, wider
  schema).
- **Bucket = byte threshold over whole files.** A bucket closes at the first file
  boundary at-or-after its threshold.

The bucket record is the manifests: every `DataFile` carries `(p_bucket_version,
p_bucket)` in its partition tuple plus p/rank min-max stats. A bucket map is a
manifest aggregation; resume reads the open bucket's fill from the sum of its
files' bytes. `p_bucket_version` lets a re-bucketing land alongside existing files
without rewriting them: new files get the next version, old files keep theirs, and
`Transform::project` translates predicates per the spec.

## Filesystem layout

```
.../ib-staging/primeparts/
├── primes/
│   ├── data/p_bucket_version=V/p_bucket=B/primes_v{VVVV}_b{NNNNNN}_{SSSS}.parquet
│   └── metadata/0000N-*.metadata.json + manifest avro
└── partitions/
    ├── data/p_bucket_version=V/p_bucket=B/partitions_v{VVVV}_b{NNNNNN}_{SSSS}.parquet
    └── metadata/0000N-*.metadata.json + manifest avro
```

## Writing (native, iceberg-cpp + IRC)


- `primes`: `p` strictly increasing within and across files; `prime_rank`
  contiguous and unique, first value 2, last value `2 + rows(primes) - 1`.
- `partitions`: each `prime_rank` appears exactly `k_p` times; rows sorted by
  `(p, m_k)`.
- `sum(primes.k) == rows(partitions)`.
- Every `partitions.p` appears in `primes.p`; every `partitions.prime_rank`
  appears in `primes.prime_rank`.
- A given `p` lands in the same `p_bucket` in both tables.
