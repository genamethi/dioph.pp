# primeparts/catalog

The native local catalog of record and its tools. The engine is an in-process
`iceberg::sql::SqlCatalog` (upstream, store-agnostic) backed by a vendored
**LMDB** `CatalogStore`; base tables stay as Parquet + `metadata.json` on the
filesystem, read/written through iceberg-cpp FileIO.

| File | Role |
|---|---|
| `pp_lmdb_store.{h,cc}` | `LmdbCatalogStore : iceberg::sql::CatalogStore` over vendored `liblmdb`. Two named sub-DBs (`tables`, `nsprops`); single-writer under one recursive mutex. `MakeLmdbCatalogStore(path, name, map_size)`. |
| `pp_iceberg_rest.{h,cc}` | Catalog construction + shared helpers: `MakeLocalCatalog` (SqlCatalog over the LMDB store — the catalog of record), `MakeCatalog` (a RestCatalog/IRC client), `EnsureNamespace`, `PublishTable` (register-or-create + FastAppend), `LatestMetadataJson`, `LocalIO`. |
| `pp_lmdb_smoke.cc` | `make smoke` — store-contract + SqlCatalog round-trip over the LMDB store. |

## The catalog seam

Tools never touch LMDB directly. `MakeLocalCatalog` returns an
`iceberg::Catalog` whose metadata engine (apply `TableUpdate`s, write
`metadata.json`, optimistic-concurrency commit) lives in `SqlCatalog`; the LMDB
store is injected through its `CatalogStore` interface and holds only the head
pointer per table (the `metadata_location` CAS). See
`markdown/data_eng/irc_catalog_design.md` for the full design and the
write/commit model, and `HANDOFF.md` §6 for the verified Phase 0/1 state.

The next build (Phase 2) is `pp-catalogd`, a native IRC HTTP server that wraps
this same `MakeLocalCatalog` engine behind the REST routes from
`docs/vendor/iceberg/open-api/rest-catalog-open-api.yaml`.
