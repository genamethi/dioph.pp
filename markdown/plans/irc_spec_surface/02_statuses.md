# 02 — status conformance

Three routes answered with a status the yaml does not list for them. All three
are now the listed status; each was verified by exercising the route.

**plan routes.** One `planning_unsupported` handler was bound to all four,
returning 406. The yaml lists 406 only on `planTableScan` (yaml:787), which
stays — server-side planning genuinely is unsupported and
`scan-planning-mode: client` is advertised, so refusing is conformant. The other
three list 404 with typed exceptions and no 406: `fetchPlanningResult` and
`cancelPlanning` take a `plan-id` (yaml:838–854), `fetchScanTasks` a `plan-task`
(yaml:947–963). With no server-side planning no plan-id or plan-task can ever
exist, so every request to them names something unknown — 404 is not a
concession, it is the accurate answer. `no_such_plan_id` echoes the rejected
plan-id back.

**`renameTable`.** Returned 200; the yaml lists only 204 (yaml:1457).

**`namespaceExists` / `tableExists`.** Returned 200; the yaml lists only 204
(yaml:395, yaml:1277). cpp-httplib dispatches HEAD→GET, so both inherited the
GET handler's status. The handlers now branch on `req.method` and return 204
with no body. The 404 path was already correct and is unchanged.

## verification

- `PlanRoutesMatchSpecStatuses` (replaces `PlanRoutesReturn406`) asserts the
  per-route status *and* exception type across all four routes.
- `ExistenceChecksReturn204` asserts 204 present / 404 missing for both
  namespace and table.
- `ConfigAdvertisesSupersetOfSpecDefaultEndpoints` pins the phase-01 invariant
  as a test: every spec-default route stays advertised, so a future route edit
  cannot silently withdraw one.
- `renameTable` has no e2e coverage (renaming would mutate the shared e2e
  warehouse). Verified by hand against a scratch warehouse: 204 on success,
  404 on a missing source, and the identifier actually moved.

Suite: 8 passing. `FullTableReadSynthesizesIdentityColumns` remains red — the
pre-existing identity-partition-synthesis hole, untouched here.

## test ratchet

`PlanRoutesReturn406` was retired, not weakened. It asserted 406 on all four
routes, pinning the non-conformance on three of them; that is a capability
assertion, which retires with its hole. The spec-correct assertions it also
carried (that the routes answer at all, with a typed error body) survive in
`PlanRoutesMatchSpecStatuses`.

## found here, not fixed

`createTable` returns 500 (`IOError: Failed to open local file …`) when the
table's parent directory does not exist — FileIO does not create it. Our own
tools pre-create the tree, so this is invisible to `generate`/the sieve, but it
is on the path any third-party IRC client takes to create a table, which is the
interop `irc_catalog_design.md` claims. 5XX is a listed status for the route, so
this is a robustness hole rather than a status-conformance one. Unassigned.
