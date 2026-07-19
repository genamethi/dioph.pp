# 01 — layer boundary

dep: none

Split planning at the metadata/data line so the server path can stop at the
manifests. No server changes in this phase.

- [x] `scan_planner.h:PlanTableScan` plans to the metadata layer only — manifest
      scan, metrics pruning, bound ordering, `record_count` row estimates. It no
      longer calls `SelectSplits`.
- [x] `scan_planner.h:RefineSplits` is the data-layer step: takes a planned
      `ScanPlan`, opens each non-trivial task's parquet, replaces it with its
      selected splits. In-process callers run it after `PlanTableScan`.
- [x] `source_scan.cc:321` calls both.
- [x] `query_service.cc:552` calls both.
- [x] `min_rows_requested` accounting is unchanged — `guaranteed_rows` only ever
      counted trivial-residual tasks, which never reached `SelectSplits`.
- [x] `scan_planner_test.cc` covers the boundary: a metadata-only plan opens no
      data file, and refining it reproduces the previous combined result.

grep-gate: `SelectSplits` appears in `scan_planner.cc` and the tests only —
never on a path reachable from `pp_catalogd.cc`.

## notes

`planned_rows` after metadata-only planning is `DataFile::record_count`, an
upper bound. `RefineSplits` narrows it to the kept row groups' rows. Both are
honest; the server transmits neither, so the difference is in-process only.
