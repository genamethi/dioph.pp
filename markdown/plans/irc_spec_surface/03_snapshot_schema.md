# 03 — `use-snapshot-schema`

The field existed on `ScanPlanRequest` but nothing read it, and the behavior
underneath was already wrong. The vendored `TableScanBuilder::ResolveSnapshotSchema`
(`table_scan.cc:424`) resolves the *snapshot's* schema whenever a snapshot-id is
set and the table's current schema otherwise — it never consults the request. So
every point-in-time scan silently took the `use-snapshot-schema: true` branch
while the spec field defaults to `false` (yaml:5151). This was mis-defaulted
behavior, not an absent capability.

Two rules are in play:

- spec: schema = `use-snapshot-schema` ? resolved snapshot's schema : table schema
- builder: schema = snapshot-id set ? snapshot's schema : table schema

They agree on two of four combinations. Crucially they only differ *observably*
when the resolved snapshot's schema id differs from the table's current schema
id — on a table that has never evolved its schema, every combination coincides.
`CheckSnapshotSchemaSupported` (`scan_planner.cc`) therefore resolves the
snapshot (explicit `snapshot-id`, else `end-snapshot-id` for incremental, else
current), compares schema ids, and refuses only when the ids differ *and* the
builder's rule contradicts the request. Refusing on the flag alone would reject
scans that are correct.

Per the recorded decision, `true` is honored and `false` is refused loudly. Both
refusal messages name the vendored constraint and the escape hatch
(`DataTableScan::Make` with a hand-built `TableScanContext`), so whoever picks
this up does not have to re-derive why the obvious fix is unavailable.

## verification

Four cases in `scan_planner_test.cc`, over hand-built `TableMetadata` carrying
two schemas and a snapshot written under the older one. The guard runs before
any manifest access, so the refusal paths are reachable without a real table.

- `RefusesBranchSchemaWhenSnapshotSchemaDiffers` — `false` + snapshot-id.
- `RefusesSnapshotSchemaWithoutSnapshotId` — `true` + no snapshot-id.
- `AllowsSnapshotSchemaWhenRequestMatchesResolution` — `true` + snapshot-id
  passes the guard.
- `AllowsEitherFlagWhenSnapshotSchemaIsCurrent` — both flags pass when the
  snapshot's schema is the current one, pinning that the guard is not
  over-broad.

Unit suite 20 passing, e2e 8 passing; the pre-existing reds
(`StringStatColumnCapturesBounds`, two `PartitionStatsTest` transform cases,
`FullTableReadSynthesizesIdentityColumns`) are unchanged and belong to other
holes.

## surviving invariants

- The guard fires on observable divergence, never on the flag alone. A table
  that has not evolved its schema must plan under every flag combination.
- `use-snapshot-schema: false` with a schema-evolved snapshot stays refused
  until branch-schema resolution is actually implemented. It must not become a
  silent fallback to the snapshot schema — that is the bug this phase closed.
