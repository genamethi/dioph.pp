# Covering-sieve rewrite — fan-out brief & source index

Entry point for exploring/scoping the covering-sieve rewrite (the next concrete
step; both gating spikes are green as of 2026-05-31). Written pre-compaction so
the material survives. Read `HANDOFF.md` first for the objective and the
"Covering-sieve rewrite" design block; this doc is the **map of where things
live** so exploration is targeted, not a re-derivation.

## The objective in one paragraph

Classify `k=0` (obstructed) primes by minimal covering systems. A covering is
**minimal = irredundant** (every residue class required). Moduli are **odd
primes**; propagation sifting steps by `d = ord_s(2)`. Persistence is
**position-delete MOR over a `primes_k0` copy**: the live (post-delete) rows are
the current uncovered set; the **first hole** is `MIN(p)` of that live set, and
tracking how it moves as moduli are added is the primary progress signal. The
stepper is interactive (user picks next moduli from threshold-ranked triage).

## Source index (verified to exist 2026-05-31)

### Math / design (the "what" and "why")
- `markdown/math/modular_filter_more_ideas.md` (13 KB) — the consumer-side
  reasoning; n=1 worst-case argument; the "cheat code" lookups. **Primary.**
- `markdown/math/modular-filter-idea.md` (5 KB) — earlier/seed version.
- `markdown/math/structures.md` (33 KB) — broadest algebraic/geometric framing.
- `HANDOFF.md` — "Mathematical Context", "Covering-sieve rewrite" (design,
  agreed-but-not-built), and the RESOLVED block (minimal=irredundant, odd-prime
  moduli, first-hole = MIN(p) of anti-join).

### Native code (the "how", current state)
- `native/src/covering_sieve_main.cc` (24 KB, **untracked, being rewritten in
  place**) — the single covering-sieve main. Old "covered_/uncovered_passN
  string-label partition" behavior is DISCARDED.
- `native/src/sieve_triage_main.cc` (5 KB) — bitmask-distribution triage to pick
  the next sieve prime; **folding into** the interactive stepper, then removed.
- `native/src/primitive_factors.cc` + `include/primeparts/primitive_factors.h`
  — `GetCoverageMask` / `MersenneHelper`; `ord_s(2)` machinery.
- `native/src/mersenne_sidecar.cc` + `mersenne_reference.parquet` (4.4 KB) —
  factorization + primitive factors of M_m=2^m-1 up to m=64 for ord_s(2) lookup.
- `native/src/core.c` — solution-generation heart (modular-filter ideas apply).

### Persistence / catalog plumbing (VERIFIED working — built this session)
- `native/src/catalog/pp_row_delta.{h,cc}` — `RowDelta`: committable
  position-delete `SnapshotUpdate` subclass, in-tree. The sieve's delete-commit.
- `native/src/catalog/pp_iceberg_rest.{h,cc}` — IRC `MakeCatalog`/`PublishTable`.
- `native/src/catalog/pp_hive_sync.{h,cc}` + `scripts/hive_register.sh` — Hive
  engine sync (beeline subprocess) so deletes become visible to MV consumers.
- `native/src/catalog/pp_catalog_main.cc` — `pp-catalog`; `--smoke-test` and
  `--delete-spike` (both green). `native/src/catalog/README.md` explains the split.
- `markdown/data_eng/delete_primitive_spike.md` — GREEN writeup + the read-back
  caveat (spike used beeline; native delete-aware scan not separately exercised).

### Data substrate
- `markdown/data_eng/iceberg_data_setup.md` — schemas, warehouse layout,
  `primes`/`partitions`/`primes_k0`, row-level-deletes section.
- `markdown/data_eng/MV_list.md` — `primes_k0` (3.87 B rows) is the sieve source.
- `markdown/data_eng/hive_mr3_stack.md` — cluster control, HMS-sync mechanism.
- `native/vendor/iceberg-refs/` — cached REST spec + iceberg-cpp #273 tracker
  (+ refresh.sh) for any iceberg conformance question.
