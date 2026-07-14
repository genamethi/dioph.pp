# 02_scan_plan

deps: none | status: done

done: `include/primeparts/scan/{table_traits,scan_plan,scan_planner,column_binder}.h` + `src/scan/{table_traits,scan_planner}.cc`. TableReadTraits::FromMetadata (default sort order, identity-only, else unsorted; properties passthrough). ScanPlanRequest mirrors PlanTableScanRequest; scan::FileScanTask = iceberg task + kept row_groups + planned_rows (empty row_groups = no selection ran; fully-pruned tasks are dropped from the plan). PlanTableScan: point-in-time xor incremental validated (incremental requires both ids), IncludeColumnStats(sort keys + stats_fields), task order by primary sort-key lower bound (missing bound = loud error), row-group selection per residual-carrying task via parquet footer stats → synthetic per-row-group DataFile → InclusiveMetricsEvaluator (missing/unsupported stats keep the group). BindInt64/BindInt32 loud column access. test_scan_planner (in `make test`): 10-row-group file, narrow residual keeps exactly group 2; impossible residual prunes all; task ordering + missing-bound error.

grep gate: `grep -rn 'kPColumnFieldId' native/src` → only source_scan.cc (dies in 03)

## notes

- PlanTableScan not unit-covered (needs TableMetadata fixture); exercised via source_scan in 03.
- Makefile: SCAN_OBJS (`table_traits.o scan_planner.o`) defined; consumers link in 03.
