# Iceberg data setup

Canonical layout of the `primeparts` Iceberg warehouse: the bucket-
partitioned staging tables produced by `primeparts-generate` and the
`prime_rank` it materializes in stream. This is the schema / warehouse-layout
reference; for the **catalog of record** (native local LMDB-backed
`SqlCatalog`, fronted by the `pp-catalogd` IRC server) see
`markdown/data_eng/irc_catalog_design.md` and `HANDOFF.md` §5–6.

For surrounding context see:

- `markdown/data_eng/log_bucket_repartition_spec.md` — design rationale
  for the bucket layout, BPR calibration, `prime_rank` materialization,
  the `boundaries` table.

---

## Warehouse location

```
/media/extssd/research/dioph.pp/data/ib-staging/
└── primeparts/
    ├── boundaries/{data,metadata}/
    ├── primes/{data,metadata}/
    └── partitions/{data,metadata}/
```

The catalog of record is a native local LMDB-backed `SqlCatalog`
(`<warehouse>/catalog.lmdb`), reached in-process via `MakeLocalCatalog` or over
HTTP through `pp-catalogd`; it holds each table's current `metadata_location`
pointer. Base table data stays as Parquet + `metadata/*.metadata.json` on the
filesystem.

### Measured dataset basics (2026-05-27)

| Quantity | Value | Notes |
|---|---|---|
| `MAX(p)` | `564_575_405_239` ≈ 5.6×10¹¹ | Confirmed via `SELECT MAX(p) FROM primeparts.primes`. |
| `COUNT(*)` of `primes` | `21_698_850_257` ≈ 21.7 B | Consistent with π(5.6×10¹¹) ≈ 2.07×10¹⁰. |
| `MIN(p)` | 3 | p=2 intentionally absent (see prime_rank semantics). |
| `partitions` rows | ~40 B | ~99.9 % are n=1 edges. |
| q_k magnitude distribution | bit-widths 36-38 hold ~84 % of partition edges | Measured over the partition edges. |

Older docs referred to a "20 B-prime dataset" — that's the row *count*,
not the maximum p. Both numbers are linked by π(x) ≈ x/log(x); the
correct way to size memory or cardinality estimates is to run the
`SELECT MIN, MAX, COUNT(*)` queries against the current snapshot
rather than relying on these numbers as gospel.

---

## Tables

Three first-class tables under namespace `primeparts`. (`primes_k0`, the
`k=0` subset, is a derived base table the covering sieve consumes — see
HANDOFF §8.)

| Table                          | Rows         | Partition spec                              |
|--------------------------------|--------------|---------------------------------------------|
| `primeparts.primes`            | ~21.70 B     | `identity(p_bucket_version, p_bucket)`      |
| `primeparts.partitions`        | ~40.84 B     | `identity(p_bucket_version, p_bucket)`      |
| `primeparts.boundaries`        | small        | unpartitioned                               |

Every prime `p` appears in `primes` exactly once. Obstructed primes
(`k=0`, no decomposition) are present with `k=0` and have **no** rows
in `partitions`. The cross-table invariant is `sum(primes.k) ==
rows(partitions)`.

`partitions` was previously named `decompositions`. The table is
renamed; the prose terminology "partitions of p" / "(m_k, n_k, q_k)
tuple" is unchanged.

---

## Schemas

Source of truth: `native/src/writer.cc` (`PrimesSchema`,
`PartitionsSchema`, `BoundariesSchema`). All fields are **required**.
Field IDs are contiguous; the earlier scheme that retired ids 3/5/8 to
honor a retired `commit_seq` slot turned out to be incidental and was
dropped.

**`primeparts.primes`**

| field_id | name               | type   | notes                                       |
|----------|--------------------|--------|---------------------------------------------|
| 1        | `p`                | Long   | the prime                                   |
| 2        | `k`                | Int    | # decompositions; 0 = obstructed            |
| 3        | `prime_rank`       | Long   | π(p); see below                             |
| 4        | `p_bucket_version` | Int    | partition coord                             |
| 5        | `p_bucket`         | Int    | partition coord                             |

**`primeparts.partitions`**

| field_id | name               | type   | notes                                       |
|----------|--------------------|--------|---------------------------------------------|
| 1        | `p`                | Long   | prime                                       |
| 2        | `m_k`              | Int    | exponent on 2, ≥ 1                          |
| 3        | `n_k`              | Int    | exponent on q, ≥ 1                          |
| 4        | `q_k`              | Long   | odd prime base                              |
| 5        | `prime_rank`       | Long   | π(p), replicated per row from `primes`      |
| 6        | `p_bucket_version` | Int    | partition coord                             |
| 7        | `p_bucket`         | Int    | partition coord                             |

**`primeparts.boundaries`**

| field_id | name               | type   | notes                                       |
|----------|--------------------|--------|---------------------------------------------|
| 1        | `p_bucket_version` | Int    | active bucket plan                          |
| 2        | `p_bucket`         | Int    | 0..B-1, contiguous                          |
| 3        | `p_min`            | Long   | smallest prime in the bucket                |
| 4        | `rank_min`         | Long   | **row-count** offset (0-indexed), not π     |

