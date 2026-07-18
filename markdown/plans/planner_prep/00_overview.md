# planner_prep — DONE, merged to tui-query (0bb87bd, 2026-07-17)

Descriptor prep toward query/file-scan planner: spec types (`docs/vendor/iceberg/open-api/rest-catalog-open-api.yaml`) drive shapes; callers/catalog declare what middle layers assumed; plan atom = spec FileScanTask + `iceberg::Split`, client-side, liftable behind catalogd. Phase files hold one-paragraph summaries + surviving invariants; full detail in git history (one commit per phase, message = phase file; reworks named in messages). Verification record: `09_merge.md`.

## phases (all done)

01 writer descriptors | 02 traits+planner (Split rework) | 03 executor over vendored reader | 04 query/verify seams | 05 partition-stats resume | 06 declare-at-create | 07 namespace threading | 08 catalogd 406 stubs | 09 gates+e2e+merge

## invariants (carried forward to follow-up branches)

- Spec names for new types. Zero code comments; strip on touch.
- No quiet fallbacks: missing design/data → named NotImplemented error, registered below.
- vendor/ untouched — meaning no patches to vendored sources. Build flags and CMake options exposed by the vendored trees are NOT patches and are in scope.
- Deleted capability is deleted. Every phase boundary compiles.
- Live-warehouse mutations are user-only; user makes merge calls.

## registry criterion

A hole is a gap between our surface and `rest-catalog-open-api.yaml`, or an unimplemented capability behind a loud error. Whether the current applications exercise it is NOT a criterion — surface coverage is the goal, and narrowing a type or guard to fit today's callers is itself a hole. Entries below document what is missing only; resolutions live in the separate section at the end.

## holes registry (LIVING — follow-up branches inherit this)

**spec surface gaps**

- `ScanPlanRequest` (`scan/scan_plan.h`) omits two `PlanTableScanRequest` fields: `min-rows-requested` and `use-snapshot-schema`. `ScanByK`'s LIMIT early-stop is the `min-rows-requested` capability implemented off-surface, so it is unreachable through the spec shape; `use-snapshot-schema` (time-travel vs branch schema resolution) has no representation at all — filed-by spec-diff audit 2026-07-18 — owner: unassigned
- server-side scan planning is unimplemented: only one of the spec's two planning modes exists. `scan-planning-mode: client` is advertised, so client-side planning is conformant — filed-by 08 — owner: future server-side lift invoking the 02 planner module
- three of the four plan routes answer with a status the spec does not list for them. `pp_catalogd.cc` binds one `planning_unsupported` handler to all four, returning 406 UnsupportedOperationException. The yaml lists 406 only on `planTableScan` (POST plan); `fetchPlanningResult` (GET plan/{plan-id}), `cancelPlanning` (DELETE plan/{plan-id}), and `fetchScanTasks` (POST tasks) list 200/204, 400, 401, 403, 404, 419, 503, 5XX and no 406. With no server-side planning, no plan-id or plan-task ever exists, which is the 404 case (NoSuchPlanIdException / NoSuchPlanTaskException) — filed-by spec-diff audit 2026-07-18 — owner: unassigned
- the `/v1/config` response omits `endpoints`. `CatalogConfig` carries an optional `endpoints` array as the mechanism for advertising route support; absence makes clients assume the spec's default set, which excludes every route outside it — filed-by spec-diff audit 2026-07-18 — owner: unassigned
- `ScanPlan.key_lo`/`key_hi` are `optional<int64_t>` on a structure whose spec counterpart carries a general `Expression` residual. The key window is derived from the filter but typed more narrowly than it, so no predicate over a non-int column can fold or prune — filed-by spec-diff audit 2026-07-18 — owner: unassigned

**type narrowing (one cause, five sites)**

- values are erased to `int64_t` at every seam instead of carried as `iceberg::Literal`. `DecodeIntegerBound` (`scan_planner.cc`) is the funnel: it deserializes a `Literal`, then discards the type through an `int64_t*`. Exits: `writer.cc` stat-column bounds (Make), `partition_stats.cc` tuple values and partition source types, `SortTasksByLowerBound` task-ordering bound decode, `ScanPlan.key_lo/key_hi`. Non-int declarations error loudly naming the found type; non-int predicates do not fold or prune (inclusive, correct) — filed-by post-review rework, consolidated 2026-07-18 — owner: unassigned

**transform coverage**

- `partition_stats.cc` resolves identity partition transforms only. The guard rejects on transform identity alone, but `bucket`/`year`/`month`/`day`/`hour` all produce int32 partition values that the existing int/long stats tuples already represent — the guard is broader than the limitation beneath it, which is the source-type check (see type narrowing) — filed-by 05 rework, corrected 2026-07-18 — owner: unassigned
- `table_traits.cc` rejects declared sort orders with non-identity transforms. The actual requirement is monotonicity, not identity: `truncate` and the time transforms preserve order, so manifest bounds map through and task ordering is derivable; `bucket` is a hash and destroys order, so ordering on a bucket sort key is not derivable from source bounds under any implementation — filed-by post-review rework, split 2026-07-18 — owner: unassigned
- `partition_stats.cc` also errors loudly on snapshots carrying delete manifests and on multi-spec tables (partition evolution) — filed-by 05 rework — owner: unassigned

