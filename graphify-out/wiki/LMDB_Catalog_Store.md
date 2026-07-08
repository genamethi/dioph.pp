# LMDB Catalog Store

> 52 nodes · cohesion 0.12

## Key Concepts

- **LmdbCatalogStore** (34 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **pp_lmdb_store.cc** (21 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **string_view** (17 connections)
- **ToVal()** (15 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **Result** (12 connections)
- **string** (12 connections)
- **JoinKey()** (11 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.GetNamespaceProperties()** (10 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.GetTableMetadataLocation()** (10 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **ToView()** (10 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **WithWrite()** (10 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.ListTableNames()** (9 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.RenameNamespace()** (9 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.UpdateTableMetadataLocation()** (8 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **MakeLmdbCatalogStore()** (8 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.DeleteNamespace()** (7 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.InsertNamespaceProperty()** (7 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.ListNamespaceNames()** (7 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **NsPrefix()** (7 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **WithRead()** (7 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.DeleteNamespaceProperty()** (6 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.DeleteTable()** (6 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.InsertTable()** (6 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.RenameTable()** (6 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **.TableExists()** (6 connections) — `native/src/catalog/pp_lmdb_store.cc`
- *... and 27 more nodes in this community*

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [Source Table Reader](Source_Table_Reader.md) (2 shared connections)
- [Covering Sieve](Covering_Sieve.md) (1 shared connections)
- [mdiff Table Builder](mdiff_Table_Builder.md) (1 shared connections)

## Source Files

- `native/src/catalog/pp_lmdb_store.cc`

## Audit Trail

- EXTRACTED: 161 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*