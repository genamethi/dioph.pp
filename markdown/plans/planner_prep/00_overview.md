# planner_prep — DONE, merged to tui-query (0bb87bd, 2026-07-17)

Descriptor prep toward query/file-scan planner: spec types (`native/vendor/iceberg-refs/rest-catalog-open-api.yaml`) drive shapes; callers/catalog declare what middle layers assumed; plan atom = spec FileScanTask + `iceberg::Split`, client-side, liftable behind catalogd. Phase files hold one-paragraph summaries + surviving invariants; full detail in git history (one commit per phase, message = phase file; reworks named in messages). Verification record: `09_merge.md`.

## phases (all done)

01 writer descriptors | 02 traits+planner (Split rework) | 03 executor over vendored reader | 04 query/verify seams | 05 partition-stats resume | 06 declare-at-create | 07 namespace threading | 08 catalogd 406 stubs | 09 gates+e2e+merge

## invariants (carried forward to follow-up branches)

- Spec names for new types. Zero code comments; strip on touch.
- No quiet fallbacks: missing design/data → named NotImplemented error, registered below.
- vendor/ untouched. Deleted capability is deleted. Every phase boundary compiles.
- Live-warehouse mutations are user-only; user makes merge calls.

## holes registry (LIVING — follow-up branches inherit this)

- selecting identity-partition columns (`p_bucket_version`/`p_bucket`) — BOTH read paths error loudly ("Missing required field with id: N" from the vendored projection): the vendored reader implements no constant-column synthesis from the partition tuple (Java PartitionUtil equivalent absent from iceberg-cpp). 09 e2e confirmed: full-table `read` on live warehouse errors; any explicit column subset works — filed-by 03, corrected-by 09 (earlier claim that the MOR path serves these was wrong) — owner: unassigned
- `pp_catalogd.cc` plan routes (planTableScan / fetchPlanningResult / cancelPlanning / fetchScanTasks) — 406 UnsupportedOperationException — filed-by 08 — owner: future server-side lift invoking the 02 planner module
- generate-commit-smoke (`make smoke`) — fails at arg parse: 916563a added `kMinCount = 1e9` but generate_smoke.cc still passes `--count 1000`; broken since before planner-prep — filed-by 07 — owner: testing-overhaul PR (all smokes scrapped there)
- existing-table declaration surface (see 06) — order-requiring paths error on live tables until it lands; testing/e2e of those paths is blocked on it — owner: future design session
- `partition_stats.cc` — non-identity partition transforms, non-integer partition source fields, snapshots carrying delete manifests, and multi-spec tables (partition evolution) → loud NotImplemented — filed-by 05 rework — owner: unassigned
- int/long-only value handling across scan/writer seams — stat-column bounds (`writer.cc` Make), task-ordering bound decode (`SortTasksByLowerBound`), key window (`ScanPlan.key_lo/key_hi` int64; `FoldKeyConjunct`/`SliceToKeyWindow`) — non-int declarations error loudly naming the found type; non-int predicates don't fold or prune (inclusive, correct) — filed-by post-review rework — owner: unassigned
- `table_traits.cc` — declared sort order with non-identity transform → loud NotImplemented (was a quiet unsorted fallback) — filed-by post-review rework — owner: unassigned
- superseded partition-stats files and stats files orphaned by failed transactions accumulate in table metadata dirs; no expiry/cleanup surface — filed-by 05 rework — owner: unassigned
- declaring sort order / properties on EXISTING tables — no surface exists (pp-declare-sort binary built then deleted: packaging undecided; user direction leans config.lua `tables` section + Lua interface) — filed-by 06 — owner: future design session. Until filled, order-requiring paths (ScanByK, windowed hist, verify primes, Extent frontier) error on the live tables. Preconditions the surface must keep: uuid-guarded updateTable, refuse if primary-key bounds missing on any committed file, refuse to replace a different existing order, idempotent no-op.
