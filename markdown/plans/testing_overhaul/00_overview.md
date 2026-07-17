# testing_overhaul — gtest suite + e2e harness + sanitizer + CI

Branch `testing-overhaul` off `tui-query`. Scrap-and-replace the hand-rolled tests and smokes with a
gtest unit suite, a repeatable e2e fixture harness asserting the loud-by-design behaviors, a local
sanitizer build, and a self-hosted CI job. Full brief: `00_prompt.md`. Approved design decisions and
rationale recorded per phase; one commit per phase, message = phase file name, every boundary compiles.

## decisions (settled)

- gtest: reuse arrow's bundled gtest in the prefix (`-I$(PREFIX)/include/arrow-gtest`,
  `-larrow_gtest_main -larrow_gmock`). No submodule, no separate install, no floor, no pkgconf.
  Version tracks arrow. See memory `gtest-via-arrow-bundle`.
- e2e: checked-in fixture builder driving `BucketParquetWriter` + `pp_commit` below `kMinCount`
  (which lives only in `src/generate.cc`), into a scratch catalogd. No 1e9 generate.
- sanitizers: `./configure --sanitize` → ASAN/UBSAN over our code + suite only (partial coverage over
  uninstrumented vendored libs). Local target, not CI.
- CI: self-hosted promix GitHub Actions; jobs `make test` + `make e2e`. No sanitizer job.

## doctrine (from planner_prep, unchanged)

- Zero code comments; strip legacy comments on touch. No quiet fallbacks; loud named errors.
- New gaps → holes-registry line in `planner_prep/00_overview.md` (the living registry), `owner: unassigned`.
- vendor/ untouched. Deleted capability is deleted. User makes the merge call.
- Live-warehouse mutations are user-only; e2e writes go to scratch warehouses only.

## phases

- [x] 00 branch + tracker scaffold
- [x] 01 gtest harness wiring
- [x] 02 re-express hand-rolled tests
- [ ] 03 scrap smokes
- [ ] 04 e2e fixture harness
- [ ] 05 sanitizer build
- [ ] 06 CI
- [ ] 07 gates + merge
