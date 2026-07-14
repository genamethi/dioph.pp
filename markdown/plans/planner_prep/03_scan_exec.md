# 03_scan_exec

deps: 02 | status: todo

Rework `source_scan.{h,cc}` over ScanPlan; SourceTableReader stays the reader every consumer uses.

- [ ] `OpenMetadata`/`OpenIncremental` keep signatures → read TableMetadata, `TableReadTraits::FromMetadata`, `PlanTableScan`, open; add `Open(ScanPlan, io, ...)` for callers planning separately
- [ ] sharding = modulo over plan's ordered tasks; delete hand-rolled sort + `decode_int64_le`
- [ ] per-task open: deletes present or `row_groups` empty → vendored `FileScanTaskReader`; row-group selection + no deletes → `parquet::arrow::FileReader::GetRecordBatchReader(row_groups, column_indices)` projected by field id
- [ ] arrow path: selected field absent from physical file (identity-partition col) → named NotImplemented (register in 00 holes)
- [ ] residual: executor slices each batch to the sort-key range conjuncts via binary search (zero-copy Slice); remaining residual exposed as `reader->residual()`; unsorted → no slicing, full residual exposed
- [ ] drop `SourceFileInfo`/`source_files()`; add `planned_records()`; keep `total_records()`, `_file`/`_pos` metadata columns, `current_data_file_path()`
- [ ] `verify.cc:224` — switch source_files() sum to `planned_records()`
- [ ] delete `kPColumnFieldId`, `IncludeColumnStats({"p"})`
- [ ] build green + smokes that don't need warehouse

grep gate: `grep -rn 'kPColumnFieldId\|SourceFileInfo\|decode_int64_le' native` → 0

## notes
