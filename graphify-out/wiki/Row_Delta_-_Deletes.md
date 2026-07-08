# Row Delta / Deletes

> 35 nodes · cohesion 0.08

## Key Concepts

- **RowDelta** (16 connections) — `native/include/primeparts/catalog/pp_row_delta.h`
- **pp_row_delta.cc** (11 connections) — `native/src/catalog/pp_row_delta.cc`
- **unordered_map** (9 connections)
- **RowDelta::Apply()** (8 connections) — `native/src/catalog/pp_row_delta.cc`
- **pp_row_delta.h** (6 connections) — `native/include/primeparts/catalog/pp_row_delta.h`
- **RowDelta::Make()** (6 connections) — `native/src/catalog/pp_row_delta.cc`
- **Snapshot** (5 connections)
- **unordered_set** (5 connections)
- **RowDelta::CleanUncommitted()** (4 connections) — `native/src/catalog/pp_row_delta.cc`
- **RowDelta::WriteNewDeleteManifests()** (4 connections) — `native/src/catalog/pp_row_delta.cc`
- **Result** (3 connections)
- **shared_ptr** (3 connections)
- **string** (3 connections)
- **RowDelta::Summary()** (3 connections) — `native/src/catalog/pp_row_delta.cc`
- **WriteNewDeleteManifests** (2 connections) — `native/include/primeparts/catalog/pp_row_delta.h`
- **ManifestFile** (2 connections)
- **Table** (2 connections)
- **vector** (2 connections)
- **RowDelta::operation()** (2 connections) — `native/src/catalog/pp_row_delta.cc`
- **RowDelta::RowDelta()** (2 connections) — `native/src/catalog/pp_row_delta.cc`
- **DataFileSet** (1 connections)
- **ManifestFile** (1 connections)
- **Apply** (1 connections) — `native/include/primeparts/catalog/pp_row_delta.h`
- **CleanUncommitted** (1 connections) — `native/include/primeparts/catalog/pp_row_delta.h`
- **has_new_files_** (1 connections) — `native/include/primeparts/catalog/pp_row_delta.h`
- *... and 10 more nodes in this community*

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (3 shared connections)

## Source Files

- `native/include/primeparts/catalog/pp_row_delta.h`
- `native/src/catalog/pp_row_delta.cc`

## Audit Trail

- EXTRACTED: 50 (98%)
- INFERRED: 1 (2%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*