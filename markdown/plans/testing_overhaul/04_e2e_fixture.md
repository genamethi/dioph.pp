# 04 e2e fixture harness

deps: 03 | status: todo

Checked-in fixture builder + loud-by-design assertions against a scratch catalogd.

- [ ] `native/e2e/` builder: spin scratch catalogd (sqlite + lmdb, scratch warehouse dir); write a tiny
      warehouse via `BucketParquetWriter` + `pp_commit` (bypasses `kMinCount`).
- [ ] assert loud-by-design behaviors:
  - [ ] ScanByK / windowed GroupCount on undeclared-order table → error naming missing sort order.
  - [ ] full-table `read` (all columns) → "Missing required field with id: N"; column-subset read ok.
  - [ ] catalogd plan routes → 406 UnsupportedOperationException, all four.
  - [ ] non-int stat-column / sort-key declaration → loud NotImplemented naming the found type.
- [ ] Makefile `.PHONY: e2e` target: build + run against fresh scratch catalogd each run; tear down after.
- [ ] boundary: `make e2e` green on promix.
