# planner_prep

Descriptor prep toward query/file-scan planner. Spec types (`native/vendor/iceberg-refs/rest-catalog-open-api.yaml`: planTableScan yaml:707, PlanTableScanRequest yaml:5089, FileScanTask yaml:5176) drive new shapes; plan atom = FileScanTask{data file, deletes, residual, row-group ranges}, built client-side, lifted behind catalogd later. Callers/catalog declare (sort order, stat columns, bucket fields) what middle layers currently assume. Full design: `~/.claude/plans/hidden-dancing-pearl.md`.

Branch `planner-prep` (off `tui-query`). One commit per phase, message = phase file. Merge only after 09.

## phases

| file | scope | deps | status |
|---|---|---|---|
| 01_writer | stat-column descriptors, BatchStats removal, partition-fallback deletion | — | done |
| 02_scan_plan | TableReadTraits, spec-shaped plan types, planner, column_binder | — | done |
| 03_scan_exec | SourceTableReader over ScanPlan, row-group read path, key slicing | 02 | done |
| 04_query_verify | QueryService + verify on new seams, GroupKey, check registry | 03 | done |
| 05_resume | LoadAlignedResume via snapshot manifests | 01 | done |
| 06_declare | sort_order/properties in commit path, pp-declare-sort tool | — | done |
| 07_namespace | namespace-as-argument threading | — | done |
| 08_catalogd | 406 stubs for plan routes | — | done |
| 09_merge | gates, hole audit, verifiable e2e, merge to tui-query (testing overhaul is a separate follow-up PR) | all | todo |

## invariants

- Spec names for new types. Zero code comments; delete comments falsified by changes.
- No quiet fallbacks: missing design/data → named NotImplemented error, registered below.
- vendor/ untouched. Deleted capability is deleted.
- Every phase boundary compiles.

## holes registry

- `SourceTableReader::Impl::OpenRowGroupTask` — selected field not physical in file (identity-partition column) → NotImplemented error — filed-by 03 — by design (MOR path serves those selects); revisit if a consumer ever selects bucket columns with a residual
- `pp_catalogd.cc` plan routes (planTableScan / fetchPlanningResult / cancelPlanning / fetchScanTasks) — 406 UnsupportedOperationException — filed-by 08 — owner: future server-side lift invoking the 02 planner module
- generate-commit-smoke (`make smoke`) — fails at arg parse: 916563a added `kMinCount = 1e9` but generate_smoke.cc still passes `--count 1000`; broken since before planner-prep — filed-by 07 — owner: superseded by the testing-overhaul PR (all smokes scrapped there)
- existing-table declaration surface (see 06) — order-requiring paths error on live tables until it lands; testing/e2e of those paths is blocked on it — owner: future design session
- declaring sort order / properties on EXISTING tables — no surface exists (pp-declare-sort binary built then deleted: packaging undecided; user direction leans config.lua `tables` section + Lua interface) — filed-by 06 — owner: future design session. Until filled, order-requiring paths (ScanByK, windowed hist, verify primes, Extent frontier) error on the live tables. Preconditions the surface must keep: uuid-guarded updateTable, refuse if primary-key bounds missing on any committed file, refuse to replace a different existing order, idempotent no-op.

## session protocol

Read this file + current phase file only. Check boxes as done; deviations → phase `## notes`; on phase close prune detail to one `done` line + notes. New holes → registry line `symbol — filed-by phase — owner phase`.
