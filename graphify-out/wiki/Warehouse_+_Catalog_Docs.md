# Warehouse + Catalog Docs

> 9 nodes · cohesion 0.33

## Key Concepts

- **primeparts Iceberg warehouse layout & schemas** (6 connections) — `markdown/data_eng/iceberg_data_setup.md`
- **Native IRC catalog (LMDB-backed SqlCatalog)** (5 connections) — `markdown/data_eng/irc_catalog_design.md`
- **dioph.pp research engine (project overview)** (5 connections) — `markdown/misc/README.md`
- **LmdbCatalogStore (CatalogStore over liblmdb)** (4 connections) — `markdown/data_eng/irc_catalog_design.md`
- **primeparts-generate producer pipeline** (4 connections) — `native/README.md`
- **CommitFiles seam (create-or-load + FastAppend)** (3 connections) — `markdown/data_eng/irc_catalog_design.md`
- **pp-catalogd native IRC server (cpp-httplib)** (3 connections) — `markdown/data_eng/irc_catalog_design.md`
- **HANDOFF living-state doc (catalog, tooling, roadmap)** (3 connections) — `markdown/misc/HANDOFF.md`
- **Commit ordering — LMDB CAS last, single-writer** (2 connections) — `markdown/data_eng/irc_catalog_design.md`

## Relationships

- [Query Layer + Build Docs](Query_Layer_%2B_Build_Docs.md) (2 shared connections)
- [Covering Filter + NT Core](Covering_Filter_%2B_NT_Core.md) (2 shared connections)
- [TUI + Lua Design Docs](TUI_%2B_Lua_Design_Docs.md) (1 shared connections)
- [Primes Table + Read Paths](Primes_Table_%2B_Read_Paths.md) (1 shared connections)
- [Source Table Reader](Source_Table_Reader.md) (1 shared connections)
- [LMDB Catalog Store](LMDB_Catalog_Store.md) (1 shared connections)
- [Catalogd HTTP Server](Catalogd_HTTP_Server.md) (1 shared connections)
- [Prime Obstruction Theory](Prime_Obstruction_Theory.md) (1 shared connections)
- [Parquet Data File Writer](Parquet_Data_File_Writer.md) (1 shared connections)

## Source Files

- `markdown/data_eng/iceberg_data_setup.md`
- `markdown/data_eng/irc_catalog_design.md`
- `markdown/misc/HANDOFF.md`
- `markdown/misc/README.md`
- `native/README.md`

## Audit Trail

- EXTRACTED: 20 (87%)
- INFERRED: 3 (13%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*