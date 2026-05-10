# Log-Bucket Repartition Spec

Repartitioning design for the `funbuns` Iceberg tables on the production
warehouse at `/media/extssd/research/dioph.pp/data/iceberg/`. Implementation
handoff, not yet a migration runbook.

## Goal

Rebuild into a range-local layout that:

- Keeps Parquet files near `1 GiB` with `~4` row groups, with no small
  trailing files (file count per bucket is dynamic; see §Planner Sizing).
- Uses a partition transform Iceberg readers can interpret without custom code.
- Aligns *bucket boundaries* (p-cuts) between the primes table and the
  partitions table so HS2/MR3 and polars prune both tables identically.
  File-level alignment within a bucket is not required.
- Carries `prime_rank` so generation, resume, and bucket-cut snapping all
  work in dense integer space without recomputing `pi(p)`.

## Tables and Naming

The new staging warehouse drops the `decompositions` name from the table
identifier. Two tables under namespace `funbuns`:

| Table             | Rows                          | Notes                                  |
|-------------------|-------------------------------|----------------------------------------|
| `primes`          | one per prime `p` incl. `k=0` | universe of primes ≤ current max       |
| `partitions`      | one per `(p, m_k, n_k, q_k)`  | fact table; non-obstructed `p` only    |

The "decompositions" code identifier is retired at the same cutover; downstream
readers retarget to `funbuns.partitions`. In prose throughout the codebase the
`(m_k, n_k, q_k)` tuples remain "partitions of `p`."

## Current Warehouse Shape

Production was originally written with `truncate[10000000000](p)` for both
tables. On 2026-05-10 the source metadata was normalized in place to
`void(p)` so iceberg-rust can load the tables without the truncate-width
parser blocker. Data files remain at their original paths:

```text
warehouse/funbuns/primes/data/p_trunc=N/*.parquet         (57 files)
warehouse/funbuns/decompositions/data/p_trunc=N/*.parquet (198 files)
```

Audit on 2026-05-07: every referenced file exists; no unreferenced parquets
on disk in either tree. Live row counts (manifest-aggregated): 21,699,850,257
primes / 40,842,554,340 decompositions.

Catalog: `SqlCatalog` (sqlite) at `…/iceberg/catalog.db`, mirrored to HMS via
`scripts/sync_hms.py` for HS2/MR3. The previous metadata locations still point
at the pre-normalization `truncate[10000000000](p)` specs.

## Partition Spec

Materialized two-field identity, with both tables carrying the same boundaries:

```text
p_bucket_version: int    # field id allocated fresh, never reused
p_bucket:         int    # field id allocated fresh, never reused
prime_rank:       long   # field id allocated fresh, never reused
```

PartitionSpec on each table:

```text
identity(p_bucket_version)
identity(p_bucket)
```

Two fields rather than one because partition spec evolution rejects parameter
changes on an existing transform (`Transform::dedup_name`). Bumping
`p_bucket_version` is how we re-bucket without rewriting old manifests: old
files keep `(v=1, bucket=k)`, new files write `(v=2, bucket=k')`, predicate
pushdown still works via `Transform::project` per spec.

Boundaries are **shared between primes and partitions** by p-range. File
sizing is **independent** per table — see §File Sizing.

### Why materialize (not Bucket / Truncate)

