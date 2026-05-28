# Iceberg data setup

Canonical layout of the `primeparts` Iceberg warehouse: the bucket-
partitioned staging tables produced by `primeparts-rewrite` and the
`prime_rank` backfill that lands on top. This doc describes the
**post-cutover** state — the legacy `funbuns.{primes,decompositions}`
warehouse with `commit_seq` partitioning and the pyiceberg-driven write
path have been retired.

For surrounding context see:

- `markdown/data_eng/log_bucket_repartition_spec.md` — design rationale
  for the bucket layout, BPR calibration, `prime_rank` materialization,
  the `boundaries` table.
- `markdown/data_eng/hive_mr3_stack.md` — Hive 4 + MR3 + HMS + k3s
  control plane; HMS serves the Iceberg REST Catalog at
  `http://192.168.1.202:9090/iceberg/v1/...` (the same OpenAPI surface
  iceberg-rest-fixture exposes, backed by HMS + MySQL).

---

## Warehouse location

```
/media/extssd/research/dioph.pp/data/ib-staging/
└── primeparts/
    ├── boundaries/{data,metadata}/
    ├── primes/{data,metadata}/
    ├── partitions/{data,metadata}/
    └── obstruction_catalog/{data,metadata}/    # covering-system passes
```

There is no SQLite catalog at this warehouse. The on-disk
`metadata/*.metadata.json` is the only persistent state; the catalog of
record is HMS at `metastore-rest:9090` (the IRC servlet), which holds
each table's current `metadata_location` pointer.

### Measured dataset basics (2026-05-27)

| Quantity | Value | Notes |
|---|---|---|
| `MAX(p)` | `564_575_405_239` ≈ 5.6×10¹¹ | Confirmed via `SELECT MAX(p) FROM primeparts.primes`. |
| `COUNT(*)` of `primes` | `21_698_850_257` ≈ 21.7 B | Consistent with π(5.6×10¹¹) ≈ 2.07×10¹⁰. |
| `MIN(p)` | 3 | p=2 intentionally absent (see prime_rank semantics). |
| `partitions` rows | ~40 B | ~99.9 % are n=1 edges. |
| q_k magnitude distribution | bit-widths 36-38 hold ~84 % of partition edges | See `q_k_histogram` MV (`MV_list.md`). |

Older docs referred to a "20 B-prime dataset" — that's the row *count*,
not the maximum p. Both numbers are linked by π(x) ≈ x/log(x); the
correct way to size memory or cardinality estimates is to run the
`SELECT MIN, MAX, COUNT(*)` queries against the current snapshot
rather than relying on these numbers as gospel.

---

## Tables

Three first-class tables under namespace `primeparts`, plus the
covering-systems derivative.

| Table                          | Rows         | Partition spec                              |
|--------------------------------|--------------|---------------------------------------------|
| `primeparts.primes`            | ~21.70 B     | `identity(p_bucket_version, p_bucket)`      |
| `primeparts.partitions`        | ~40.84 B     | `identity(p_bucket_version, p_bucket)`      |
| `primeparts.boundaries`        | small        | unpartitioned                               |
| `primeparts.obstruction_catalog` | grows w/ passes | `identity(covering_system)` (string label) |

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
2026-05-26 cutover (see `native/src/drop_bucket_cols_main.cc`), the
identity-partition source fields **`p_bucket_version`** and
**`p_bucket`** are **omitted from the physical parquet schema** in
both `primes` and `partitions`. Their values live exclusively in each
DataFile's manifest partition tuple; iceberg readers (iceberg-cpp, the
iceberg-handler in Hive 4, polars `scan_iceberg`) synthesize the
columns at read time from the partition values.

This is per Iceberg spec — identity-partition source columns may be
resolved from the manifest rather than the data file. The on-disk
parquet schemas are now:

- `primes` (physical): `p, k, prime_rank` — 3 columns.
- `partitions` (physical): `p, m_k, n_k, q_k, prime_rank` — 5 columns.

Writer behavior is centralized in
`IcebergToArrowSchemaWithFieldIds(..., partition_spec)` in
`native/src/writer.cc`: identity-transform partition source IDs are
stripped from the arrow schema before parquet file creation.

