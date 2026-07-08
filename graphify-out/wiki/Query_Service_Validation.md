# Query Service Validation

> 10 nodes · cohesion 0.33

## Key Concepts

- **query_service.cc** (30 connections) — `native/src/query/query_service.cc`
- **tui_app.h** (17 connections) — `native/include/primeparts/tui/tui_app.h`
- **thread** (14 connections)
- **atomic** (11 connections)
- **query_service_smoke.cc** (6 connections) — `native/src/query/query_service_smoke.cc`
- **QueryService::ValidatePreset()** (3 connections) — `native/src/query/query_service.cc`
- **secs_since()** (3 connections) — `native/src/query/query_service_smoke.cc`
- **main()** (2 connections) — `native/src/query/query_service_smoke.cc`
- **ncplane** (1 connections) — `native/include/primeparts/tui/tui_app.h`
- **time_point** (1 connections)

## Relationships

- [Query Service Scan Control](Query_Service_Scan_Control.md) (9 shared connections)
- [Query/Preset Headers](Query-Preset_Headers.md) (5 shared connections)
- [TUI Query View](TUI_Query_View.md) (3 shared connections)
- [Source Table Reader](Source_Table_Reader.md) (3 shared connections)
- [Query Service Impl](Query_Service_Impl.md) (3 shared connections)
- [Iceberg REST Catalog](Iceberg_REST_Catalog.md) (2 shared connections)
- [Generate CLI Args](Generate_CLI_Args.md) (1 shared connections)
- [TUI App State](TUI_App_State.md) (1 shared connections)
- [Query Literal Bounds](Query_Literal_Bounds.md) (1 shared connections)
- [Row Delta / Deletes](Row_Delta_-_Deletes.md) (1 shared connections)
- [Lua Query Presets](Lua_Query_Presets.md) (1 shared connections)

## Source Files

- `native/include/primeparts/tui/tui_app.h`
- `native/src/query/query_service.cc`
- `native/src/query/query_service_smoke.cc`

## Audit Trail

- EXTRACTED: 45 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*