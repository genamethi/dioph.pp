# pp clients — Iceberg REST access & behavior

New 2026-07-09. The client side of the IRC seam; companion to
`catalogd_rest_gap.md` (server routes), `irc_catalog_design.md`,
`iceberg_data_setup.md`.

A client is any native tool/library that speaks IRC: `generate`, `covering-sieve`,
`verify`, `query`/`tui` (`QueryService`), `pp-catalog`. All reach data through the
catalog seam, never the filesystem directly.

## Access path

`OpenCatalog` → catalogd (`GET /v1/config`, then `LoadTable`) → `metadata_location`
pointer → client reads manifests + Parquet from storage → Arrow batches → consumer.
catalogd is the path; the in-process LMDB catalog is a **deprecated** fallback (do
not rely on it).

Catalog authority is REST end to end. Manifest read + file selection (planning) and
data reads are client→storage, per the standard IRC client model when
`scan-planning-mode: client`.

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
| Row-group / page pruning (zone map on `p`) | **not done** — see below |
| Server-side planning (`planTableScan`) | not done — `catalogd_rest_gap.md` |

**Row-group pruning.** A `p`-window prunes only to overlapping files today; the whole
file is decoded. The vendored high-level `FileScanTaskReader` receives the residual
but does not apply it (`parquet_reader.cc` builds `row_group_indices` from split
offsets only; zone-map is its own TODO). This is **not** iceberg-cpp-blocked: the fix
is client-side, composing lower-level primitives —

- `parquet-cpp`: footer `RowGroup(i)->ColumnChunk(p)->statistics()` min/max vs the
  residual → `parquet::arrow::FileReader::GetRecordBatchReader({kept}, {cols})`; or
- `arrow::dataset`: hand it the file + a `p` filter — it prunes row groups by
  statistics itself.

Since both base tables are p-sorted, a narrow window collapses to 1–2 row groups.
Build it as the API-shaped scan-plan atom (residual → `FileScanTask{ file,
row-group ranges, residual }`), not a consumer-local read: one implementation then
serves client-side planning now and server-side `planTableScan` later. Delete
awareness and field-id mapping are sibling capabilities of the same scan-execution
abstraction, composed — not a reason to fork a parallel reader.

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

## Open question — is client-side planning in the spirit of the API?

Stance (2026-07-09): no. "Local" file selection is not planning in the API sense
while there is no planner. Real planning is server-side `planTableScan`, which
catalogd fulfills by **invoking** a planner module and returning the plan as if the
catalog produced it (`catalogd_rest_gap.md`). Client-side planning is transitional;
keeping the plan atom API-shaped makes the lift mechanical.
