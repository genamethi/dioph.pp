# primeparts TUI — Application Design

**Status:** 2026-06-14, design lock in progress (user-driven). This is the spec
for the new notcurses TUI (`native/src/tui/`), replacing the scrapped
`tui_frontend.c`. The query backend it sits on is `QueryService`
(`markdown/arch/tui_query_design.md`). Built in C++ calling `QueryService`
directly (no C ABI layer for now; JSON only for cross-boundary serialization).

The guiding intent (user): **not** the old panel-soup. An extensible,
multi-**screen** app where queries are **presets** (Lua-configured later,
hardcoded now), values carry **type** so cross-query drill-down is
mathematically sane, and long operations are **cancellable** with **progress**.

> **Terminology (load-bearing):** UI surfaces are **screens** and **panels**.
> "**view**" / "**materialized view**" is reserved for the database sense (a
> future data option), never the UI.

---

## 1. Screens

A persistent top-level lets you switch between screens; `Ctrl+L` overlays the
error log from anywhere. (Screen-switch mechanism: see Open Questions.)

| Screen | Purpose | First cut |
|---|---|---|
| **Run Saved Query** | Pick a preset, set its variable fields, run, browse results. Two panels (query top / results bottom), `Tab` switches focus. | **BUILD** |
| **Make Query** | General/ad-hoc queries; where saving a query (to a preset) happens. | **STUB** |
| **Status** | Warehouse status (max_p, row counts, snapshots) — the `ui_iceberg` reads, re-pointed onto the catalog seam. | **STUB** |
| **Generate** | Run the generation binary; same idea as the old TUI's generate, new layout. Subprocess output fills the lower half, scrollable, 10 000-line cap. | **STUB** |
| **Config** | Show the resolved `conf` table and its file; `e` opens it in `$EDITOR` and reloads. | **BUILD** |
| **Error log** (`Ctrl+L`) | Scrollable error/event log overlay. | **BUILD (minimal)** |

---

## 2. Query Presets

A **preset** = a named, parameterized query. **Hardcoded in C++ now**, with a
provisional Lua proposal in a TODO at the execution seam. Lua is the **config
language** (`scripts/lua/`); reload presets in-app by key, and a Lua→C
`pp.reload()` re-reads externally-edited scripts.

A preset fixes some fields and leaves others **variable**. Editing a variable
field = a **modal** with one input box per variable field.

Every query (preset or ad-hoc) exposes these controls:
- **value predicate** on its value column: exact (`k == 16`, `p == X`) or a
  range (`k in [lo,hi]`, `p in [lo,hi]`) — see Open Q1.
- **p-scan window** `[p_lo, p_hi]` — bounds how far the scan runs. **Load-bearing:**
  e.g. `k = 16` does not occur in the first ~900M rows, and there is **no max_k
  statistic** in manifests/footers to prune on (we keep standard parquet footers,
  no custom metadata). So the user must bound the scan or it runs to EOF.
- **LIMIT** — max rows returned (early-stop).

First-cut presets:
- `by-k` — "k == {k}  in p∈[{p_lo},{p_hi}]  LIMIT {limit}" → `ScanByK` (general
  over k; k=0 is the obstructed case). Fields variable: `k`, `p_lo`, `p_hi`,
  `limit`. accepts `{k}`.
- `lookup` — "p == {p}" → `LookupPrime` (k + prime_rank). Field variable: `p`.
  accepts `{p, q_k}`.

Provisional Lua (TODO at the C++ exec block):
```lua
-- scripts/lua/presets.lua  (future; hardcoded in C++ for now)
preset("obstructed", {
  title  = "Obstructed primes",
  format = "k == {k}  p∈[{p_lo},{p_hi}]  LIMIT {limit}",
  accepts = "k",                       -- value type this preset consumes
  fields = { k = 0, p_lo = 0, p_hi = 0, limit = 10 },
  run    = function(f) return pp.scan_k(f.k, f.p_lo, f.p_hi, f.limit) end,
})
-- pp.reload() re-reads this dir; a key in-app triggers it.
```

---

## 3. Schema-field-name value dispatch (the `c` key)

