# 04_query_verify

deps: 03 | status: done

done: query_service.cc on the new seams — all column access via column_binder / WidenedColumn (zero unchecked casts); in_window/past_window deleted (executor slices the key window); ScanByK and windowed GroupCount check `reader->traits()` and fail loudly naming pp-declare-sort; GroupCount takes a `GroupKey` descriptor (Column | Derived{inputs, fn}); lua_query_module maps "bits"/"r" to Derived keys at the composition root; Extent rebuilt over PlanTableScan with the traits sort key (`max_p` → `key_max` + `key_name`), PP_EXTENT_DEBUG deleted; progress denominators use planned_records(). verify: Check::Eval → bool+error with binder access; CheckSpec.requires_ascending declares the order dependency (PrimeRankCheck: "p"); AllChecks() registry replaces the if/else dispatch; RunTable enforces the declared-sort precondition via TableReadTraits. query_service_smoke updated to key_max/key_name. Verified: make all+test green, lua smokes pass, gates clean.

grep gates: `grep -rn 'static_pointer_cast<arrow::Int' native/src native/include | grep -v column_binder` → 0; `grep -rn 'PP_EXTENT_DEBUG' native` → 0

## notes

- SourceTableReader gained `traits()` accessor.
- Pre-migration (until 06 runs on the live warehouse): ScanByK and windowed GroupCount error; Extent reports no key stats (TUI frontier blank); LookupPrime/LookupPartitions/unwindowed GroupCount/ReadTable unaffected.
- LookupPrime/LookupPartitions keep their per-row equality check — that is the residual application for the unsorted case, still correct post-migration.
