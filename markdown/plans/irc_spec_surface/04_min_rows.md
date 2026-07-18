# 04 — `min-rows-requested`

The field existed on `ScanPlanRequest` and nothing read it. The inherited
registry entry said `ScanByK`'s LIMIT early-stop was this capability
"implemented off-surface"; it is not, and implementing it that way would have
been a bug. `min-rows-requested` (yaml:5139) bounds how many rows *planning*
need produce. `ScanByK`'s `limit` (`query_service.cc:234,248`) bounds rows
surviving the `k` predicate after decode. The planner cannot know residual
selectivity, so wiring one to the other would truncate planning and drop
matches. They are left as two separate things; `ScanByK` is untouched.

`PlanTableScan` now stops accumulating tasks once the hint is met, counted
against `guaranteed_rows` rather than `planned_rows`. The distinction is the
whole of the soundness argument: `planned_rows` is exact only for a task with a
trivial residual and no delete files. Under a non-trivial residual it is a
row-group upper bound, and with delete files it is a pre-deletion count — in
both cases the task may yield fewer rows than it claims, so counting it could
stop planning short and under-deliver. Such tasks are still planned, they just
do not count toward the hint.

The spec permits returning fewer rows than requested, but the stated reason is
"the scan may not produce that many rows" — that licenses under-delivery when
the table is genuinely small, not arbitrary truncation. Hence the conservative
rule: if every task carries a non-trivial residual the hint never fires and the
whole table is planned, which is correct if not tight.

## verification

Two e2e cases against the real two-file, six-row primes fixture, planning
directly off the committed metadata json.

- `MinRowsRequestedStopsPlanningEarly` — no filter, so residuals are trivial and
  counts are exact; `min_rows_requested = 1` plans strictly fewer tasks and
  strictly fewer rows than the unbounded plan.
- `MinRowsRequestedDoesNotStopOnUnprovenRowCounts` — same table under a `k = 1`
  filter; bounded and unbounded plans must agree exactly, since no task's row
  count is proven. Asserts the fixture plans more than one task first, so the
  equality cannot hold vacuously.

Unit suite 20 passing, e2e 10 passing. The four pre-existing reds are unchanged.

## surviving invariants

- Only exact row counts count toward the hint. A task with a non-trivial
  residual or any delete file is planned but never satisfies
  `min-rows-requested`.
- `min-rows-requested` and `ScanByK`'s `limit` stay separate. Identifying them
  is the error this phase exists to correct.