A read regression caused by this change broke Hive's vectorized
reader; the upstream fix lives in
`~/fluid/byo/repos/hive-mr3` branch `master4.2.0` commit `b5bc76328e`
and is baked into the local image `mr3-hive-4.2.0-local/hive:4.2.0`.
See `markdown/data_eng/hive_mr3_stack.md`.

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
the KV block (`k_histogram`, `n_primes`, etc.) move to Hive 4 + MR3
materialized views — see `hive_mr3_stack.md`.

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
`SourceTableReader` + `BucketParquetWriter` + `RestCatalog` chain.
There is no Python in the commit path.

The canonical pattern (see `native/src/drop_bucket_cols_main.cc` for
the current reference):

1. Read source files via `SourceTableReader::OpenMetadata`
   (iceberg-cpp manifest walk; no fs globbing).
2. Stream batches; transform columns as needed; write new parquet files
   via `.tmp` + atomic rename (resumable, never leaves corrupt files).
3. Build `iceberg::DataFile` records with the right partition tuple
   (identity-partition source columns supplied here, not in the parquet).
4. Commit via either:
   - `RestCatalog::RegisterTable(ident, metadata_location)` — when an
     on-disk `metadata.json` already exists; preserves snapshot
     history. Drops the catalog row first if it exists at a stale
     location, then registers the on-disk pointer.
   - `Catalog::CreateTable(...) + Transaction::NewFastAppend()` —
     fresh-publish; writes new metadata.json and registers it.

Three binaries currently in tree exemplify this pattern:

- **`primeparts-drop-bucket-cols`** (2026-05-26) — current reference.
  Strips the physical `p_bucket_version`/`p_bucket` columns from all
  files in `primes` and `partitions`, re-registers the result via IRC.
  Ran across the full warehouse in ~26 min total wall clock at
  Config A (see `hive_mr3_stack.md`). Idempotent: files whose physical
  schema already excludes the bucket cols are skipped.
- **`primeparts-backfill-rank`** — populates `prime_rank` in-place
  after the initial bucketed rewrite, then re-publishes via IRC. Same
  resume/atomic-rename discipline.
- **`primeparts-rewrite`** — design reference only; the legacy funbuns
  warehouse it consumed has been deleted. Retained because the bucket
  job split pattern is reusable. Any future re-bucketing should reuse
  the source-scan / partition-spec / writer chain.

The pyiceberg-driven `IcebergWriter.flush` write path of the legacy
warehouse is **retired** for `primeparts.*`. New ingest, refreshes,
and rewrites go native through IRC. `scripts/sync_hms.py` remains in
tree only for the residual funbuns-era tables; don't extend it.

---

## Cataloging — HMS as Iceberg REST Catalog

The HMS pod ships the Hive 4.2 IRC servlet at port 9090 on the host
(LoadBalancer Service `metastore-rest`). It serves the full Iceberg
REST OpenAPI surface: config, namespaces, tables, views, register,
rename, transactions/commit, metrics. Endpoint base:

```
http://192.168.1.202:9090/iceberg/v1/
```

This is the catalog of record for tables visible to HS2 / MR3 / Tez
and to any IRC client (iceberg-cpp, iceberg-rust, pyiceberg-with-REST,
DuckDB).

### Registering a table from on-disk metadata

After `primeparts-backfill-rank` writes a fresh metadata.json, register
it via:

```bash
curl -s -X POST \
    "http://192.168.1.202:9090/iceberg/v1/namespaces/primeparts/register" \
    -H 'Content-Type: application/json' \
    -d '{"name":"primes",
         "metadata-location":"/media/extssd/.../primeparts/primes/metadata/00001-....metadata.json"}'
```

Or, equivalently, pass `--rest-uri http://192.168.1.202:9090/iceberg`
to `primeparts-backfill-rank` and the binary will register through
iceberg-cpp's `RestCatalog`.

Per-namespace property `prefix` is unused at this deployment; namespaces
are addressed directly (`/iceberg/v1/namespaces/primeparts`, not
`/iceberg/v1/{prefix}/namespaces/primeparts`).

### Hive-catalog database location (separate from IRC)

IRC `RegisterTable` writes the iceberg-table row in HMS. It does **not**
update the namespace/database's `location_uri`, which Hive uses as the
default parent path for any tables Hive itself creates (materialized
views especially). If the database in HMS still points at an obsolete
warehouse path, Hive-created MVs will land at the old location even
though the source iceberg tables sit at the new one.

For `primeparts` this cutover step (run 2026-05-27) is:

