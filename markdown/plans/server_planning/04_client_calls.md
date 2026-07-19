# 04 — client calls

dep: 03

The other side of the interface. Vendored `RestCatalog` has the four
`Endpoint::` constants but no methods, so the calls are ours to write.

- [x] `catalog/plan_status.h` holds `PlanStatus` so both sides speak one status
      enum; `plan_store.h` includes it rather than declaring its own.
- [x] `NamespaceUrlPath` moves to the client header instead of being duplicated
      — URL encoding drifting between call sites is a real failure mode.
- [x] `catalog/rest_scan_plan.h` exposes the four calls plus one driver.
- [x] The public header takes `scan::ScanPlanRequest` — our own planning request
      type, not the vendored `iceberg::rest` one — so a consumer builds the same
      request whether planning runs in-process or on the server.
- [x] The public header exposes no `iceberg/catalog/rest/` internal types;
      results come back as `iceberg::FileScanTask`, which is public API.
- [x] `PlanScanOnServer` drives the whole lifecycle: submit, poll to a terminal
      status, then page every plan-task into one task list.
- [x] Polling has a caller-supplied interval and timeout; a timeout is a named
      error, not an empty result.
- [x] e2e drives the client against a live catalogd and asserts the task set
      matches what in-process `PlanTableScan` produces for the same request.

grep-gate: `rest_scan_plan.h` includes nothing from `iceberg/catalog/rest/`.

## notes

The public header deliberately exposes no `iceberg::rest` type. Those live under
`vendor/iceberg-cpp/src` and reaching them requires `ICEBERG_SRC_CPPFLAGS`, so
putting them in our header would push that flag onto every consumer. Requests go
in as `scan::ScanPlanRequest` and tasks come back as `iceberg::FileScanTask`,
both public.

Taking our own request type is the point rather than a convenience: a consumer
builds one request and chooses in-process or server-side planning without
rewriting it.

`rest_scan_plan.cc` is a separate translation unit from `pp_iceberg_rest.cc`
because it is the only part of the client needing the vendored internal serde
headers; keeping it separate leaves the existing client TU's flags untouched.

A poll timeout is a named error carrying the plan-id, not an empty task list.

`ServerAndInProcessPlanningAgreeOnTheTaskSet` is the phase's real assertion:
the same `ScanPlanRequest` planned both ways yields the same data files. With
the fixture's `--plan-batch 1` the REST side reaches that answer only by paging
every plan-task, so the driver's paging loop is covered by the same test.