Iceberg's portable transform set is `Identity, Bucket(N), Truncate(W), Year,
Month, Day, Hour, Void`. `Bucket(N)` is hash-mod (no range locality);
`Truncate(W)` is fixed-width and can't widen as prime density falls. Cuts
that fall on observed prime ranks (so all decompositions of one prime stay
in one bucket) and produce roughly equal bytes per bucket aren't expressible
as any stock transform, so the canonical portable path is to materialize the
bucket assignment as a stored integer column and partition by `identity` over
it.

### Partition path encoding

Iceberg paths are Hive-style `<field>=<value>/`, derived from
`Transform::to_human_string`. The `=` is spec-mandated and shared by every
reader (PyIceberg, iceberg-rust, HS2/MR3). Removing it would fork the spec.
Shell-side friction is mitigated by quoting paths or escaping `=` at the
shell layer, not at the warehouse layer.

## Sort Order

- `primes`: `p ASC`
- `partitions`: `(p ASC, m_k ASC)`

`p ASC` satisfies the order of `identity(p_bucket)` (the partition coordinate
is monotone in `p`), so within a partition files are p-sorted and Parquet
row-group statistics prune range scans. Sort order is declared on the table
but not enforced by PyIceberg on append; the writer enforces it.

## File Sizing

Both tables target `~1 GiB` files with `~4` row groups, but the per-prime byte
weights differ:

- A row in `primes` is one row per prime.
- A row in `partitions` is `k` rows per prime; `k̄ ≈ 1.882` from the
  per-100M-rank diagnostic, with stddev/mean of 0.87% across 217 windows.
  `k_median = 2`, `k_p99 = 6`, `k_max ∈ {13..16}` everywhere.

### Calibration (2026-05-07)

Manifest-aggregated stats on the live warehouse (no `prime_rank` yet):

| Table        | Rows           | Bytes (zstd-3) | B/row |
|--------------|----------------|----------------|-------|
| primes       | 21,699,850,257 | 31.88 GB       | 1.469 |
| decomp       | 40,842,554,340 | 183.40 GB      | 4.491 |

Drift across p-range < 3%, so one number per table is enough.

Empirical delta from adding `prime_rank` at pyarrow defaults
(`scripts/measure_prime_rank_bpr.py`, representative file per table, zstd-3
with ~200M-row row-groups):

| Table         | B/row no rank | B/row with rank | Δ      |
|---------------|---------------|-----------------|--------|
| primes        | 1.474         | 2.500           | +1.026 |
| partitions    | 4.513         | 5.097           | +0.584 |

These numbers are **superseded** by the encoding-pinned calibration below;
they are kept as historical record. The naive +1.026 B/row delta on primes
prompted investigation into per-column encoding (see §Encoding-Pinned
Calibration) and the result is that `prime_rank` storage is essentially
free.

### Encoding-Pinned Calibration (2026-05-09)

Two changes shift the per-row byte budget vs. the 2026-05-07 numbers:

1. **`commit_seq` and the per-file KV footer are dropped** in the new
   schema. Empirically both contribute ~0 bytes/row (commit_seq RLEs to
   nothing; the live KV block is ~120 B/file across 5 keys). Analytics
   that previously read KV (`k_histogram`, `n_primes`, etc. as documented
   in `markdown/iceberg_data_setup.md`) move to Iceberg materialized
   views.

2. **`p` and `prime_rank` are pinned to `DELTA_BINARY_PACKED + zstd-3`** in
   the new writer. Measured per-column cost on a 200M-row sample
   (`scripts/measure_prime_rank_encoding.py`):

| Column      | pyarrow default | DELTA + zstd-3 | × smaller | Read M rows/s (default → DELTA) |
|-------------|-----------------|----------------|-----------|----------------------------------|
| prime_rank  | 1.026           | 0.005          | 191×      | 86 → 486                         |
| p           | 1.103           | 0.741          | 1.49×     | 79 → 326                         |

DELTA is a clean win on both axes: smaller bytes *and* faster decode,
because the bit-packed payload stays in cache and the inner loop is
branch-predictable. No retrieval-side cost makes derive-on-the-fly for
`prime_rank` uninteresting — store it.

Resulting BPR for the new schema:

| Table        | B/row (new schema) | Source                                |
|--------------|--------------------|---------------------------------------|
| primes       | **1.114**          | live 1.469 − Δp(0.36) + prime_rank(0.005) |
| partitions   | **~4.0**           | extrapolated; remeasure during rewrite calibration |

The partitions number is extrapolated: `p` should compress further than
on primes due to within-prime duplicates (deltas mostly 0 with occasional
prime-gap jumps), and `prime_rank` similarly compresses tighter than its
already-low primes value. Conservative estimate: −0.5 B/row from `p`
under DELTA, +~0.05 B/row for `prime_rank` (vs +0.584 at PLAIN). Worth
remeasuring on a partitions sample with the encoding pinned before
locking the planner.

Per-prime byte-weight ratio: ~3.6× (4.0 / 1.114 × k̄≈1.882) — close to
the 3.84× from the prior calibration. This is the *consequence* of the
schema, not an enforced constraint.

### Planner Sizing

> **Stale numbers.** The bucket count, per-bucket size, and `F_*` table
> below were computed from the 2026-05-07 BPR (2.500 / 5.097). Under the
> 2026-05-09 encoding-pinned constants (1.114 / ~4.0) the dataset is
> ~24 GB primes + ~163 GB partitions, and the bucket count drops
> substantially. Bucket-sizing approach is the next open design
> discussion; this section will be rewritten once that's settled.

With the post-rank B/row numbers above, total dataset = 54.3 GB primes +
208.2 GB partitions. Each bucket targets ~4 GiB on the primes side (~1.72B
primes), giving **~13 buckets** total.

`F` (file count within a bucket) is **dynamic per bucket per table**:

```text
F_table_k = round(B_table_k / 1 GiB)
file_size_table_k = B_table_k / F_table_k
```

This avoids small trailing files: a bucket with 1.5 GiB doesn't split into
one 1 GiB file plus one 500 MiB file; it produces one 1.5 GiB file. A bucket
with 3.5 GiB produces three ~1.17 GiB files, not three 1 GiB files plus a
500 MiB tail. Worst case is a single 1.5 GiB file when `B = 1.5 × 1 GiB`
exactly and rounding goes either direction.

Expected per-bucket layout:

| Bucket   | primes B  | F_primes | partitions B | F_partitions |
|----------|-----------|----------|--------------|--------------|
| 1..12    | 4.29 GiB  | 4        | 16.47 GiB    | 16           |
| 13       | ~2.74 GiB | 3        | ~10.5 GiB    | 11           |

Bucket 13 holds the trailing ~1.1B primes left after 12 full buckets of
1.72B each. Both F values float to keep all files at ~1 GiB.

The two tables share **bucket boundaries** (the same prime is in `p_bucket=k`
in both); files within a bucket are not aligned across tables. The k-th
primes file in a bucket does not cover the same p-subrange as the k-th
partitions file. The per-prime byte-weight ratio of 3.84× (`5.097 / 2.500 ×
k̄ = 1.882`) is the *consequence* of the calibration, not an enforced
constraint — `F_partitions ≈ 4 × F_primes` falls out of computing each from
its own bucket's bytes.

### Backfill Deferral

Backfilling `prime_rank` into the live tables in place is still unnecessary.
The old blocker was the `truncate[10_000_000_000]` transform; current source
metadata has been normalized to `void(p)`, so iceberg-rust can load the source
tables. Since the repartition pass rewrites every file under the new
identity(p_bucket) spec anyway, `prime_rank` is computed in-stream during that
pass rather than through a separate backfill writer.

## Bucket Boundary Table

One shared Iceberg table already exists under `funbuns`:

```text
funbuns.boundaries
  p_bucket_version: int
  p_bucket:         int
  p_min:            long
