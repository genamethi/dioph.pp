# Lua API

The native tooling embeds Lua 5.5 in two distinct places. Both happen to expose
a global named `query`, but they live in **separate `lua_State`s** and mean
different things:

1. **Preset DSL** — *declarative* files the TUI loads
   (`scripts/lua/queries.lua`). Here `query(...)` is a function that
   *registers* presets. Implemented in `native/src/tui/lua_presets.cc`.
2. **The `query` reader module** — a *table* of callable functions
   (number theory + data lookups) installed into a `lua_State` bound to a
   `QueryService` (the TUI's query execution, preset `run` bodies, a future REPL
   or scripted `pp` shell). Implemented in
   `native/src/query/lua_query_module.cc`.

This is a small, evolving surface. Design rationale and the planned-but-unbuilt
extensions live in [`../arch/lua_query_api.md`](../arch/lua_query_api.md); this
document describes only what is **actually implemented**.

---

## 1. Preset DSL (TUI-loaded files)

The TUI owns the `lua_State`, sources these files, and collects what the
functions register. They are plain Lua, so comments/loops/locals are fine.

### `query(id, spec)` — declare a saved query preset

```lua
query("by-k", {
  desc    = "primes where k == {k}, p in [{p_lo},{p_hi}], LIMIT {limit}",
  kind    = "by_k",                       -- "by_k" | "lookup"
  fields  = { k = 0, p_lo = 0, p_hi = 0, limit = 10 },  -- name = integer default
  accepts = { "k" },                      -- schema field names this preset filters on
  target  = "k",                          -- the primary schema field
})

query("lookup", {
  desc   = "prime p == {p}  ->  k + partitions",
  kind   = "lookup",
  fields = { p = 11 },
  accepts = { "p", "q_k" },
  target  = "p",
})
```

| key | type | meaning |
|---|---|---|
| `id` (arg 1) | string | preset identifier |
| `desc` | string | human description; `{field}` placeholders are filled from `fields` |
| `kind` | string | `"by_k"` → `QueryService::ScanByK`; `"lookup"` → `LookupPrime` + partitions |
| `fields` | table | `name = integer` map of form fields and their defaults |
| `accepts` | array of strings | schema field names the preset filters on (`p`, `k`, `m_k`, `n_k`, `prime_rank`) |
| `target` | string | the primary schema field |

Presets are validated by the reader (`QueryService::ValidatePreset`) against the
catalog schema. Field values are integers. Calling `query(...)` multiple times
registers multiple presets.

---

## 2. The `query` reader module (`RegisterQueryModule`)

```cpp
// native/src/query/lua_query_module.cc
namespace primeparts::query {
void RegisterQueryModule(lua_State* L, QueryService* qs);  // installs global table `query`
}
```

Each function is a C closure carrying the `QueryService*` as an upvalue. The
number-theory functions need no catalog (FLINT / primecount / primesieve are set
up once via `pp_init`); the data functions return empty/`nil` if no
`QueryService` is bound.

### Number theory (no catalog needed)

| function | returns | backing |
|---|---|---|
| `query.pi(x)` | integer — π(x), the prime-counting function | `pp_prime_pi` |
| `query.nth_prime(n)` | integer — the n-th prime | `pp_nth_prime` |
| `query.next_prime(p)` | integer — next prime > p | `pp_next_prime` |
| `query.prev_prime(p)` | integer — previous prime < p | `pp_previous_prime` |
| `query.is_prime(n)` | boolean | `pp_is_prime_power_u64` (exp == 1) |
| `query.is_prime_power(n)` | `base, exp` (two values) or `nil` | `pp_is_prime_power_u64` |

```lua
print(query.pi(100))            -- 25
print(query.nth_prime(25))      -- 97
print(query.next_prime(11))     -- 13
print(query.is_prime(91))       -- false
local base, exp = query.is_prime_power(27)  -- 3, 3   (nil if not a prime power)
```

### Data lookups (catalog-backed)

#### `query.pget{ p=, [q_k=], [m_k=], [n_k=] }` → prime record | `nil`

`p` is **required**. Returns the prime record at `p` with all its partitions
joined in; `q_k`/`m_k`/`n_k` filter the returned `partitions`. Returns `nil` if
`p` is absent (not prime / out of range) — a missing prime is a normal empty
result, **not** an error. → `QueryService::LookupPrime` + `LookupPartitions`.

```lua
local pr = query.pget{ p = 11 }
-- pr = { p = 11, k = 3, prime_rank = 5,
--        partitions = { {m=1,n=2,q=3}, {m=2,n=1,q=7}, {m=3,n=1,q=3} } }
local q3 = query.pget{ p = 11, q_k = 3 }   -- same record, partitions filtered to q == 3
```

#### `query.kget{ k=, [init=], [end=] | [hi=], [limit=10] }` → array of prime records

`k` is **required**. Returns an array (ordered by `p`) of `{p, k, prime_rank}`
for primes with `k == spec.k`, scanned within `[init, end]` (recommended for
sparse high `k`), up to `limit` (default 10). → `QueryService::ScanByK`.

```lua
for _, pr in ipairs(query.kget{ k = 0, limit = 10 }) do print(pr.p) end
local big = query.kget{ k = 16, init = 1e11, hi = 2e11, limit = 5 }
```

