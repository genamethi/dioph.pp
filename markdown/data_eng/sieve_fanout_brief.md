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
- `native/src/coverings/covering_sieve_main.cc` — the covering-sieve FILTER /
  interactive stepper (BUILT & VALIDATED at scale; HANDOFF §8). Old
  "covered_/uncovered_passN string-label partition" behavior is DISCARDED.
- `native/src/coverings/sieve_triage_main.cc` — bitmask-distribution triage to
  pick the next sieve prime; **folding into** the stepper, then removed.
- `native/src/coverings/primitive_factors.cc` +
  `include/primeparts/coverings/primitive_factors.h` — `GetCoverageMask` /
  `MersenneHelper`; `ord_s(2)` machinery.
- `native/src/mersenne_sidecar.cc` + `mersenne_reference.parquet` —
  factorization + primitive factors of M_m=2^m-1 up to m=64 for ord_s(2) lookup.
- `native/src/core.c` — solution-generation heart (modular-filter ideas apply).

### Persistence / catalog plumbing (VERIFIED working)
- `native/src/catalog/pp_row_delta.{h,cc}` — `RowDelta`: committable
  position-delete `SnapshotUpdate` subclass, in-tree. The sieve's delete-commit.
- `native/src/catalog/pp_iceberg_rest.{h,cc}` — catalog construction + commit
  helpers: `MakeLocalCatalog` (SqlCatalog over LMDB), `MakeCatalog` (RestCatalog
  client → `pp-catalogd`), `CommitFiles`, `PublishTable`.
- `native/src/catalog/pp_catalog_main.cc` — `pp-catalog` (`--register`,
  `--clone-sieve`). `native/src/catalog/README.md` explains the split.
- `native/src/catalog/pp_catalogd.{h,cc}` — the IRC HTTP server (the sieve's
  re-target commit target, replacing the dead HMS servlet).
- `markdown/data_eng/delete_primitive_spike.md` — GREEN writeup (RowDelta +
  native delete-aware MOR read-back).

### Data substrate
- `markdown/data_eng/iceberg_data_setup.md` — schemas, warehouse layout,
  `primes`/`partitions`/`primes_k0`, row-level-deletes section.
- `primeparts.primes_k0` (3.87 B rows) is a base table — the sieve source.
- `native/vendor/iceberg-refs/` — cached REST spec + iceberg-cpp tracker
  (+ refresh.sh) for any iceberg conformance question.
- `native/vendor/PATCHES.md` — the local vendored-lib patches the catalog path
  depends on (re-apply if re-vendored).

## What is settled vs open (don't re-litigate the settled)

SETTLED: minimal=irredundant; odd-prime moduli; first-hole=MIN(p) of anti-join;
persistence=position-delete MOR; RowDelta + native delete-aware MOR read-back
work; n_k (NOT q_k-count) is the "almost always 1" fact.

OPEN (genuine work for the rewrite): re-target the sieve `--clone-sieve` /
RowDelta commits onto the local catalog / `pp-catalogd` (the way `generate`
already does, via `CommitFiles`); fold `sieve_triage` into the stepper; the
interactive stepper UX; how triage ranks candidate next-moduli and the
threshold; how a "pass" snapshot is structured.

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
  (`source_scan.cc`). NB: the `file:/` single-slash URI fix in `arrow_io.cc`
  (vendored patch) is what lets it scan tables whose manifests carry `file:`
  paths; confirm it reads delete-bearing tables.
