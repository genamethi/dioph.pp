# 05 — the collapse consumer + derived representation

The first real FileScanTask consumer + the query engine behind it (holes
registry: open ground). Built on the phase-04 substrate. The math is settled in
`../../math/collapse_findings.md`; the algebraic backbone in
`../../math/hopf_structure.md`.

## Done

- **Collapse DP, native** (`native/src/graph/pp_graph.cc`). Reads `partitions`
  via the server plan (`PlanScanOnServer`) + in-process DuckDB `read_parquet` —
  edges are read, never `is_prime_power`'d. Forward sweep in p-order (every parent
  `q < p`), no recursion. Validated exact against the Sage reference; the 1e6 run
  reproduces the documented spectrum.
- **Trie-DAG word store** = the Hopf coproduct: a word is `(a0, tail)` over
  interned segments `Seg{parent, n, C}`, so shared prefixes are shared. The memo
  is a set of int32 word-ids; the root is derived (`q = W⁻¹(p)`, unique because
  `W` is monotone), and count is a comodule multiplicity — both dropped from the
  core. `node_classes = (node, word)`; the store itself is the product, destined
  for disk.
- **Derived tables** via `MaterializeColumns` (typed int/long/string, stat
  bounds) published over the REST catalog (`MakeCatalog({rest_uri}, warehouse)`)
  through the daemon — parquet on the warehouse FS, metadata CAS by catalogd:
  `chain_words(word_id, degree, skeleton, translations, hermite, symbolic)` (GiNaC
  Hermite features) and `node_classes`. `word_id` is dense-by-sort; a
  stable/append-only id across bounds is a scaling concern.
- **Parallel sweep** (`--workers`): a Kahn topological sweep over a sharded
  concurrent store. Contention-limited by a single queue lock; a work-stealing or
  sharded queue is the lever, tuned after the sliced algorithm settles.

## Sliced sweep (in progress) — the RAM bound

Process contiguous p-range slices (default width 1e6, 12 workers per slice) and
dump each slice independently — no frontier memo carried across the boundary.

- Trace normally within a slice. When a chain reaches a node **below the current
  range**, that node is the meeting point (a k=0 root, or a node an earlier slice
  computed): stop, record the **connection** (the node id), do not re-trace. The
  trie makes this fall out — a word crossing the boundary is `(upper segments,
  connection → node q)`, `q` an id into the earlier slice's dump. The connection
  is opaque during compute, so no prior-slice read happens mid-sweep, and each
  slice gets a fresh store (connections reference nodes, not words).
- Per-slice output: `(node, partial-word, connection)` + the slice's trie. The
  whole graph is the union of slice files stitched at connection nodes.
- **The connection algebra is validated**: deferring below-range parents as
  connections and reassembling by composition (replay the upper word's
  push-sequence onto each connected chain) reproduces the single-pass result
  exactly, serial and parallel, for any slicing.

## Done 2026-07-23 — sliced disk path (`native/src/graph/pp_graph_store.{h,cc}`)

- **Per-slice disk dump/reload.** `SweepAndSpill` processes contiguous p-range
  slices (`--slice-width`, default 1e6); each slice sweeps into a fresh `ConcStore`
  + memo, spills to a private binary log (`slice-<s>.pplog`) in local scratch
  (`--spill-dir`), then frees before the next slice allocates. One slice resident
  during the sweep — the 24–28 GB budget. Reload via `LoadSlice`.
- **Reassembly is bottom-up + memoized with eviction.** `ReassembleForMaterialize`
  reads slices ascending, interns every complete word into one global `ConcStore`,
  and evicts a connection node's cached expansion once the last slice that
  references it (`last_ref_slice`, computed during the sweep) is past — bounding
  reassembly like the single-pass `last_child` eviction. The near-cartesian bulk
  blowup is avoided per-node but the *output* forms (`node_words`) can still be
  large; a genuinely lazy read is unnecessary because the durable tables are
  complete (see below) — a query is a direct key-window read on the sorted tables.

## Persist the trie/coproduct — resolved 2026-07-23

