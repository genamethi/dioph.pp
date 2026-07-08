# Catalogd HTTP Server

> 25 nodes · cohesion 0.16

## Key Concepts

- **pp_catalogd.cc** (25 connections) — `native/src/catalog/pp_catalogd.cc`
- **RunCatalogd()** (9 connections) — `native/src/catalog/pp_catalogd.cc`
- **SendError()** (7 connections) — `native/src/catalog/pp_catalogd.cc`
- **SendIcebergError()** (7 connections) — `native/src/catalog/pp_catalogd.cc`
- **SendTableResult()** (7 connections) — `native/src/catalog/pp_catalogd.cc`
- **ParseBody()** (6 connections) — `native/src/catalog/pp_catalogd.cc`
- **TableResultBody()** (6 connections) — `native/src/catalog/pp_catalogd.cc`
- **json** (5 connections)
- **SendJson()** (5 connections) — `native/src/catalog/pp_catalogd.cc`
- **Response** (5 connections)
- **pp_catalogd_main.cc** (4 connections) — `native/src/catalog/pp_catalogd_main.cc`
- **ParseNamespace()** (4 connections) — `native/src/catalog/pp_catalogd.cc`
- **Table** (3 connections)
- **HttpStatusFor()** (3 connections) — `native/src/catalog/pp_catalogd.cc`
- **Result** (2 connections)
- **shared_ptr** (2 connections)
- **string** (2 connections)
- **main()** (2 connections) — `native/src/catalog/pp_catalogd_main.cc`
- **Usage()** (2 connections) — `native/src/catalog/pp_catalogd_main.cc`
- **E** (1 connections)
- **ErrorKind** (1 connections)
- **Namespace** (1 connections)
- **string_view** (1 connections)
- **HandleSignal()** (1 connections) — `native/src/catalog/pp_catalogd.cc`
- **Request** (1 connections)

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (4 shared connections)
- [Iceberg REST Catalog](Iceberg_REST_Catalog.md) (2 shared connections)
- [Covering Sieve](Covering_Sieve.md) (1 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (1 shared connections)
- [Row Delta / Deletes](Row_Delta_-_Deletes.md) (1 shared connections)

## Source Files

- `native/src/catalog/pp_catalogd.cc`
- `native/src/catalog/pp_catalogd_main.cc`

## Audit Trail

- EXTRACTED: 57 (98%)
- INFERRED: 1 (2%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*