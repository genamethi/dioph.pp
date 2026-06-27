# Lua API

The native tooling embeds Lua 5.5 in two distinct places. Both happen to expose
a global named `query`, but they live in **separate `lua_State`s** and mean
different things:

1. **Preset / config DSL** — *declarative* files the TUI loads
   (`scripts/lua/queries.lua` and a config file). Here `query(...)` and
   `config(...)` are functions that *register* presets / settings.
   Implemented in `native/src/tui/lua_presets.cc`.
2. **The `query` reader module** — a *table* of callable functions
   (number theory + data lookups) installed into a `lua_State` bound to a
   `QueryService` (the TUI's query execution, preset `run` bodies, a future REPL
   or scripted `pp` shell). Implemented in
   `native/src/query/lua_query_module.cc`.

This is a small, evolving surface. Design rationale and the planned-but-unbuilt
extensions live in [`../arch/lua_query_api.md`](../arch/lua_query_api.md); this
document describes only what is **actually implemented**.

---

## 1. Preset & config DSL (TUI-loaded files)

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
| `accepts` | array of strings | schema field names the preset filters on (`p`, `k`, `q_k`, `m_k`, `n_k`, `prime_rank`) |
| `target` | string | the primary schema field |

Presets are validated by the reader (`QueryService::ValidatePreset`) against the
catalog schema. Field values are integers. Calling `query(...)` multiple times
registers multiple presets.

### `config(tbl)` — settings

```lua
config({ log_limit = 500, log_format = "json", autosave = true })
```

`config(tbl)` flattens one table of `key = value` pairs into the config sink;
values may be **boolean, number, or string**. Multiple `config(...)` calls merge,
**last value wins per key**. The TUI defines which keys it reads — currently
`log_limit` (int), `log_format` (string, e.g. `"json"`), and `autosave` (bool).
The TUI can serialize the current config back out to a Lua file (round-trips
through this same shape).

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
built**: `query.parts`, `query.extent`, `query.reload`, and the `on_progress` /
cooperative-cancellation hooks. `query.count` is subsumed by the more general
`query.hist` (above). `is_obstructed(p)` is not a distinct function — use
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
