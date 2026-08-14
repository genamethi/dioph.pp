# pp-catalogd — Iceberg REST spec conformance

Route surface of `native/src/catalog/pp_catalogd.cc` against the OpenAPI spec at
`docs/vendor/iceberg/open-api/rest-catalog-open-api.yaml` (anchors are line numbers
of each `path:` entry). Companion docs: `irc_catalog_design.md`,
`clients_rest_gap.md` (the client side of the same seam), `iceberg_data_setup.md`,
`../misc/HANDOFF.md`.

Routes are bare `/v1/...` with no `{prefix}` segment (legal while `getConfig`
returns no prefix). HEAD existence checks are served by the GET handlers
(cpp-httplib dispatches HEAD→GET).

## Route status

| Operation | Route | Spec | Status |
|---|---|---|---|
| getConfig | `GET /v1/config` | yaml:65 | done; `endpoints` derived from the router |
| getToken | `POST /v1/oauth/tokens` | yaml:181 | skip (deprecated in spec) |
| listNamespaces | `GET .../namespaces` | yaml:250 | done; no pagination |
| createNamespace | `POST .../namespaces` | yaml:250 | done |
| loadNamespaceMetadata | `GET .../namespaces/{ns}` | yaml:351 | done |
| namespaceExists | `HEAD .../namespaces/{ns}` | yaml:351 | done (HEAD→GET, 204) |
| dropNamespace | `DELETE .../namespaces/{ns}` | yaml:351 | done |
| updateProperties | `POST .../{ns}/properties` | yaml:460 | done |
| listTables | `GET .../{ns}/tables` | yaml:525 | done; no pagination |
| createTable | `POST .../{ns}/tables` | yaml:525 | done; no `stage-create` |
| planTableScan | `POST .../tables/{t}/plan` | yaml:707 | done; always answers 200 `submitted` |
| fetchPlanningResult | `GET .../plan/{plan-id}` | yaml:796 | done; 404 NoSuchPlanIdException when unknown |
| cancelPlanning | `DELETE .../plan/{plan-id}` | yaml:796 | done; 204, or 404 NoSuchPlanIdException |
| fetchScanTasks | `POST .../tables/{t}/tasks` | yaml:919 | done; 404 NoSuchPlanTaskException when unknown |
| registerTable | `POST .../{ns}/register` | yaml:971 | done |
| loadTable | `GET .../tables/{t}` | yaml:1027 | done; no `?snapshots=`, no ETag |
| updateTable | `POST .../tables/{t}` | yaml:1027 | done |
| dropTable | `DELETE .../tables/{t}` | yaml:1027 | done (`?purgeRequested=`) |
| tableExists | `HEAD .../tables/{t}` | yaml:1027 | done (HEAD→GET, 204) |
| unregisterTable | `POST .../tables/{t}/unregister` | yaml:1302 | not implemented |
| loadCredentials | `GET .../tables/{t}/credentials` | yaml:1352 | skip (local fs) |
| signRequest | `POST .../tables/{t}/sign` | yaml:1398 | skip (local fs) |
| renameTable | `POST /v1/tables/rename` | yaml:1432 | done |
| reportMetrics | `POST .../tables/{t}/metrics` | yaml:1496 | 204 stub |
| commitTransaction | `POST /v1/transactions/commit` | yaml:1540 | done |
| views (list/create/load/replace/drop/head/rename/register) | yaml:1657-2020 | not implemented |
| `{prefix}` route segment | all | yaml:250+ | not parsed |

## `endpoints` is load-bearing

`GET /v1/config` advertises an `endpoints` array built by `RouteTable`
(`pp_catalogd.cc`), which takes the spec path and advertised verbs at the same
call that binds each handler — a route cannot be served without stating how it
is advertised. Non-spec extension routes (`field-upper-bound`) pass an empty
advertise list and stay out of the array.

