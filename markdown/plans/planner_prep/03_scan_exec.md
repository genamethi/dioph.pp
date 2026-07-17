# 03_scan_exec

deps: 02 | status: done

done: source_scan.{h,cc} rebuilt over ScanPlan. OpenMetadata/OpenIncremental = read metadata → PlanTableScan → Open(plan, io); Open(ScanPlan,...) public for separate planners. Shard = modulo over plan-ordered tasks. Per-task open: deletes or no row-group selection → FileScanTaskReader (MOR), else parquet::arrow GetRecordBatchReader(row_groups, column_indices by PARQUET:field_id). Planner extracts sort-key window (ascending primary key only) from the filter's And-conjuncts into ScanPlan.key_lo/key_hi + non-key `residual`; executor slices each batch to the window by binary search (zero-copy Slice); `reader->residual()` exposes the remainder for consumers. SourceFileInfo/source_files() deleted → planned_records(); verify probe switched. kPColumnFieldId / decode_int64_le / IncludeColumnStats({"p"}) deleted. Verified: make all+test green, lmdb/lua-presets/lua-query smokes pass (generate-smoke fails on pre-existing kMinCount issue).

grep gate: `grep -rn 'kPColumnFieldId\|SourceFileInfo\|decode_int64_le' native` → 0

## notes

- `_pos`/`_file` metadata-column support deleted — no consumers existed.
- HOLE (registered in 00): row-group read path errors NotImplemented when a selected field is not physical in the file (identity-partition columns); MOR path still serves those selects.
- Transient until 04+06: base tables declare no sort order yet → traits unsorted → no task ordering, no window slicing, full filter exposed as residual; consumers' own row checks (removed in 04) still cover correctness, but ScanByK's early-stop rests on manifest order until 04 adds the loud traits check.
- `set_total_records` accessor: factory paths report manifest totals, Open(plan) path reports planned rows.
- Live-warehouse read validation deferred to 09 (catalogd inactive on this machine).
- rework (post-review): hand-rolled row-group open path deleted (local `ReadableFile::Open`, ordinal `GetRecordBatchReader`, manual column-index mapping). Non-MOR tasks with a split open through `ReaderFactoryRegistry::Open(file_format, ReaderOptions{path, length, split, io, projection})` — the vendored reader maps the byte range to row groups, projects by field id, and reads through FileIO; `IcebergReaderBatchReader` adapts `iceberg::Reader` to `arrow::RecordBatchReader`. Missing-physical-field now errors from the vendored projection (required-field check) instead of our bespoke message. `current_data_file_path()` reports the catalog URI unstripped.
