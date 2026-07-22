# holes registry — LIVING

The single living record of what is missing. Consolidated 2026-07-18 from four
completed plans — `planner_prep`, `testing_overhaul`, `irc_spec_surface`,
`expression_surface` — and again 2026-07-18 from `server_planning`, whose phase
documents were cleared out. Per-phase detail
lives in git history: one commit per phase, commit message = the phase document.

## goal

Perfect the **interfaces**, not what sits behind them.

- **producer:** `primeparts-generate`. Writes; resume is the only nontrivial
  processing asked of it today. It may be broken as needed.
- **client:** one REST client, standing between the producers and catalogd.
  Tools do not each speak IRC in their own way.
- **server:** `primeparts-catalogd`, to be a *full* realization of the IRC
  server (`docs/vendor/iceberg/open-api/rest-catalog-open-api.yaml`).
- **consumers:** `pp`, `primeparts-tui`, `primeparts-verify` exist, but the real
  consumers of `FileScanTask` and the query engine behind them are
  substantially unwritten. Their interfaces are open ground — the reason to fix
  contracts now is that no consumer has yet been trapped by them.

Storage topology is not assumed. Catalog and metadata may live server-side on a
different machine from the data; spec conformance and storage flexibility are
competing constraints to be met together, not traded off.

## invariants

- Spec names for new types. Zero code comments; strip on touch.
- No quiet fallbacks: missing design/data → named error, registered here.
- `vendor/` untouched — no patches to vendored sources. Build flags and CMake
  options the vendored trees expose are NOT patches and are in scope.
- Deleted capability is deleted. Every phase boundary compiles.
- Live-warehouse mutations are user-only; the user makes merge calls.
- Hole entries document what is missing only. Resolutions live in the separate
  section at the end. Unowned → `owner: unassigned`.

## registry criterion

A hole is a gap between our surface and the yaml, or an unimplemented capability
behind a loud error. Whether current applications exercise it is NOT a criterion
— surface coverage is the goal, and narrowing a type or guard to fit today's
callers is itself a hole.

Before closing any hole, check whether a passing test asserts it. Spec-correct
assertions stay; capability assertions retire with their holes.

## decisions of record

| date | decision |
|---|---|
| 2026-07-18 | `use-snapshot-schema`: honor `true`; refuse `false` loudly rather than silently resolving the wrong schema |
| 2026-07-18 | `/v1/config` `endpoints` derived from the router, never hand-maintained |
| 2026-07-18 | plan routes: 406 on `planTableScan` only; typed 404s elsewhere |
| 2026-07-18 | `min-rows-requested` is a planning early-stop counted against *proven* row counts only |
| 2026-07-18 | plan-level residual stays **complete**; the key window is a hint a consumer may decline |
| 2026-07-18 | `ScanPlan.key_lo/key_hi` retained, carried as `iceberg::Literal` |
| 2026-07-18 | **server-side planning: implement in full** — all four routes plus plan-id lifecycle, flipping `scan-planning-mode` to `server`. Next branch. |
| 2026-07-18 | `createTable`: the server ensures the table location exists, expressed so it is a no-op where directories are not a concept |
| 2026-07-18 | existing-table declaration: **REST-only**, no local tool |
| 2026-07-18 | the server owns catalog and metadata; the **data layer is not its purview**, so server-side planning never opens a parquet file |
| 2026-07-18 | planning wire shape is **spec-only** — no namespaced extension; splits are re-derivable from `split-offsets` plus the complete residual |
| 2026-07-18 | `plan-tasks` are emitted above a configurable batch size, so `fetchScanTasks` is a live route rather than a permanent 404 |
| 2026-07-18 | planning is **genuinely async**; `planTableScan` always answers `submitted`, never racing to `completed` |
| 2026-07-18 | cancellation has teeth only against planning still in flight; against a finished plan it is advisory |
| 2026-07-18 | a cancelled plan-id stays answerable, because the spec's `cancelled` status requires it |
| 2026-07-18 | `scan-planning-mode` is configurable and **read by the client**, not a constant |

## open holes

**server surface**

- server-side scan planning: **closed** 2026-07-18 on branch `server-planning`.
  All four routes serve, plan state is held behind opaque plan-ids with batching
  and TTL, the client has the four matching calls plus a lifecycle driver, and
  `scan-planning-mode` advertises `server` with a client that reads it. The
  residue is filed as the four entries below.
