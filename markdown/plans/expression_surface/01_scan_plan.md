# 01 — `ScanPlan`: complete residual, `Literal` key window

Two defects in one structure.

**The residual was incomplete.** `ExtractKeyWindow` folded conjuncts over the
sort key into `key_lo`/`key_hi` and *removed* them from the residual. The window
was therefore load-bearing for correctness: a consumer that read `ScanPlan` and
evaluated only `residual` silently returned too many rows. An optimization that
cannot be declined is a trap, and with the file-scan-task consumers unwritten
this was the moment to fix it for free.

`ExtractKeyWindow` is now `DeriveKeyWindow`: the residual is the filter,
unchanged and complete, and the window is derived alongside it. Declining the
window costs speed, never correctness.

**The window was `int64_t`.** `key_lo`/`key_hi` are now
`optional<iceberg::Literal>`, and folding casts the predicate literal to the
*key's* type via `Literal::CastTo` — so the window carries the column's type
rather than whatever the caller happened to write. A literal that saturates to
`AboveMax`/`BelowMin`, or is null, is not a usable bound and is skipped; it
remains in the residual, which is now complete, so skipping is always safe.

Because the reader returns a superset and consumers filter for themselves, a
strict `>` folds to an **inclusive** lower bound. Over-inclusion is free;
under-inclusion is wrong. This removes the need for a type-specific successor
function and makes the fold work for any comparable type, not just integers.

`DeriveKeyWindow` is exposed on `scan_planner.h` rather than kept file-local: it
is the operation "derive the acceleration from the general expression", which a
consumer may want to run itself, and exposing it makes the fold directly
testable.

`SliceToKeyWindow` now declines rather than fails when the sort key is not
int32/long — it slices what it can and returns the batch untouched otherwise.
That is the new contract paying for itself immediately: a non-integer sort key
degrades to a slower correct read instead of an error.

## verification

Five `KeyWindow` unit cases over `DeriveKeyWindow` directly:

- `FoldsInclusiveAndStrictBoundsInclusively` — `p > 10` folds to lo = 10, not 11.
- `TightensToTheNarrowestBound` — two lower bounds keep the tighter.
- `CastsPredicateLiteralToTheKeyType` — a `Long` literal against an `int32` key
  yields an `int32` bound.
- `IgnoresPredicatesOverOtherColumnsAndUnfoldableOps` — `k = 1` and `p != 3`
  derive nothing.
- `DoesNotFoldAnOutOfRangeLiteral` — `int64` max against an `int32` key
  saturates to `AboveMax` and is refused as a bound.

Plus `ResidualStaysCompleteAndKeyWindowIsDerived` (e2e), asserting the residual
is byte-for-byte the requested filter. The e2e primes fixture is unsorted — that
unsortedness is load-bearing for `ScanByKErrorsWithoutSortOrder` — so window
*derivation* is covered by the unit cases rather than by contorting the fixture.

Unit 25 passing, e2e 11 passing. The four pre-existing reds are unchanged.

**No behavior changed today.** `SourceTableReader::residual()` has zero callers
(`source_scan.cc:403` defines it; nothing calls it). Completing the residual
alters only what a future consumer sees, which is the point of doing it before
the consumers exist.

## surviving invariants

- The residual is complete. Every conjunct the caller supplied is in it; the key
  window never subtracts.
- The key window may over-include but must never under-include. This is what
  licenses folding strict comparisons to inclusive bounds, and it depends on the
  reader returning a superset that consumers filter.
- The window carries the key column's type, not the predicate literal's.
