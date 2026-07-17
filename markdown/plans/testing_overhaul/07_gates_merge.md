# 07 gates + merge

deps: 06 | status: todo

- [ ] full suite green: promix (`make -j20`) and Debian box (`make -j4`); re-run surviving phase grep gates.
- [ ] update `planner_prep/00_overview.md` living holes registry (`owner: unassigned` each):
  - [ ] full-stack sanitizer coverage deferred (iceberg has `ICEBERG_ENABLE_ASAN`/`UBSAN`; arrow uninstrumented).
  - [ ] smoke coverage removed — lua query/presets, lmdb, query-service now only via e2e/unit.
- [ ] grep gate: no `*smoke*` sources or Makefile smoke targets remain; `grep -rn 'kMinCount' src include`
      still only in `src/generate.cc`.
- [ ] user makes the merge call (`testing-overhaul` → `tui-query`).