- planning results carry no `storage-credentials`. The spec puts them on
  `CompletedPlanningResult` so a client can read the returned files; vendored
  `PlanTableScanResponse` carries a TODO in place of the field
  (`catalog/rest/types.h:341`). Reachable only where the data layer needs
  credentials the client does not already hold — filed-by server_planning 03 —
  owner: unassigned
- plan expiry sweeps on request, not on a clock. `ExpireIdle` runs at the top of
  each of the four planning handlers, so a server that goes idle holds its last
  plans until traffic resumes. A timer thread would need shutdown coordination
  with httplib's signal handling — filed-by server_planning 03 — owner: unassigned
- cancelling a plan whose worker is still running cannot interrupt the work. The
  injected `PlanFn` has no cancellation point, so the planning runs to
  completion and the result is discarded at publish time — filed-by
  server_planning 02 — owner: unassigned
- no single planning entry point reads the advertised `scan-planning-mode` and
  dispatches. `FetchScanPlanningMode` exposes the value and `PlanScanOnServer`
  and `scan::PlanTableScan` are both callable, but choosing between them is left
  to each consumer. In-process consumers (`source_scan.cc`, `query_service.cc`)
  read metadata off disk and never consult the advertisement at all; that is
  consistent only while metadata is local to the consumer — filed-by
  server_planning 05 — owner: unassigned
- `createTable` answers 500 when the metadata location's parent does not exist.
  arrow's `LocalFileSystem` does not create parents on open-for-write; object
  stores have no directories at all, so this is specific to filesystem-backed
  storage and must not be fixed by assuming a local tree — catalog and metadata
  may sit on a different machine from the data. 5XX is a listed status, so this
  is reachability rather than status conformance — filed-by irc_spec_surface 02,
  reframed 2026-07-18 — owner: unassigned
- branch-schema resolution is unimplemented. `use-snapshot-schema: false`
  against a snapshot whose schema id differs from the current one errors loudly
  (`CheckSnapshotSchemaSupported`). The vendored `TableScanBuilder` resolves the
  snapshot's schema whenever a snapshot-id is set and exposes no override —
  `snapshot_schema_` is private (`table_scan.h:398`). Tables whose schema has
  never evolved are unaffected — filed-by irc_spec_surface 03 — owner: unassigned
- no pagination (`listNamespaces`, `listTables`), no `stage-create`, no
  `?snapshots=`, no ETag/`If-None-Match`, no `{prefix}` parsing, no
  `unregisterTable`, no views, no functions, metrics is a 204 sink — filed-by
  catalogd_rest_gap.md — owner: unassigned

**consumer surface**

- no `FileScanTask` consumer reads a server plan and then reads its returned data
  files. `PlanScanOnServer` yields the task set and `SourceTableReader` reads
  Parquet, but no consumer joins the two — the query engine behind the plan is
  unwritten — filed-by pp_graph 05 — owner: pp-graph
- DuckDB's iceberg REST client (1.5.4) cannot read data files vended by
  `pp-catalogd`: its `ATTACH (TYPE ICEBERG)` read path assumes cloud object
  storage and demands a region / vended credentials, and `DEFAULT_REGION` is
  rejected as an unhandled ATTACH option. Catalog navigation over `/v1` and
  direct `read_parquet`/`iceberg_scan(metadata.json)` both work; the local-file
  read path through the REST catalog does not — filed-by pp_graph 04 (conformance
  probe 2026-07-22) — owner: pp-graph

**type narrowing — producer side**

The plan path is closed (`DecodeIntegerBound` deleted, `Literal` key window,
`Literal` task ordering). Two sites remain, both behind the interface.

- `writer.cc` stat-column bounds: `BatchColumnBounds` reads an arrow array to
  `pair<int64_t,int64_t>` and `TypedLiteral` rebuilds a `Literal` from it, so a
  non-int stat column cannot record bounds. `WriterStatColumns.StringStatColumnCapturesBounds`
  is red pending this — owner: unassigned
- `partition_stats.cc` tuple values and partition source types: tuples are
  `std::vector<int64_t>` used as `std::map` keys. A `Literal` tuple needs a
  **total** order, but `Literal::operator<=>` yields `std::partial_ordering` —
  `kUuid` compares distinct values as unordered (`literal.cc:537`) and unknown
  type ids fall through likewise. Choosing the map's ordering is a design
  question, not a retype — owner: unassigned

