# pp-graph collapse consumer — handoff

**Status: NOT SETTLED. The whole storage approach may need rethinking before it is
correct at full scale.** This documents where the code stands and the open decisions,
so a fresh agent can pick up without re-deriving. Read `05_collapse_consumer.md`,
`../../math/collapse_findings.md`, and `../../math/hopf_structure.md` first.

## The problem, sized

Source `partitions` = **44,655,205,838** edges (rows), max `p ≈ 6.19e11`, 182 GB, 170
files. Each row is an edge `q --(m,n)--> p` for `p = 2^m + q^n`. A chain root→…→p
collapses to a canonical word `(A0; (n1,C1);…;(nr,Cr))`. The degree ceiling
`max_degree = floor(log_3 B)` (measured 12@1e6, 13@3e6) gives 24 at full range, and
every `n_i ≥ 2`, so **r ≤ 4** over the whole dataset — a hard, load-bearing bound.

## The core unresolved decision: what is the durable store?

Three representations, with measured/derived sizes at full range:

| form | full-range size | lossless? | notes |
|---|---|---|---|
| complete words (`node_words`=node→complete word) | **~538 PB** | yes | near-cartesian `node_word_pairs ~ edges^~2`; INFEASIBLE beyond ~1e7 |
| compact coproduct (partial trie + `conn`) | **~1 TB** | yes | measured via `--sweep-only`; the current prospective path |
| `(node, skeleton, count)` rollup | ~83× smaller than complete | **NO** | lossy — cannot recover which translations are realized without recomputing the collapse (pinning equation over-generates) |

Decision history (important, because it thrashed): the durable `word_id` was first
chosen as **complete word** (drop slice/conn). The scale numbers then showed that is
538 PB and infeasible, so direction shifted to the **coproduct**. Much code was
nonetheless built on the complete-word path before the shift. The coproduct is
currently only a **PROSPECTIVE option** — it builds and runs but is **not verified
lossless** (no test reads the coproduct tables back and reproduces the collapse).

**Open question the next agent must answer before trusting any of this:** is the
coproduct-as-stored actually sufficient to reconstruct the collapse results
correctly and efficiently (complete words on demand, `(node,skeleton)` rollups,
Hermite features)? If reconstruction is expensive or ambiguous, the representation
needs rethinking — possibly a different factoring of skeleton vs translations, or
accepting a lossy rollup plus a bounded exact prefix.

## What is built (native/src/graph/pp_graph.cc, pp_graph_store.{h,cc})

Pipeline: **plan → stream edges in p-order → per-slice sweep+spill → persist**.

- **Streaming ingest** (`--stream`, `SliceSweeper`): one DuckDB `UNION ALL` of
  `partitions` edges + `primes` k=0 roots, `ORDER BY p` (temp on spill disk), grouped
  by `p` into `SliceSweeper::AddNode`, which holds ONE slice's `ConcStore`+memo and
  spills+frees at each boundary. Roots MUST come from `primes` (they never appear as
  edge-children). VERIFIED: identical collapse counts vs the old global-load path at
  1e6 (`9,939,483/1,175,715/12`) and 1e7 (`701,780,577/27,948,680/14`). Replaces the
  1.07 TB global edge-vector load. Unit: `PpGraphStore.SliceSweeperStreamsToSameResult`.
- **Spill format** (`SpillSlice`/`LoadSlice`, `slice-<s>.pplog`): per slice, segments
  `(parent,n,c)` + partial words `(a0,tail)` + memo `node→[(word,conn)]`, dense-local
  ids. This IS the coproduct in transient form.
- **Coproduct persistence** (`BuildCoproductSlice` + the `--materialize` branch of
  `PublishFromSpill`): interns per-slice into typed columns with **slice-prefixed**
  word ids `(slice<<40)|local` (no global-intern blowup, append-stable) and a global
  `SkeletonDict`. Writes three tables via the materialize seam:
  - `words(word_id, a0, skeleton_id, c1..c4, slice)` sorted by word_id
  - `node_words(node_id, word_id, conn)` sorted by node_id
  - `skeletons(skeleton_id, n1..n4, len)` dict (~21 rows @1e6, ~100 full range)
  RAN at 1e6: words=1,158,669, node_words=6,294,460, skeletons=21, **build=0.42 s**
  (vs 35 s for the complete-word reassembly it replaces). NOT verified lossless.
