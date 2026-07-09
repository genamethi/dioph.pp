# pp-catalogd — Iceberg REST spec conformance

Route surface of `native/src/catalog/pp_catalogd.cc` against the OpenAPI spec at
`native/vendor/iceberg-refs/rest-catalog-open-api.yaml` (anchors are line numbers
of each `path:` entry). Companion docs: `irc_catalog_design.md`,
`clients_rest_gap.md` (the client side of the same seam), `iceberg_data_setup.md`,
`../misc/HANDOFF.md`.

Routes are bare `/v1/...` with no `{prefix}` segment (legal while `getConfig`
returns no prefix). HEAD existence checks are served by the GET handlers
(cpp-httplib dispatches HEAD→GET).

## Route status

| Operation | Route | Spec | Status |
|---|---|---|---|
| getConfig | `GET /v1/config` | yaml:65 | done |
| getToken | `POST /v1/oauth/tokens` | yaml:181 | skip (deprecated in spec) |
| listNamespaces | `GET .../namespaces` | yaml:250 | done; no pagination |
| createNamespace | `POST .../namespaces` | yaml:250 | done |
| loadNamespaceMetadata | `GET .../namespaces/{ns}` | yaml:351 | done |
| namespaceExists | `HEAD .../namespaces/{ns}` | yaml:351 | done (HEAD→GET) |
| dropNamespace | `DELETE .../namespaces/{ns}` | yaml:351 | done |
| updateProperties | `POST .../{ns}/properties` | yaml:460 | done |
| listTables | `GET .../{ns}/tables` | yaml:525 | done; no pagination |
| createTable | `POST .../{ns}/tables` | yaml:525 | done; no `stage-create` |
| planTableScan | `POST .../tables/{t}/plan` | yaml:707 | not implemented |
| fetchPlanningResult | `GET .../plan/{plan-id}` | yaml:796 | not implemented |
| cancelPlanning | `DELETE .../plan/{plan-id}` | yaml:796 | not implemented |
| fetchScanTasks | `POST .../tables/{t}/tasks` | yaml:919 | not implemented |
| registerTable | `POST .../{ns}/register` | yaml:971 | done |
| loadTable | `GET .../tables/{t}` | yaml:1027 | done; no `?snapshots=`, no ETag |
| updateTable | `POST .../tables/{t}` | yaml:1027 | done |
| dropTable | `DELETE .../tables/{t}` | yaml:1027 | done (`?purgeRequested=`) |
| tableExists | `HEAD .../tables/{t}` | yaml:1027 | done (HEAD→GET) |
| unregisterTable | `POST .../tables/{t}/unregister` | yaml:1302 | not implemented |
| loadCredentials | `GET .../tables/{t}/credentials` | yaml:1352 | skip (local fs) |
| signRequest | `POST .../tables/{t}/sign` | yaml:1398 | skip (local fs) |
| renameTable | `POST /v1/tables/rename` | yaml:1432 | done |
| reportMetrics | `POST .../tables/{t}/metrics` | yaml:1496 | 204 stub |
| commitTransaction | `POST /v1/transactions/commit` | yaml:1540 | done |
| views (list/create/load/replace/drop/head/rename/register) | yaml:1657-2020 | not implemented |
| `{prefix}` route segment | all | yaml:250+ | not parsed |

## commitTransaction

`POST /v1/transactions/commit` parses each `table-changes[]` element's
`requirements[]`/`updates[]` and applies them through the engine inside one
`store->RunInTransaction`, swapping every table's `metadata_location` head pointer
in a single LMDB write txn (atomic-or-abort). The client
(`CommitFilesAtomic`, `pp_commit.{h,cc}`) writes all parquet + manifests and
assembles the request; the server only validates requirements and does the CAS.

## Scan planning (server-side model, 2026-07-09)

`planTableScan`/`fetchScanTasks` are unimplemented; `loadTable` advertises
`scan-planning-mode: client` and clients plan for themselves (see
`clients_rest_gap.md`). Target model when they land:

- catalogd implements the **catalog API surface** only. It does not contain the
  planner. On `planTableScan` it **invokes** a planner module (a specialized
  consumer — separate logic/binary) and holds the request until a plan is formed,
  then returns `FileScanTask`s. To the client the plan appears to come from the
  catalog; the planner is never a party the client addresses.
- A `FileScanTask` is the plan atom: `{ data-file, delete-files, residual,
  row-group ranges }`. The same atom the client builds locally today; server-side
  planning just moves its production behind the API and flips `scan-planning-mode`
  to `server`, letting clients drop manifest walking + path parsing.
- Residual → row-group selection (zone-map pruning) is the core of the plan and
  is **not** iceberg-cpp-blocked — it composes parquet/arrow primitives. Built as
  the API-shaped scan-plan atom, one implementation serves client-side planning
  now and server-side `planTableScan` later. Detail: `clients_rest_gap.md`.

## Not implemented
- **Views.** `QueryService::Materialize`'s MV cache (replace-semantics
  `primeparts.<name>` tables) covers the current need.
- **`stage-create`, pagination, `?snapshots=`, ETag/If-None-Match, `{prefix}`,
  functions, real metrics sink, `unregisterTable`.**