**unimplemented read-path capability**

- selecting identity-partition columns (`p_bucket_version`/`p_bucket`) errors on BOTH read paths ("Missing required field with id: N"). Partition values are present at `DataFile::partition` (`manifest/manifest_entry.h`) and reachable via `FileScanTask::data_file()`; `writer.cc` omits identity-partitioned columns from the parquet on purpose (`IcebergToArrowSchemaWithFieldIds` skips identity source ids), so the values exist only in the tuple. Neither read path consults it. iceberg-cpp has no constant-column synthesis, but `SourceTableReader` is our code and nothing in vendor/ blocks doing it there — filed-by 03, corrected-by 09 (MOR path does not serve these either), reframed 2026-07-18 (not vendor-blocked) — owner: unassigned

**mutation and lifecycle surfaces**

- declaring sort order / properties on EXISTING tables — no surface exists (pp-declare-sort binary built then deleted: packaging undecided; user direction leans config.lua `tables` section + Lua interface) — filed-by 06 — owner: future design session. Until filled, order-requiring paths (ScanByK, windowed hist, verify primes, Extent frontier) error on the live tables. Preconditions the surface must keep: uuid-guarded updateTable, refuse if primary-key bounds missing on any committed file, refuse to replace a different existing order, idempotent no-op.
- snapshot expiry is not wired. `ExpireSnapshots` (`iceberg/update/expire_snapshots.h`) exists in the vendored tree with `CleanupLevel{kNone, kMetadataOnly, kAll}` and deletes superseded partition-statistics files by diffing `metadata.partition_statistics` before/after; nothing in our tree calls it. Only `SetSnapshotRef` is used (`pp_commit.cc`) — filed-by 05 rework, corrected 2026-07-18 (previously filed as "no expiry/cleanup surface", which understated that the upstream API is present) — owner: unassigned
- files orphaned by failed transactions are unreachable by expiry: they are referenced by no snapshot, and expiry works from metadata reachability. iceberg-cpp has no `RemoveOrphanFiles` equivalent. Applies to stats parquets and to data files alike — `MoveStagedFilesInto` relocates into `data/` BEFORE the commit, so a post-move failure leaves them behind (the staging root is removed only on success) — filed-by 05 rework, split 2026-07-18 — owner: unassigned
- rollback has no surface. Mechanically it is `SetSnapshotRef` pointed at an older snapshot, already used on the commit path; nothing exposes it. Distinct from expiry — previously conflated — filed-by 2026-07-18 — owner: unassigned

**writer shape**

- `ShapePolicy` is uniform across all tables: `generate.cc` default-constructs one instance and passes it to an `AlignedBucketWriter` spanning every `BoundTable`, so primes and the 1.884B-row partitions table share `file_target_bytes`, `rgs_per_file`, `bucket_target_bytes`, and a single `ref_bytes_per_row_prior = 1.1`. No per-table override exists — raised in `running_notes.md` and unanswered — filed-by 2026-07-18 — owner: unassigned

**test and build coverage**

- the e2e suite asserted incompletenesses as expected behavior, tracing to the testing_overhaul kickoff brief's "behaviors e2e must ASSERT (loud-by-design, do not fix)" list, which did not distinguish spec-correct refusals from unimplemented capability. Reversed 2026-07-18 for the identity-column and non-int-Literal cases. Still standing: `PlanRoutesReturn406` asserts 406 on all four plan routes, which pins non-conformance on three of them — filed-by 2026-07-18 — owner: unassigned
- sanitizer coverage is our-code-only: `./configure --sanitize` instruments every object primeparts compiles, but the vendored static arrow/iceberg in the prefix are release-built. Both trees expose sanitizers as build options — `ARROW_USE_ASAN`/`ARROW_USE_UBSAN` (`arrow/cpp/cmake_modules/DefineOptions.cmake`) and `ICEBERG_ENABLE_ASAN`/`ICEBERG_ENABLE_UBSAN` (`iceberg-cpp/CMakeLists.txt`), so this is a prefix-build gap, not a vendor-boundary limit — filed-by testing_overhaul 05, corrected 2026-07-18 — owner: unassigned
- lua query module, lua presets, and pp_lmdb_store have no direct test coverage: their smokes were deleted in testing_overhaul and not re-expressed as gtest; these paths are exercised only indirectly by the gtest suite and e2e where reached — filed-by testing_overhaul 03 — owner: unassigned

**retired**

- generate-commit-smoke (`make smoke`) arg-parse failure — obsolete: all smokes deleted in testing_overhaul 03. `kMinCount` remains confined to `src/generate.cc`.

