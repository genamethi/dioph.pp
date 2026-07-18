# testing_overhaul — kickoff prompt

You are starting the testing-overhaul PR for primeparts. Branch `testing-overhaul` off `tui-query` (current head 0bb87bd = planner-prep merge). This file is your brief; do not start coding before the design questions at the bottom are settled with the user.

## read first

- `markdown/plans/planner_prep/00_overview.md` — invariants (they carry forward) and the LIVING holes registry.
- `markdown/plans/planner_prep/09_merge.md` — verification record: what e2e was proven on the live warehouse and which failures are BY DESIGN.
- `PRODUCT.md` before any UI-adjacent work.

## mission

Replace all hand-rolled tests and smokes with a gtest-based suite plus a repeatable e2e harness; add sanitizer builds (ASAN/UBSAN). Scrap-and-replace, not port: the user explicitly ruled smokes die rather than get migrated.

Inventory to scrap:
- hand-rolled tests (assertions worth keeping — re-express in gtest): `tests/test_core.c`, `tests/test_iceberg_writer.cc`, `tests/test_scan_planner.cc`, `tests/test_partition_stats.cc`
- smokes (delete, do not port): generate_smoke (already red: kMinCount 1e9 vs --count 1000), query_service_smoke (3 stale-expectation failures documented in 09 notes), lua_query_smoke, lua_presets_smoke, lmdb smoke targets
- Makefile `test` and `smoke` targets get rebuilt around the new suite

## doctrine (unchanged from planner_prep)

- Tracker: this directory, checkbox phase files, one commit per phase, message = phase file name. Every phase boundary compiles.
- Zero code comments; strip legacy comments from any file touched.
- No quiet fallbacks; loud named errors; new gaps → holes registry line in planner_prep/00 (it stays the living registry) with `owner: unassigned` unless the user assigns. Hole entries document what's missing only — never prescribe resolutions.
- vendor/ untouched. Deleted capability is deleted. User makes the merge call.
- Live warehouse: reads are fine; MUTATIONS ARE USER-ONLY. e2e writes go through `generate --temp` (files-only, no commit) or scratch warehouses.

## environment facts

- promix-lan: 24 threads, `make -j20`. Old Debian box: 4 threads, `-j4`.
- nlohmann ABI trap: every iceberg-cpp install must refresh `/usr/local/include/nlohmann` from the build's `_deps/nlohmann_json-src` — symptom is undefined `*FromJson` with mismatched `json_abi_v3_X_Y` mangling.
- Vendored-deps policy: check vendor/ gitlinks and version floors before assuming any system dep; never map deps 1:1 to distro packages. gtest is NOT currently installed or vendored — sourcing it is a design question below.
- `generate --temp --count 1000000000` measured on promix: 199s, 8.8GB, exit 0 — viable as an e2e fixture generator but heavyweight; kMinCount forbids smaller runs (do NOT silently lower it; parameterizing minimum for tests is a user decision).

## errors: two kinds, do not conflate

SUPERSEDED as originally written — this section listed four loud errors as "behaviors e2e must ASSERT, do not fix", which pinned unimplemented capability as intended behavior. Corrected 2026-07-18; see the registry in `planner_prep/00_overview.md`.

- **Spec-correct refusals** — assert these. Plan routes serve 406 because `scan-planning-mode: client` is advertised; a missing sort order must error rather than silently return unsorted rows.
- **Unimplemented capability** — do NOT assert the failure. Identity-partition column selection and non-int stat columns are holes, not design. A test that asserts they fail turns closing the hole into a red test.

Before asserting an error, decide which kind it is. If it is the second, the test belongs to the desired behavior and should be red until the hole closes.

## design questions to settle with the user BEFORE building

1. gtest sourcing: vendor as submodule (matches vendored-deps policy) vs /usr/local install. Version floor?
2. e2e fixture strategy: scratch catalogd + generate --temp each run (199s), vs a small checked-in fixture warehouse builder, vs snapshotting a mini warehouse. kMinCount interacts here.
3. Sanitizer builds: separate Makefile config (configure flag?) — full-suite ASAN/UBSAN or targeted binaries? iceberg/arrow static libs are NOT sanitizer-built; interceptor coverage will be partial — acceptable?
4. CI story: is there any runner, or are these local `make` targets only for now?
