# Iceberg data setup

This document describes the canonical data layout for the funbuns dataset
after the legacy `blocks/` glob was retired. It is narrowly focused on the
**write-side** schema and filesystem layout — things that are stable and
will not change as downstream consumers are retargeted off DuckDB.

Ground truth lives in `src/funbuns/iceberg_schema.py`. This doc summarizes;
the module is authoritative.

---

## Tables

The dataset is expressed as two Apache Iceberg tables under the namespace
`funbuns`:

| Table                    | Rows                           | Purpose                                    |
|--------------------------|--------------------------------|--------------------------------------------|
| `funbuns.primes`         | one per prime `p`, incl. `k=0` | universe of primes ≤ current max           |
| `funbuns.decompositions` | one per `(p, m_k, n_k, q_k)`   | fact table; non-obstructed partitions only |

Every prime `p` that has been processed appears in `primes` exactly once.
Obstructed primes (no decomposition exists) are stored with `k=0` and do
**not** appear in `decompositions`. The row in `primes` is the marker.

Current state (2026-04-14 post-cutover):

- 504 commit_seqs (0–503); new ingest appends directly from
  `core.PPConsumer` via `IcebergWriter.flush`.
- 10,098,850,000 rows in `primes`
- 19,009,703,663 rows in `decompositions`
- `k_max = 16`
- `sum(primes.k) == rows(decompositions)` — the key cross-table invariant

### commit_seq semantics

