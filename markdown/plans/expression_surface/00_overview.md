# expression_surface — IN PROGRESS (branch `expression-surface`, stacked on `irc-spec-surface`)

Attacks the P1 **type / expression narrowing** group of the holes registry
(`../planner_prep/00_overview.md`): values erased to `int64_t` at five seams
instead of carried as `iceberg::Literal`.

Scoped by the standing direction that the **interfaces** are the deliverable —
the one REST client and catalogd as a full server realization — and that what
sits behind them is expected to move. Producers may be broken as needed.
Consumers of `FileScanTask` and the query engine behind them are substantially
unwritten, so their interfaces are open ground and are the priority.

## re-prioritization against the registry

The registry treats the five sites as one change. Two of them —
`writer.cc` stat bounds and `partition_stats.cc` tuples — sit *behind* the
interface, on the producer side, and are filed separately rather than bundled
here. `partition_stats.cc` additionally carries a real obstacle: it keys
`std::map<std::vector<int64_t>, size_t>`, and a `Literal` tuple needs a total
order while `Literal::operator<=>` yields only a *partial* ordering
(`std::partial_ordering`, unordered for Null/AboveMax/BelowMin). That is a
design question, not a retype.

## phases

- [x] 01 `ScanPlan`: complete residual, `Literal` key window, `DeriveKeyWindow` exposed
- [ ] 02 `SortTasksByLowerBound` over `Literal` bounds
- [ ] 03 registry corrections + merge

## decisions taken (user, 2026-07-18)

| question | decision |
|---|---|
| plan-level residual | stays **complete** — the key window is a hint a consumer may decline |
| what `ScanPlan` exposes for the window | `key_lo`/`key_hi` retained, retyped `optional<iceberg::Literal>` |
| cross-type comparison semantics | not asked — `Literal::CastTo` already defines them, with `BelowMin`/`AboveMax` documented for predicate simplification; non-foldable predicates stay in the residual (inclusive, correct) |

## the invariant that makes the window safe

`SourceTableReader` returns a **superset**: it slices by the key window and does
not evaluate the residual, and consumers filter for themselves (`ScanByK`
re-tests `k` per row, `query_service.cc:250`). So the window must never *exclude*
a row satisfying the filter; over-inclusion is free. That is why a strict `>`
folds to an *inclusive* lower bound rather than incrementing the literal — no
type-specific successor function is needed, and the fold works for any comparable
type.
