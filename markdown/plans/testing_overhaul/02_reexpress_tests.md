# 02 re-express hand-rolled tests

deps: 01 | status: done (16 cases, 4 suites green)

Re-express surviving assertions as gtest `TEST()` cases; scrap the old `int main()` binaries.

- [x] `tests/test_core.c` (C, 28 asserts) → C++ TU exercising `core.h` via its C API.
- [x] `tests/test_iceberg_writer.cc` → writer descriptor / stat-column cases.
- [x] `tests/test_scan_planner.cc` → `SelectSplits` pruning (kept / pruned / all-kept), split read-back,
      `SortTasksByLowerBound` ordering + loud missing-lower-bound error.
- [x] `tests/test_partition_stats.cc` → `PartitionStatsFields`, `MergePartitionStats` aggregation,
      write/read round-trip, incremental merge.
- [x] delete the four old test files + their Makefile objects / recipes / `test` wiring.
- [x] boundary: `make test` green with migrated cases.
