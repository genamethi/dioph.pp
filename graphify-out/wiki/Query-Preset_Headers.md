# Query/Preset Headers

> 16 nodes · cohesion 0.21

## Key Concepts

- **string** (50 connections)
- **vector** (36 connections)
- **query_service.h** (12 connections) — `native/include/primeparts/query/query_service.h`
- **lua_presets.h** (8 connections) — `native/include/primeparts/tui/lua_presets.h`
- **CatalogdOptions** (6 connections) — `native/include/primeparts/catalog/pp_catalogd.h`
- **query_preset.h** (6 connections) — `native/include/primeparts/query/query_preset.h`
- **materialize.h** (5 connections) — `native/include/primeparts/query/materialize.h`
- **lua_presets_smoke.cc** (5 connections) — `native/src/tui/lua_presets_smoke.cc`
- **mersenne_sidecar.cc** (4 connections) — `native/src/mersenne_sidecar.cc`
- **pp_iceberg_rest.h** (3 connections) — `native/include/primeparts/catalog/pp_iceberg_rest.h`
- **pp_catalogd.h** (2 connections) — `native/include/primeparts/catalog/pp_catalogd.h`
- **host** (1 connections) — `native/include/primeparts/catalog/pp_catalogd.h`
- **port** (1 connections) — `native/include/primeparts/catalog/pp_catalogd.h`
- **warehouse** (1 connections) — `native/include/primeparts/catalog/pp_catalogd.h`
- **pp_lmdb_store.h** (1 connections) — `native/include/primeparts/catalog/pp_lmdb_store.h`
- **main()** (1 connections) — `native/src/mersenne_sidecar.cc`

## Relationships

- [Lua Query Presets](Lua_Query_Presets.md) (5 shared connections)
- [Iceberg REST Catalog](Iceberg_REST_Catalog.md) (2 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (2 shared connections)
- [Query Service Scan Control](Query_Service_Scan_Control.md) (2 shared connections)
- [Group Count Row](Group_Count_Row.md) (1 shared connections)
- [Partition Tuple Schema](Partition_Tuple_Schema.md) (1 shared connections)
- [Prime Info Schema](Prime_Info_Schema.md) (1 shared connections)
- [Query Service Facade](Query_Service_Facade.md) (1 shared connections)
- [Scan Hit Row](Scan_Hit_Row.md) (1 shared connections)
- [Table Extent Metadata](Table_Extent_Metadata.md) (1 shared connections)
- [Writer Config + Partition Spec](Writer_Config_%2B_Partition_Spec.md) (1 shared connections)

## Source Files

- `native/include/primeparts/catalog/pp_catalogd.h`
- `native/include/primeparts/catalog/pp_iceberg_rest.h`
- `native/include/primeparts/catalog/pp_lmdb_store.h`
- `native/include/primeparts/query/materialize.h`
- `native/include/primeparts/query/query_preset.h`
- `native/include/primeparts/query/query_service.h`
- `native/include/primeparts/tui/lua_presets.h`
- `native/src/mersenne_sidecar.cc`
- `native/src/tui/lua_presets_smoke.cc`

## Audit Trail

- EXTRACTED: 43 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*