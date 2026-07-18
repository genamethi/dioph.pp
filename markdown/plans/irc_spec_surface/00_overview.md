# irc_spec_surface — IN PROGRESS (branch `irc-spec-surface`)

Closes the P1 **spec surface** group of the `planner_prep` holes registry
(`../planner_prep/00_overview.md`), plus two conformance defects found while
reading the code that the registry did not carry. Criterion is unchanged: a hole
is a gap between our surface and `docs/vendor/iceberg/open-api/rest-catalog-open-api.yaml`.
Whether current callers exercise it is not a criterion.

Inherits every `planner_prep` invariant: spec names for new types; zero code
comments; no quiet fallbacks (missing capability → named error); `vendor/`
untouched; deleted capability is deleted; every phase boundary compiles;
live-warehouse mutations are user-only.

## phases

- [x] 01 route table + `/v1/config` `endpoints` derived from it
- [x] 02 status conformance: plan-route typed 404s, `renameTable` 204, `HEAD` 204 + e2e ratchet
- [x] 03 `use-snapshot-schema` honored for `true`, loud refusal for `false`
- [x] 04 `min-rows-requested` as a planning early-stop
- [x] 05 registry corrections + merge

## findings that amend the planner_prep registry

Recorded here because they change what the inherited entries say, not just
whether they are done. Phase 05 writes them back.

**`min-rows-requested` is not `ScanByK`'s LIMIT.** The registry entry claims
"`ScanByK`'s LIMIT early-stop is the `min-rows-requested` capability implemented
off-surface". It is not. `min-rows-requested` (yaml:5139) is a hint bounding how
many rows *planning* need produce; `ScanByK`'s `limit`
(`query_service.cc:234,248`) bounds rows surviving the `k` predicate after
decode. The planner cannot know residual selectivity, so identifying the two
would truncate planning and drop matches. Two capabilities, one word.

**`use-snapshot-schema` is mis-defaulted, not absent.**
`TableScanBuilder::ResolveSnapshotSchema` (vendored, `table_scan.cc:424`)
unconditionally resolves the *snapshot's* schema whenever a snapshot id is set.
That is `use-snapshot-schema: true` behavior; the spec field defaults to `false`
(yaml:5151). Every point-in-time scan we plan today silently takes the `true`
branch. The `false` branch is unreachable through the builder —
`snapshot_schema_` is private with no setter (`table_scan.h:398`) — and would
need `DataTableScan::Make` (`table_scan.h:450`) with a hand-built
`internal::TableScanContext`.

**`renameTable` returns 200.** `pp_catalogd.cc:466` sets 200; the yaml lists
only 204 for that route (yaml:1457). Not previously filed.

**`HEAD` returns 200, not 204.** Found in phase 01 by advertising the HEAD
verbs and then exercising them. `namespaceExists` (yaml:395) and `tableExists`
(yaml:1277) list only 204 on success; because cpp-httplib dispatches HEAD→GET,
both inherit the GET handler's 200. Advertising HEAD is still correct — the
existence check itself answers, and 404 on a missing namespace/table is right —
but the success status is not. Fixing it means branching on `req.method` in the
two shared handlers.

**Advertising `endpoints` replaces the default set.** Per yaml:105–135 a client
that receives `endpoints` assumes *only* those routes. An incomplete list is
worse than omission, which is why phase 01 derives it from the router rather
than maintaining it by hand.

## decisions taken (user, 2026-07-18)

| question | decision |
|---|---|
| `use-snapshot-schema` with a vendored builder that hardcodes resolution | honor `true`; refuse `false` loudly with a named error citing the vendored constraint |
| what `/v1/config` advertises | derive from the router so the list cannot drift |
| the three non-`planTableScan` plan routes | typed 404s (`NoSuchPlanIdException`, `NoSuchPlanTaskException`) |
| `min-rows-requested` | implement as a planning early-stop, sound only where the task residual is trivial |