```

`p_min` rows define lower bounds. The writer derives each bucket's exclusive
upper bound from the next row, with the final frontier bucket open-ended. The
table is small enough to keep warmed in memory in the writer process. A new
bucket plan is a new `p_bucket_version` row-set; existing rows are immutable.

Sharing the table reflects the shared-p alignment: both output tables use the
same bucket id for the same prime. Per-table file counts are computed during
the rewrite from calibration constants and observed source row counts; they
are not stored in the boundary table.

## Bucket Planner

> **Pseudocode below uses the stale 2026-05-07 BPR (2.500). Replace with
> the 2026-05-09 constants (BYTES_PER_ROW_PRIMES_RANKED = 1.114,
> BYTES_PER_ROW_PARTITIONS_RANKED ≈ 4.0) once bucket-sizing strategy is
> settled.** The streaming-scan shape is also up for revision: with
> `total_primes_rows` known and BPR known, boundaries are determined
> arithmetically — rank-to-p resolution at boundary positions is the only
> data-dependent step, and that's a constant-bounded set of point reads
> against manifest stats, not a full scan.

The planner is a single streaming pass over `primes` in p-order. Sized by
the primes table (which fills the bucket to ~4 GiB ≈ F_primes · 1 GiB);
partitions follow exactly the same p-cuts.

```text
target_primes_bytes = 4 GiB        # 4 × 1 GiB target file size

cum_primes_bytes = 0
bucket_id = 0
bucket_min_rank = 0
bucket_min_p = primes.first().p

