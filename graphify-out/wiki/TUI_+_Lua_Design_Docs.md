# TUI + Lua Design Docs

> 10 nodes · cohesion 0.22

## Key Concepts

- **primeparts TUI application design (screens, presets, dispatch)** (6 connections) — `markdown/arch/tui_app_design.md`
- **Lua API (implemented) — preset DSL + query reader module** (5 connections) — `markdown/api/lua.md`
- **query Lua module proposal (design, not-yet-built extensions)** (4 connections) — `markdown/arch/lua_query_api.md`
- **Preset / config DSL (query(id,spec), config(tbl))** (2 connections) — `markdown/api/lua.md`
- **query Lua module proposal (api copy)** (2 connections) — `markdown/api/lua_query_api.md`
- **Schema-field-name value dispatch (the 'c' key, accept-sets)** (2 connections) — `markdown/arch/tui_app_design.md`
- **primeparts.partitions table (p, m_k, n_k, q_k)** (2 connections) — `markdown/data_eng/iceberg_data_setup.md`
- **pp Lua shell (binds query module to live warehouse)** (1 connections) — `markdown/api/lua.md`
- **Cancellable, progress-tracked, threaded execution** (1 connections) — `markdown/arch/tui_app_design.md`
- **primeparts TUI application design (tui copy)** (1 connections) — `markdown/tui/tui_app_design.md`

## Relationships

- [Query Layer + Build Docs](Query_Layer_%2B_Build_Docs.md) (2 shared connections)
- [Query Lua Module + REPL](Query_Lua_Module_%2B_REPL.md) (1 shared connections)
- [Lua Query Presets](Lua_Query_Presets.md) (1 shared connections)
- [Primes Table + Read Paths](Primes_Table_%2B_Read_Paths.md) (1 shared connections)

## Source Files

- `markdown/api/lua.md`
- `markdown/api/lua_query_api.md`
- `markdown/arch/lua_query_api.md`
- `markdown/arch/tui_app_design.md`
- `markdown/data_eng/iceberg_data_setup.md`
- `markdown/tui/tui_app_design.md`

## Audit Trail

- EXTRACTED: 10 (67%)
- INFERRED: 5 (33%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*