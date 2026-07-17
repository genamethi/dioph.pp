# 09_merge

deps: all | status: gates done; merge pending user call

Testing overhaul (gtest migration, scrap all current smokes + hand-rolled tests, e2e harness, sanitizer/CI-adjacent cleanup) is a SEPARATE follow-up PR off tui-query after this merges — NOT in scope here. Do not author or extend tests in 09; the interim hand-rolled tests (test_core, test_iceberg_writer, test_scan_planner) ride along as-is and get replaced there.

- [x] `make -C native all test` green (promix, fresh iceberg-cpp f9193ad7 install)
- [x] every phase file's grep gate re-run clean (01,02,03,04,05,06,07; 08 curl-406 live-verified). Debris removed to get there: stale tracked ctags file, test locals renamed off stat-channel names
- [x] 00 holes registry audited: each hole loud + named. One correction: 03's "MOR path serves identity-partition selects" was FALSE — both read paths error "Missing required field with id: N" (no constant-column synthesis in iceberg-cpp); registry entry rewritten
- [x] zero-comment check: 8 legacy-commented files stripped (~136 lines), rebuild green
- [x] verifiable e2e on promix against live warehouse (reads only): catalogd 4x plan routes 406 spec body; pget/LookupPartitions ok; unwindowed hist 17 groups; column-subset reads ok both tables; Extent summary ok with key_max correctly absent (unsorted declared); ScanByK + windowed hist fail LOUD with no-sort-order error; generate --temp --count 1e9: 1e9 prime rows + 1.884B partition rows, 4 files, 8.8GB, 199s, 5.03M primes/s, exit 0
- [ ] merge `planner-prep` → `tui-query` — user call

NOT a merge gate: `make smoke` (all smokes scrapped in the testing PR; generate-smoke already red pre-branch — kMinCount 1e9 vs --count 1000).

BLOCKED until the declaration surface hole (00 registry) lands: live-warehouse e2e of the order-requiring paths against declared primes/partitions. Merging leaves those paths erroring-on-live by design (prep-pass philosophy: named hole on the critical path). tui-query is a feature branch, not trunk — merging with the hole stays in integration territory.

## notes

- Full-table `read` (all columns) on live warehouse errors loudly — selecting identity-partition columns unsupported (registry). Column-subset reads unaffected.
- Stale query-service-smoke: 3 failures, all pre-hole expectations (discards ScanByK error, expects key_max on unsorted table). Dies in testing PR; not a gate.
- nlohmann ABI trap hit on promix (json_abi_v3_11_3 vs v3_12_0) — /usr/local header copy must be refreshed on every iceberg-cpp install.
- generate --temp writes to .pp-staging under the temp warehouse; uuid-tokened filenames, seq dense from 0 per bucket.
