# Proposal — `query` Lua module (reader/catalog interface)

**Status:** proposal, 2026-06-15. Spells out a Lua API, defined by the
reader/query layer and bound to a `QueryService` instance, that any `lua_State`
can register (the TUI's interactive REPL, preset `run` functions, or a standalone
`pp` shell). The TUI owns the `lua_State` (per `feedback_tui_design_vision`); this
module is the *content* installed into it.

Not built yet — this is the design to implement after the Config tab. It
supersedes the placeholder `pp.*` sketch in `tui_query_design.md`.

## Hosting

```cpp
// in the reader/query layer
namespace primeparts::query {
void RegisterQueryModule(lua_State* L, QueryService* qs);  // installs global `query`
}
```
Each function is a C closure carrying `QueryService*` as an upvalue; results are
plain Lua tables (numbers/strings/arrays), so nothing leaks C++ across the
boundary. Errors surface as a second return `nil, "message"` (Lua idiom) — never
a hard `luaL_error` for a missing prime (that's a normal empty result).

## Record shapes

```lua
-- a prime record (partitions empty for k=0)
prime = {
  p = 11, k = 3, prime_rank = 5,
  partitions = { {m=1, n=2, q=3}, {m=2, n=1, q=7}, {m=3, n=1, q=3} },
}
-- an edge/partition record
edge = { p = 11, m = 1, n = 2, q = 3 }
```
For a `k=0` prime, `partitions = {}` and the per-edge fields (`m`,`n`,`q`) are
absent — i.e. **nil** if you index a partition that isn't there.

## The shared filter spec

Both `pget` and `kget` take one table with the same keys; which key is
**required** differs, the rest are **optional** refinements:

| key | meaning |
|---|---|
| `p` | a prime value (required for `pget`) |
| `k` | partition count (required for `kget`) |
| `q_k`, `m_k`, `n_k` | constrain to primes/edges with a partition matching these |
| `init`, `end` | **range over p** — the scan window (the pushdown we built; load-bearing for sparse high `k`) |
| `limit` | max results (kget / scans) |

## Functions

### `query.pget(spec) -> prime | nil`
`spec.p` **required**; everything else optional. Returns the `prime` record at
`p` — its `k`, `prime_rank`, and **all** its partitions (joined from both
tables). Optional `q_k`/`m_k`/`n_k` filter the returned `partitions` (e.g.
`pget{p=11, q_k=3}` → only the `q=3` edges); optional `k` asserts the prime's k
(nil if it differs). Returns nil if `p` is absent (not prime / out of range).
→ `QueryService::LookupPrime` + `LookupPartitions`.

```lua
local pr = query.pget{ p = 11 }          -- pr.k == 3, #pr.partitions == 3
local q3 = query.pget{ p = 11, q_k = 3 } -- only edges with q == 3
```

### `query.kget(spec) -> { prime, ... }`
`spec.k` **required**; returns an **array of prime records ordered
monotonically by p**, for primes with `k == spec.k`, scanned within
`[init, end]` (strongly recommended for sparse high k), up to `limit`. Optional
`q_k`/`m_k`/`n_k` keep only primes having a matching partition.
→ `QueryService::ScanByK` (+ a per-hit partition fetch).

```lua
for _, pr in ipairs(query.kget{ k = 0, limit = 10 }) do print(pr.p) end
local big = query.kget{ k = 16, init = 1e11, ['end'] = 2e11, limit = 5 }
```
(`end` is a Lua keyword → write `['end']` or alias the param to `e`/`hi`.)

### `query.count(spec) -> { total=, k0= }`
Counts primes in `[init, end]` matching `spec` (e.g. a `k` filter). The obstruction
rate is `k0 / total`. → new `QueryService::CountRange` (sketched in the prior plan).

### `query.parts(spec) -> { edge, ... }`
Edge/partition view: rows `{p, m, n, q}` matching a filter — e.g.
`parts{ q_k = 3, init=.., ['end']=.. }` for every edge with ancestor `q=3` in a
window (the "who does q=3 generate" view). Ordered by p. → a partitions-table
scan (new QueryService method, mirrors ScanByK on the partitions table).

### Number-theory helpers (already linked: primecount / primesieve / FLINT)
- `query.pi(x) -> n` — π(x) (prime_rank of the largest prime ≤ x).
- `query.nth_prime(n) -> p` — the n-th prime (inverse of prime_rank).
- `query.next_prime(p) -> p'`, `query.prev_prime(p) -> p'` — neighbours
  (backs a "+/- step to the next prime and re-run" UX).
- `query.is_prime(n) -> bool`, `query.is_obstructed(p) -> bool` (k(p)==0).

### Catalog / dataset
- `query.extent() -> { max_p=, count= }` — dataset bounds (manifest aggregates,
  via the catalog — the `ui_iceberg` reads re-pointed onto the seam).
- `query.reload()` — re-read presets/config (the `pp.reload` the preset TODO
  references).

## QueryService additions this needs
- `CountRange(p_lo, p_hi) -> {total, k0}` (planned).
- `ScanPartitions(filter) -> edges` (partitions-table scan; reuse the SourceTableReader
  + Expressions filter path, on `partitions`).
- thin wrappers over the existing C number-theory core (`pp_prime_pi`,
  `pp_nth_prime`, `pp_next_prime`, `pp_is_prime_power_*`) exposed to Lua.

## Cancellation / progress
Interactive Lua calls run on the calling thread; long scans (`kget` with a wide
window) should accept the same `ScanControl` the TUI uses — exposed to Lua as an
optional `spec.on_progress` callback and cooperative interrupt via
`lua_sethook` (a future refinement; v1 can run to completion with a sane default
`limit`).

## Why this is the right seam
The functions are **field-name / record oriented** (matching the `c`-dispatch
model and the schema), bound to the catalog-validated `QueryService`. The same
module powers: preset `run = function(f) ... end` bodies, an interactive REPL
panel, and scripted batch queries — without the TUI re-implementing query logic.
