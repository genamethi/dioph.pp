# 07 gates + merge

deps: 06 | status: gates green on promix; Debian + merge pending user

- [x] full suite green on promix (`make -j24 all test e2e`: 16 unit + 6 e2e); grep gates re-run clean.
- [x] Debian laptop (`make -j4`) — green after two fixes 2026-07-18. Arrow rebuilt with
      `ARROW_TESTING=ON` and installed (user). Then a real Makefile bug: `$(CPPFLAGS)` (carrying
      `-I$(PREFIX)/include`) preceded `$(GTEST_CPPFLAGS)` in the test and e2e compile rules, so with a
      system gtest also present at `$(PREFIX)/include/gtest` the headers resolved there while the link
      pulled `libarrow_gtest` — undefined `MakeAndRegisterTestInfo`. `$(GTEST_CPPFLAGS)` now comes
      first (Makefile 284, 294). The bundled-gtest decision only ever worked on a box with no system
      gtest installed.
- [x] update `planner_prep/00_overview.md` living holes registry (`owner: unassigned` each):
  - [x] sanitizer coverage our-code-only; vendored arrow/iceberg uninstrumented.
  - [x] smoke coverage removed — lua query/presets + pp_lmdb_store now only indirectly covered.
- [x] grep gate: no `*smoke*` sources or Makefile smoke targets remain; `grep -rl 'kMinCount' src include`
      still only `src/generate.cc`.
- [ ] user makes the merge call (`testing-overhaul` → `tui-query`).
