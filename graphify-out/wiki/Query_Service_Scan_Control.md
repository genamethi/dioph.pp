# Query Service Scan Control

> 21 nodes · cohesion 0.18

## Key Concepts

- **string** (12 connections)
- **ScanControl** (10 connections) — `native/include/primeparts/query/query_service.h`
- **TableRows** (10 connections) — `native/include/primeparts/query/query_service.h`
- **.ResolveMeta()** (8 connections) — `native/src/query/query_service.cc`
- **vector** (7 connections)
- **QueryService::GroupCount()** (7 connections) — `native/src/query/query_service.cc`
- **QueryService::LookupPartitions()** (7 connections) — `native/src/query/query_service.cc`
- **QueryService::ScanByK()** (7 connections) — `native/src/query/query_service.cc`
- **QueryService::LookupPrime()** (6 connections) — `native/src/query/query_service.cc`
- **QueryService::ReadTable()** (5 connections) — `native/src/query/query_service.cc`
- **QueryService::Extent()** (4 connections) — `native/src/query/query_service.cc`
- **QueryService::ListTables()** (4 connections) — `native/src/query/query_service.cc`
- **QueryService::Materialize()** (4 connections) — `native/src/query/query_service.cc`
- **string** (2 connections)
- **function** (1 connections)
- **vector** (1 connections)
- **cancel** (1 connections) — `native/include/primeparts/query/query_service.h`
- **progress** (1 connections) — `native/include/primeparts/query/query_service.h`
- **cols** (1 connections) — `native/include/primeparts/query/query_service.h`
- **rows** (1 connections) — `native/include/primeparts/query/query_service.h`
- **optional** (1 connections)

## Relationships

- [Query Service Validation](Query_Service_Validation.md) (1 shared connections)
- [Table Extent Metadata](Table_Extent_Metadata.md) (1 shared connections)
- [Group Count Row](Group_Count_Row.md) (1 shared connections)
- [Query Service Impl](Query_Service_Impl.md) (1 shared connections)
- [Partition Tuple Schema](Partition_Tuple_Schema.md) (1 shared connections)
- [Prime Info Schema](Prime_Info_Schema.md) (1 shared connections)
- [Iceberg REST Catalog](Iceberg_REST_Catalog.md) (1 shared connections)
- [Scan Hit Row](Scan_Hit_Row.md) (1 shared connections)

## Source Files

- `native/include/primeparts/query/query_service.h`
- `native/src/query/query_service.cc`

## Audit Trail

- EXTRACTED: 41 (89%)
- INFERRED: 5 (11%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*