# Catalog Seam Design Docs

> 23 nodes · cohesion 0.10

## Key Concepts

- **LmdbCatalogStore** (6 connections) — `native/src/catalog/README.md`
- **MakeLocalCatalog** (6 connections) — `native/src/catalog/README.md`
- **iceberg::sql::SqlCatalog** (4 connections) — `native/src/catalog/README.md`
- **The Catalog Seam** (3 connections) — `native/src/catalog/README.md`
- **pp-catalogd (Phase 2 Native IRC HTTP Server)** (3 connections) — `native/src/catalog/README.md`
- **pp_iceberg_rest (Catalog Construction + Shared Helpers)** (3 connections) — `native/src/catalog/README.md`
- **pp_row_delta (Position-Delete SnapshotUpdate)** (3 connections) — `native/src/catalog/README.md`
- **pp_sieve_clone (--clone-sieve MOR Shallow Clone)** (3 connections) — `native/src/catalog/README.md`
- **MakeCatalog (RestCatalog/IRC Client)** (2 connections) — `native/src/catalog/README.md`
- **pp-catalog Binary (pp_catalog_main.cc)** (2 connections) — `native/src/catalog/README.md`
- **pp_lmdb_smoke (make smoke)** (2 connections) — `native/src/catalog/README.md`
- **primeparts/catalog (Native Local Catalog of Record)** (2 connections) — `native/src/catalog/README.md`
- **iceberg::sql::CatalogStore Interface** (1 connections) — `native/src/catalog/README.md`
- **Covering-Sieve Progress Persistence** (1 connections) — `native/src/catalog/README.md`
- **HANDOFF.md Section 6 (Verified Phase 0/1 State)** (1 connections) — `native/src/catalog/README.md`
- **iceberg-cpp FileIO** (1 connections) — `native/src/catalog/README.md`
- **IRC Catalog Design Doc (markdown/data_eng/irc_catalog_design.md)** (1 connections) — `native/src/catalog/README.md`
- **Vendored liblmdb** (1 connections) — `native/src/catalog/README.md`
- **Merge-on-Read (v2 Position Deletes)** (1 connections) — `native/src/catalog/README.md`
- **metadata_location CAS (Head Pointer per Table)** (1 connections) — `native/src/catalog/README.md`
- **primes_k0_sieve Table (v2 MOR Shallow Clone of primes_k0)** (1 connections) — `native/src/catalog/README.md`
- **PublishTable (Register-or-Create + FastAppend)** (1 connections) — `native/src/catalog/README.md`
- **rest-catalog-open-api.yaml (Iceberg REST Catalog OpenAPI Spec)** (1 connections) — `native/src/catalog/README.md`

## Relationships

- No strong cross-community connections detected

## Source Files

- `native/src/catalog/README.md`

## Audit Trail

- EXTRACTED: 22 (88%)
- INFERRED: 3 (12%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*