This is not decoration. The vendored `RestCatalog` client parses `endpoints`
into `supported_endpoints_` and gates every call on it
(`ICEBERG_ENDPOINT_CHECK`, `rest_catalog.cc`), and a server that sends the field
*replaces* the client's assumed default set rather than extending it
(yaml:105-135). Dropping an entry therefore disables that route for our own
tools, not just for third parties. The advertised strings must match
`Endpoint::` path templates in `catalog/rest/endpoint.h` exactly.
`ConfigAdvertisesSupersetOfSpecDefaultEndpoints` (e2e) pins the superset
property.

## commitTransaction

`POST /v1/transactions/commit` parses each `table-changes[]` element's
`requirements[]`/`updates[]` and applies them through the engine inside one
`store->RunInTransaction`, swapping every table's `metadata_location` head pointer
in a single LMDB write txn (atomic-or-abort). The client
(`CommitFilesAtomic`, `pp_commit.{h,cc}`) writes all parquet + manifests and
assembles the request; the server only validates requirements and does the CAS.

## Scan planning

Implemented 2026-07-18. `loadTable` advertises `scan-planning-mode: server`
(configurable, see below) and the client reads it.

**The server plans to the metadata layer only.** catalogd owns the catalog and
metadata layers; the data layer is not its purview. `scan::PlanTableScan` walks
manifests, prunes on `DataFile` statistics, orders by bound and estimates rows
from `record_count` — it never opens a parquet file. The one data-layer step,
`scan::RefineSplits`, selects row groups and is called only by in-process
consumers. So catalogd needs no FileIO reach to wherever the data lives, and
catalog/metadata and data may sit on different machines.

**Wire shape is spec-only.** A `FileScanTask` on the wire is
`{ data-file, delete-file-references, residual-filter }` — the spec has no field
for a selected row-group split, so none is invented. A consumer re-derives
splits from the data file's `split-offsets` plus the residual, which is why the
plan-level residual is kept complete. Responses are built as vendored
`iceberg::rest` types and serialized by the vendored serde, which derives
`delete-files` and the reference indices itself; each is run through its
`Validate()` before serializing.

**Held state.** `PlanStore` (`catalog/plan_store.{h,cc}`) keys plans by opaque
random plan-id across the spec's four statuses. Planning runs on a worker
thread, so `planTableScan` always answers `submitted` rather than racing to
`completed` — the same request would otherwise return different shapes run to
run. Tasks beyond `--plan-batch` become plan-task tokens for `fetchScanTasks`.
Fetches are non-destructive; tokens die with their plan.

Cancellation has teeth only against planning still in flight. Against a
finished plan it is advisory, because the spec says cancellation is unnecessary
once every plan task has been fetched — a client may cancel while still holding
tokens it means to use. A cancelled plan-id stays answerable, since the spec's
`cancelled` status requires it. `PlanFn` has no cancellation point, so a
running plan runs to completion and its result is discarded at publish time.

Expiry sweeps at the top of each of the four handlers rather than on a timer,
so an idle server holds its last plans until traffic resumes.

Note this diverges from the 2026-07-09 target model recorded here previously,
which had catalogd invoking a *separate* planner binary. catalogd links the
planner module directly. The seam that mattered — the client never addresses
the planner — holds either way.

Flags: `--scan-planning-mode server|client`, `--plan-batch N`,
`--plan-ttl N`. The e2e fixture runs with `--plan-batch 1` so the plan-task
paging path is exercised rather than skipped.

## Not implemented
- **Views.** `QueryService::Materialize`'s MV cache (replace-semantics
  `primeparts.<name>` tables) covers the current need.
- **`stage-create`, pagination, `?snapshots=`, ETag/If-None-Match, `{prefix}`,
  functions, real metrics sink, `unregisterTable`.**
- **`storage-credentials`** on completed planning results. The spec allows a
  server to hand back credentials for reading the returned files; vendored
  `PlanTableScanResponse` carries a TODO where the field would go
  (`catalog/rest/types.h:341`). Only reachable where the data layer needs
  credentials the consumer does not already hold.

Each of these is tracked in `../plans/holes_registry.md`, which is the living
record; this doc describes the route surface as it stands.
