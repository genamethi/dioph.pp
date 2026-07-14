# 06_declare

deps: none | status: done (create path); existing-table surface = open hole in 00

done: `TableDeclaration{sort_order, properties}` descriptor in pp_iceberg_rest.h; `TableCommitSpec.declare` threads through AssembleChange/EnsureTable/CommitFiles into CreateTable (hard-coded `SortOrder::Unsorted()` gone; properties merge over zstd write defaults). `schemas.cc:AscendingSortOrder(schema, fields, error)`; generate declares primes `(p)`, partitions `(p, m_k)` + `pp.buckets.self-contained=true` at CREATE. Unit-covered (test_iceberg_writer). Retired: PublishTable + LatestMetadataJson (dead since pp-catalog gut).

NOT done (deliberate): any surface for declaring on EXISTING tables — see 00 holes registry. A pp-declare-sort binary was built, e2e-proven against scratch catalogd (updateTable add-sort-order/set-default works end-to-end, AddSortOrder::ApplyTo confirmed live), then DELETED: single-purpose binary was an unauthorized packaging guess; user direction leans a config.lua `tables` section + Lua interface, undecided. Recover the deleted implementation from git history (`git show 8dd8cf0` / `f08b729^..HEAD~1`) when the surface is designed.

grep gate: `grep -n 'SortOrder::Unsorted()' native/src/catalog/pp_iceberg_rest.cc` → 1 hit (declared-nothing default only)

## notes

- E2E finding preserved: a declared-sorted table whose files lack primary-key bounds is unplannable (loud SortTasksByLowerBound error on every scan) — the future declaration surface must check bounds before declaring (the deleted tool did).
- Live primes/partitions files all carry p bounds; whenever the surface lands, declaring is safe there.
- pp_commit.h now includes pp_iceberg_rest.h (TableDeclaration).
