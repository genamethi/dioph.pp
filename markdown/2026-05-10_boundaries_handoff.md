# funbuns.boundaries — Handoff

**Date:** 2026-05-10
**Scope:** Create the Iceberg table that pins the v1 bucket boundaries.
Upstream (BPR calibration, rpb derivation, helper compilation) is **done**;
do not re-derive or re-discuss it.

---

## What's already in place

- C helper: `native/build/nth_prime_boundaries` (built by `make all` in `native/`).
  Reads ASCII ranks (one per line, ascending) from argv[1]; writes
  `<rank>\t<prime>\n` to stdout. Uses FLINT's `n_primes_t` iterator —
  single forward sieve.
- Input ranks for v1: `native/bin/boundary_ranks.txt`. Contents:
  `2, 3855446407, 7710892812, 11566339217, 15421785622, 19277232027`.
- Constants behind those ranks (recorded for traceability — not stored in the
  table): `BPR = 1.1140`, `F = 4`, `TARGET_FILE_BYTES = 2^30`,
  `rpb = floor(F · 2^30 / BPR) = 3,855,446,405`,
  `N = 21,699,850,257`, `B = ceil(N/rpb) = 6`. The ranks are
  `i · rpb + 2` for `i ∈ [0, B)`.

---

## What to build

### 1. Run the helper

```bash
cd native
./build/nth_prime_boundaries bin/boundary_ranks.txt > bin/boundary_primes.tsv
```

Expected: 6 lines, `<rank>\t<prime>`. Largest rank is ~1.93×10¹⁰, so the
sieve runs to ~4.5×10¹¹ — expect minutes to tens of minutes; check `top`
to confirm forward progress, do not assume it's hung.

### 2. Define the table

Catalog: existing `funbuns` namespace (same catalog as `funbuns.primes`).
Use `src/primeparts/iceberg_schema.py::open_catalog` and the project's
schema-creation helpers.

```
funbuns.boundaries
  schema:
    p_bucket_version : int   (required, fresh field id)
    p_bucket         : int   (required, fresh field id)
    p_min            : long  (required, fresh field id)   # FLINT-rank prime
  partition spec:
    (none — table is tiny, 6 rows for v1)
  sort order:
    p_bucket_version ASC, p_bucket ASC
  properties:
    write.format.default = parquet
    write.parquet.compression-codec = zstd
    write.parquet.compression-level = 3
```

Field-id rules from `iceberg_schema.py` apply: allocate fresh ids, never
reuse. `p_min` is the prime itself (FLINT rank → prime), not the rank.

### 3. Populate v1

Parse `bin/boundary_primes.tsv` and write **6 rows**:

| p_bucket_version | p_bucket | p_min                |
|------------------|----------|----------------------|
| 1                | 0        | (prime at rank 2)    |
| 1                | 1        | (rank 3,855,446,407) |
| 1                | 2        | (rank 7,710,892,812) |
| 1                | 3        | (rank 11,566,339,217)|
| 1                | 4        | (rank 15,421,785,622)|
| 1                | 5        | (rank 19,277,232,027)|

`p_bucket = i` corresponds to `[p_min[i], p_min[i+1])`; bucket `B-1` is
open-ended (the frontier).

Single Iceberg append transaction. No partition writers, no clustered
writer, no field-id-reset gymnastics — it's a plain unpartitioned table.

### 4. Smoke check

After commit, verify by reading back via `pyiceberg`:
```python
import polars as pl

t = open_catalog().load_table("funbuns.boundaries")
df = pl.from_arrow(t.scan().to_arrow())
assert df.height == 6
assert df.select((pl.col("p_bucket_version") == 1).all()).item()
assert df.sort("p_bucket").get_column("p_bucket").to_list() == [0, 1, 2, 3, 4, 5]
assert df.sort("p_bucket").get_column("p_min").is_sorted()
```

---

## Out of scope for this handoff

- Recomputing or recalibrating BPR / rpb / B.
- Anything about the rewriter, planner manifest math, or the sibling
  `partitions` table — those land later, *after* boundaries exist.
- Storing BPR / rpb / N as table properties or row-level metadata. Once
  the rewrite has used these constants, they are not load-bearing for
  query-time correctness; keep `funbuns.boundaries` clean.

---

## Files touched / to touch

- ✅ `native/bin/nth_prime_boundaries.c`
- ✅ `native/bin/boundary_ranks.txt`
- ✅ `native/Makefile` (added `nth_prime_boundaries` target to `all`)
- ✅  `native/bin/boundary_primes.tsv` (output of helper run)
- ✅  `scripts/init_boundaries_table.py`
  creates `funbuns.boundaries` and appends the 6 v1 rows idempotently.

## Completion notes

- Ran `pixi run python scripts/init_boundaries_table.py` against the default
  production catalog. It created/populated the Iceberg table
  `funbuns.boundaries`.
- Production table location:
  `file:///media/extssd/research/dioph.pp/data/iceberg/warehouse/funbuns/boundaries`
- Current metadata file:
  `file:///media/extssd/research/dioph.pp/data/iceberg/warehouse/funbuns/boundaries/metadata/00001-a1bf503a-4622-4a2a-a4e8-31c42b4e024d.metadata.json`
- Current data file:
  `file:///media/extssd/research/dioph.pp/data/iceberg/warehouse/funbuns/boundaries/data/00000-0-18b9193f-87ff-4990-b1ad-abe88d167f6d.parquet`
- Current manifest list:
  `file:///media/extssd/research/dioph.pp/data/iceberg/warehouse/funbuns/boundaries/metadata/snap-1646338778888966395-0-18b9193f-87ff-4990-b1ad-abe88d167f6d.avro`
- Current manifest:
  `file:///media/extssd/research/dioph.pp/data/iceberg/warehouse/funbuns/boundaries/metadata/18b9193f-87ff-4990-b1ad-abe88d167f6d-m0.avro`
- Current table metadata checks:
  format version `2`, current snapshot id `1646338778888966395`,
  default spec id `0`, partition spec `fields: []`, sort order
  `p_bucket_version ASC, p_bucket ASC`, and snapshot summary
  `operation=append`, `added-data-files=1`, `added-records=6`,
  `funbuns.boundary_version=1`.
- Current `inspect.files()` check:
  one data file, `record_count=6`, `file_size_in_bytes=1372`.
- Current `inspect.partitions()` check:
  one unpartitioned aggregate row, `record_count=6`, `file_count=1`,
  `total_data_file_size_in_bytes=1372`, `last_updated_snapshot_id=1646338778888966395`.
- Current contents:

  | p_bucket_version | p_bucket | p_min        |
  |------------------|----------|--------------|
  | 1                | 0        | 3            |
  | 1                | 1        | 93357498629  |
  | 1                | 2        | 192298134389 |
  | 1                | 3        | 293340633197 |
  | 1                | 4        | 395747140337 |
  | 1                | 5        | 499167448849 |
- Local helper output:
  `native/bin/boundary_primes.tsv` with `<rank>\t<prime>` rows.
- Reran the initializer; it reported `verified existing 6 boundary row(s)`,
  confirming it does not duplicate rows on repeat runs.
- Smoke check was run with `polars.from_arrow(t.scan().to_arrow())`, because
  this Pixi env has Polars/PyArrow but not pandas. Verified 6 rows,
  `p_bucket_version = 1`, buckets `[0, 1, 2, 3, 4, 5]`, and sorted `p_min`.
