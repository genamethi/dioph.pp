# server-side scan planning

Branch `server-planning`, off `expression-surface`. Closes the P1 server-surface
hole in `markdown/plans/holes_registry.md`: only one of the spec's two planning
modes exists.

## goal

All four planning routes live, plan-id lifecycle held server-side, the four
matching calls on the client, and `scan-planning-mode` flipped to `server`.

## layer principle

The server owns the catalog and metadata layers. The data layer is not its
purview. Planning server-side therefore reads `metadata.json` and manifests and
stops there — it never opens a parquet file. `SelectSplits` is the only
data-layer read in the planner and is excluded from the server path.

Consequence: catalogd needs no FileIO reach to wherever the data lives, so
catalog/metadata and data may sit on different machines.

## what the vendored library already provides

`vendor/iceberg-cpp/src/iceberg/catalog/rest/`:

- `types.h:315-380` — `PlanTableScanRequest`, `PlanTableScanResponse`,
  `FetchPlanningResultResponse`, `FetchScanTasksRequest/Response`, `PlanStatus`
- `json_serde_internal.h:92-144` — `ToJson`/`FromJson` both directions for all
  of the above, plus `DataFileFromJson`
- `expression/json_serde_internal.h` — `Expression` and `Literal` serde
- `endpoint.h:126-138` — the three `Endpoint::` constants

`rest_catalog.h` has **no** plan methods; the client side is ours to write.
`PlanTableScanRequest` is field-identical to our `ScanPlanRequest`.

## invariants

- `vendor/` untouched.
- Zero code comments.
- No quiet fallbacks: missing design or data becomes a named error, filed in the
  registry.
- Every phase boundary compiles; deleted capability stays deleted.
- The plan-level residual stays complete (registry decision 2026-07-18), which
  is what lets a consumer re-derive splits the server declined to select.

## phases

| # | file | status |
|---|---|---|
| 01 | `01_layer_boundary.md` | done |
| 02 | `02_plan_state.md` | done |
| 03 | `03_server_routes.md` | done |
| 04 | `04_client_calls.md` | done |
| 05 | `05_advertise_and_e2e.md` | not started |

## decisions

Settled 2026-07-18 by the user:

| decision |
|---|
| wire shape is **spec-only** — no namespaced extension; the consumer re-derives splits from `split-offsets` plus the complete residual |
| `plan-tasks` are emitted above a threshold, so `fetchScanTasks` is a live route rather than a permanent 404 |
| planning is **genuinely async** — worker thread, `submitted` returned immediately, `completed` once done |
| both sides this branch — catalogd routes and the client calls, then flip the advertisement |

Chosen as defaults, not asked (change freely):

| default |
|---|
| plan-ids are opaque random tokens, not a guessable sequence |
| fetches are non-destructive; a completed result is re-servable until cancel or expiry |
| plan-task tokens are scoped to their plan and die with it |
| inline-task threshold and idle TTL are both server config, not constants |