- `native/vendor/PATCHES.md` — the 4 local vendored-lib patches the catalog
  path depends on (re-apply if re-vendored).

### Operational / cluster-tuning references (for the TESTING phase, not design)
Not needed to scope/design the rewrite. Pull these in once we actually run real
DAGs against `primes_k0` (3.87 B rows) and start hitting OOM / parallelism /
DAGMaster+worker-sizing questions — which this task may be the first workload to
genuinely exercise.
- `mr3/docs/mr3docs.datamonad.com/docs/guides/performance/outofmemory` — OOM
  diagnosis (HTML, mirrored). Companions in the same dir: `memory-setting`,
  `resources`, `performance-tuning-k8s`, `tuning-bi-query`, `auto-parallelism`,
  `shuffle`, `metastore`, `s3-tuning`, `configure-kernel`.
- `mr3/docs/mr3docs.datamonad.com/docs/guides/troubleshoot` — general troubleshooting.
- `mr3/docs/mr3docs.datamonad.com/docs/guides/configure/{configure-mr3,configure-hivemr3,configure-tez}`
  — what each knob means.
- `mr3/kubernetes/conf/mr3-site.xml` — the LIVE cluster config. The
  DAGMaster/worker sizing knobs we have **never had a workload large enough to
  tune** (defaults, may need raising for the sieve): `mr3.am.resource.memory.mb`
  (2048, DAGAppMaster heap) / `mr3.am.resource.cpu.cores` (1);
  `mr3.k8s.worker.total.max.memory.gb` (8) / `...max.cpu.cores` (8);
  `mr3.am.max.num.concurrent.dags` (16); the `mr3.*.launch.cmd-opts` JVM blocks
  (G1GC, `MetaspaceSize=1024m`); and the auto-scaling block (on; out@80% in@50%).
  Read the tuning guides above *before* turning these — and per
  `[[feedback_per_task_memory_floor]]`, shrink Tez `sort.mb` when shrinking
  task memory, since sort buffer + parquet decode + vector batches dominate.

## What is settled vs open (don't re-litigate the settled)

SETTLED: minimal=irredundant; odd-prime moduli; first-hole=MIN(p) of anti-join;
persistence=position-delete MOR; RowDelta works; HMS-sync works; n_k (NOT
q_k-count) is the "almost always 1" fact.

OPEN (genuine design work for the rewrite): the interactive stepper UX
(notcurses? plain \r bar — see HANDOFF); how triage ranks candidate next-moduli
and the threshold; how a "pass" snapshot is structured; the native delete-aware
read-back path (vs beeline); how `primes_k0` is copied as the delete target.

## API anchors (in-conversation only — captured here so they survive)

iceberg-cpp calls the catalog/sieve path uses, with installed-header anchors:
- RowDelta: subclass `SnapshotUpdate` (`update/snapshot_update.h`); protected
  `WriteDeleteManifests` (`snapshot_update.cc:194`); parent carry-forward via
  `SnapshotCache(snap).Manifests(io)` (`snapshot.h`); `summary_.AddedFile`
  branches on `kPositionDeletes` content (`snapshot.cc:342`); commits via
  generic `PendingUpdate::Commit`→`Transaction::ApplyUpdateSnapshot`
  (`pending_update.cc:34`, `transaction.cc:167,263`); ctx from
  `TransactionContext::Make(table, kUpdate)` (`transaction.h:166`).
- Delete write: `PositionDeleteWriter::Make/WriteDelete/Close/Metadata`
  (`data/position_delete_writer.h`); options need `{path,schema,spec,partition,
  format,io,flush_threshold,properties}`.
- Scan to find data-file path: `tbl->NewScan()->Build()->PlanFiles()` →
  `task->data_file()->file_path` (`table_scan.h:448,74`).
- Read-back applies deletes via `FileScanTaskReader::Open(task)` honoring
  `task->delete_files()` — primeparts wraps this in `SourceTableReader`
  (`source_scan.cc`). NB: `SourceTableReader::OpenMetadata` hit the `file:/` URI
  bug on Hive tables (now patched); confirm it reads delete-bearing tables.
