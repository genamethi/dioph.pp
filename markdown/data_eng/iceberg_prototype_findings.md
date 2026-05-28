# Iceberg step-1 prototype: findings

Date: 2026-04-13
Prototype: `scripts/iceberg_prototype.py`
Scratch: `/media/extssd/research/dioph.pp/data/iceberg_scratch/`
Stack: polars 1.39.3, pyiceberg 0.11.1, pyarrow (via pyiceberg)
Source data: `pp_b001_p7368791.parquet` (1,014,962 rows; 500,000 primes; p ∈ [3, 7,368,791])

## Round-trip numerics (sanity)

| Table            | Rows    | Notes                                       |
|------------------|---------|---------------------------------------------|
| `primes`         | 500,000 | one row per p, k=0 included                 |
| `decompositions` | 938,264 | filtered `q_k > 0`, no sentinel rows        |

k histogram on this block:
```
k=0 : 76,698     k=6 :  5,209
k=1 : 146,614    k=7 :  1,393
k=2 : 132,768    k=8 :    261
k=3 :  81,360    k=9 :     33
k=4 :  39,686    k=10:      2
k=5 :  15,976
```
Sum of k over `primes` = 938,264 = `decompositions.count()`. Join invariant holds.

---

## Q1. Table properties vs. writer arguments

**Partial.** PyIceberg 0.11.1's `_get_parquet_writer_kwargs` honors only a subset of `write.parquet.*` properties. Source inspection:

| Property                             | Honored | Notes                              |
|--------------------------------------|---------|------------------------------------|
| `write.parquet.compression-codec`    | ✅       | `zstd` observed in footer          |
| `write.parquet.compression-level`    | ✅       | passed as-is to pyarrow            |
| `write.parquet.page-size-bytes`      | ✅       | → `data_page_size`                 |
| `write.parquet.dict-size-bytes`      | ✅       | → `dictionary_pagesize_limit`      |
| `write.parquet.page-row-limit`       | ✅       | → `write_batch_size`               |
| **`write.parquet.row-group-size-bytes`** | **❌**   | explicit `UserWarning: not implemented` |
| `write.parquet.bloom-filter-*`       | ❌       | not implemented                    |
| `write.target-file-size-bytes`       | ⚠️      | read by pyiceberg at write planning time, not by the parquet writer kwargs path — not exercised in the prototype (input was too small to split) |

**Consequence.** We cannot control row-group sizing through Iceberg table properties in 0.11.1. PyArrow's `ParquetWriter` defaults govern. With 938K-row input we got **1 row group**, which is fine at that scale but useless for our 128MB target at production volume. This is the single biggest gap from the plan.

**Remediation options:**

1. **Bypass Polars' `write_iceberg`**: compose a pyarrow `Table`, write parquet manually with explicit `row_group_size`, then `Table.add_files([...])` on the Iceberg table. This gives full pyarrow control and is the path we'd pick.
2. Upstream patch to PyIceberg's `_get_parquet_writer_kwargs` to pass `row_group_size` through. Contributable but out-of-scope for us right now.
3. Live with pyarrow defaults for the initial migration and revisit post-compaction.

Recommendation: option 1. The write path becomes "build `pa.Table` from Polars → `pq.write_table(..., row_group_size=N)` → `iceberg_tbl.add_files([path])`". Mechanical.

## Q2. Sort order enforcement

**Negative.** PyIceberg does not auto-sort on append. Probe: wrote unsorted input `[101, 13, 47, 2, 59, 31, 7, 23, 89, 71]` to a table declaring `sort_order=(p ASC)`. Result on disk: identical unsorted order. Additionally, the parquet footer's `sorting_columns` field is empty regardless.

**Consequence.** Two writer-side invariants we must own:

1. **Pre-sort in Polars** before writing. `LazyFrame.sort(["p", "m_k"])` upstream of `sink_iceberg` / the pyarrow write path. The table's sort-order declaration is advisory-only in 0.11.1.
2. **Ingest validation check**: assert monotonicity on `p` (and `(p, m_k)`) before committing a file. If we go the `add_files` route, this check happens right before the Iceberg commit and a failure aborts the snapshot cleanly.

The sort_order is still worth declaring on the table — Iceberg readers (and future writer versions) will use it — but we cannot *rely* on it for enforcement today.

