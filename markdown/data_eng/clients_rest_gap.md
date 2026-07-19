# pp REST client — Iceberg REST access & behavior

New 2026-07-09; framing corrected 2026-07-18. The client side of the IRC seam;
companion to `catalogd_rest_gap.md` (server routes), `irc_catalog_design.md`,
`iceberg_data_setup.md`.

There is **one** REST client. It stands between the producers and catalogd;
tools do not each speak IRC in their own way, and "a client" is not a synonym
for "a tool". Perfecting that client and the server it talks to is the goal —
what sits behind either is expected to move.

- **producer:** `primeparts-generate`. Writes; resume is the only nontrivial
  processing it needs today.
- **server:** `primeparts-catalogd`, intended as a full realization of the IRC
  server.
- **consumers:** `pp` (query), `primeparts-tui`, `primeparts-verify` exist, but
  the real consumers of `FileScanTask` and the query engine behind them are
  substantially unwritten. Their interfaces are open ground.

An earlier revision of this file listed `covering-sieve` and `pp-catalog` as
clients. Neither is a binary in this repo — the built set is
`primeparts-generate`, `pp`, `primeparts-catalogd`, `primeparts-tui`,
`primeparts-verify`, and two benches (`native/Makefile`, `all:`).
`covering-sieve` survives only as an aspirational name in a comment
(`common/thread_pool.h:4`).

## Access path

`OpenCatalog` → catalogd (`GET /v1/config`, then `LoadTable`) → `metadata_location`
pointer → client reads manifests + Parquet from storage → Arrow batches → consumer.
catalogd is the path; the in-process LMDB catalog is a **deprecated** fallback (do
not rely on it).

Catalog authority is REST end to end. Since 2026-07-18 catalogd advertises
`scan-planning-mode: server` and serves the four planning routes, so a client
may have the plan produced for it (`rest_scan_plan.{h,cc}`) or plan in-process
from metadata it reads itself. Data reads are always client→storage: the server
plans to the metadata layer only and never opens a parquet file.

## Read seam

`SourceTableReader` (`source_scan.{h,cc}`) is the generic reader every consumer uses:

| Factory | Plan | Use |
|---|---|---|
| `OpenMetadata(meta, cols, filter, shard, n)` | `DataTableScan` over current snapshot | full / windowed reads |
| `OpenIncremental(meta, cols, filter, from_snap, shard, n)` | `IncrementalAppendScan` (`FromSnapshot`/`ToSnapshot`) | rows added since a snapshot (`verify --tail N`) |

Both plan files, sort by `p` lower-bound, shard modulo the p-sorted task order, and
expose an Arrow batch stream. `cols` + `filter` are the IRC `PlanTableScanRequest`
`select` + `filter` shape.

## Gaps

| Behavior | Status |
|---|---|
| File pruning (partition + manifest `p` bounds) | done (server-planned `PlanFiles`) |
| Incremental append scan | done (`OpenIncremental`) |
| MOR position-delete awareness | done (via `FileScanTaskReader`) |
| Row-group / page pruning (zone map on `p`) | done (`scan::RefineSplits`) |
| Server-side planning (`planTableScan`) | done — `catalogd_rest_gap.md` |
| Planning dispatch on the advertised mode | not done — each consumer chooses |

**Row-group pruning** is `scan::SelectSplits`, driven by `scan::RefineSplits`:
`InclusiveMetricsEvaluator` over the residual against each row group's parquet
statistics, keeping the overlapping groups as `iceberg::Split`s. The vendored
high-level `FileScanTaskReader` still does not apply the residual itself
(`parquet_reader.cc` builds `row_group_indices` from split offsets only), which
is why selection happens here and is handed down as a split.

Because both base tables are p-sorted, a narrow window collapses to 1-2 row
groups.

This is the one **data-layer** step in planning, and it is deliberately not on
the server path: the wire has no field for a selected split, and the server does
not open data files. A consumer of a server-produced plan re-derives it from the
data file's `split-offsets` and the complete residual.

## Client / consumer separation

Producer and consumer specifics stay at the leaves; generic client atoms in the
middle; instances formed at the composition root and passed first-class. Mirror of
the write path (`generate.cc`):

- write: `make_*_batch` / `BoundTable` (leaf) → `AlignedBucketWriter`,
  `TableCommitSpec`, `CommitFilesAtomic` (generic) — bound in `run_generation`.
- read: `Check::Eval` (leaf) → `SourceTableReader`, `TableVerifier` (generic) —
  bound in `verify_main`. `Check`/`Window`/`from_snapshot` are first-class values.

`Eval` is a pure leaf: no catalog, no I/O, no planning; it computes over
catalog-sourced buffers only.

## Planning: server-side or in-process

Resolved 2026-07-18. Server-side `planTableScan` exists and is advertised, so the
2026-07-09 stance — that local file selection was transitional until a planner
existed — no longer describes the code. Keeping the plan atom API-shaped did make
the lift mechanical, as predicted.

Both paths are live and take the **same request type**:

| Path | Call | Notes |
|---|---|---|
| server | `PlanScanOnServer` (`catalog/rest_scan_plan.h`) | submit → poll → page every plan-task |
| in-process | `scan::PlanTableScan` + `scan::RefineSplits` | needs metadata and FileIO locally |

`ScanPlanRequest` is the input either way, so a consumer builds one request and
chooses. `FetchScanPlanningMode` reads the server's advertisement, but **nothing
dispatches on it automatically** — that choice is each consumer's, and a single
entry point that reads the mode and routes would couple this client to the
in-process planner. Filed as a hole.

In-process consumers (`source_scan.cc`, `query_service.cc`) read metadata off
disk and never consult the advertisement. That is coherent only while metadata is
local to the consumer; a deployment that moves metadata server-side puts them on
the REST path.

## The scan plan a consumer receives

`ScanPlan` (`scan/scan_plan.h`) is the shape handed to a consumer of file scan
tasks. Two properties are contractual, and both exist so an unwritten consumer
cannot be trapped by them:

- **`residual` is complete.** It carries every conjunct the caller supplied.
  `key_lo`/`key_hi` are *derived* from it, not subtracted out of it, so a
  consumer that ignores the key window is slower but never wrong. Before
  2026-07-18 the window was load-bearing and ignoring it silently returned too
  many rows.
- **the key window may over-include, never under-include.** It is an
  ordered-data acceleration: `SliceToKeyWindow` binary-searches a batch sorted
  on the key. This is why a strict `>` folds to an *inclusive* bound — no
  type-specific successor function is needed, and the fold works for any
  comparable type. `DeriveKeyWindow` (`scan/scan_planner.h`) is public so a
  consumer can compute the window itself.

`SourceTableReader` returns a **superset**: it slices by the window and does not
evaluate the residual. Consumers filter for themselves — `ScanByK` re-tests `k`
per row. A consumer that wants exact rows must evaluate `residual`.
