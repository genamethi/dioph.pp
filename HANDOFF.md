# Project Handoff: Prime Power Partition Obstruction Analysis

## Objective
Classify $k=0$ primes from the `primeparts.primes` dataset by their **minimal covering systems**. This explains why $p = 2^m + q^n$ has no solutions for these primes.

## Mathematical Context
- **Propagation Sifting**: If a sieve prime $s$ divides $p - 2^r$, it also divides $p - 2^{r + j \cdot d}$ where $d = ord_s(2)$.
- **Covering Systems**: A prime $p$ is obstructed if the arithmetic progressions for a set of primes $S$ cover all $m \in [1, \log_2(p)]$.
- **Subgroup Exclusion**: Even if only $s=3$ covers an $m$, it may be blocked if $(p - 2^m) \pmod \ell$ falls outside the subgroup $\langle 3 \rangle \subset (\mathbb{Z}/\ell\mathbb{Z})^*$.

## Dataset basics (verified 2026-05-27)
- `primeparts.primes`: `MAX(p) = 564_575_405_239 ≈ 5.6×10¹¹`; `COUNT(*) = 21_698_850_257 ≈ 21.7 B` rows; `MIN(p) = 3` (p=2 absent).
- `primeparts.partitions`: ~40 B rows. 99.9 % are n=1 edges.
- These are linked by $\pi(x) \approx x/\log(x)$; older docs saying "20 B dataset" mean the *count*, not max p.
- Empirical q_k magnitude distribution is in [markdown/data_eng/MV_list.md].

## Current State of Play

### Stack
Cluster-side end-to-end pipeline is working:
- Native rewrite + IRC publish path (`primeparts-drop-bucket-cols`) writes iceberg metadata via HMS-backed REST catalog at `http://192.168.1.202:9090/iceberg/v1/`.
- Hive 4.2 MR3 cluster running a custom-baked image (`mr3-hive-4.2.0-local/hive:4.2.0`) that includes the DELTA_BINARY_PACKED vectorized-read fix (upstream hive-mr3 branch `master4.2.0` commit `b5bc76328e`).
- Cluster tuned to **Config A** for MV builds: 2 worker pods × 4 GiB envelope × 2 concurrent Tez tasks → 4 concurrent total. Pipelining `hive.mr3.am.task.concurrent.run.threshold.percent=80`. See `markdown/data_eng/hive_mr3_stack.md` for the propagation procedure.

### MV inventory
See [markdown/data_eng/MV_list.md] for the full table. As of 2026-05-27:

- **Built**: `primes_k0` (3.87 B obstructed primes), `q_k_freq_lo`, `q_k_freq_mid`, `q_k_histogram` (39-row magnitude index).
- **Planned**: `partitions_n_ge_2` (small filtered MV, n≥2 forward-seeded table), `partitions_low_q` (q < 10⁵ inverted index).
- **Rejected/replaced**: `q_k_freq_hi`/`q_k_freq_top` collapsed into `q_k_histogram` — their full-cardinality form doesn't fit on a 16 GiB single-node cluster.

### Covering-sieve progress (the underlying objective)
1. **Pass 1 Complete**: Backbone $\{3, 5, 7\}$ applied. Data is in `ib-staging/primeparts/obstruction_catalog`.
   - `covering_system=3,5,7`: Fully covered primes.
   - `covering_system=uncovered_pass1`: Survivors (~3.8B primes) with an `uncovered_mask` (Int64).
2. **Sidecar References**:
   - `mersenne_reference.parquet`: Full factorization and primitive factors for $M_m = 2^m - 1$ up to $m=64$. Used to lookup $ord_s(2)$.
3. **Triage Results**: Analysis of `uncovered_pass1` shows $m=12$ is the most frequent gap, making $s=13$ the optimal next prime.

## Tooling

### Native
- `native/src/covering_sieve_main.cc`: Multi-threaded sieve over an input partition, applies a covering-prime set, splits into `covered_...` and `uncovered_passN` partitions.
- `native/src/sieve_triage_main.cc`: Analyzes the bitmask distribution of an uncovered partition to inform the next sieve prime choice.
- `native/src/primitive_factors.cc`: `GetCoverageMask` / `MersenneHelper`.
- `native/src/writer.cc`: Iceberg-aware parquet writer; skips identity-partition source fields (`p_bucket_version`, `p_bucket`) from the physical schema as of 2026-05-26 (uncommitted in primeparts repo as of the wind-down).
- `native/src/drop_bucket_cols_main.cc`: One-shot in-place rewriter that strips physical bucket cols from existing files and re-registers via IRC. Already ran across the full warehouse.
- `native/src/backfill_prime_rank_main.cc`: Existing in-place backfill template (uncommitted reference for any future stream-and-rewrite work).

### Hive-side
- HS2 + MR3 + HMS-IRC stack in `mr3/kubernetes/`. See `hive_mr3_stack.md` for the propagation pattern after config edits.
- `scripts/hive_sql.py`: thin JDBC client for ad hoc queries.

## Known Issues / Tasks for Next Assistant

