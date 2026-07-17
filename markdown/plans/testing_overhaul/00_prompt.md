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

## behaviors e2e must ASSERT (loud-by-design, do not "fix")

- ScanByK / windowed GroupCount on undeclared-order tables: error naming the missing sort order.
- Full-table `read` (all columns): "Missing required field with id: N" (identity-partition columns; subset selects work).
- catalogd plan routes: 406 UnsupportedOperationException, all four.
- Non-int stat-column/sort-key declarations: loud NotImplemented naming the found type.

## design questions to settle with the user BEFORE building

1. gtest sourcing: vendor as submodule (matches vendored-deps policy) vs /usr/local install. Version floor?
2. e2e fixture strategy: scratch catalogd + generate --temp each run (199s), vs a small checked-in fixture warehouse builder, vs snapshotting a mini warehouse. kMinCount interacts here.
3. Sanitizer builds: separate Makefile config (configure flag?) — full-suite ASAN/UBSAN or targeted binaries? iceberg/arrow static libs are NOT sanitizer-built; interceptor coverage will be partial — acceptable?
4. CI story: is there any runner, or are these local `make` targets only for now?
