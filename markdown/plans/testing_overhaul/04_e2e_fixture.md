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
- [x] assert spec-correct refusals:
  - [x] ScanByK / windowed GroupCount on undeclared-order table → error naming missing sort order.
  - [x] catalogd plan routes → 406, all four. NOT spec-correct: the yaml lists 406 only on
        `planTableScan`; the other three want 404 for an unknown plan-id/plan-task. Registry hole.
- [x] column-subset read ok.
- superseded: full-table `read` and non-int stat-column cases were written as assertions that the
  error is correct. They are holes, not design — reversed 2026-07-18 to assert desired behavior and
  are red until the holes close. Scope note: e2e covers entry points, data integrity, and REST
  status behavior; capability gaps belong in unit tests.
  - [x] partition stats present after commit: fixture seeds via `CommitFiles` then appends via the
        real `CommitFilesAtomic` (store path, writes `SetPartitionStatistics`); `LoadPartitionStats`
        asserts the (1,2) row is registered with 2 files / 6 records — guards stats going missing.
- [x] Makefile `.PHONY: e2e` target: build + run against fresh scratch catalogd each run; tear down after.
- [x] boundary: `make e2e` green on promix.
