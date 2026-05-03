# Remaining consumers to port off `blocks/`

## Status as of 2026-05-02

This document is historical for the current checkout. The modules listed below
under `src/funbuns/...` are no longer tracked source files in this repository;
only stale `__pycache__` artifacts remain under `src/primeparts/__pycache__`.
Do not use this as an active punch list without first re-inventorying the
current `src/primeparts/` tree.

Current tracked Python source is much smaller and already has Iceberg scan
helpers:

- `src/primeparts/iceberg_schema.py::scan_primes`
- `src/primeparts/iceberg_schema.py::scan_decompositions`
- `src/primeparts/iceberg_query.py`

Decision point:

- If old analysis modules are restored from history, port them directly to the
  Iceberg helpers above. Otherwise archive this document and replace it with a
  current analysis/query roadmap.

Context: the iceberg ingest cutover (step 7 of the migration plan, finished
2026-04-14) wired `core.PPConsumer.save_callback` directly to
`iceberg_schema.IcebergWriter.flush`. New data lands in the iceberg
catalog at `/media/extssd/research/dioph.pp/data/iceberg/` with no
on-disk staging. The legacy `blocks/pp_b*.parquet` tree was deleted in
step 6 (130 GB).

Every read-side consumer that still globs `blocks/pp_b*.parquet` is now
broken at first call. None of them have been ported yet — the cutover
only touched the write path. This document is the punch list for the
next session.

Current state of ground truth:
- `src/funbuns/iceberg_schema.py` — authoritative schema and writer
- `markdown/iceberg_data_setup.md` — stable write-side layout doc
- `markdown/iceberg_prototype_findings.md` — 2026-04-13 step-1 prototype (historical)
- `tests/test_iceberg_commit_seq.py` — pins the `commit_seq` reorder invariant
- `CLAUDE.md` — updated to reflect iceberg as primary store

---

## 1. Analysis modules still globbing `blocks/` (broken on first call)

All six of these will raise on first invocation because the glob returns
an empty list. They should be ported to `pl.scan_iceberg(...)` against
the `funbuns.primes` and `funbuns.decompositions` tables. Predicate
pushdown via manifest stats happens transparently, so p-range filters
only open overlapping files.