Dispatch is purely **schema-field-name based** — no type/domain abstraction.
Each result cell knows the **schema field name** it came from (`p`, `k`, `q_k`,
`m_k`, `n_k`). Each query declares a set of **field names it accepts**. Pressing
`c` on a focused cell opens a dialogue of queries whose accept-set contains that
cell's field name, then runs the chosen one on that value. (That is "drill into a
result"; the p-lookup "detail" is just this on a `p` or `q_k` cell.)

Accept-sets are literal field-name sets:
- p-lookup accepts `{p, q_k}` — so a `q_k` cell can feed it (q_k values are
  primes, same column meaning as p). No "type" is inferred; the set just lists
  both field names.
- the k-query accepts `{k}`. **`k` is any int32** (k=0 is merely the obstructed
  case, not the query's identity) — the k-query is general over k.
- nothing accepts `m_k` / `n_k` yet, so `c` there shows "no queries accept m_k".

TUI fn `queries_accepting(field_name) -> [preset]` returns candidates.

---

## 4. Interaction model

- `+` / `-` — cycle through **any** option set (even binary toggles). The
  universal "change this option" key. (SPACE: unused for now.)
- `Tab` — switch focus between panels (e.g. query ↔ results).
- digits / `Backspace` — edit a field value (inside the field-edit modal).
- `Enter` — drill in / change screen / open detail (not "run-in-place").
- `c` — on a focused result cell: type-dispatched query menu (§3).
- `Esc` — back / close modal.
- `q` — quit, with `(y│N)` confirmation.
- `Ctrl+L` — error log overlay.
- `j`/`k` / arrows — move focus / scroll results.

Per notcurses-ux guardrails: one plane per widget, `> ` caret + bg-invert focus,
glyph-prefixed states (`[ok]`/`[!]`/`[..]`), ≤2 box nesting, **reserved bottom
status row**, scroll indicators on overflow, one `notcurses_render()` per frame.

---

## 5. Execution: cancellable, progress-tracked, threaded

- **Every query attempts progress.** The scan reports `(scanned, total)`
  (`SourceTableReader::total_records()` + a running scanned count); the UI shows
  a progress indicator.
- **Cancellation is clean and quick.** Queries run on a **worker thread**; the UI
  thread stays responsive, polls progress, and can set a cancel flag. The scan
  loop checks an `atomic<bool>* cancel` each batch and returns promptly.
- **Subprocesses** (generation binary): output streams into a plane occupying the
  **full lower half**, scrollable, **10 000-line ring buffer**.

`QueryService` gains a control struct (cancel flag + progress callback) and
p-range params; methods run synchronously but are driven from the worker thread.

---

## 6. Config

- Read-only view of the resolved `conf` table and the file it came from.
- `e` suspends notcurses, runs `$EDITOR` on that file, reloads, and reopens the
  editor on a parse or enum error.

---

## 7. Build order

1. **Run Saved Query screen** (core): two panels + `Tab`; the two presets; field-
   edit modal; `+/-` cycling; cancellable + progress + threaded query; `c`
   type-dispatch; `Ctrl+L` error log; `q` confirm-quit. Backed by extended
   `QueryService` (p-range, cancel, progress).
2. Screen framework + stubs for Make Query / Status / Generate / Config.
3. Flesh out Generate (subprocess lower-half), Config (save/load), Status
   (re-pointed `ui_iceberg`).
4. Lua presets (replace the hardcoded table; `pp.reload`).

---

## Resolved decisions (2026-06-14)

1. **Range = a p pushdown.** The value is **exact** (`k == K`, `p == X`); the
   **p-range `[p_lo, p_hi]` is a predicate pushdown on `p`** that bounds the scan
   (`And(Equal(k,K), GtEq(p,p_lo), LtEq(p,p_hi))`). This is what makes `k=16`
   tractable — bound the p-window or the scan runs to EOF (no `max_k` stat to
   prune on). Unbounded ends allowed (p_lo=0 / p_hi=0 → open).
2. **Screen nav = F-keys** with a labeled top bar (F1 Run Query · F2 Make Query ·
   F3 Status · F4 Generate · F5 Config). Collision-free vs in-screen letters
   (`c`, `q`, `j/k`). (`Tab` = panels within a screen; `Ctrl+L` = log.)
3. **Dispatch = field-name accept-sets** (§3): p-lookup `{p, q_k}`, k-query
   `{k}`; `m_k`/`n_k` unhandled. `k` general over int32.
