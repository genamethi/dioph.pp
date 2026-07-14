# 04_query_verify

deps: 03 | status: todo

query_service.cc:
- [ ] all column access via `column_binder.h`; zero `static_pointer_cast<arrow::Int*>`
- [ ] delete `in_window`/`past_window` residual re-implementations (executor slices sort key); `ScanByK` keeps k==K leaf check + limit stop
- [ ] p-order-promising methods check `traits.sort_keys`, loud error if undeclared (names the 06 migration)
- [ ] `GroupCount` — `"bits"`/`"r"` enum → `GroupKey{column | inputs+fn}` descriptor param; `lua_query_module.cc`/TUI map product vocabulary to GroupKeys at composition root
- [ ] `Extent` — inline NewScan/manifest walk + `"p"` hardcode → `PlanTableScan` with traits' primary sort key; `max_p` → `key_max`+`key_name`; delete `PP_EXTENT_DEBUG` fprintfs
- [ ] TUI status labels use `key_name`

verify/:
- [ ] `verify.cc:I64/I32` → column_binder
- [ ] `verify.h:AllChecks()` registry; `verify_main.cc` selects by `spec().table` (kill if/else dispatch)
- [ ] `RunTable` precondition: table declares ascending sort on check's key (traits) else fail naming migration
- [ ] build green

grep gate: `grep -rn 'static_pointer_cast<arrow::Int' native/src | grep -v column_binder` → 0; `grep -n 'PP_EXTENT_DEBUG' -r native` → 0

## notes