for (rank, p) in enumerate(primes_in_p_order):
    cum_primes_bytes += BYTES_PER_ROW_PRIMES_RANKED   # 2.500
    if cum_primes_bytes >= target_primes_bytes:
        emit boundary row(
            bucket_id, bucket_min_rank, rank, bucket_min_p, p,
            expected_primes_rows = rank - bucket_min_rank + 1,
            expected_partitions_rows = ⌈k̄ · (rank - bucket_min_rank + 1)⌉,
            expected_primes_bytes = round(cum_primes_bytes),
            expected_partitions_bytes = round(
                k̄ · BYTES_PER_ROW_PARTITIONS_RANKED · (rank - bucket_min_rank + 1)
            ),
            target_files_primes = round(expected_primes_bytes / 1 GiB),
            target_files_partitions = round(expected_partitions_bytes / 1 GiB),
        )
        bucket_id += 1
        bucket_min_rank = rank + 1
        bucket_min_p = next_p
        cum_primes_bytes = 0

# trailing rows after the last full cut emit the final (smaller) bucket row.
```

Boundaries snap to prime ranks by construction (the cut happens *between*
primes). The planner doesn't need log2(p) or any continuous coordinate; the
dense `prime_rank` integer is sufficient.

A small +1.6% headroom factor on high-`p` buckets accounts for the slight
upward drift of `k_mean` at the high end of the diagnostic. Apply it to
`expected_partitions_bytes` and `target_files_partitions` for buckets above
some `min_rank` threshold (e.g., last quartile of the dataset).

## prime_rank Materialization

`prime_rank` is materialized during the repartition pass itself, not as a
separate backfill. Three things make this efficient:

1. The repartition pass already reads `primes` in p-order via manifests, so a
   running counter assigns rank with no extra I/O. p=2 is intentionally
   absent from the table; rank 0 corresponds to the smallest present prime
   (p=3), so `prime_rank` is contiguous over the rows that exist rather than
   matching the standard prime-counting offset.
2. The pass also reads `partitions` in p-order; a streaming sort-merge by `p`
   against the rank-bearing primes stream assigns `prime_rank` to each
   `partitions` row in the same scan.
3. Schema evolution is already done (`prime_rank` field_id 4 in primes,
   field_id 6 in partitions, both nullable; see commit history). Existing
   files read NULL until the staging warehouse replaces them.

## Migration Sequence

```text
1. Freeze writes  [DONE -- ingest is already idle]
   |
   v
2. Standing check  [DONE -- 0 orphan files, 0 missing references]
   +-- 57/57 primes files referenced and present
   +-- 198/198 decomposition files referenced and present
   |
   v
3. Schema evolution  [DONE -- 2026-05-07]
   +-- prime_rank: long, nullable, field_id 4 in primes, 6 in decompositions
   +-- HMS synced
   +-- old files read NULL for the new column (no in-place backfill;
       see §prime_rank Materialization)
   |
   v
4. Calibrate bytes-per-row  [DONE -- 2026-05-07]
   +-- live: 1.469 (primes) / 4.491 (decomp)
   +-- ranked: 2.500 (primes) / 5.097 (partitions) -- see §File Sizing
   |
   v
5. Boundary table  [DONE -- 2026-05-10]
   +-- `funbuns.boundaries` populated from `native/bin/boundary_primes.tsv`
   +-- schema: `(p_bucket_version, p_bucket, p_min)`
   |
   v
6. Normalize source partition metadata  [DONE -- 2026-05-10]
   +-- `scripts/normalize_void_partitions.py --apply`
   +-- rewrote current manifests and manifest lists to `void(p)`
   +-- SqlCatalog now points at metadata version 00003 for both source tables
   +-- data files were not rewritten
   |
   v
7. Rewrite into staging warehouse
   +-- staging at /media/extssd/research/dioph.pp/data/iceberg-staging/
   +-- new tables: funbuns.primes, funbuns.partitions
   +-- partition spec: identity(p_bucket_version, p_bucket)
   +-- sort order declared and enforced
   +-- writer: iceberg-rust ClusteredWriter (input pre-sorted by p_bucket then p)
   +-- per-bucket per-table file size = expected_*_bytes / target_files_*
   +-- prime_rank assigned in-stream: running counter on primes, sort-merge
       by p for partitions
   |
   v
