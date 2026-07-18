# 07 gates + merge

deps: 06 | status: gates green on promix; Debian + merge pending user

- [x] full suite green on promix (`make -j24 all test e2e`: 16 unit + 6 e2e); grep gates re-run clean.
- [ ] Debian box (`make -j4`) — separate machine, user-verify (cannot run from here).
- [x] update `planner_prep/00_overview.md` living holes registry (`owner: unassigned` each):
  - [x] sanitizer coverage our-code-only; vendored arrow/iceberg uninstrumented.
  - [x] smoke coverage removed — lua query/presets + pp_lmdb_store now only indirectly covered.
- [x] grep gate: no `*smoke*` sources or Makefile smoke targets remain; `grep -rl 'kMinCount' src include`
      still only `src/generate.cc`.
- [ ] user makes the merge call (`testing-overhaul` → `tui-query`).
