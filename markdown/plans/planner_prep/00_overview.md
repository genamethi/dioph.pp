# planner_prep

Descriptor prep toward query/file-scan planner. Spec types (`native/vendor/iceberg-refs/rest-catalog-open-api.yaml`: planTableScan yaml:707, PlanTableScanRequest yaml:5089, FileScanTask yaml:5176) drive new shapes; plan atom = FileScanTask{data file, deletes, residual, row-group ranges}, built client-side, lifted behind catalogd later. Callers/catalog declare (sort order, stat columns, bucket fields) what middle layers currently assume. Full design: `~/.claude/plans/hidden-dancing-pearl.md`.

Branch `planner-prep` (off `tui-query`). One commit per phase, message = phase file. Merge only after 09.

## phases

| file | scope | deps | status |
|---|---|---|---|
| 01_writer | stat-column descriptors, BatchStats removal, partition-fallback deletion | — | todo |
| 02_scan_plan | TableReadTraits, spec-shaped plan types, planner, column_binder | — | todo |
| 03_scan_exec | SourceTableReader over ScanPlan, row-group read path, key slicing | 02 | todo |
| 04_query_verify | QueryService + verify on new seams, GroupKey, check registry | 03 | todo |
| 05_resume | LoadAlignedResume via snapshot manifests | 01 | todo |
| 06_declare | sort_order/properties in commit path, pp-declare-sort tool | — | todo |
| 07_namespace | namespace-as-argument threading | — | todo |
| 08_catalogd | 406 stubs for plan routes | — | todo |
| 09_merge | grep gates, tests, smokes, hole audit, merge to tui-query | all | todo |

## invariants

- Spec names for new types. Zero code comments; delete comments falsified by changes.
- No quiet fallbacks: missing design/data → named NotImplemented error, registered below.
- vendor/ untouched. Deleted capability is deleted.
- Every phase boundary compiles.

## holes registry

(none yet)

## session protocol

Read this file + current phase file only. Check boxes as done; deviations → phase `## notes`; on phase close prune detail to one `done` line + notes. New holes → registry line `symbol — filed-by phase — owner phase`.