`commit_seq` is a **write-side flush counter**, not a position along the
p-axis. Under multiprocessing ingest (`imap_unordered`), workers finish in
non-deterministic order, so a batch with lower `commit_seq` may cover a
higher p-range than a later one. Every `(p_lo, p_hi)` range is still
disjoint and contiguous across the dataset, but `ORDER BY commit_seq`
does not walk primes in p-order. Queries that want p-ordering must sort
on `p` (or read the manifest's `p.lower_bound` per file). Resume state is
derived from `max(p)`, not from `max(commit_seq)`, so reorder is safe for
the write path. Pinned by `tests/test_iceberg_commit_seq.py`.

Pre-cutover catalogs (where the column was named `batch_id`) are
upgradable in place via `iceberg_schema.migrate_rename_batch_id_to_commit_seq`.

## Schemas

**`funbuns.primes`** (field_ids fixed; never reuse):

| field_id | name         | type    | notes                            |
|----------|--------------|---------|----------------------------------|
| 1        | `p`          | Long    | prime                            |
| 2        | `k`          | Integer | # decompositions; 0 = obstructed |
| 3        | `commit_seq` | Integer | partition key (flush counter)    |

**`funbuns.decompositions`**:

| field_id | name         | type    | notes                         |
|----------|--------------|---------|-------------------------------|
| 1        | `p`          | Long    | prime                         |
| 2        | `m_k`        | Integer | exponent on 2, ≥ 1            |
| 3        | `n_k`        | Integer | exponent on q, ≥ 1            |
| 4        | `q_k`        | Long    | odd prime base, > 0           |
| 5        | `commit_seq` | Integer | partition key (flush counter) |

All fields are non-required at the iceberg schema level but the writer
enforces non-null at validation time. `q_k == 0` sentinel rows (legacy
obstruction marker) are filtered out of `decompositions` — their
information is captured by `primes.k == 0` instead.

## Sort order

- `primes`: `p ASC`
- `decompositions`: `(p ASC, m_k ASC)`

Sort order is **declared** on the table but PyIceberg 0.11.1 does not
enforce it on append. The writer owns the invariant: `write_batch()`
sorts before the parquet write and validators reject unsorted batches.

## Partitioning

Both tables use identity partitioning on `commit_seq`:

```
funbuns/primes/data/commit_seq=N/primes_sNNNNNN_000.parquet
funbuns/decompositions/data/commit_seq=N/decompositions_sNNNNNN_000.parquet
```

Note: commit_seqs 0–503 were written by the legacy compactor under the
old `batch_id=N/` directory name and still live there on disk. The
iceberg column rename (`batch_id` → `commit_seq`) is a schema-only
operation and preserves field_ids, so the old directories remain
readable through the manifest; only newly appended commit_seqs use the
new path. A batch is the atomic unit of ingest. Post-cutover,
`core.PPConsumer` flushes one commit_seq per buffer drain (target size
governed by `buffer_size`), directly from worker result aggregation —
there is no staging step.

## Filesystem layout

```
/media/extssd/research/dioph.pp/data/iceberg/
├── catalog.db                          # SqlCatalog SQLite state
└── warehouse/
    └── funbuns/
        ├── primes/
        │   ├── data/commit_seq=N/*.parquet   # new appends
        │   ├── data/batch_id=N/*.parquet     # legacy compactor output (0–503)
        │   └── metadata/
        │       ├── NNNNN-<uuid>.metadata.json
        │       ├── <uuid>-m0.avro      # manifests
        │       ├── snap-<snap-id>-*.avro
        │       └── current.metadata.json.txt
        └── decompositions/
            ├── data/commit_seq=N/*.parquet
            ├── data/batch_id=N/*.parquet
            └── metadata/
                └── ... (same shape)
```

**Catalog**: `SqlCatalog` backed by SQLite at `catalog.db`. The catalog
is the source of truth for the current metadata.json path.

**Pointer file**: `current.metadata.json.txt` contains the filename
(relative to `metadata/`) of the live metadata.json. Written after every
`write_batch()` commit. Exists so that non-Python readers (e.g. a future
Rust consumer via `iceberg-rust::StaticTable`) can resolve the current
snapshot without running PyIceberg.

## Table properties

Declared in `TABLE_PROPERTIES` and propagated into every `metadata.json`:

```
write.parquet.compression-codec    = zstd
write.parquet.compression-level    = 3
write.target-file-size-bytes       = 1073741824   (1 GiB)
funbuns.schema_version             = 1
funbuns.algorithm_version          = <package version>
```

Note: PyIceberg 0.11.1 **silently ignores** `write.parquet.row-group-size-bytes`
and bloom filter properties. Row-group sizing is controlled directly by
`PARQUET_WRITER_KWARGS` in `iceberg_schema.py`, not through iceberg
properties.

## Parquet writer configuration

Not routed through PyIceberg's writer. `write_batch()` uses
`pyarrow.parquet.write_table` directly with:

```
compression      = zstd
compression_level= 3
row_group_size   = 1_048_576 rows
data_page_size   = 1 MiB
write_statistics = True
use_dictionary   = True
```

Files are written as `.<name>.parquet.tmp` in the canonical partition dir,
then atomically renamed before `add_files` is called.

## Per-file KV metadata

Every parquet file carries a `funbuns.*` key-value block in its footer,
built by `build_file_kv()` and preserved through the `add_files` path:

| Key                          | Meaning                                         |
|------------------------------|-------------------------------------------------|
| `funbuns.schema_version`     | matches `SCHEMA_VERSION` constant               |
| `funbuns.algorithm_version`  | funbuns package version at write time          |
| `funbuns.algorithm_git_sha`  | short git SHA of the writer                     |
| `funbuns.table`              | `primes` or `decompositions`                    |
| `funbuns.commit_seq`         | matches the partition directory                 |
| `funbuns.p_min`, `funbuns.p_max` | p range in this file                        |
| `funbuns.n_rows`             | row count                                       |
| `funbuns.n_primes`           | distinct primes (== n_rows for `primes`)        |
| `funbuns.content_sha256`     | sha256 over column buffers, order-sensitive     |
| `funbuns.generated_at`       | ISO-8601 UTC timestamp                          |
| `funbuns.generator`          | writer name (default `funbuns-core`)            |
| `funbuns.k_histogram`        | JSON `{k: count}` — **primes only**             |

This is a third tier of metadata on top of iceberg's own per-file
manifest statistics (min/max/null counts). Manifest stats cover
predicate pushdown; the KV block carries provenance and domain-specific
summaries.

## Write path

`IcebergWriter.flush(raw_batch)` is the single entry point. The writer
is constructed once by `core.PPManager` and wired in as
`PPConsumer.save_callback`, so buffer drains flow directly into iceberg
without any on-disk staging. Flow per flush:

```
shape_for_write (stamp commit_seq, split into primes+decomp frames)
  → sort → validate → to_arrow → chunk at p-boundaries
  → build KV → pq.write_table (tmp)
  → atomic rename
  → tbl.add_files([...])   # commits an iceberg snapshot
  → reload table
  → write current.metadata.json.txt pointer
  → next_commit_seq += 1
```

Chunking within a flush is driven by `TARGET_FILE_SIZE_BYTES /
bytes_per_row` estimates. For `decompositions`, chunk boundaries are
snapped forward so that no single `p` value straddles two files.

## Validation invariants

Enforced before commit. A failure raises `ValidationError` and aborts
without writing anything committable:

**`primes` batch:**
- Schema `{p: Int64, k: Int32, commit_seq: Int32}`
- No nulls
- Non-empty
- `p` strictly ascending, no duplicates
- `k ≥ 0`
- `commit_seq` column matches the commit_seq stamped by `shape_for_write`

**`decompositions` batch:**
- Schema `{p: Int64, m_k: Int32, n_k: Int32, q_k: Int64, commit_seq: Int32}`
- No nulls
- Non-empty
- `q_k > 0` (no sentinel rows)
- `m_k ≥ 1`, `n_k ≥ 1`
- Sorted by `(p, m_k)` ascending
- `commit_seq` column matches the commit_seq stamped by `shape_for_write`

**Cross-table:**
- `sum(primes.k) == rows(decompositions)`
- No `decompositions` rows reference a `primes.p` with `k == 0`
- Every `decompositions.p` appears in `primes.p` (anti-join is empty)

## Reading

### Python (polars)

```python
import polars as pl

primes = pl.scan_iceberg(
    "/media/extssd/research/dioph.pp/data/iceberg/warehouse/funbuns/primes"
)
decomp = pl.scan_iceberg(
    "/media/extssd/research/dioph.pp/data/iceberg/warehouse/funbuns/decompositions"
)

# Example: count obstructed primes below 10^9
(
    primes
    .filter(pl.col("p") < 1_000_000_000)
    .filter(pl.col("k") == 0)
    .select(pl.len())
    .collect()
)
```

Predicate pushdown via manifest stats happens transparently; `p` range
filters only open the files whose min/max overlap the range.

### Python (PyIceberg, for metadata inspection)

```python
from pyiceberg.catalog.sql import SqlCatalog

cat = SqlCatalog(
    "funbuns",
    uri="sqlite:////media/extssd/research/dioph.pp/data/iceberg/catalog.db",
    warehouse="file:///media/extssd/research/dioph.pp/data/iceberg/warehouse",
)
tbl = cat.load_table("funbuns.primes")
tbl.inspect.files()          # per-file stats, metadata-only, no data reads
tbl.inspect.snapshots()
tbl.current_snapshot()
```

### Rust / other

Consumers that can't run PyIceberg should:

1. Read `metadata/current.metadata.json.txt` (single filename line).
2. Open the indicated `*.metadata.json`.
3. Walk its current snapshot's manifest list for the data file paths.

The `iceberg-rust` crate's `StaticTable::from_metadata_file` does exactly
this. Filesystem globbing of partition dirs is **not** safe and not
recommended: commit_seqs are flush-ordered, not p-ordered, and a bulk
rewrite or future compaction could reshuffle files.

## Bulk rebuild

The original `scripts/iceberg_compactor.py` (deleted in the step-7
cutover) read legacy `blocks/*.parquet` in true p-order, deduplicated
them (`unique(subset=["p", "m_k", "n_k", "q_k"])`), and ran them through
`write_batch` in ~20M-prime batches to seed commit_seqs 0–503. It is
gone from the tree; if a bulk rebuild is ever needed again it should be
rewritten against a fresh `--temp` warehouse from `core.py`'s generation
path, not resurrected from git history.

## Schema evolution

To add a column without breaking existing files:

1. Bump `SCHEMA_VERSION` in `iceberg_schema.py`.
2. Add the field to the relevant `Schema` with the next available
   `field_id`. **Never reuse or reassign field_ids.**
3. Run a one-off migration:
   ```python
   with tbl.update_schema() as us:
       us.add_column("new_col", IntegerType(), required=False)
   ```
4. Old files remain readable — Iceberg fills missing columns with NULL.
5. Update `build_file_kv` and validators as needed.

Renames go through `update_schema().rename_column()`; drops through
`delete_column()`. Never change field types of existing fields.

## Environment

- `FUNBUNS_ICEBERG_DIR` — overrides `[tool.funbuns.directories].iceberg_dir`
  for scratch/test runs.
- Warehouse location otherwise read from `pixi.toml
  [tool.funbuns.directories] iceberg_dir = "/media/extssd/.../iceberg"`.
