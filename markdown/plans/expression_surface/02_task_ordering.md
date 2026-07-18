# 02 — `SortTasksByLowerBound` over `Literal`

The last narrowing on the plan path. Task ordering decoded manifest lower bounds
through `DecodeIntegerBound`, which deserialized a `Literal` and immediately
discarded its type through an `int64_t*`, so the guard above it rejected any
sort key that was not int or long. `DecodeIntegerBound` was the funnel the
registry named; it is now deleted, and nothing in the tree decodes a bound to
`int64_t`.

Bounds are decoded as `iceberg::Literal` against the sort key's own primitive
type and ordered with `operator<=>`. String, decimal, date/time/timestamp,
binary and the rest now order correctly — the capability follows from the type,
not from an enumerated allow-list.

## the total-order guard

`std::stable_sort` requires a strict weak ordering, and `Literal::operator<=>`
returns `std::partial_ordering`. Handing it a comparator that reports
`unordered` is undefined behavior, not a wrong answer, so comparability has to
be established before sorting rather than tolerated during it.

Three sources of `unordered`: a null / `AboveMax` / `BelowMin` bound, a type
whose `TypeId` falls through the vendored switch, and `kUuid` — which returns
`equivalent` for equal values but `unordered` for distinct ones
(`literal.cc:537`). That last case is why the guard cannot be a type check: a
UUID sort key is orderable exactly when every bound is identical, which is a
property of the data, not the schema.

So the guard is a probe, not a list: decode every bound, refuse any that is
null/above/below, then compare each against the first and refuse if any pair is
`unordered`. This asks the vendored implementation what it can order instead of
hardcoding an allow-list that would rot against the pinned tree, and it admits
the all-equal UUID case that a type check would reject. The error names the type
and the two values that failed to compare.

## verification

- `TaskOrdering.SortsByStringBounds` — three tasks with string lower bounds sort
  correctly; this is the capability that did not exist before.
- `TaskOrdering.RefusesASortKeyWithoutATotalOrder` — two distinct UUID bounds
  are refused with an error naming `unordered`, rather than being fed to
  `stable_sort`.
- `ScanPlannerTest.SortsTasksByLowerBoundAndErrorsOnMissing` — unchanged, still
  passing: int64 ordering and the missing-bound refusal survive the retype.

Unit 27 passing, e2e 11 passing. Four pre-existing reds unchanged; one of them,
`WriterStatColumns.StringStatColumnCapturesBounds`, belongs to the deferred
producer-side `writer.cc` site.

## surviving invariants

- No bound is decoded to `int64_t` anywhere on the plan path.
- Comparability is established before sorting, never assumed. Any `unordered`
  pair is a loud refusal — the alternative is undefined behavior.
- A missing lower bound for the sort key stays a hard error. Order-dependent
  consumers early-stop on the assumption of order, so degrading silently to
  unsorted would corrupt their results rather than slow them.