| Module | Lines referencing `blocks/` | Notes |
|--------|------------------------------|-------|
| `src/funbuns/ladic.py` | 137, 170 | `sorted(get_data_dir().joinpath("blocks").glob("pp_b*.parquet"))`, then `pl.scan_parquet(block_path)` per block. The per-block loop structure should collapse into a single lazy scan with filters. |
| `src/funbuns/spectral.py` | 28, 40, 140 | `_block_pattern()` helper + two scan sites. FFT over obstruction indicator — needs the full `p`-column and `k==0` marker, both in `primes`. |
| `src/funbuns/remainder.py` | 35, 128 | Same pattern as ladic. Uses Rust `full_profile` per row — unchanged, only the source scan moves. |
| `src/funbuns/zipf.py` | 29 | Bounded-memory Zipf fit on `q_k`. Needs `decompositions.q_k` column only. |
| `src/funbuns/data_exploration.py` | 28, 43, 60, 244 | `_block_pattern()` + four scan sites (q-chains, bipartite adjacency, gap anatomy). |
| `src/funbuns/fixed_mod.py` | 232, 248, 293 | Still pending a separate SageMath rewrite (pending-work item #1) — coordinate with that task. The read path is orthogonal to the arithmetic rewrite but both touch the same module. |

**Suggested approach**: introduce a single helper in `utils.py` (or a
new `funbuns.read` submodule) that returns `(primes_lf, decomp_lf)` as
polars lazy frames scanning the iceberg catalog, read from
`[tool.funbuns.directories].iceberg_dir`. Every module above switches to
that helper and drops its local `_block_pattern()` / glob helper.

**Decision to make**: whether to preserve the per-block iteration
pattern (useful for bounded-memory streaming in `zipf` and `remainder`)
or collapse to one lazy scan and rely on polars streaming. For modules
that already aggregate into a single frame (`ladic`, `spectral`,
`data_exploration`) the collapse is safe. For `zipf` the current
per-block flush exists specifically to bound memory — verify a lazy
streaming scan does the same before deleting the loop.

---

## 2. DuckDB retirement (issue #2, parallel track)

DuckDB was the query layer over the old `blocks/` glob. The webserver
and `funbuns-admin db *` commands still read from it, but the index is
stale (built against deleted block files) and the underlying
`decompositions` view points at a parquet glob that no longer resolves.

| File | Role | State |
|------|------|-------|
| `src/funbuns/querydb.py` | DuckDB wrapper, `partition_counts` table + `decompositions` view | `_parquet_pattern()` at L116 still returns `blocks/pp_b*.parquet`; `sync_blocks` at L277 globs the same tree. Entire view layer is broken. |
| `src/funbuns/admin.py` | `funbuns-admin db {build,sync,status,sync-blocks}` | Thin CLI over `QueryDB`. `sync-blocks` subcommand is meaningless post-cutover. |
| `src/funbuns/webserver.py` | FastAPI partition browser | Consumes `QueryDB`; broken until querydb is retargeted or replaced. |
| `src/funbuns/viewer.py` | Altair dashboard; L24 still reads `config.get('blocks_dir', 'data/blocks')` | Same breakage. |

**Option A — retarget querydb onto iceberg.** Swap `_parquet_pattern()`
for a PyIceberg-driven file list (or a DuckDB `iceberg_scan()` call if
the DuckDB iceberg extension is available in the pixi env). Keeps the
existing `partition_counts` index rebuild semantics. Cheapest path to
restore the webserver.

**Option B — retire DuckDB entirely.** Move partition-count queries into
polars `scan_iceberg` + `group_by("p").agg(pl.col("k").sum())`. Rebuild
the webserver as a FastAPI shell over polars. More work but kills the
issue #2 track permanently.

User has historically leaned toward Option B (notebooks replacing the
webserver). Confirm direction before starting.

---

## 3. Rust `funbuns_graph` retarget (deferred)

`funbuns_graph/src/bin/build_graph.rs` (currently modified on the
`liminal` branch) reads parquet from the legacy blocks path. This was
explicitly **deferred** in the cutover plan pending a
webgraph-vs-alternatives evaluation — the graph build is big enough that
the choice of graph storage format matters more than the parquet source.

Unblocking this needs a decision: webgraph (current prototype) vs. a
simpler CSR-on-disk representation vs. something else. No work should
happen on the parquet read side until that's settled, because the graph
storage decision may reshape what the reader wants to pull out of each
row.

See `project_graph_build.md` in memory for the standing notes.

---

## 4. Notebook rewrite

`notebooks/exploration.ipynb` currently mixes DuckDB, SageMath, and
VegaFusion. It depends on the DuckDB stack from track (2) and will
break the moment the webserver-backing view does. Lowest-effort fix is
to rip out the DuckDB cells and rewrite against `pl.scan_iceberg`
directly; this also happens to be the target end-state if Option B
above wins.

---

## 5. Iceberg integrity tool (deferred; reborn `bmgr --integrity`)

The old `bmgr --integrity` / `bmgr --integrate-check` commands did:
- duplicate row detection across blocks
- block filename consistency
- `dataset_summary` logging

The iceberg port would be:
- `sum(primes.k) == rows(decompositions)` (already a writer invariant,
  but worth a standalone verifier)
- no duplicate `p` in `primes`
- every `decompositions.p` joins into `primes`
- anti-join audit vs. SageMath `prime_range` for gap detection

Not urgent — the write path enforces all of these synchronously — but
will be wanted once analysis consumers are producing results worth
cross-checking. Low priority; do after track (1).

---

## 6. SIGINT durability test rewrite (task #6)

`tests/test_core.py::TestGracefulSIGINT::test_sigint_flushes_completed_results`
is currently marked `@pytest.mark.skip`. The original test polled the
legacy `runs/*.parquet` flush directory, which no longer exists, and
unsafely wrote into the production iceberg catalog because
`iceberg_dir` was not sandboxed.

Rewrite target: spin up a `--temp` warehouse in `tmp_path`, launch
`funbuns -n 10000000 -b 1000000`, poll `tbl.current_snapshot()` until
at least one commit lands, then `SIGINT`. Assert:
1. Clean exit (rc 0, no `KeyboardInterrupt` traceback).
2. `tbl.current_snapshot()` is non-null after termination.
3. `sum(primes.k) == rows(decompositions)` still holds across the
   partial dataset.

Self-contained, no dependency on tracks (1)–(5).

---

## Recommended order for the next session

1. **Ship track (1)** first — it's the widest break surface (six
   modules) and the fix is mechanical once the `scan_iceberg` helper
   lands. Start with `ladic.py` since it's the user's current research
   direction.
2. **Decide DuckDB Option A vs B** (track 2), then execute. This
   unblocks the webserver and notebooks simultaneously.
3. **Task #6** (SIGINT test) whenever there's a quiet moment — small,
   self-contained, won't conflict with anything else.
4. **Notebook rewrite** (track 4) after (2) lands.
5. **Iceberg integrity tool** (track 5) when analysis results need
   cross-checking.
6. **Rust graph retarget** (track 3) gated on the webgraph decision.
