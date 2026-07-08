# Prime Generator + Lua Core

> 77 nodes · cohesion 0.06

## Key Concepts

- **core.c** (29 connections) — `native/src/core.c`
- **lua_query_module.cc** (24 connections) — `native/src/query/lua_query_module.cc`
- **lua_State** (22 connections) — `native/include/primeparts/query/lua_query_module.h`
- **pp_batch_result** (12 connections)
- **main()** (12 connections) — `native/tests/test_core.c`
- **process_prime()** (11 connections) — `native/src/core.c`
- **pp_process_rank_batch()** (9 connections) — `native/src/core.c`
- **materialize_bench.c** (8 connections) — `native/src/materialize_bench.c`
- **qs_upvalue()** (8 connections) — `native/src/query/lua_query_module.cc`
- **bench.c** (7 connections) — `native/src/bench.c`
- **count_prime()** (7 connections) — `native/src/core.c`
- **pp_count_rank_batch()** (7 connections) — `native/src/core.c`
- **pp_init()** (7 connections) — `native/src/core.c`
- **pp_is_prime_power_u64()** (7 connections) — `native/src/core.c`
- **prepare_result()** (7 connections) — `native/src/core.c`
- **core.h** (6 connections) — `native/include/primeparts/core.h`
- **main()** (6 connections) — `native/src/bench.c`
- **pp_batch_result_clear()** (6 connections) — `native/src/core.c`
- **main()** (6 connections) — `native/src/materialize_bench.c`
- **update_stats()** (6 connections) — `native/src/materialize_bench.c`
- **opt_int()** (6 connections) — `native/src/query/lua_query_module.cc`
- **opt_str()** (6 connections) — `native/src/query/lua_query_module.cc`
- **q_read()** (6 connections) — `native/src/query/lua_query_module.cc`
- **read_str_array()** (6 connections) — `native/src/query/lua_query_module.cc`
- **pp_batch_result_init()** (5 connections) — `native/src/core.c`
- *... and 52 more nodes in this community*

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (5 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (2 shared connections)
- [Batch Group Writer](Batch_Group_Writer.md) (1 shared connections)
- [Query Lua Module + REPL](Query_Lua_Module_%2B_REPL.md) (1 shared connections)
- [Source Table Reader](Source_Table_Reader.md) (1 shared connections)

## Source Files

- `native/include/primeparts/core.h`
- `native/include/primeparts/query/lua_query_module.h`
- `native/src/bench.c`
- `native/src/core.c`
- `native/src/generate.cc`
- `native/src/materialize_bench.c`
- `native/src/query/lua_query_module.cc`
- `native/tests/test_core.c`

## Audit Trail

- EXTRACTED: 163 (85%)
- INFERRED: 29 (15%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*