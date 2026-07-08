# Query Lua Module + REPL

> 8 nodes · cohesion 0.39

## Key Concepts

- **query reader module (pget/kget/hist/materialize/read + NT helpers)** (7 connections) — `markdown/api/lua.md`
- **pp_main.cc** (7 connections) — `native/src/query/pp_main.cc`
- **lua_query_smoke.cc** (4 connections) — `native/src/query/lua_query_smoke.cc`
- **main()** (4 connections) — `native/src/query/pp_main.cc`
- **ReportLuaError()** (4 connections) — `native/src/query/pp_main.cc`
- **RunRepl()** (4 connections) — `native/src/query/pp_main.cc`
- **Usage()** (2 connections) — `native/src/query/pp_main.cc`
- **main()** (1 connections) — `native/src/query/lua_query_smoke.cc`

## Relationships

- [Prime Generator + Lua Core](Prime_Generator_%2B_Lua_Core.md) (3 shared connections)
- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (2 shared connections)
- [Query Layer + Build Docs](Query_Layer_%2B_Build_Docs.md) (1 shared connections)
- [Primes Table + Read Paths](Primes_Table_%2B_Read_Paths.md) (1 shared connections)

## Source Files

- `markdown/api/lua.md`
- `native/src/query/lua_query_smoke.cc`
- `native/src/query/pp_main.cc`

## Audit Trail

- EXTRACTED: 18 (90%)
- INFERRED: 2 (10%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*