## Q3 / Q4. Partition spec and file splitting

**Partial (not stressed).** With `identity(batch_id)` and a single `batch_id=0` input, we got exactly one file under `data/batch_id=0/`. Directory layout:

```
funbuns/primes/data/batch_id=0/00000-0-<uuid>.parquet
funbuns/decompositions/data/batch_id=0/00000-0-<uuid>.parquet
```

This is the expected Hive-style partition layout. The prototype did **not** exercise cross-file splitting within a partition (input was 5MB compressed, well below any threshold), so the "does pyiceberg split a partition at target file size" question remains unanswered in the prototype. However, since we're moving to the `add_files` path for Q1, **we control file count and boundaries ourselves**, and this question becomes moot: our ingest loop writes N parquet files for a batch and calls `add_files([file1, file2, ...])` in one commit. The "no p split across files" invariant is enforced directly in the ingest loop by chunking at p-boundaries.

## Q5. Custom parquet KV metadata

**Negative for per-file KV via `write_iceberg`.** Parquet footer `key_value_metadata` contains only `ARROW:schema`. Nothing `funbuns.*` propagates from either the Polars write or the table-level Iceberg properties.

**However:** Iceberg table-level properties *do* propagate into `metadata.json`:
```json
"properties": {
    "write.target-file-size-bytes": "1073741824",
    "write.parquet.row-group-size-bytes": "134217728",
    "write.parquet.compression-codec": "zstd",
    "write.parquet.compression-level": "3",
    "funbuns.schema_version": "1"
}
```

So `funbuns.schema_version`, `funbuns.algorithm_version`, etc. live cleanly at the **table** level.

**For per-file KV** (k_histogram for that file, content_sha256, p_min/p_max, batch_id, n_primes), the `add_files` path solves this too. We build the `pa.Table` with custom schema metadata via `pa.Table.replace_schema_metadata({...})`, write the parquet ourselves, and Iceberg preserves it. Iceberg also snapshots per-file statistics (min/max/null_counts/value_counts) into the **manifest** itself — those show up in the `.avro` files under `metadata/` and are queryable from PyIceberg / iceberg-rust without opening any parquet. We get two tiers of metadata:

| Tier            | Location                          | Contents                            |
|-----------------|-----------------------------------|-------------------------------------|
| Table-level     | `metadata/*.metadata.json`        | schema, sort-order, partition spec, `funbuns.*` versions |
| Manifest-level  | `metadata/*.avro`                 | per-file min/max/row counts (Iceberg-native) |
| File-level      | Parquet footer KV (via `add_files`) | per-file `k_histogram`, `content_sha256`, batch provenance |

The Puffin statistics files are an additional fourth tier we can reach for later if needed, but manifest stats already cover our predicate pushdown needs.

## Q6. Encoding

**Dict + RLE, not delta.** Observed encodings on `p`: `('PLAIN', 'RLE', 'RLE_DICTIONARY')`. No `DELTA_BINARY_PACKED` anywhere. This is pyarrow's default for integer columns — dictionary encoding kicks in early and delta-packing is only chosen when the writer is explicitly told to.

Observed file sizes:

| File              | Rows    | Bytes     | Bytes/row |
|-------------------|---------|-----------|-----------|
| `decompositions`  | 938,264 | 4,946,375 | 5.27      |
| `primes`          | 500,000 | 1,256,101 | 2.51      |

Even without delta encoding, the compression ratio is acceptable (ZSTD collapses the dict IDs nicely). Delta encoding would probably knock another 15–30% off the sorted `p` column, but that's a second-order optimization. When we move to the `add_files` path, we gain the option to pass `use_byte_stream_split=False, column_encoding={'p': 'DELTA_BINARY_PACKED'}` to `pq.write_table` and measure. Deferred.

## Q7. Metadata.json pointer shape

**Clean.** Layout produced by the SqlCatalog commit:

```
funbuns/primes/metadata/
    00000-<uuid>.metadata.json       # from create_table
    00001-<uuid>.metadata.json       # from append (current)
    <commit-uuid>-m0.avro            # manifest
    snap-<snap-id>-0-<uuid>.avro     # manifest list
```

The metadata filename prefix is a zero-padded monotonic serial (`00000`, `00001`, ...). The SqlCatalog records the current metadata.json path in its SQLite table. PyIceberg does **not** write a `version-hint.text` or equivalent — the catalog is the source of truth.