> `end` is a Lua keyword. Write `['end'] = ...` or use the `hi` alias (`kget`
> accepts both; `hi` is used only if `end` is absent/0).

> Unlike `pget`, `kget` records carry **no** `partitions` array — just `p`, `k`,
> `prime_rank`. Fetch partitions per hit with `pget{p=...}` if needed.

#### `query.hist{ col=, [table="primes"], [init=], [end=] | [hi=], [threads=] }` → histogram

General group-by-value count: scans `table` and returns, ordered by value, one
row `{ [col]=value, count=n }` per distinct value of the **integer** column
`col`. The optional p-window `[init, end]` is row-accurate. `threads` (<=0 =
auto) drives sharded parallel readers. → `QueryService::GroupCount`.

Intended for **low-cardinality** columns (`k`, `m_k`, `n_k`, …); grouping by a
high-cardinality column (`p`) would build a huge table.

```lua
-- full per-k distribution over primeparts.primes (the obstruction spectrum)
for _, r in ipairs(query.hist{ col = "k" }) do print(r.k, r.count) end
-- windowed:
local h = query.hist{ col = "k", init = 3, hi = 1e6 }   -- row-accurate window
```

> Same `end`/`hi` keyword caveat as `kget`. The value field is named after `col`
> (e.g. `r.k` when `col="k"`), alongside `r.count`.

#### `query.materialize{ name=, cols={...}, rows={...} }` → metadata location

Cache an in-memory integer result (e.g. a `query.hist` output) as the
unpartitioned Iceberg MV `primeparts.<name>` (**replace** semantics — refreshes
on re-run). `cols` lists the integer fields to pull from each row of `rows`. →
`QueryService::Materialize`.

#### `query.read{ table=, [cols={...}], [limit=] }` → array of rows

Read a (small) integer table/MV back. Empty `cols` = every int column.
→ `QueryService::ReadTable`. Together with `materialize` this is the MV cache
lifecycle.

```lua
-- cache the per-k distribution, then read it back with no rescan:
query.materialize{ name = "k_freq", cols = {"k","count"}, rows = query.hist{col="k"} }
for _, r in ipairs(query.read{ table = "k_freq" }) do print(r.k, r.count) end
```

#### `query.extent{ [table="primes"], [key_max=true] }` → dataset bounds

Snapshot-level bounds for a table — no row scan; everything comes from the
Iceberg manifest/snapshot summary plus per-file upper bounds. Callable bare
(`query.extent()`) for the `primes` default. → `QueryService::Extent`.

```lua
local x = query.extent()
print(x.count, x.max_p)   --> 102120000000   2821481272793
```

Returned fields (any the snapshot doesn't report are **nil**, not `-1`):

| field | meaning |
|---|---|
| `table` | table name echoed back |
| `count` | total records in the current snapshot |
| `data_files`, `file_bytes` | current-snapshot totals |
| `snapshots`, `snapshot_id`, `sequence` | snapshot history / current pointer |
| `key`, `key_max` | sort-key name and its max, from data-file upper bounds |
| `max_p` | alias of `key_max` when `key == "p"` |

`key_max=false` skips the scan-plan pass that reads per-file upper bounds — the
rest of the record is unaffected. Failure is the Lua idiom `nil, "message"`
(e.g. a table that doesn't exist), not an error.

> **Cached MVs** (warehouse `primeparts.*`): `k_freq` (k,count) and `r_freq`
> (r,count) hold the full-census k- and r=⌊log₂p⌋−k distributions — read them
> instead of rescanning 21.7 B rows.

---

## 3. Record shapes

```lua
-- prime record (from pget; partitions empty for k=0)
prime = {
  p = 11, k = 3, prime_rank = 5,
  partitions = { {m=1, n=2, q=3}, {m=2, n=1, q=7}, {m=3, n=1, q=3} },
}
-- kget hit (no partitions)
hit = { p = 11, k = 3, prime_rank = 5 }
```

For a `k=0` prime, `partitions = {}`.

---

## Not yet implemented

Sketched in [`../arch/lua_query_api.md`](../arch/lua_query_api.md) but **not
built**: `query.parts`, `query.reload`, and the `on_progress` /
cooperative-cancellation hooks. `query.count` is subsumed by the more general
`query.hist` (above); `query.materialize`/`query.read` cover the MV cache. `is_obstructed(p)` is not a distinct function — use
`query.pget{p=p}.k == 0` (or `kget{k=0,...}`).

## Running Lua against the warehouse — the `pp` shell

`pp` (`native/build/pp`) binds this `query` module to a live warehouse and runs
Lua — so any query is a script, not a new binary:

```sh
pp -e 'for _,r in ipairs(query.hist{col="k"}) do print(r.k, r.count) end'
pp run hist.lua                 # run a script file
pp                              # interactive REPL  (\q to quit)
pp --warehouse DIR ...          # else $PRIMEPARTS_WAREHOUSE_ROOT / staging default
```

## Building / testing the Lua path

`primeparts-tui` and the Lua smokes link embedded Lua (notcurses-core from the
configure prefix, Lua 5.5 from the system). They build as part of `make all`
(TUI) and `make smoke` (`primeparts-lua-presets-smoke`,
`primeparts-lua-query-smoke`). See `BUILD.md`.
