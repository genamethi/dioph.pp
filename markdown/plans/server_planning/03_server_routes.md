# 03 — server routes

dep: 02

The four planning routes, wired to the phase-02 store and the phase-01
metadata-only planner. Advertisement still says `client` — phase 05 flips it.

- [x] `planTableScan` parses the body with vendored
      `PlanTableScanRequestFromJson`, submits the metadata-only planner to the
      store, and answers 200 `submitted` with a plan-id.
- [x] `fetchPlanningResult` answers 200 with the held status; `completed`
      carries the inline batch and any plan-task tokens, `failed` carries the
      error. Unknown plan-id is 404 `NoSuchPlanIdException`.
- [x] `cancelPlanning` answers 204, or 404 `NoSuchPlanIdException`.
- [x] `fetchScanTasks` resolves a plan-task to its batch and answers 200. An
      unknown token is 404 `NoSuchPlanTaskException`.
- [x] Responses are built as vendored `iceberg::rest` types and serialized with
      the vendored serde, which derives `delete-files` and
      `delete-file-references` from the tasks. No hand-written wire shape.
- [x] Every response is run through its `Validate()` before serializing, so a
      malformed response is a loud server error rather than bad JSON on the
      wire.
- [x] `RefineSplits` stays unreachable from `pp_catalogd.cc`.
- [x] `primeparts-catalogd` links the scan objects and the plan store.
- [x] batch size and idle TTL are `--plan-batch` / `--plan-ttl`, so e2e can drive
      the paging path instead of skipping it.

grep-gate: `RefineSplits` and `SelectSplits` appear nowhere in
`src/catalog/`.

## notes

`planTableScan` always answers `submitted`, never `completed`. Planning is
genuinely asynchronous, so answering `completed` would depend on whether the
worker happened to finish first — deterministic beats marginally fewer round
trips. `PlanTableScanResponse::Validate` rejects `cancelled` here, which the
always-submitted answer never produces.

Expiry is swept at the top of each of the four plan handlers rather than by a
timer thread. A plan-id nobody fetches is therefore collected on the next
planning request, not on a clock — an idle server holds whatever it last
planned until traffic resumes. A sweeper thread would need shutdown
coordination with httplib's signal handling; that is a live question, not a
settled one.

One `FileIO` is shared by every plan worker. Arrow's filesystem handles are
thread-safe, so concurrent metadata reads are fine.

The e2e fixture starts catalogd with `--plan-batch 1` so `fetchScanTasks` is
exercised against real plan-task tokens; at the default batch size the fixture
plans within a single batch and the paging path would never run.