**transform coverage**

- `partition_stats.cc` resolves identity partition transforms only. The guard
  rejects on transform identity alone, but `bucket`/`year`/`month`/`day`/`hour`
  all produce int32 partition values the existing tuples already represent — the
  guard is broader than the limitation beneath it, which is the source-type
  check — filed-by 05 rework, corrected 2026-07-18 — owner: unassigned
- `table_traits.cc` rejects declared sort orders with non-identity transforms.
  The actual requirement is monotonicity: `truncate` and the time transforms
  preserve order, so manifest bounds map through; `bucket` is a hash and
  destroys order, so ordering on a bucket sort key is not derivable from source
  bounds under any implementation. Composes with task ordering — clearing one
  guard does not reach the other — filed-by post-review rework, split
  2026-07-18 — owner: unassigned
- `partition_stats.cc` errors loudly on snapshots carrying delete manifests and
  on multi-spec tables (partition evolution) — filed-by 05 rework — owner: unassigned

**read-path capability**

- selecting identity-partition columns (`p_bucket_version`/`p_bucket`) errors on
  BOTH read paths ("Missing required field with id: N"). Values are present at
  `DataFile::partition` and reachable via `FileScanTask::data_file()`;
  `writer.cc` omits identity-partitioned columns from the parquet on purpose
  (`IcebergToArrowSchemaWithFieldIds` skips identity source ids), so they exist
  only in the tuple. Neither read path consults it. iceberg-cpp has no
  constant-column synthesis, but `SourceTableReader` is our code and nothing in
  `vendor/` blocks doing it there. `FullTableReadSynthesizesIdentityColumns`
  (e2e) is red pending this — filed-by 03, reframed 2026-07-18 — owner: unassigned

**mutation and lifecycle**

- declaring sort order / properties on EXISTING tables. Decided 2026-07-18:
  REST-only, no local tool. The preconditions map onto spec `TableRequirements`
  — uuid guard is `assert-table-uuid`, refusing to replace a different order is
  `assert-default-sort-order-id`. The domain precondition (refuse if primary-key
  bounds are missing on any committed file) has **no spec counterpart** and
  still needs a home. Until filled, order-requiring paths (ScanByK, windowed
  hist, verify primes, Extent frontier) error on the live tables.
  *The earlier note associating this with a `config.lua` `tables` section and
  the Lua interface was incorrect and is withdrawn — the Lua interface has
  nothing to do with table declaration* — filed-by 06, corrected 2026-07-18 —
  owner: unassigned
- snapshot expiry is not wired. `ExpireSnapshots` (`iceberg/update/expire_snapshots.h`)
  exists in the vendored tree with `CleanupLevel{kNone, kMetadataOnly, kAll}`
  and deletes superseded partition-statistics files by diffing
  `metadata.partition_statistics`; nothing in our tree calls it. Only
  `SetSnapshotRef` is used — filed-by 05 rework — owner: unassigned
- files orphaned by failed transactions are unreachable by expiry: referenced by
  no snapshot, and expiry works from metadata reachability. iceberg-cpp has no
  `RemoveOrphanFiles` equivalent. Applies to stats parquets and data files alike
  — `MoveStagedFilesInto` relocates into `data/` BEFORE the commit, so a
  post-move failure leaves them behind — filed-by 05 rework — owner: unassigned
- rollback has no surface. Mechanically `SetSnapshotRef` at an older snapshot,
  already used on the commit path; nothing exposes it — filed-by 2026-07-18 —
  owner: unassigned

**writer shape**

- `ShapePolicy` is uniform across all tables: `generate.cc` default-constructs
  one instance for an `AlignedBucketWriter` spanning every `BoundTable`, so
  `primes` and `partitions` — the latter carrying one row per representation,
  so `sum(primes.k)` rows against `primes`' one row per prime — share
  `file_target_bytes`, `rgs_per_file`, `bucket_target_bytes`, and one
  `ref_bytes_per_row_prior = 1.1` (`aligned_writer.h:45`, consumed at
  `aligned_writer.cc:170` as the estimation prior for byte-driven cuts). No
  per-table override exists. `file_target_bytes` and `rgs_per_file` shadow
  spec'd table properties — `write.target-file-size-bytes`
  (`table_properties.h:245`, default 512 MB) and
  `write.parquet.row-group-size-bytes` (`:114`, default 128 MB) — which
  `CreateTableRequest.properties` can set per table at creation.
  `bucket_target_bytes` has no spec counterpart — filed-by 2026-07-18 —
  owner: unassigned