### Physical vs. iceberg schema (post-2026-05-26 cutover)

The iceberg schemas above are the *catalog* contract. As of the
2026-05-26 cutover, the identity-partition source fields
**`p_bucket_version`** and **`p_bucket`** are **omitted from the physical
parquet schema** in both `primes` and `partitions`. Their values live
exclusively in each DataFile's manifest partition tuple; iceberg readers
(iceberg-cpp, polars `scan_iceberg`) synthesize the columns at read time from
the partition values.

This is per Iceberg spec — identity-partition source columns may be
resolved from the manifest rather than the data file. The on-disk
parquet schemas are now:

- `primes` (physical): `p, k, prime_rank` — 3 columns.
- `partitions` (physical): `p, m_k, n_k, q_k, prime_rank` — 5 columns.

Writer behavior is centralized in
`IcebergToArrowSchemaWithFieldIds(..., partition_spec)` in
`native/src/writer.cc`: identity-transform partition source IDs are
stripped from the arrow schema before parquet file creation.

### prime_rank semantics

`prime_rank` is the prime-counting function π(p): the count of primes
≤ p. `p=2` (the smallest prime, π=1) is intentionally absent from
`primes`, so the first present row carries `p=3, prime_rank=2`. Each
subsequent row in `primes` (in p-sorted order) increments `prime_rank`
by one.

`boundaries.rank_min[B]` is a **row-count** offset — the number of
primes in buckets 0..B-1. It is *not* a π-value. The relationship is
`prime_rank_of_first_row_in_bucket_B = boundaries.rank_min[B] + 2`.

### Sort order

- `primes`: `p ASC`
- `partitions`: `(p ASC, m_k ASC)`

Declared on the table; the writer enforces it (input arrives p-sorted
from manifest scan order).

### Encoding policy

Pinned in `native/src/writer.cc::ParquetWriterProperties`:

- `p`, `prime_rank`, `q_k`, `p_bucket_version`, `p_bucket` use
  `DELTA_BINARY_PACKED + zstd-3`, dictionary disabled. The monotone
  columns compress to a few bytes/row total; the bucket-coord constants
  RLE-collapse the same way under DELTA regardless of position in file.
- Everything else: default with zstd-3.
- Row group target: 240 M rows (~4 row groups per ~1 GiB file).

`commit_seq`, the legacy `funbuns.*` parquet KV footer, and all
`q_k == 0` sentinel rows are gone. Any analytics that used to consume
the KV block (`k_histogram`, `n_primes`, etc.) become derived read indexes —
deferred (HANDOFF §10 Phase 4).

---

## Filesystem layout

```
.../ib-staging/primeparts/
├── boundaries/
│   ├── data/boundaries_0000.parquet
│   └── metadata/0000N-*.metadata.json + manifest avro
├── primes/
│   ├── data/p_bucket_version=1/p_bucket={0..B-1}/primes_v0001_b{NNNNNN}_{SSSS}.parquet
│   └── metadata/0000N-*.metadata.json + manifest avro
└── partitions/
    ├── data/p_bucket_version=1/p_bucket={0..B-1}/partitions_v0001_b{NNNNNN}_{SSSS}.parquet
    └── metadata/0000N-*.metadata.json + manifest avro
```

The `p_bucket_version` field exists so that a future re-bucketing
(different boundaries) can land alongside the v1 files without
rewriting them: new files get `(v=2, bucket=k')`, old files keep
`(v=1, bucket=k)`, `Transform::project` translates predicates per the
spec. See log_bucket_repartition_spec.md.

`boundaries.p_min` defines lower bounds; each bucket's exclusive upper
bound is the next row's `p_min`, with the final bucket open-ended.

---

## Writing (native, iceberg-cpp + IRC)

All writes to `primeparts.*` are native C++ tools using iceberg-cpp's
`SourceTableReader` + `BucketParquetWriter` chain, committing through the local
catalog (`MakeLocalCatalog` or `pp-catalogd`). There is no Python in the commit
path.

The canonical pattern:

1. Read source files via `SourceTableReader::OpenMetadata`
   (iceberg-cpp manifest walk; no fs globbing).
2. Stream batches; transform/generate columns as needed; write new parquet
   files via the writer (identity-partition source columns supplied to the
   `iceberg::DataFile`, not in the parquet physical schema).
3. Commit the written `DataFile`s through `CommitFiles(catalog, warehouse,
   table, schema, spec, files)` — ensures the namespace, **load-or-creates** the
   table, then FastAppends in one snapshot. Resume-safe.

`generate` is the live producer: it writes the bucketed parquet for
`primes` + `partitions` and commits them via `CommitFiles` (partitions before
primes, so resume sees a hole-free primes frontier). `--rest-uri` routes the
commit through `pp-catalogd`; default is in-process `MakeLocalCatalog`.
(The earlier one-shot rewrite/backfill/drop-bucket binaries that exercised this
pattern were removed; the `MakeDataFile` partition-value convention survives in
git history if a future re-bucketing tool needs it.)

