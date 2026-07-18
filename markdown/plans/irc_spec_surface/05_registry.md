# 05 — registry corrections

Writes phases 01–04 back to the inherited registry
(`../planner_prep/00_overview.md`) and to `../../data_eng/catalogd_rest_gap.md`.

## closed, moved to retired

The `ScanPlanRequest` field entry, the three-plan-routes entry, and the
`/v1/config` `endpoints` entry. The **spec surface** group is now closed as a
group; its residue is filed as three independent entries rather than left
implied.

Two retired entries carry a correction rather than a checkmark, because their
premise was wrong and a future reader would otherwise re-derive the same
mistake:

- `ScanByK`'s LIMIT is *not* `min-rows-requested` implemented off-surface. The
  registry said it was, and the paths section proposed rewiring one to the
  other, which would have truncated planning and dropped matches.
- `use-snapshot-schema` was not absent but mis-defaulted — the vendored builder
  was already taking the `true` branch on every point-in-time scan.

## newly filed

- branch-schema resolution (`use-snapshot-schema: false` against a
  schema-evolved snapshot) — refused loudly, blocked on the vendored builder
  exposing no schema override.
- `createTable` answers 500 when the table's parent directory does not exist.
  Invisible to our tools, which pre-create the tree; on the path of any
  third-party IRC client.

## `catalogd_rest_gap.md`

Route statuses updated for the four plan routes and the two HEAD verbs, and a
new section records that `endpoints` is load-bearing: the vendored `RestCatalog`
client gates every call on it, and sending the field replaces the client's
assumed default set. Dropping an entry disables that route for our own tools.
That fact is the reason phase 01 derived the list rather than hand-maintaining
it, and it is not evident from the yaml alone.

## state at merge

Unit 20 passing, e2e 10 passing. Four reds remain, all pre-existing and all
owned by holes outside this branch: `StringStatColumnCapturesBounds` and the two
`PartitionStatsTest` transform cases (commit 267c8cf, deliberately red), and
`FullTableReadSynthesizesIdentityColumns` (identity-partition synthesis).