### Cutover sweep (open)
- **`primeparts.primes_k0` lives at the old funbuns warehouse path** (`/iceberg/warehouse/primeparts.db/primes_k0/`), not in `/ib-staging/`. DROP + recreate procedure documented in `markdown/data_eng/MV_list.md` §"MVs at wrong on-disk location"; ~290 s rebuild then `rm -rf` the old dir. The DB-location ALTER on 2026-05-27 prevents new MVs from landing there.
- **`obstruction_catalog` at `ib-staging`** is in place but partition shape may need a re-sweep after the s=13 / s=17 passes.
- **Archive the old funbuns layout**. `sync_hms.py` and the SqlCatalog at `/iceberg/warehouse/catalog.db` are dead code paths for the current stack — keep them out of automation, plan to delete after one more pass.
- **busybox init-container GC** still recurs (manually re-pulled this session). Three durable fixes documented in `hive_mr3_stack.md` §"Known gotchas" #9 — none applied yet. Lowest-friction option is replacing the init image with the hive image itself (`command: ["/bin/true"]`); a one-line config change pinning the layer to the worker container's image reference.

### Native-commit progression (open)
- **Native covering-sieve emits via IRC.** The C++ tool currently writes parquet + does a native FastAppend via iceberg-cpp's REST catalog. The interaction with the existing `obstruction_catalog` partition values needs the same identity-partition cleanup we just did for primes/partitions (see `drop_bucket_cols_main.cc` for the pattern).
- **MV REBUILD against externally-committed iceberg snapshots**: untested. Hive expects transactional source semantics; we commit from native via IRC. Decide before any MV is bound to a refresh loop.

### Cluster config (uncommitted in primeparts repo)
Config files edited 2026-05-27 to switch to Config A + ALTER DATABASE primeparts location. The diffs live in the working tree only:
- `mr3/kubernetes/conf/hive-site.xml` (task.memory.mb, containergroup envelope, pipelining threshold)
- `mr3/kubernetes/conf/mr3-site.xml` (autoscaling enabled with caps, total.max.memory.gb=8)
- `mr3/kubernetes/conf/tez-site.xml` (sort.mb, unordered.output.buffer)
- `mr3/kubernetes/yaml/{hive,metastore}.yaml` (pod requests/limits)
- `mr3/kubernetes/env.sh` (HS2 + metastore heap sizes; HIVE_WAREHOUSE_DIR points at /ib-staging)

User has held off on commits pending more validation; commit these together with the new MV docs once the cutover sweep is in.

### Original known issues (still relevant)
- **Writer Bug** (covering-sieve path): `BucketParquetWriter` enforces a Hive-style path naming convention that conflicts with the custom `covering_system` partitioning. It also has a strict "refusing to overwrite" check that needs careful handling in multi-threaded runs.
- **Commit Logic**: The C++ tool performs a native Iceberg commit via `FastAppend`. Ensure the `partition_values` are correctly mapped to the `DataFile` objects (see `drop_bucket_cols_main.cc:MakeDataFile` for the working pattern that omits identity-partition source columns from `value_counts`).
- **Scalability**: The input partition is ~8.6GB (39 files). The tool must stream batches (currently using 500k row chunks) and avoid collecting large tables into memory to prevent OOM.

### Deferred / preserved paths (intentionally not addressed in the 2026-05-27 wind-down)

The following options were raised in conversation, weighed, and intentionally deferred. Reviving any of them is straightforward:

- **Cluster expansion** to fit `q_k_freq_top`-style aggregates (one-worker × 8 GiB envelope, or 3 workers via larger node). The user said "we can try expanding" but it wasn't needed once we pivoted to the histogram.
- **`hive.mr3.container.max.java.heap.fraction` 0.7 → 0.8** for ~20 % more heap in the same container. Untested; risk is parquet's DirectByteBuffer use.
- **Bucket-map-join EXPLAIN check** for `primes ⨝ partitions ON p`. The layout supports zero-shuffle; planner behavior not verified.
- **MV `ALTER TABLE ... WRITE ORDERED BY p`** to propagate sort_order. `ORDER BY` queries on built MVs are paying a full resort right now.
- **Drop the busybox initContainer in favor of the hive image itself** (`command: ["/bin/true"]`). The user picked "grow the partition" over this; the disk move was deferred too.
- **Native MV equivalent for `q_k_freq_top`** — streaming aggregate in C++ via iceberg-cpp, committed via IRC. Would sidestep Hive entirely for the cardinality-hardest aggregate. Sketch: 12-thread parallel chunked, sparse-hash, emit (q, count) pairs only where count ≥ 2 (Evertse bound + the n=1 power-of-2-difference rarity argument both predict most q_k counts are exactly 1).
- **Sort-merge bucket join** validation for any future join-heavy MV (e.g. `primes_with_first_parent`). Both tables share the `(p_bucket_version, p_bucket)` identity partition; the SMB path should be available.
- **GPU polars engine** (`cudf-polars-cu12`) for any ad hoc analytical query that doesn't need Hive's transactional semantics. Flagged in `hive_mr3_stack.md` "Next steps".

### Next Concrete Step
Successfully run the sieve for $s=13$ and $s=17$ to move primes from `uncovered_pass1` into minimal buckets. Apply the identity-partition-cleanup pattern from `drop_bucket_cols_main.cc` to the sieve's output before committing.
