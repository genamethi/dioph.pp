# 06_declare

deps: none | status: done

done: `TableDeclaration{sort_order, properties}` descriptor in pp_iceberg_rest.h; `TableCommitSpec.declare` threads it through AssembleChange/EnsureTable/CommitFiles into CreateTable (hard-coded `SortOrder::Unsorted()` gone — null declaration defaults to unsorted, properties merge over zstd write defaults). `schemas.cc:PAscendingSortOrder`. generate declares p-asc + `pp.buckets.self-contained=true` for both tables. New `build/pp-declare-sort --table T --field F [--ns/--rest-uri/--warehouse]`: assert-table-uuid-guarded updateTable POST of add-sort-order + set-default-sort-order via internal serde; idempotent no-op when already declared; REFUSES if any committed file lacks manifest bounds for the field, or if a different default order exists. E2E-proven against a scratch catalogd: declare → metadata carries default-sort-order-id=1 identity-asc; re-run no-ops; bounds guard refuses an MV without key bounds. Retired: PublishTable + LatestMetadataJson (dead since pp-catalog gut).

grep gate: `grep -n 'SortOrder::Unsorted()' native/src/catalog/pp_iceberg_rest.cc` → 1 hit, the declared-nothing default only (not a per-call hard-code)

Migration run is USER-ONLY (live warehouse): `pp-declare-sort --table primes --field p` then `--table partitions --field p`.

## notes

- Found via e2e: a table with a declared sort order whose files lack key bounds is unplannable (SortTasksByLowerBound loud error on every scan). The declaration seam now checks the physical precondition instead — the tool refuses. Live primes/partitions files all carry p bounds, so the live migration is unaffected.
- pp_commit.h now includes pp_iceberg_rest.h (TableDeclaration).
