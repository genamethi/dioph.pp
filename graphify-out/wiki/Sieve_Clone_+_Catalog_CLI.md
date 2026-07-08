# Sieve Clone + Catalog CLI

> 23 nodes · cohesion 0.13

## Key Concepts

- **pp_sieve_clone.cc** (11 connections) — `native/src/catalog/pp_sieve_clone.cc`
- **pp_catalog_main.cc** (9 connections) — `native/src/catalog/pp_catalog_main.cc`
- **Args** (7 connections) — `native/src/catalog/pp_catalog_main.cc`
- **RunCloneSieve()** (7 connections) — `native/src/catalog/pp_sieve_clone.cc`
- **CloneSieveOptions** (6 connections) — `native/include/primeparts/catalog/pp_sieve_clone.h`
- **ScanCounts()** (5 connections) — `native/src/catalog/pp_sieve_clone.cc`
- **pp_sieve_clone.h** (4 connections) — `native/include/primeparts/catalog/pp_sieve_clone.h`
- **main()** (4 connections) — `native/src/catalog/pp_catalog_main.cc`
- **ParseArgs()** (4 connections) — `native/src/catalog/pp_catalog_main.cc`
- **RunRegister()** (4 connections) — `native/src/catalog/pp_catalog_main.cc`
- **Usage()** (3 connections) — `native/src/catalog/pp_catalog_main.cc`
- **FindJsonInt()** (3 connections) — `native/src/catalog/pp_sieve_clone.cc`
- **string** (2 connections)
- **string** (2 connections)
- **Table** (2 connections)
- **dest_table** (1 connections) — `native/include/primeparts/catalog/pp_sieve_clone.h`
- **source_table** (1 connections) — `native/include/primeparts/catalog/pp_sieve_clone.h`
- **warehouse** (1 connections) — `native/include/primeparts/catalog/pp_sieve_clone.h`
- **string** (1 connections)
- **clone_sieve** (1 connections) — `native/src/catalog/pp_catalog_main.cc`
- **register_tables** (1 connections) — `native/src/catalog/pp_catalog_main.cc`
- **warehouse** (1 connections) — `native/src/catalog/pp_catalog_main.cc`
- **shared_ptr** (1 connections)

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (5 shared connections)
- [Iceberg REST Catalog](Iceberg_REST_Catalog.md) (4 shared connections)
- [Covering Sieve](Covering_Sieve.md) (2 shared connections)
- [Row Delta / Deletes](Row_Delta_-_Deletes.md) (1 shared connections)
- [mdiff Table Builder](mdiff_Table_Builder.md) (1 shared connections)

## Source Files

- `native/include/primeparts/catalog/pp_sieve_clone.h`
- `native/src/catalog/pp_catalog_main.cc`
- `native/src/catalog/pp_sieve_clone.cc`

## Audit Trail

- EXTRACTED: 44 (94%)
- INFERRED: 3 (6%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*