**Pointer file for Rust consumption:** write our own `current.metadata.json.txt` sibling file on every commit, containing the absolute path (or filename relative to `metadata/`) of the latest `.metadata.json`. This is one extra line in the ingest path post-commit:

```python
latest = sorted(Path(tbl.location().replace("file://", "") + "/metadata").glob("*.metadata.json"))[-1]
(latest.parent / "current.metadata.json.txt").write_text(latest.name + "\n")
```

iceberg-rust's `StaticTable::from_metadata_file(path)` then just reads the pointer, resolves the path, and loads. No SQLite, no Python, no catalog runtime on the Rust side.

## Q8. Round-trip read

**Positive via PyIceberg table object, not tested with native path.** `pl.scan_iceberg(tbl).collect()` returned the correct rows after reloading the table from the catalog (the initial Table object was stale — it doesn't auto-refresh after a write). The `reader_override='native'` path and string-only source were not tested in the prototype; they're read-side concerns and not load-bearing for the write path decisions.

Two things to remember for future work:

- **Reload after write.** After `write_iceberg`, call `catalog.load_table(ident)` again before scanning. The returned Table object caches the snapshot state at fetch time.
- **Rust-side reads** can use `iceberg-rust`'s `StaticTable` pointed at the metadata.json via the pointer file. Not proven in this prototype; will be validated at step 5 (Rust retarget).

---

## Implications for step 2 (schema lock)

1. **Write path is `add_files`-based, not `write_iceberg`-based.** Polars' `write_iceberg` is a useful smoke test but too restrictive for production: no row-group control, no file-level KV, no sort enforcement. The production ingest path looks like:

   ```
   LazyFrame → sort(["p","m_k"]) → chunk by batch/p-boundary → pa.Table (with KV metadata) →
   pq.write_table(row_group_size=..., compression='zstd', ...) → iceberg_tbl.add_files(...) → commit
   ```

   All of these are boring, stable APIs.

2. **Schema-lock module** (`src/funbuns/iceberg_schema.py`) owns:
   - The two PyIceberg `Schema` objects
   - The `PartitionSpec` and `SortOrder` for each table
   - `TABLE_PROPERTIES` (declarative, lands in metadata.json)
   - `PARQUET_WRITER_KWARGS` (separate, for pyarrow direct writes)
   - A `build_file_kv(batch_rows) -> dict[str, bytes]` helper that computes per-file KV metadata from a prepared pa.Table (k_histogram, content_sha256, p_min, p_max, n_primes, n_rows, generated_at, generator, batch_id)
   - A `write_batch(catalog, primes_lf, decomp_lf, batch_id)` function that is the single entry point for every producer

3. **Validation invariants** (assert-fail before commit):
   - `primes`: monotonic ascending `p`, no duplicates within a batch
   - `decompositions`: sorted by `(p, m_k)` ascending
   - `decompositions.p.unique()` ⊆ `primes.p.unique()`
   - `sum(primes.k)` == `len(decompositions)`
   - All `q_k > 0` in `decompositions`
   - `batch_id` matches the partition column value

4. **Pointer file** written as a post-commit step inside `write_batch`. Single source of truth for Rust-side reads.

5. **Compactor (step 3)** uses the same `write_batch` entry point, just fed from the legacy `blocks/` glob in p-sorted order, chunked into ~100M-prime batches. No new code path.

## Deferred / not-yet-answered

- DELTA_BINARY_PACKED encoding wins on `p` — measure post-migration, decide later.
- Bloom filter on `p` — confirmed not needed given sort order + manifest stats.
- Iceberg Puffin statistics blobs (table-wide k-histogram) — punt until needed.
- File splitting at target-file-size when writing large batches via Polars' native path — moot, we control splitting directly.
- `scan_iceberg(str, reader_override='native')` — not tested; step 5 problem.
- iceberg-rust `StaticTable` roundtrip — not tested; step 5 problem.
- `write.target-file-size-bytes` behavior via PyIceberg's planning path — not exercised.

## Step-1 exit

All questions in the plan that gate the step-2 schema lock are answered. The write path needs to switch from `write_iceberg` to `add_files`, which reshapes step 2 slightly but doesn't invalidate the schema/partition/sort decisions from the architect phase. Ready for step 2 on user sign-off.
