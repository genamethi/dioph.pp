# Source Table Reader

> 55 nodes · cohesion 0.05

## Key Concepts

- **source_scan.cc** (28 connections) — `native/src/source_scan.cc`
- **SourceTableReader::Impl** (22 connections) — `native/src/source_scan.cc`
- **mutex_** (14 connections) — `native/src/catalog/pp_lmdb_store.cc`
- **SourceTableReader** (12 connections) — `native/include/primeparts/source_scan.h`
- **SourceFileInfo** (9 connections) — `native/include/primeparts/source_scan.h`
- **SourceTableReader::OpenMetadata()** (9 connections) — `native/src/source_scan.cc`
- **source_scan.h** (7 connections) — `native/include/primeparts/source_scan.h`
- **string** (5 connections)
- **SourceTableReader::Next()** (5 connections) — `native/src/source_scan.cc`
- **arrow_init.h** (4 connections) — `native/include/primeparts/common/arrow_init.h`
- **vector** (4 connections)
- **decode_int64_le()** (4 connections) — `native/src/source_scan.cc`
- **.EnsureTaskReader()** (4 connections) — `native/src/source_scan.cc`
- **.OpenTaskAtCursor()** (4 connections) — `native/src/source_scan.cc`
- **SourceTableReader::source_files()** (4 connections) — `native/src/source_scan.cc`
- **Schema** (3 connections)
- **shared_ptr** (3 connections)
- **unique_ptr** (3 connections)
- **Expression** (2 connections)
- **.SourceTableReader()** (2 connections) — `native/include/primeparts/source_scan.h`
- **SourceTableReader::SourceTableReader()** (2 connections) — `native/src/source_scan.cc`
- **FileScanTask** (1 connections)
- **FileScanTaskReader** (1 connections)
- **Raw-SQLite reader migration onto the catalog seam** (1 connections) — `markdown/data_eng/irc_catalog_design.md`
- **EnsureArrowRegistration()** (1 connections) — `native/include/primeparts/common/arrow_init.h`
- *... and 30 more nodes in this community*

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (4 shared connections)
- [Covering Sieve](Covering_Sieve.md) (2 shared connections)
- [Catalogd HTTP Server](Catalogd_HTTP_Server.md) (1 shared connections)
- [Row Delta / Deletes](Row_Delta_-_Deletes.md) (1 shared connections)

## Source Files

- `markdown/data_eng/irc_catalog_design.md`
- `native/include/primeparts/common/arrow_init.h`
- `native/include/primeparts/source_scan.h`
- `native/src/catalog/pp_lmdb_store.cc`
- `native/src/source_scan.cc`

## Audit Trail

- EXTRACTED: 84 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*