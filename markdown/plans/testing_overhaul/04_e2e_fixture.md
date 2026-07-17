# 04 e2e fixture harness

deps: 03 | status: done (6 cases green)

Harness: `native/e2e/e2e_test.cc`, gtest binary `primeparts-e2e`. One fixture builds a tiny
partitioned primes warehouse offline (`MakeLocalCatalog` + `BucketParquetWriter` into
`StagingDataDir` + `CommitFiles`, no declared sort order), forks `primeparts-catalogd` on :18181,
points `PRIMEPARTS_REST_URI` at it (QueryService is REST-only — no offline path), tears down on
suite end. httplib client hits the 4 plan routes.

Checked-in fixture builder + loud-by-design assertions against a scratch catalogd.

- [x] `native/e2e/` builder: spin scratch catalogd (sqlite + lmdb, scratch warehouse dir); write a tiny
      warehouse via `BucketParquetWriter` + `pp_commit` (bypasses `kMinCount`).
- [x] assert loud-by-design behaviors:
  - [x] ScanByK / windowed GroupCount on undeclared-order table → error naming missing sort order.
  - [x] full-table `read` (all columns) → "Missing required field with id: N"; column-subset read ok.
  - [x] catalogd plan routes → 406 UnsupportedOperationException, all four.
  - [x] non-int stat-column / sort-key declaration → loud NotImplemented naming the found type.
- [x] Makefile `.PHONY: e2e` target: build + run against fresh scratch catalogd each run; tear down after.
- [x] boundary: `make e2e` green on promix.
