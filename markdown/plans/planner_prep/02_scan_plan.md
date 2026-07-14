# 02_scan_plan

deps: none | status: todo

New: `native/include/primeparts/scan/{table_traits.h,scan_plan.h,scan_planner.h,column_binder.h}`, `native/src/scan/{table_traits.cc,scan_planner.cc}`, Makefile objs `TABLE_TRAITS_OBJ`/`SCAN_PLANNER_OBJ` linked wherever `SOURCE_SCAN_OBJ` is.

- [ ] `table_traits.h:TableReadTraits` — `SortKey{field_id,name,ascending}` vector from default sort order (identity transforms only; else unsorted), `properties` passthrough; `FromMetadata(const iceberg::TableMetadata&, TableReadTraits*, std::string*)`; unsorted = valid state
- [ ] `scan_plan.h:ScanPlanRequest` — mirrors PlanTableScanRequest (yaml:5089): `snapshot_id`, `select`, `filter`, `case_sensitive`, `start_snapshot_id`/`end_snapshot_id`, `stats_fields`
- [ ] `scan_plan.h:FileScanTask` — `{shared_ptr<iceberg::FileScanTask> inner; vector<int32_t> row_groups; int64_t planned_rows;}`; empty row_groups = all
- [ ] `scan_plan.h:ScanPlan` — `{projected_schema, traits, tasks, planned_rows}`
- [ ] `scan_planner.cc:PlanTableScan(metadata, io, request, ScanPlan*, error)` — point-in-time xor incremental (yaml:721); incremental → `TableScanBuilder<IncrementalAppendScan>` FromSnapshot(excl)/ToSnapshot; else DataTableScan (+UseSnapshot)
- [ ] planner — `IncludeColumnStats(sort keys + stats_fields)`; task order by sort-key lower bound decoded per schema type; missing bound while sorted → loud error (no `: 0` fallback)
- [ ] planner row-group selection (residual tasks only) — parquet footer via `arrow::io::ReadableFile`, columns by `PARQUET:field_id`, per-row-group synthetic `iceberg::DataFile` stats → `iceberg::InclusiveMetricsEvaluator::Evaluate`; missing stats/unsupported expr → keep group
- [ ] `column_binder.h` — `BindInt64/BindInt32(batch, name, error)` → raw ptr or nullptr+error (type-checked)
- [ ] planner unit test — writer-produced p-sorted multi-row-group file: narrow residual keeps only overlapping group; unsorted metadata → no task ordering
- [ ] build green

Not modeled client-side: plan-id / PlanStatus / plan-task paging (wire concerns, catalogd lift later).

grep gate: `grep -rn 'kPColumnFieldId' native/src` → only source_scan.cc (dies in 03)

## notes
