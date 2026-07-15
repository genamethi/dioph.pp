# 09_merge

deps: all | status: todo

Testing overhaul (gtest migration, scrap all current smokes + hand-rolled tests, e2e harness, sanitizer/CI-adjacent cleanup) is a SEPARATE follow-up PR off tui-query after this merges — NOT in scope here. Do not author or extend tests in 09; the interim hand-rolled tests (test_core, test_iceberg_writer, test_scan_planner) ride along as-is and get replaced there.

- [ ] `make -C native all test` green
- [ ] every phase file's grep gate re-run clean (01,02,03,04,06,07 gates + 08 curl-406)
- [ ] 00 holes registry audited: each hole still deliberate, named, loud (NotImplemented/error, no quiet fallback crept in)
- [ ] zero-comment check across all files touched this branch (`git diff tui-query..HEAD --name-only` → grep for `//` / `/*` in changed .cc/.h)
- [ ] verifiable e2e (no declaration surface needed): scratch catalogd + `pp` materialize/read, generate `--temp`, LookupPrime/LookupPartitions, unwindowed ReadTable/hist, Extent summary. Confirm order-requiring paths (ScanByK, windowed hist, verify primes) fail LOUDLY with the no-sort-order error (not misbehave).
- [ ] merge `planner-prep` → `tui-query`

NOT a merge gate: `make smoke` (all smokes scrapped in the testing PR; generate-smoke already red pre-branch — kMinCount 1e9 vs --count 1000).

BLOCKED until the declaration surface hole (00 registry) lands: live-warehouse e2e of the order-requiring paths against declared primes/partitions. Merging leaves those paths erroring-on-live by design (prep-pass philosophy: named hole on the critical path). tui-query is a feature branch, not trunk — merging with the hole stays in integration territory.

## notes
