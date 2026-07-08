# Catalogd Smoke Test

> 15 nodes · cohesion 0.26

## Key Concepts

- **pp_catalogd_smoke.cc** (15 connections) — `native/src/catalog/pp_catalogd_smoke.cc`
- **WriteDataFile()** (8 connections) — `native/src/catalog/pp_catalogd_smoke.cc`
- **RunClient()** (7 connections) — `native/src/catalog/pp_catalogd_smoke.cc`
- **MakePrimesBatch()** (6 connections) — `native/src/catalog/pp_catalogd_smoke.cc`
- **FinishOrDie()** (5 connections) — `native/src/catalog/pp_catalogd_smoke.cc`
- **string** (4 connections)
- **shared_ptr** (3 connections)
- **Check()** (3 connections) — `native/src/catalog/pp_catalogd_smoke.cc`
- **main()** (3 connections) — `native/src/catalog/pp_catalogd_smoke.cc`
- **path** (2 connections)
- **Schema** (2 connections)
- **WaitForServer()** (2 connections) — `native/src/catalog/pp_catalogd_smoke.cc`
- **Array** (1 connections)
- **RecordBatch** (1 connections)
- **T** (1 connections)

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (1 shared connections)
- [Catalogd HTTP Server](Catalogd_HTTP_Server.md) (1 shared connections)
- [Iceberg REST Catalog](Iceberg_REST_Catalog.md) (1 shared connections)
- [Iceberg Schemas + Mersenne](Iceberg_Schemas_%2B_Mersenne.md) (1 shared connections)
- [Writer Config + Partition Spec](Writer_Config_%2B_Partition_Spec.md) (1 shared connections)
- [mdiff Table Builder](mdiff_Table_Builder.md) (1 shared connections)
- [Written File Metadata](Written_File_Metadata.md) (1 shared connections)

## Source Files

- `native/src/catalog/pp_catalogd_smoke.cc`

## Audit Trail

- EXTRACTED: 35 (97%)
- INFERRED: 1 (3%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*