- **Materialize seam fixed** (`query/materialize.{h,cc}`): `MaterializeColumns` now
  takes `MaterializeOptions{sort_keys, properties, target_file_bytes}`, declares the
  sort order at creation, is analytics-first on stats (`MaterializeColumn.no_stats`
  opt-out, set only on presentational columns), and byte-slices into target files.

## What is dead / stale / inconsistent (clean up or decide)

- **Complete-word path is now unused by default but still present**:
  `ReassembleForMaterialize` (+ its parallel `workers` scaffolding),
  `ReassembleFromDisk` (still used by the non-materialize counts path and tests).
  Parallelizing `ReassembleForMaterialize` was a **dead end** (verified: threads
  1→20 = 34.7→35.8 s; work concentrates in a few high-degree nodes). Kept only for
  the counts/validation path and unit tests.
- **`chain_words` on disk is now STALE** (last written by the complete-word path). The
  coproduct path does not write it. Hermite/`symbolic` features are not currently
  materialized at all — they must become a derived, bounded side view.
- **Legacy single-pass path** (bottom of `main`, no `--stream`/`--spill-dir`) still
  writes complete-word `chain_words` + `node_classes` via `NodeClassWriter`. This is
  inconsistent with the coproduct `node_words` (same conceptual object, different name
  and schema). Decide whether to retire the single-pass materialize.

## Residuals / risks for the full 44e9 run

- `has_children` is a global in-memory set (~node count; needs spill at billions).
- Coproduct persistence currently **accumulates all columns in memory** then calls
  `MaterializeColumns` (one batch). Fine to ~1e8; for full scale it must write
  **per-slice incrementally** (append), not accumulate. The slice-prefixed ids and
  per-slice `BuildCoproductSlice` already make this straightforward.
- `ORDER BY p` spills to disk; the no-sort ordered-file read (exploit the physical
  `(p,m_k)` sort + p-disjoint bit-band buckets) is the follow-up. Partitions/primes
  are physically `(p,m_k)`-sorted though metadata declares unsorted.
- **Partitioning**: derived tables are Unpartitioned. Once any exceeds ~4 GB they
  should be partitioned p-contiguous, aligned to the source `(p_bucket_version,
  p_bucket)` bit-band spec (`schemas.cc:56`) so joins co-prune. Identity partition
  columns can be pruned on but not projected (known-red synthesis hole).
- **Parquet footer stats** are int/long-only (arrow default); manifest bounds cover
  strings. String bounds are lexicographically inert — typing (done for the coproduct)
  is the fix. See holes registry `writer stats layers`.

## Engine (decided substrate, not yet wired)

DuckDB CLI + `iceberg` + `substrait` community ext installed in `configure:249-276`;
Acero is in vendored arrow. Only DuckDB `read_parquet` is used today (in
`pp_graph.cc`); Acero/Substrait are unwired. The execution engine drop-in is meant to
follow the settled data shape — which is exactly what is not settled. DuckDB caching
is an option for analysis-time speed so the parquet/metadata layer is not the sole
lever.

## Verification / repro

- `cd native && make pp-graph && make test` (48 pass / 2 known-red partition-stats).
- Coproduct run: `build/pp-graph --bound 1000000 --spill-dir <scratch> --slice-width
  250000 --stream --materialize` → `[coproduct] words=… node_words=… skeletons=…`.
- Counts validation (complete-word, for parity): drop `--materialize` → `[collapse]`.
- `--sweep-only` reports compact-form sizes without the reassembly.

## First thing the next agent should do

Write a test that reads `words`+`node_words`+`skeletons` back and reconstructs the
collapse (complete words / counts), proving the coproduct is lossless — OR conclude
it is not and redesign. Everything downstream depends on that answer.