8. Validate staging
   +-- row counts match (21.70B primes, 40.84B partitions)
   +-- sum(primes.k) == rows(partitions)
   +-- prime_rank contiguous and unique in primes (rank 0 = smallest present
       prime; p=2 is intentionally absent)
   +-- partitions.prime_rank ⊂ primes.prime_rank
   +-- files sorted, row-group counts match target
   |
   v
9. Cutover
   +-- SqlCatalog: register new tables, retire old
   +-- HMS: scripts/sync_hms.py for HS2/MR3 visibility
   +-- archive old layout in place (tarball + delete)
```

## Writer Path

The old `crates/primeparts-compact` implementation was removed. The fresh
crate starts from a source-load check:

```text
cargo run --manifest-path crates/Cargo.toml -p primeparts-compact -- check-source
```

The staging writer should use iceberg-rust's `PartitioningWriter` chain:

- `ClusteredWriter` is correct here: input is pre-sorted by `p_bucket` then
  `p`, so memory stays bounded to one partition's in-flight rows.
- `PartitionValueCalculator::try_new(spec, schema)` validates the spec
  resolves cleanly; reuse one calculator for the writer's lifetime.
- `Transform::to_human_string` derives partition path strings; do not
  format manually.

The removed hand-routed path was justified for the earlier truncate(1e10)
compaction. For the new spec, byte-precise flushing can stay if needed, but
partition routing, path encoding, and field-id matching go through the writer
chain.

## Query And Lookup

For range queries:

```text
WHERE p BETWEEN lo AND hi
```

Engines apply the predicate; manifest stats prune by `p` min/max within
buckets. `p_bucket` partition pruning narrows the manifest scan first.

For direct lookup:

```text
1. Load `funbuns.boundaries` for the active version into memory.
2. Binary-search by p or prime_rank to candidate p_bucket(s).
3. Scan only those bucket partitions.
4. Apply exact predicate.
```

## Iceberg Compatibility

The design uses only stock transforms (`Identity`) over materialized columns,
which keeps the layout portable across PyIceberg, iceberg-rust readers, HMS-
backed engines, and any future tooling. Spec evolution is supported: adding
buckets under a new `p_bucket_version` does not invalidate old manifests; old
files keep their `partition_spec_id`, new files use the current one,
`Transform::project` translates predicates per spec.

## Open Knobs

- Headroom factor — provisional 1.016 from diagnostic; revisit if calibration
  on the high-`p` end disagrees.
- Append routing during the freeze window — out of scope for this one-time
  staging rewrite.

## Implementation Notes

- Iceberg manifests are the source of truth; do not glob the data tree.
- Source tables are now `void(p)` in current metadata; do not reintroduce the
  huge truncate transform as a reader dependency.
- Do not reuse field ids when adding `prime_rank`, `p_bucket_version`,
  `p_bucket`. Fresh ids per the `iceberg_schema.py` rules.
- **Drop `commit_seq` in the new schema.** It duplicates the Iceberg
  snapshot sequence number and was the source of repair-script debt. The
  Iceberg-native `Snapshot.sequence_number` covers ordering needs.
- **Drop the per-file `funbuns.*` KV footer.** Empirically ~120 B/file
  across 5 keys (file 5 of the documented 13 are actually written:
  `table`, `commit_seq`, `n_rows`, `p_min`, `p_max`). Analytics that
  consumed it (`k_histogram`, `n_primes`, etc.) move to Iceberg
  materialized views.
- **Pin column encodings in the writer**: `prime_rank` and `p` use
  `DELTA_BINARY_PACKED + zstd-3`. Configured on `WriterProperties` via
  `set_column_encoding(ColumnPath, Encoding::DELTA_BINARY_PACKED)` and
  `set_dictionary_enabled(false)` for those columns. Measurement: 191×
  smaller `prime_rank`, 1.49× smaller `p`, with reads 4–6× faster
  (`scripts/measure_prime_rank_encoding.py`).
- Calibration evidence:
  - k diagnostic at `markdown/k_window_stats.jsonl` (217 windows × 100M ranks)
  - BPR at `scripts/measure_prime_rank_bpr.py`
  - encoding ceiling at `scripts/measure_prime_rank_encoding.py`
