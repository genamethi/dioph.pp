# 06_declare

deps: none | status: todo

- [ ] `pp_commit.h:TableCommitSpec` — add `shared_ptr<iceberg::SortOrder> sort_order`, `map<string,string> properties`
- [ ] `pp_iceberg_rest.{h,cc}:EnsureTable/PublishTable/CommitFiles` — thread both into CreateTable; kill hard-coded `SortOrder::Unsorted()` (pp_iceberg_rest.cc:328)
- [ ] `schemas.{h,cc}:PAscendingSortOrder(const iceberg::Schema&)` — field "p", ascending, identity
- [ ] `generate.cc` — declare sort order for both tables + `{"pp.buckets.self-contained":"true"}`
- [ ] new `native/src/catalog/pp_declare_sort_main.cc` → `build/pp-declare-sort --table T --field F [--rest-uri U]`: updateTable POST `{requirements:[assert-table-uuid], updates:[add-sort-order, set-default-sort-order]}` via internal serde (pp_commit.cc pattern); catalogd route applies (`AddSortOrder::ApplyTo` exists)
- [ ] Makefile: pp-declare-sort target
- [ ] build green

Migration run is USER-ONLY (live warehouse): `pp-declare-sort --table primes --field p`, same for partitions. Pre-migration, order-requiring paths fail loudly by design.

grep gate: `grep -n 'Unsorted()' native/src/catalog/pp_iceberg_rest.cc` → 0

## notes
