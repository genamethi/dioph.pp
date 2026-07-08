# LmdbCatalogStore

> God node · 34 connections · `native/src/catalog/pp_lmdb_store.cc`

**Community:** [LMDB Catalog Store](LMDB_Catalog_Store.md)

## Connections by Relation

### defines
- mutex_ `EXTRACTED`
- active_txn_ `EXTRACTED`
- catalog_name_ `EXTRACTED`
- dbi_nsprops_ `EXTRACTED`
- dbi_tables_ `EXTRACTED`
- env_ `EXTRACTED`
- map_size_ `EXTRACTED`
- path_ `EXTRACTED`

### inherits
- CatalogStore `EXTRACTED`

### method
- .GetNamespaceProperties() `EXTRACTED`
- .GetTableMetadataLocation() `EXTRACTED`
- .ListTableNames() `EXTRACTED`
- .RenameNamespace() `EXTRACTED`
- .UpdateTableMetadataLocation() `EXTRACTED`
- .DeleteNamespace() `EXTRACTED`
- .InsertNamespaceProperty() `EXTRACTED`
- .ListNamespaceNames() `EXTRACTED`
- .DeleteNamespaceProperty() `EXTRACTED`
- .DeleteTable() `EXTRACTED`
- .InsertTable() `EXTRACTED`
- .RenameTable() `EXTRACTED`
- .TableExists() `EXTRACTED`
- .LmdbCatalogStore() `EXTRACTED`
- .Initialize() `EXTRACTED`
- .RunInTransaction() `EXTRACTED`

### references
- string `EXTRACTED`
- path `EXTRACTED`
- size_t `EXTRACTED`
- MDB_dbi `EXTRACTED`
- MDB_env `EXTRACTED`
- MDB_txn `EXTRACTED`
- recursive_mutex `EXTRACTED`

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*