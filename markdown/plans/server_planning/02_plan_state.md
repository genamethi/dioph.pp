# 02 — plan state

dep: 01

The held state behind the four routes, built standalone and unit-tested before
anything is routed. No `pp_catalogd.cc` changes in this phase.

- [x] `catalog/plan_store.h:PlanStore` holds plans by opaque plan-id across the
      four statuses — submitted, completed, failed, cancelled.
- [x] The store knows nothing about scan planning: work is injected as
      `PlanFn = bool(scan::ScanPlan*, std::string*)`, so tests drive it without
      metadata or parquet.
- [x] `Submit` spawns a worker and returns immediately with `submitted`.
- [x] `Fetch` reports the snapshot: status, error, the inline task batch, and
      plan-task tokens for the remainder. Non-destructive.
- [x] `FetchTasks` resolves a plan-task token to its batch. Tokens are scoped to
      their plan and die with it.
- [x] `Cancel` marks a plan cancelled; unknown plan-ids are distinguishable from
      cancelled ones so the routes can answer 404 vs 200.
- [x] `ExpireIdle` takes the current time as an argument rather than reading the
      clock, so TTL is testable without sleeping.
- [x] plan-ids and plan-task tokens are random hex, not a sequence.
- [x] Workers are joined on destruction; no detached threads.
- [x] `plan_store_test.cc` covers lifecycle, batching, token scoping, expiry,
      unknown-id rejection, and concurrent submits.

grep-gate: `plan_store.h` includes nothing from `iceberg/catalog/rest/` — wire
shape belongs to phase 03.

## notes

Cancellation only has teeth against planning still in flight. Against a
completed, failed or already-cancelled plan it is advisory: accepted, 204, and
nothing torn down. The spec says cancellation is unnecessary once every plan
task has been fetched, which implies a client may cancel while still holding
tokens it means to use — destroying them would strand it. A failed plan keeps
its error rather than having it overwritten.

A cancelled plan-id stays answerable rather than being erased. The spec gives
`fetchPlanningResult` a `cancelled` status, which only means something if the
server can still answer for the id; erasing would force a 404 instead.

`PlanFn` cannot be interrupted mid-flight. Cancelling a running plan sets the
flag and the worker's result is discarded at publish time rather than stored.
The work still runs to completion.

A plan whose worker is still running is never expired — evicting it would
orphan the thread's entry. It becomes eligible on the next sweep after it
finishes.

Nested `Config`'s default member initializers are not usable in a defaulted
argument of the enclosing class, so `PlanStore` has two constructors rather
than one with `Config config = {}`.

Verified under ThreadSanitizer with 8 threads interleaving submit, fetch,
fetch-tasks, cancel and expire: clean.
