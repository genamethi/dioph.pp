# Project Handoff: Prime Power Partition Obstruction Analysis

## Overall Objective

This is an open-ended project. The main object of study is the solutions to the diophantine equation defined as follows:

For a given $p \in \mathbb{P}$, the set of prime numbers, we consider all solutions such that $p = 2^m + q^n$, where $m, n \geq 1, q \in \mathbb{P}$.

We define $k$ as the number of solutions for a given $p$. This particular document is concerned with one important concept, where $k = 0$. However, our broader object is to determine the algebraic and geometric structure that allows us to fully classify the set of solutions. Other less ambitious objectives that concern us are ascertaining effective bounds for various quantities. The computational complexity is also of interest. The process itself often folds its results into further investigation. So, we want to keep these concepts in mind as we proceed with the more narrow "Current Objective".

## Current Objective
Classify $k=0$ primes from the `primeparts.primes` dataset by their **minimal covering systems**. This explains why $p = 2^m + q^n$ has no solutions for these primes.

## Mathematical Context and Operative concepts

### Necessary background

- **Parity of Partition Summands**: The description of the above equation can be simplified to: A solution is a length two partition where both terms/summands are prime powers. By parity, since $p$ is an odd prime ($p=2$ is a trivial example), then one term must be odd and the other even. Hence, one term has $2$ as its base.
- **Bit Length Framing**: Since we are summing a power of two with another prime power, we can consider the question as taking a bitmask of $p$. The different bit lengths also give us important information about the size of $q^n$ quickly.
- **Mersenne Factors and Primitive Factors**: Define Mersenne Numbers as $M_n = 2^n - 1$, this is more general than Mersenne Primes, in the obvious way. We say a prime factor $s$ which divides $M_n$ is primitive if for all $M_k$ with $k < n$ if $t \divides M_k$ with $t \in \mathbb{P}$, then $t \neq s$, otherwise, $s$ is called an intrinsic factor of $M_n$.
- **Factors of Mersenne Numbers Obstruct Solutions**: Consider the case where $k > 2$ then there exist $m, m', n, n', q, q'$ such that $p = 2^m + q^n = 2^m' + q'^n$'. Assuming (WLOG), $m' < m$ we have $p - 2^m' = 2^m'(2^{m-m'} - 1) + q^n = q'^n'$. We ask this question for the $k = 0$ case, we know that for all $m$, $p - 2^m = c$ for $c$ composite, then which $m'$ exist such that $p - 2^m' = b$ where $gcd(b, c) > 1$? For such coprime, $b$ and $c$, $p - 2^m' = 2^m'(2^{m-m') - 1) + c = b$, since $c - b =  2^m'(2^{m-m') - 1)$ we can consider the odd prime factors of $b$ and $c$ as factors of a Mersenne number.

### Utilized Concepts for Analyzing $k = 0 $primes

The below concepts wind up being vital for understanding these cases.

- **Propagation Sifting**: If a sieve prime $s$ divides $p - 2^r$, it also divides $p - 2^{r + j \cdot d}$ where $d = ord_s(2)$.
- **Covering Systems**: A prime $p$ is obstructed if the arithmetic progressions for a set of primes $S$ cover all $m \in [1, \log_2(p)]$.
- **Subgroup Exclusion**: Even if only $s=3$ covers an $m$, it may be blocked if $(p - 2^m) \pmod \ell$ falls outside the subgroup $\langle 3 \rangle \subset (\mathbb{Z}/\ell\mathbb{Z})^*$.

## Dataset basics (verified 2026-05-27)

- `primeparts.primes`: `MAX(p) = 564_575_405_239 ≈ 5.6×10¹¹`; `COUNT(*) = 21_698_850_257 ≈ 21.7 B` rows; `MIN(p) = 3` (p=2 absent). No primes skipped from 3 to MAX(p).
- `primeparts.partitions`: ~40 B rows. 99.9 % are n=1 edges. These are solutions for all primes in primeparts.primes (with non-solutions omitted).
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

### Covering-sieve rewrite (the underlying objective)

The Pass-1 artifact in `ib-staging/primeparts/obstruction_catalog`
(comma-string `covering_system` labels like `3,5,7`) is being
**discarded, not extended** — that string-label partitioning *is* the
naming bug, and we want a tighter implementation.
`native/src/covering_sieve_main.cc` is rewritten in place — it is the
single covering-sieve main. `sieve_triage_main.cc`'s triage folds into it;
that standalone main goes away once the stepper owns the triage.

> **BUILT & VALIDATED (2026-06-01).** `native/src/covering_sieve_main.cc` is the
> covering FILTER: VECTORIZED (per-modulus pattern table `pat_s[r]` + Barrett mod,
> no scalar phase search) and MULTITHREADED (N delete-aware `SourceTableReader`
> shards over the 111 data files — `OpenMetadata(...,shard,count)`). Persistence is
> position-delete MOR over `primes_k0_sieve`, a metadata-only SHALLOW CLONE
> (`pp-catalog --clone-sieve`: native IRC CreateTable + FastAppend of primes_k0's
> existing DataFiles, zero row-copy; native CreateTable against the HMS REST servlet
> yields v2 — verified). One odd-prime modulus per pass (`--apply S`); the `r=1`
> trivial obstruction (only p=3) is handled, which unsticks the first hole. Triage
> ranks the next modulus from a residual-bitmask tally against the Mersenne
> primitive factors: `--metric expected|bestclass|hybrid` (3-way compared —
> `expected` ≈ "ascending small primes" won on deletions + coverage + matches the
> backbone). `--report` dumps the per-pass arc from snapshot summaries (the
> snapshots ARE the sieve_passes log). `--classify` gives the backbone
> {3,5,7,11,13,17} residual gap-count distribution in one ~6.5s scan over 3.87B.
>
> KEY FINDINGS: first hole now climbs (149→6073), but greedy depletion is CONCAVE
> (per-pass deletions peak ~9M then decline — won't reach the local→global residue
> greedily). Only **0.13%** of k0 primes are backbone-covered (mode 6 residual gaps).
> `--classify` cross-validates the campaign exactly (0-gaps = 5,185,251 = cumulative
> deletions after {3,5,7,11,13,17}, off by exactly p=3). ~6–11s/pass at 24 shards;
> local runs need MR3 scaled to 0 (keep metastore + mysql only) to free RAM. NEXT
> (research, not a build gap): `--classify` against the FULL small-order
> Mersenne-factor set with an order sweep to size the genuine residue; the 92,160
> residue-class (mod 255255) breakdown for the unconditional-obstruction classes.
> Full state: `~/.claude/.../memory/project_sieve_build_state.md`.

Design (agreed in discussion; now BUILT):

- **Interactive stepper.** `main` steps through one covering pass at a
  time under user control. Plain `\r` progress bar per covering system.
  After each pass, rich triage suggests candidate next-moduli by gap
  frequency with a **threshold** (so ties surface), and the user selects
  the next set as a space-separated list. Goal is the *minimal* covering,
  but with the user in the loop on each choice rather than a fixed greedy
  rule.
- **Persistence = position-delete MOR over a `primes_k0` copy.** Keep
  `primes_k0` as a fixed source; the sieve subtracts covered primes from
  a copy via merge-on-read position deletes, so the live (post-delete)
  rows *are* the current uncovered set. No per-pass string-label table —
  a pass is a snapshot. ("More init time, but no uncovered-list to
  maintain.") Mind how many blocks are held at once; stream batches as
  the existing tools do.
- **Sidecar:** `mersenne_reference.parquet` (factorization + primitive
  factors of $M_m = 2^m-1$ up to $m=64$) for $ord_s(2)$ lookup. Triage
  on the old Pass-1 data had flagged $m=12$ / $s=13$ as the next prime;
  re-derive against the rebuilt sieve rather than trusting that.

> **OPEN / spike-gated — now SCOPED (2026-05-31).** Position-delete
> persistence depends on iceberg-cpp writing row-level deletes. Reading the
> vendored tree confirmed it's further along than this section implied:
> `PositionDeleteWriter` is first-class+tested, `WriteDeleteManifests`
> (`snapshot_update.cc:194`) is *implemented*, and snapshot dispatch is
> generic. The real gap is a committable delete subclass + a
> `Transaction::NewRowDelta()` accessor (mirroring `FastAppend` /
> `NewFastAppend`). The `snapshot_update.cc:212` `data_sequence_number`
> FIXME does **not** bite us: we only delete from a *fixed* `primes_k0`, so
> deletes always post-date the data. Puffin deletion-vector blobs remain
> out of scope — position-delete *files* (MOR v2) are the on-ramp. **Full
> build plan, risks, and done-criteria: `markdown/data_eng/delete_primitive_spike.md`.**
> Prove write→commit→reopen→read on a throwaway before building the sieve;
> fallback is append-only covered-set (FastAppend only).

> **RESOLVED — minimal = irredundant.** A covering system is minimal
> (irredundant) when *every* residue class is required to cover; drop
> any one class and coverage breaks. That is the stop criterion — not a
> fuzzy "fewest moduli" optimization. The interactive threshold-select
> is the *mechanism*; irredundancy is the *definition*.
>
> **Constraint — moduli are odd primes.** The covering moduli `s` are
> odd primes (3, 5, 7, 11, …), never 2. Propagation sifting steps by
> `d = ord_s(2)`, which requires 2 invertible mod `s`.
>
> **Sub-goal — the first hole.** The cover marks covered `k=0` primes;
> the uncovered remainder is the anti-join against `primes_k0`, and its
> `min_p` is the *first hole* — the smallest `k=0` prime the current
> covering system fails to explain. Under the position-delete MOR model
> the live (post-delete) rows *are* the uncovered set, so the first hole
> is just `MIN(p)` of the live table. Tracking how that minimum moves as
> moduli are added is the primary progress signal.

## Tooling

### Native

- `native/src/covering_sieve_main.cc`: **being rewritten in place** (see
  "Covering-sieve rewrite") — interactive stepper, `\r` progress bar per
  covering system, threshold triage with user-selected moduli, persistence
  via position-delete MOR over a `primes_k0` copy. The old "splits into
  `covered_...` / `uncovered_passN` partitions" behavior is discarded.
- `native/src/sieve_triage_main.cc`: bitmask-distribution triage to choose
  the next sieve prime. **Folding into the interactive `covering_sieve_main`
  stepper** (rich post-pass triage); the standalone main is removed once the
  stepper owns the triage.
- `native/src/primitive_factors.cc`: `GetCoverageMask` / `MersenneHelper`.
- `native/src/writer.cc`: Iceberg-aware parquet writer; skips identity-partition source fields (`p_bucket_version`, `p_bucket`) from the physical schema as of 2026-05-26 (uncommitted in primeparts repo as of the wind-down).
- `native/src/drop_bucket_cols_main.cc`: One-shot in-place rewriter that strips physical bucket cols from existing files and re-registers via IRC. Already ran across the full warehouse.
- `native/src/backfill_prime_rank_main.cc`: Existing in-place backfill template (uncommitted reference for any future stream-and-rewrite work).
- `native/src/core.c`: The heart of the solution generation. This could benefit from the ideas in `markdown/math/modular-filter-idea.md` and `markdown/math/modular_filter_more_ideas.md`. 
- `native/src/catalog/` (added 2026-05-31): catalog module, split by backend.
  `pp_iceberg_rest.{h,cc}` consolidates the IRC `MakeCatalog`/`PublishTable`
  logic duplicated across the three mains above (they are NOT yet refactored
  onto it — that's a follow-up). `pp_hive_sync.{h,cc}` is a thin subprocess
  wrapper over `scripts/hive_register.sh` (no iceberg-cpp API exists for Hive
  engine sync). `pp_catalog_main.cc` builds `pp-catalog` with `--smoke-test`
  (verified PASSING 2026-05-31: IRC loadTable + beeline `3→0→3` swap +
  cleanup). See `native/src/catalog/README.md`.

### Hive-side
- HS2 + MR3 + HMS-IRC stack in `mr3/kubernetes/`. See `hive_mr3_stack.md` for the propagation pattern after config edits.
- `scripts/hive_sql.py`: thin JDBC client (pyhive) for ad hoc queries. (Use with caution).
- `scripts/hive_register.sh` (added 2026-05-31): **the permanent Hive-catalog
  sync/registration mechanism.** Remote beeline (`exec`/`describe`/`sync`/
  `register`). `sync` = `ALTER TABLE … SET TBLPROPERTIES('metadata_location'=…)`
  to make the Hive engine adopt a native/IRC-committed snapshot. Requires
  Java 21 + `-Dorg.jline.terminal.provider=dumb` + HTTP transport on :10001;
  see the script header. Verified end-to-end; see "HMS sync is a separate,
  required step".

## Known Issues / Tasks for Next Assistant

### Cutover sweep (open)
- **`primeparts.primes_k0` lives at the old funbuns warehouse path** (`/iceberg/warehouse/primeparts.db/primes_k0/`), not in `/ib-staging/`. DROP + recreate procedure documented in `markdown/data_eng/MV_list.md` §"MVs at wrong on-disk location"; ~290 s rebuild then `rm -rf` the old dir. The DB-location ALTER on 2026-05-27 prevents new MVs from landing there.
- **`obstruction_catalog` at `ib-staging`** holds the discarded Pass-1
  string-label artifact (see "Covering-sieve rewrite"); it will be
  superseded rather than re-swept.
- **Delete unused scripts.** ✅ DONE 2026-05-31: `scripts/materialize_primes_k0.py`
  and `scripts/mvs.sql` removed (were untracked; nothing imported them).
- **Archive the old funbuns layout**. `sync_hms.py` and the SqlCatalog at `/iceberg/warehouse/catalog.db` are dead code paths for the current stack — keep them out of automation, plan to delete.

### Native-commit progression (open)
- **Native covering-sieve commit path.** Under the rewrite the sieve no
  longer writes `obstruction_catalog` string-label partitions, so the
  identity-partition cleanup that applied to that table is **moot**. The
  commit becomes a position-delete MOR over a `primes_k0` copy (spike-gated
  — see "Covering-sieve rewrite") plus the separate HMS sync. The
  `drop_bucket_cols_main.cc` DataFile / partition-value pattern still applies
  to any *data* files the sieve writes.
- **MV REBUILD against externally-committed iceberg snapshots**: untested. Hive expects transactional source semantics; we commit from native via IRC. Decide before any MV is bound to a refresh loop.
- **HMS sync is a separate, required step (verified).** A native IRC /
  on-disk commit does **not** by itself make a snapshot-advancing change
  visible to the Hive engine for read / MV refresh. The remedy is an
  HMS-side operation that sets the table's `metadata_location`.
  **Mechanism chosen and verified: remote beeline.** `scripts/hive_register.sh`
  (added 2026-05-31) issues `ALTER TABLE <tbl> SET
  TBLPROPERTIES('metadata_location'=<uri>)` over a remote beeline JDBC
  connection to HS2 — routing through the same `HiveIcebergStorageHandler`
  the engine reads with, so no `get_table_req` Thrift shim is needed. The
  raw-Thrift `sync_hms.py` path is no longer needed for this.

  > **RESOLVED — beeline HMS-sync probe (run 2026-05-31).** Verified
  > end-to-end on throwaway `primeparts.zz_synctest`: baseline `COUNT(*)=3`
  > → `hive_register.sh sync` to the empty `00000` metadata → fresh
  > `COUNT(*)=0` → `sync` back → `COUNT(*)=3`. So Hive **does** adopt an
  > externally-set `metadata_location`. **Nuance:** the storage handler does
  > *not* do a raw pointer swap — on the ALTER it loads the target's snapshot
  > and **re-commits it as a new metadata.json** (the pointer landed at a
  > freshly written `00003` whose current-snapshot-id equals the target's and
  > whose metadata-log references the file we set). Data outcome is exactly
  > right; the pointer just advances one file forward. This is why the SQL
  > path differs from `sync_hms.py`'s raw Thrift swap — and it's *better* for
  > us (the engine commits through its own handler, guaranteeing visibility).
  >
  > Working invocation details (all were real failure modes hit while
  > bringing it up, now encoded in the script): **Java 21** required (host
  > default Java 25 breaks bundled jline FFM provider);
  > `-Dorg.jline.terminal.provider=dumb` required (non-interactive jline
  > can't allocate a terminal otherwise); **HTTP transport** on `:10001`
  > (`transportMode=http;httpPath=cliservice`), auth NONE; classpath =
  > dist `lib/*` + `hive-jdbc-*-standalone.jar` + a full `hadoop-common`
  > jar (standalone jar lacks `HadoopExecutors`).

### Cluster config (uncommitted in primeparts repo)
Config files edited 2026-05-27 to switch to Config A + ALTER DATABASE primeparts location. The diffs live in the working tree only:
- `mr3/kubernetes/conf/hive-site.xml` (task.memory.mb, containergroup envelope, pipelining threshold)
- `mr3/kubernetes/conf/mr3-site.xml` (autoscaling enabled with caps, total.max.memory.gb=8)
- `mr3/kubernetes/conf/tez-site.xml` (sort.mb, unordered.output.buffer)
- `mr3/kubernetes/yaml/{hive,metastore}.yaml` (pod requests/limits)
- `mr3/kubernetes/env.sh` (HS2 + metastore heap sizes; HIVE_WAREHOUSE_DIR points at /ib-staging)

- **busybox init-container GC** still recurs (manually re-pulled this session). Three durable fixes documented in `hive_mr3_stack.md` §"Known gotchas" #9 — none applied yet. Lowest-friction option is replacing the init image with the hive image itself (`command: ["/bin/true"]`); a one-line config change pinning the layer to the worker container's image reference.

-We will continue to validate these changes as we progress with foundational MVs and wiring Hive outputs into native code paths.

### Original known issues (still relevant)
- **Writer Bug** (covering-sieve path): the Hive-style path naming vs.
  string `covering_system` partition conflict is **mooted** by the
  covering-sieve rewrite (no string-label partition; persistence is
  position-delete MOR over a `primes_k0` copy). The `BucketParquetWriter`
  "refusing to overwrite" check still needs careful handling in
  multi-threaded runs.
- **Commit Logic**: The `drop_bucket_cols` / rewrite tools commit via
  `FastAppend`; ensure `partition_values` are correctly mapped to the
  `DataFile` objects (see `drop_bucket_cols_main.cc:MakeDataFile` for the
  pattern that omits identity-partition source columns from `value_counts`).
  Note: the **covering-sieve** commit is no longer FastAppend-only — its
  defining operation is the position-delete (spike-gated, see
  "Covering-sieve rewrite"); FastAppend covers only any data files it
  writes.
- **Scalability**: The input partition is ~8.6GB (39 files). The tool must stream batches (currently using 500k row chunks) and avoid collecting large tables into memory to prevent OOM.

### Should be Solidified first:

The following options were raised in conversation, weighed, and intentionally deferred. Reviving any of them is straightforward:

- **Cluster expansion** to fit `q_k_freq_top`-style aggregates (one-worker × 8 GiB envelope, or 3 workers via larger node). The user said "we can try expanding" but it wasn't needed once we pivoted to the histogram.
- **`hive.mr3.container.max.java.heap.fraction` 0.7 → 0.8** for ~20 % more heap in the same container. Untested; risk is parquet's DirectByteBuffer use.
- **Bucket-map-join EXPLAIN check** for `primes ⨝ partitions ON p`. The layout supports zero-shuffle; planner behavior not verified.
- **MV `ALTER TABLE ... WRITE ORDERED BY p`** to propagate sort_order. `ORDER BY` queries on built MVs are paying a full resort right now.
- **Drop the busybox initContainer in favor of the hive image itself** (`command: ["/bin/true"]`). 
- **Native MV equivalent for `q_k_freq_top`** — streaming aggregate in C++ via iceberg-cpp, committed via IRC. Would sidestep Hive entirely for the cardinality-hardest aggregate. Sketch: 12-thread parallel chunked, sparse-hash, emit (q, count) pairs only where count ≥ 2. **NB:** an earlier framing here claimed "most q_k counts are exactly 1" — wrong; it conflated the verified `n_k = 1` dominance (~99.9 % of edges) with `q_k` multiplicity, which is actually ~1.8× at large magnitudes (q's repeat). The distinct-`q_k`-count is its own harder problem; size this sketch against real multiplicity, not the n=1 fact.
- **Sort-merge bucket join** validation for any future join-heavy MV (e.g. `primes_with_first_parent`). Both tables share the `(p_bucket_version, p_bucket)` identity partition; the SMB path should be available.
- **UDF metadata track (deferred).** Per the Iceberg 1.11.0 UDF spec,
  attach UDF metadata as manual JSON (iceberg-cpp lacks high-level
  support; beeline is the alternative). Separate track from the sieve /
  HMS-sync work; not blocking.
- **Classify Remaining TODOs** There is a number of other prospective features, optimizations, fixes and optimizations that have surfaced in recent months. It would be nice to get a roadmap going.

### Next Concrete Step

Agreed ordering from the latest discussion:

1. **Cleanup (low-risk, approved).** ✅ DONE 2026-05-31: deleted
   `scripts/materialize_primes_k0.py` and `scripts/mvs.sql`; relocated
   `primeparts.primes_k0` to `/ib-staging/` (DROP + recreate, verified
   3,874,747,523 rows at the new location). Old funbuns-path dir `rm -rf`
   left to the user.
2. **Two unblocking spikes (before any sieve code).**
   - *beeline HMS-sync probe* — ✅ **DONE / GREEN 2026-05-31.** Verified Hive
     adopts an externally-set `metadata_location`; mechanism is
     `scripts/hive_register.sh sync` (remote beeline). See "HMS sync is a
     separate, required step" for the verified cycle and the
     re-commit-not-raw-swap nuance.
   - *delete-primitive spike* — ✅ **DONE / GREEN 2026-05-31.** `pp-catalog
     --delete-spike` proves PositionDeleteWriter → RowDelta IRC commit →
     HMS-sync → delete-aware read-back (5 → delete 2 → 3). Persistence model
     for the sieve is position-delete MOR (no append-only fallback needed).
     `RowDelta` lives in-tree (`native/src/catalog/pp_row_delta.{h,cc}`); no
     `Transaction` accessor needed. Required three vendored-lib patches
     (`native/vendor/PATCHES.md`): `file:/` URI scan fix, warning-cache, and
     the `assert-ref-snapshot-id` `ref` field fix (the latter gates ALL
     snapshot-advancing native IRC commits, not just deletes). Full writeup:
     `markdown/data_eng/delete_primitive_spike.md`.
3. **Covering-sieve rewrite** — ✅ **DONE & VALIDATED 2026-06-01** (see the
   "BUILT & VALIDATED" block under "Covering-sieve rewrite"). The filter, triage
   (3 metrics), `--report`, and `--classify` are built and proven end-to-end on the
   full 3.87B `primes_k0`. Remaining is research (sizing the local→global residue),
   not build.

The broader arc (config solidification, foundational MVs, then wiring Hive
outputs into the native graph-exploration paths) continues underneath.