`word_id` = a distinct **complete** word, **dense global by-sort** (canonical
`(a0, segments)` order). Under this identity the separate
`words(word_id, parent, n, C, slice, conn)` table is **subsumed by `chain_words`**,
which already carries the structure (`skeleton`/`translations`) keyed by the same
`word_id`; `slice`/`conn` live only in the transient spill logs. Durable output is
two tables, both matching the single-pass semantics:

- `node_words(node_id, word_id)` — node → complete word. Written via `CommitFiles`
  (single-table FastAppend over the REST catalog), sorted by `node_id` with the
  ascending sort order declared **at table creation** (`TableDeclaration.sort_order`),
  `StatColumn` bounds, rolled at ~8M rows. (This is the renamed `node_classes`.)
- `chain_words(word_id, degree, skeleton, translations, hermite, symbolic)` — the
  GiNaC features, via `MaterializeColumns` (replace).

`CommitFilesAtomic` was **not** used: it runs `BuildPartitionStatsForAppend`, which
builds a 0-child struct array on an Unpartitioned table and Arrow throws — filed in
the holes registry. Single-table `CommitFiles` is the correct spec-conformant path
here.

Verified (bound 1e6): counts byte-identical to single-pass
(`node_word_pairs=9,939,483`, `distinct_words=1,175,715`, `max_degree=12`); tables
land over REST with the declared sort order; `word_id` dense `0..N-1`; peak RSS
791 MB (10 slices). Unit regression `PpGraphStore.SlicedDiskReproducesSinglePassAcrossWidths`
pins content-equivalence across slice widths.

## Scale reckoning 2026-07-24 — the complete-word form does NOT reach full range

The full source is **44,655,205,838** partition rows (edges), max `p = 6.19e11`.
Measured growth of the complete-word materialization (fixed slice width, bounds
1e5..3e6): `node_word_pairs ~ edges^~1.9..2.0` (near-quadratic). Projected to full
scale: `node_words` ~2.4e17 rows / **~538 PB**; reassemble ~10^4 years. The
complete-word tables (`node_words`, `chain_words` keyed by complete `word_id`) are
therefore a **bounded-prefix** artifact only (good to ~1e6–1e7), not the full-range
representation. This is the near-cartesian blowup the phase warned about, landing on
the "complete word, drop slice/conn" identity chosen 2026-07-23.

The **compact coproduct** scales completely differently. Measured `--sweep-only`
(fixed width 250k): every per-edge metric IMPROVES with scale (memo/edge 42.8→5.04,
sweep 13.4→1.40 µs/edge) and `conn/memo` rises 0.22→0.64 toward 1 (saturating
compression). Segment alphabet is ~constant (~425k distinct `(n,2^m)`). Projected
full-range compact size **~0.8–1.1 TB**, sweep **~10–17 h single-threaded / ~1 h at
16×** — feasible (~5× the 182 GB source). Slicing is itself a compression/speed win,
not just a RAM bound: at bound 3e6, width 3e6→50k shrank spill 3.96 GB→47 MB and
sweep 300 s→0.56 s.

Structural reason it must scale: the degree ceiling `max_degree = floor(log_3 B)`
(measured 12@1e6, 13@3e6) gives `24` at full range, and every non-trivial segment
has `n>=2`, so `prod n_i <= 24` forces `r <= 4` — **no word over the whole dataset
has more than four non-trivial segments**, and the skeleton alphabet is ~100 for
the entire range. Nearly all information is in the translations, which are the
solutions of the per-`(node, skeleton)` pinning equation (node 137, skeleton `[2]`:
`(0,128),(2,112),(8,16)` each solving `(3+A0)^2+C1=137`). Root recovery is free
(monotone `W`, `q = W^-1(p)`). NB the algebra is **not** established as Hopf —
`hopf_structure.md` is explicitly open on the `C`-translation coproduct; what is
solid (monotone⇒derivable root, deconcatenation=trie, grading by `r`) is all the
store needs. `r<=4` follows from the degree ceiling alone, independent of Hopf.

### The ingest is the current blocker, not storage

