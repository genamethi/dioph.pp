# Query Service Impl

> 11 nodes · cohesion 0.18

## Key Concepts

- **QueryService::Impl** (11 connections) — `native/src/query/query_service.cc`
- **QueryService::Open()** (5 connections) — `native/src/query/query_service.cc`
- **path** (3 connections)
- **unique_ptr** (2 connections)
- **QueryService::QueryService()** (2 connections) — `native/src/query/query_service.cc`
- **QueryService** (1 connections)
- **shared_ptr** (1 connections)
- **catalog** (1 connections) — `native/src/query/query_service.cc`
- **schema_fields** (1 connections) — `native/src/query/query_service.cc`
- **schema_loaded** (1 connections) — `native/src/query/query_service.cc`
- **warehouse** (1 connections) — `native/src/query/query_service.cc`

## Relationships

- [Query Service Scan Control](Query_Service_Scan_Control.md) (4 shared connections)
- [Iceberg REST Catalog](Iceberg_REST_Catalog.md) (1 shared connections)

## Source Files

- `native/src/query/query_service.cc`

## Audit Trail

- EXTRACTED: 15 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*