### Row-level deletes (covering sieve)

The covering-sieve persists progress as merge-on-read **position deletes** over
a `primes_k0` shallow clone, so the live rows are the current uncovered set.
**Verified GREEN (2026-05-31):** native `PositionDeleteWriter` → `RowDelta`
commit → delete-aware read-back works end to end (`pp_row_delta.{h,cc}`). Full
writeup: `markdown/data_eng/delete_primitive_spike.md`, `HANDOFF.md` §7.

---

## Cataloging — native local LMDB IRC

The catalog of record is a native local `iceberg::sql::SqlCatalog` backed by an
LMDB `CatalogStore` (`<warehouse>/catalog.lmdb`), reached in-process via
`MakeLocalCatalog` or over HTTP through the `pp-catalogd` IRC server. It serves
the IRC `/v1` surface (config, namespaces, tables, register, rename, commit,
metrics) from `rest-catalog-open-api.yaml`. Full design + write/commit model:
`markdown/data_eng/irc_catalog_design.md`, `HANDOFF.md` §5–6.

### Registering a table from on-disk metadata

`pp-catalog --register` registers an existing on-disk `metadata.json` into the
local catalog (`RegisterTable` through `MakeLocalCatalog`). Tools that produce
fresh data (e.g. `generate`) commit through `CommitFiles` instead, which
ensures the namespace and load-or-creates the table before a FastAppend.

---

## Reading

The hot path is native; SQL and polars are for ad hoc work.

### Native (iceberg-cpp) — canonical

`SourceTableReader::OpenMetadata(metadata_path, columns, filter)` is the
reader used inside every `primeparts-*` binary. It walks the iceberg
manifest, plans the scan, sorts files by `p` lower-bound, and exposes
an Arrow RecordBatch stream. **Do not** `fs::recursive_directory_iterator`
the data dir: file order, file presence, and even file paths are
manifest-derived and can shift on a future rewrite or compaction.

The covering sieve and modular filter are native-path-only workloads: native
code does the bitmask / primality-test / hash-table work over the manifest-
planned RecordBatch stream.

### Polars (ad hoc, read-only)

`pl.scan_iceberg` against the on-disk metadata.json works for ad hoc
range queries:

```python
import polars as pl

lf = pl.scan_iceberg(
    "/media/extssd/research/dioph.pp/data/ib-staging/primeparts/primes"
)
(
    lf.filter(pl.col("p") < 1_000_000_000)
      .filter(pl.col("k") == 0)
      .select(pl.len())
      .collect()
)
```

Manifest stats prune by `p_bucket`/`p_min`/`p_max` automatically. Do
not reach for `sink_parquet` to produce derived tables at this scale —
polars streaming OOMs on partitioned writes against the 21 B-row
inputs. Produce derived tables natively (source-scan → writer →
`CommitFiles`).

---

## Derived data

`primeparts.primes_k0` (the 17.85 % of primes with `k=0`) is a **base table**
the covering sieve consumes; the sieve operates on a shallow MOR clone of it
(`primes_k0_sieve`). Broader precomputed/derived read indexes (the old
materialized-view role) are **deferred** — approach left open (HANDOFF §10
Phase 4). The discarded `obstruction_catalog` comma-string-label artifact is
superseded by the position-delete sieve model and is not maintained.

---

## Schema evolution

For changes to the staging schemas:

1. Edit `PrimesSchema` / `PartitionsSchema` in `native/src/writer.cc`
   — add a field with the next contiguous `field_id`, type, and
   required flag.
2. Bump the table via the iceberg REST endpoint
   (`POST /iceberg/v1/namespaces/primeparts/tables/{table}` with a
   `UpdateSchema` body), or commit the change as part of the next
   binary's `CreateTable` if you're rebuilding the snapshot.
3. **Never reuse a field_id.** Add new ones at the end.
4. New columns default required→broken (existing files can't satisfy
   the constraint). Add as nullable, backfill, then promote to required
   in a second commit.

`prime_rank` itself went through this pattern historically (written nullable,
backfilled, then promoted to required). `generate` now materializes it in
stream, so fresh writes carry it required from the start.

---

## Reference invariants

After backfill + publish, the staging tables guarantee:

- `primes`: `p` strictly increasing within and across files;
  `prime_rank` contiguous and unique with first value 2 and last value
  `2 + rows(primes) - 1`.
- `partitions`: each `prime_rank` value appears exactly `k_p` times
  (where `k_p` is the corresponding row's `k` in primes); rows are
  sorted by `(p, m_k)`.
- `sum(primes.k) == rows(partitions)`.
- Every `partitions.p` appears in `primes.p`.
- Every `partitions.prime_rank` appears in `primes.prime_rank`.
- All buckets share boundaries: a given `p` lands in the same
  `p_bucket` in both tables.