`pp_graph.cc main` loads **every** edge globally before sweeping: a DuckDB
`read_parquet` of all files into `std::vector<Edge>`, then a second pass building
`parents` (a second full copy) + `has_children` + sorted `nodes`, then a third for
`last_child`. At 44.66e9 edges that is ~1.07 TB for the edge vector alone (>2–3× with
the maps). This ingest is inherited from the pre-slicing single-pass DP; the sliced
`SweepAndSpill` only replaced the DP stage. The sliced path needs far less — edges in
the slice's p-range (a p-range scan; `partitions` is p-sorted so the key window
prunes), below-slice parents opaque (only `q`'s value, no adjacency lookup), and
`last_ref_slice` in place of `last_child`. What a p-range scan does NOT give: q-only
root nodes (k=0 primes, recoverable per-slice from `primes` filtered `k=0` over the
same range — the deferred `primes_k0` item) and `has_children` (needs a pass or a
per-slice sink flag).

### Direction (data shape drives the engine drop-in)

- **Streaming per-slice p-range ingest** — replace the global edge load with a
  per-slice scan over `partitions` (+ `primes` k=0 for roots). Removes the 1.07 TB
  ceiling; per-slice edge RAM ~17 MB at width 1e7.
- **Typed, analytics-first schema** — `skeleton` string → `skeleton_id int32` +
  a ~100-row `skeletons` dictionary; `translations` string → flat `A0,C1..C4 int64`
  (`r<=4` bounds the width); `hermite`/`symbolic` → a side table (presentational,
  most of the bytes, never predicates). JSON-string bounds are lexicographically
  inert (see holes registry) — typing makes bounds prune.
- **Natural partitioning** — derived tables partitioned p-contiguous, aligned to
  the source `(p_bucket_version, p_bucket)` bit-band spec (`schemas.cc:56`) so joins
  co-prune and scan plans prune on the same key window. Required once a table > 4 GB.
- **Engine** — storage stays typed + stats-bearing so Substrait predicates push into
  the scan and Acero (vendored in arrow, community substrait ext wired in
  `configure:249`) executes them; analysis-time speed also leans on DuckDB caching,
  not the parquet/metadata layer alone.

### Materialize seam fixed 2026-07-24

`MaterializeColumns` previously hardcoded `TableDeclaration{}` (no sort order, no
properties), was opt-in-and-forget on stats, and wrote a single file. Now takes a
`MaterializeOptions{sort_keys, properties, target_file_bytes}`, declares the
ascending sort order at creation, is **analytics-first on stats** (every column
statted unless `MaterializeColumn.no_stats` — set only on presentational
`hermite`/`symbolic`), and byte-slices into target-sized files. Verified on
`chain_words`: sort-order present; Iceberg manifest bounds now written for
`word_id`/`degree`/`skeleton`/`translations`, absent for `hermite`/`symbolic`.
Two stats layers clarified in the holes registry: manifest bounds (file pruning,
now correct) vs parquet footer stats (row-group pruning, int/long-only, unchanged).

## Deferred

- Fuse the two-level `Push` intern (one hash instead of seg-then-word).
- Parallelize the GiNaC feature pass, and the per-slice sweep (the disk path is
  currently sequential per slice; the in-memory `RunSlicedParallel` remains).
- Byte-shaped compaction to 1–2 GB files / 128–256 MB row groups — `node_words`
  currently rolls by row count (~8M rows/file); byte-precise sizing via
  `AlignedBucketWriter`/`ShapePolicy` if the row-count proxy proves insufficient.
- **Injectivity lemma** (collapse classes = canonical words; Ritt, nonzero
  inter-power constants) written down — underwrites the word representation.
  Empirically the Hermite vector is a unique fingerprint per word.
- **Regenerate `primes_k0`** from the `primes ⟝ partitions` anti-join under the pp
  convention, obsoleting the foreign `primeparts.db/primes_k0`. Ties into
  re-wiring `generate.cc` (see the stats-less-tables hole).
- **Incremental-idempotence regression** in `e2e_test.cc` (re-running a bound is a
  wholesale rebuild under dense-global ids). The reading==computing regression is
  done as a unit test (`tests/pp_graph_store_test.cc`).
