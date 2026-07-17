# 06_declare

status: done for CREATE path (merged 0bb87bd); existing-table surface = open hole in 00

`TableDeclaration{sort_order, properties}` threads through the commit path into CreateTable; `schemas.cc:AscendingSortOrder`; generate declares primes `(p)`, partitions `(p, m_k)` + `pp.buckets.self-contained=true` at CREATE.

Load-bearing for the future declaration surface:
- A pp-declare-sort binary was built, e2e-proven against scratch catalogd (updateTable add-sort-order/set-default-sort-order works; `AddSortOrder::ApplyTo` live-confirmed), then DELETED as an unauthorized packaging guess. Recover implementation from git: `git show 8dd8cf0`, `f08b729^..`.
- Precondition the surface must enforce: refuse to declare a sort on a table whose committed files lack primary-key bounds (declared-sorted + missing bounds = every scan errors in SortTasksByLowerBound). Live primes/partitions files all carry p bounds — declaring is safe there once the surface exists.
- User direction leans config.lua `tables` section + Lua interface; undecided — design session required.
