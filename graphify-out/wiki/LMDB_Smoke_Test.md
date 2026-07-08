# LMDB Smoke Test

> 15 nodes · cohesion 0.31

## Key Concepts

- **pp_lmdb_smoke.cc** (14 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **PartC()** (8 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **PartA()** (7 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **IsAlreadyExists()** (6 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **Ok()** (6 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **PartB()** (6 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **OkStatus()** (5 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **Check()** (4 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **main()** (4 connections) — `native/src/catalog/pp_lmdb_smoke.cc`
- **path** (3 connections)
- **expected** (2 connections)
- **Result** (2 connections)
- **T** (2 connections)
- **Error** (1 connections)
- **Status** (1 connections)

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [LMDB Catalog Store](LMDB_Catalog_Store.md) (2 shared connections)
- [Covering Sieve](Covering_Sieve.md) (1 shared connections)

## Source Files

- `native/src/catalog/pp_lmdb_smoke.cc`

## Audit Trail

- EXTRACTED: 37 (97%)
- INFERRED: 1 (3%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*