```sql
ALTER DATABASE primeparts SET LOCATION
    'file:/media/extssd/research/dioph.pp/data/ib-staging/primeparts.db';
```

Run via `pixi run python scripts/hive_sql.py -e '...'`. Verify with
`DESCRIBE DATABASE primeparts`. Existing tables keep their absolute
paths (iceberg metadata is location-authoritative); only future
Hive-created tables inherit the new default.

### Stack lifecycle

Stack control is `pixi run kube-{up,down}`. If `kube-up` fails on
`127.0.0.1:6443`, k3s itself is down — `sudo systemctl start k3s`,
then retry. Full details in `hive_mr3_stack.md`.

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

For graph-traversal workloads (covering sieve, modular filter — see
`markdown/math/modular_filter_more_ideas.md`), the native path is
the only path. Hive's role is to feed sorted batches via MV-style
indexes; native code does the bitmask / primality-test / hash-table
work.

### SQL (Hive 4 + MR3 over HS2 NodePort 31140) — for transactional / MV refresh / multi-table joins

Any IRC client works. The thin JDBC wrapper `scripts/hive_sql.py` is
convenient for one-off DDL or counts:

```bash
pixi run python scripts/hive_sql.py -e \
    "SELECT k, COUNT(*) FROM primeparts.primes GROUP BY k"
```

HS2 reads through HMS, so anything registered via IRC is visible to
SQL with no extra step. Use SQL when the query genuinely needs
transactional semantics (MV creation, ATOMIC swap-in commits) or when
the planner's bucket-aligned join shapes are wanted. Don't reach for
SQL for ad hoc filter/range scans — polars or native iceberg-cpp
beats it on latency.

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
inputs. For derived tables: native + IRC if it's a compute-heavy
transform, Hive MV if it's a SQL-shaped aggregate (see `MV_list.md`).

---

## Derived data — Hive materialized views

Filtered / aggregated / index-shaped derivatives of `primes` and
`partitions` are built as Hive 4 + MR3 + Iceberg materialized views.
Each MV is itself an Iceberg table registered in HMS, queryable by
any IRC client. **They exist primarily as precomputed indexes for
native consumers** — the modular-filter and covering-sieve pipelines
read them via iceberg-cpp; SQL queries against them are secondary.

See [`MV_list.md`](MV_list.md) for the running inventory: what's
built, what's planned, what was rejected and why, plus session-level
settings that worked.

Quick examples:

```sql
-- Filter: 17.85 % of primes (k=0 obstructed); native sieve input.
CREATE MATERIALIZED VIEW primeparts.primes_k0
    STORED BY ICEBERG STORED AS PARQUET
    AS SELECT p, prime_rank FROM primeparts.primes WHERE k = 0;

-- Aggregate: magnitude histogram for q_k (one row per bit-width).
-- Replaces the full-cardinality q_k_freq which doesn't fit on this
-- cluster (~10⁸+ distinct q values).
CREATE MATERIALIZED VIEW primeparts.q_k_histogram
    STORED BY ICEBERG STORED AS PARQUET
    AS SELECT
        CAST(FLOOR(LOG2(q_k)) AS INT) AS qk_bits,
        COUNT(*)                       AS n_edges,
        MIN(q_k)                       AS qk_min,
        MAX(q_k)                       AS qk_max
       FROM primeparts.partitions
       GROUP BY CAST(FLOOR(LOG2(q_k)) AS INT);
```

When iterating on MV builds, always prepend `DROP MATERIALIZED VIEW
IF EXISTS ...` — a failed CREATE leaves a registered stub that the
next CREATE will refuse to overwrite.

**`REBUILD` semantics vs. native-committed sources:** Hive's
incremental MV refresh expects a transactional source. We commit
`primeparts.*` natively via IRC, which writes new metadata.json
snapshots Hive doesn't see as transactional. Whether `ALTER
MATERIALIZED VIEW ... REBUILD` picks up these snapshots is **untested**;
treat all current MVs as one-shot snapshot views until this is
validated.

`obstruction_catalog` (per-prime covering-system labels) is produced
by `primeparts-covering-sieve`, not as a MV: each sieve pass is a C++
worker pipeline with bitmask logic, not a SQL transform.

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

`prime_rank` itself went through this pattern: written nullable by
`primeparts-rewrite`, populated by `primeparts-backfill-rank`, promoted
to required when the post-backfill snapshot was published.

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
