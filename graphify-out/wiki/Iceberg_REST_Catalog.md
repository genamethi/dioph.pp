# Iceberg REST Catalog

> 46 nodes · cohesion 0.11

## Key Concepts

- **pp_iceberg_rest.cc** (33 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **Catalog** (22 connections) — `native/include/primeparts/query/materialize.h`
- **CommitFiles()** (13 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **string** (12 connections)
- **PublishTable()** (12 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **generate_smoke.cc** (12 connections) — `native/src/generate_smoke.cc`
- **path** (10 connections)
- **shared_ptr** (9 connections)
- **MakeLocalCatalog()** (9 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **OpenCatalog()** (9 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **RunClient()** (9 connections) — `native/src/generate_smoke.cc`
- **MakeCatalog()** (8 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **MaterializeIntColumns()** (8 connections) — `native/src/query/materialize.cc`
- **RestOptions** (7 connections) — `native/include/primeparts/catalog/pp_iceberg_rest.h`
- **EnsureNamespace()** (7 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **DropTable()** (6 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **materialize.cc** (6 connections) — `native/src/query/materialize.cc`
- **LocalIO()** (5 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **TableMetadataPath()** (5 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **CountTable()** (5 connections) — `native/src/generate_smoke.cc`
- **LatestMetadataJson()** (4 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **MoveStagedFile()** (4 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **StagingDataDir()** (4 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- **string** (4 connections)
- **RestServerReachable()** (3 connections) — `native/src/catalog/pp_iceberg_rest.cc`
- *... and 21 more nodes in this community*

## Relationships

- [mdiff Table Builder](mdiff_Table_Builder.md) (3 shared connections)
- [LMDB Catalog Store](LMDB_Catalog_Store.md) (2 shared connections)
- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [Source Table Reader](Source_Table_Reader.md) (1 shared connections)
- [Row Delta / Deletes](Row_Delta_-_Deletes.md) (1 shared connections)
- [Covering Sieve](Covering_Sieve.md) (1 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (1 shared connections)
- [Catalogd HTTP Server](Catalogd_HTTP_Server.md) (1 shared connections)
- [Query Literal Bounds](Query_Literal_Bounds.md) (1 shared connections)
- [Writer Config + Partition Spec](Writer_Config_%2B_Partition_Spec.md) (1 shared connections)

## Source Files

- `native/include/primeparts/catalog/pp_iceberg_rest.h`
- `native/include/primeparts/query/materialize.h`
- `native/src/catalog/pp_iceberg_rest.cc`
- `native/src/generate_smoke.cc`
- `native/src/query/materialize.cc`

## Audit Trail

- EXTRACTED: 122 (98%)
- INFERRED: 2 (2%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*