## groups at a glance

| group | holes | hard | priority |
|---|---|---|---|
| type / expression narrowing | `Literal` erased to `int64_t` at five sites: writer stat bounds, stats tuple values, stats source types, task-order decode, `ScanPlan.key_lo/key_hi` | 4 | P1 — stated |
| spec surface | `ScanPlanRequest` missing `min-rows-requested` + `use-snapshot-schema` (both added 2026-07-18); 406 on three routes the yaml does not list it for; `/v1/config` omits `endpoints` | 2 | P1 — stated goal |
| read-path synthesis | identity-partition columns unreachable on both read paths | 2 | P2 — inferred |
| transform coverage | `partition_stats` identity-only guard (over-broad); `table_traits` identity vs monotonicity; delete manifests; multi-spec tables | 3 | P2 — inferred |
| test + build | e2e ratchet (`PlanRoutesReturn406` still standing); sanitizer prefix uninstrumented; lua/presets/lmdb uncovered | 2 | P2 — stated (sanitizers next) |
| mutation + lifecycle | existing-table declaration surface; expiry unwired; orphan reconciliation; rollback surface | 4 | P3 — blocked on design |
| writer shape | `ShapePolicy` uniform across all tables | 2 | P4 — inferred |

hard = lines touched + design decision points + custom code above the vendored libs + format-migration risk. 1 easiest, 5 hardest.

Notes on the numbers. Type narrowing is a 4 not a 5: the shape is known (carry `Literal`), but it crosses five call sites into the public `ScanPlan` surface and needs cross-type comparison semantics. Spec surface is a 2 because two of three items are additive and the route split is a handler split plus one test. Mutation + lifecycle is a 4 on decision content rather than volume — four independent pieces, one blocked on a packaging decision, one with no upstream API, all touching live-warehouse state.

Format-version risk is confined to the delete-manifest and MOR items under transform coverage. iceberg-cpp caps at `kSupportedTableFormatVersion = 3` and carries the v3 shapes (`IsDeletionVector()`, `next_row_id`), so v3 is reachable rather than blocked; our tables are v2 and append-only with identity partitions, so a v2→v3 move is metadata-level unless deletes are adopted. Nothing in the other groups implies a warehouse rewrite.

## potential paths (not decisions; entries above stay prescription-free)

Sketches only — each names the seam and the known blocker.

**spec surface.** `min-rows-requested` and `use-snapshot-schema` are additive to `ScanPlanRequest`; the first wants `ScanByK`'s existing early-stop rewired to read from the field rather than a separate argument, so the capability becomes reachable through the spec shape. `use-snapshot-schema` needs a schema-resolution decision (snapshot schema vs table schema) before it means anything.

**the `Literal` collapse.** One change with five call sites. `DecodeIntegerBound` is where the type dies; carrying `Literal` through is the shared path. `ScanPlan.key_lo/key_hi` is the widest edit since the type crosses into the query surface, and it is the item that most directly gates arbitrary-predicate pushdown for a dropped-in engine.

**identity-partition synthesis.** Constant-array widen from `DataFile::partition` onto the batch inside `SourceTableReader::Next`, after `ReadNext` and before `SliceToKeyWindow`, so the batch matches `projected_schema` as early as possible. Width follows the schema (`PrimesSchema` declares both bucket columns `int32`), so it is not a choice. `FullTableReadSynthesizesIdentityColumns` (e2e) is red pending this.

**transform guards.** Two separable edits: narrow the `partition_stats` guard to the source-type check it actually depends on, and split the `table_traits` guard into monotonic-transform support versus bucket ordering as a stated non-goal.

**lifecycle.** Expiry is wiring plus a retention policy against an API already in the prefix. Orphan reconciliation is ours to write: list the table directory, diff against the reachable set, age-guard so in-flight writes survive. Rollback is a surface over `SetSnapshotRef`. Three separate pieces of work.

**ShapePolicy.** Per-table policy means threading an override through `AlignedBucketWriter::Make` alongside `BoundTable`. The prior is the interesting part — one `ref_bytes_per_row_prior` for tables with very different row widths is the thing to measure before choosing a shape.

**sanitizers.** A second instrumented prefix built by `vendor_sync` with the four options above, selected by `--sanitize`. Cost is build time and disk, not design.

**plan routes.** The 02 planner module is the body for a future server-side lift; client-side planning stays correct meanwhile. Separately, the shared `planning_unsupported` handler wants splitting — 406 is right for `planTableScan`, the other three want 404 for an unknown plan-id/plan-task. `PlanRoutesReturn406` changes with it.

**test ratchet.** Before closing any hole, check whether a passing e2e case asserts it. Spec-correct assertions (missing-sort-order errors) stay; capability assertions retire with their holes; `PlanRoutesReturn406` is neither — it asserts a status the spec does not list for three of its routes.