**test and build**

- sanitizer coverage is our-code-only: `./configure --sanitize` instruments
  every object primeparts compiles, but the vendored static arrow/iceberg in the
  prefix are release-built. Both trees expose sanitizers as build options
  (`ARROW_USE_ASAN`/`ARROW_USE_UBSAN`, `ICEBERG_ENABLE_ASAN`/`ICEBERG_ENABLE_UBSAN`),
  so this is a prefix-build gap, not a vendor-boundary limit — owner: unassigned
- lua query module, lua presets, and `pp_lmdb_store` have no direct test
  coverage; exercised only indirectly — filed-by testing_overhaul 03 — owner: unassigned

## known-red tests (expected; do not "fix" without closing the hole)

As of 2026-07-19: unit 43 passing / 3 red (`make test`), e2e 18 passing / 1 red
(`make e2e`). Every red is deliberate — a test written to assert correct
behavior that is not yet implemented, so it goes green when its hole closes.

| test | hole |
|---|---|
| `WriterStatColumns.StringStatColumnCapturesBounds` | type narrowing — `writer.cc` stat bounds |
| `PartitionStatsTest.BucketTransformResolves` | transform coverage — `partition_stats` identity-only guard |
| `PartitionStatsTest.TruncateTransformResolves` | transform coverage — same guard |
| `E2ETest.FullTableReadSynthesizesIdentityColumns` | read-path — identity-partition synthesis |

Fixture hitch: the e2e `primes` table is deliberately **unsorted**, and that is
load-bearing for `ScanByKErrorsWithoutSortOrder` and
`WindowedGroupCountErrorsWithoutSortOrder`. Anything needing a sorted table
(key-window derivation, task ordering) must be unit-tested against hand-built
metadata rather than by giving the fixture a sort order.

## groups at a glance

| group | hard | priority |
|---|---|---|
| server surface — planning residue (credentials, sweep, dispatch) | 2 | P2 |
| server surface — location creation, branch schema, pagination et al. | 2 | P2 |
| read-path synthesis — identity-partition columns | 2 | P2 |
| transform coverage | 3 | P2 |
| type narrowing — producer side | 3 | P2 (behind the interface) |
| test + build | 2 | P2 |
| mutation + lifecycle | 4 | P3 |
| writer shape | 2 | P4 |

hard = lines touched + design decision points + custom code above the vendored
libs + format-migration risk. 1 easiest, 5 hardest.

Format-version risk is confined to the delete-manifest and MOR items under
transform coverage. iceberg-cpp caps at `kSupportedTableFormatVersion = 3` and
carries the v3 shapes, so v3 is reachable rather than blocked; our tables are v2
and append-only with identity partitions, so a v2→v3 move is metadata-level
unless deletes are adopted. Nothing else implies a warehouse rewrite.

## potential paths (not decisions; entries above stay prescription-free)

**identity-partition synthesis.** Constant-array widen from `DataFile::partition`
onto the batch inside `SourceTableReader::Next`, after `ReadNext` and before
`SliceToKeyWindow`, so the batch matches `projected_schema` as early as possible.
Width follows the schema (`PrimesSchema` declares both bucket columns `int32`),
so it is not a choice.

**transform guards.** Two separable edits: narrow the `partition_stats` guard to
the source-type check it actually depends on, and split the `table_traits` guard
into monotonic-transform support versus bucket ordering as a stated non-goal.

**lifecycle.** Expiry is wiring plus a retention policy against an API already in
the prefix. Orphan reconciliation is ours to write: list the table location,
diff against the reachable set, age-guard so in-flight writes survive. Rollback
is a surface over `SetSnapshotRef`.

**ShapePolicy.** Per-table policy means threading an override through
`AlignedBucketWriter::Make` alongside `BoundTable`. The prior is the interesting
part — one `ref_bytes_per_row_prior` for tables with very different row widths is
the thing to measure before choosing a shape.

**sanitizers.** A second instrumented prefix built by `vendor_sync` with the four
options above, selected by `--sanitize`. Cost is build time and disk, not design.
