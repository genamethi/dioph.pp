# HANDOFF for agents

## Guidance for agents

Please excise things once they're done. No need to have running commentary.
No progress tracker. No narrating.

Furthermore, don't add high level summaries. Keep it grounded, technical and
actionable.

I don't want any comments in the code. 

---

## Tables

- `primeparts.primes`: one row per prime, `p`-ordered. $\min(p)=3$ (p=2 absent);
  no primes skipped between 3 and $\max(p)$.
- `primeparts.partitions`: length-two integer partitions of odd `p` into prime
  power summands — by parity a power of two plus an odd prime power. Each row's
  `(m_k, n_k)` **characterizes** one such partition for that `p`. Most edges are `n=1`.

- Any other tables referenced have been retired or are deprecated (and any dependent
code should be fixed.)

## Catalog (operating notes)

- Catalog of record is the native LMDB-backed IRC (`pp-catalogd` serves it over
  `/v1`). Tools obtain a catalog through `OpenCatalog` (`pp_iceberg_rest.{h,cc}`):
  it resolves a URI (`--rest-uri` / `PRIMEPARTS_REST_URI` / compiled
  `kDefaultRestUri` = `http://127.0.0.1:8181`) and requires that server to answer
  `GET /v1/config` — no in-process fallback; nothing touches the LMDB state
  except catalogd.
- Data files enter the warehouse only through `CommitFiles` (single table) or
  `CommitFilesAtomic` (multiple tables, one transaction): clients write parquet
  to `StagingDataDir` (outside the table tree), the commit moves it in on
  success. A build killed before commit can only leave `.pp-staging` debris
  (safe to `rm`).
- Catalog authority is REST; **data reads are not**. Consumers read metadata
  and parquet themselves. Server-side scan planning exists and is advertised,
  but no shipped tool calls it (see below).
- One non-spec extension route exists:
  `GET /v1/namespaces/{ns}/tables/{t}/field-upper-bound?field=` (bound at
  `pp_catalogd.cc:533`, advertised with an empty verb list so it stays out of
  `/v1/config` `endpoints`). It answers `{field, upper_bound}` with
  `upper_bound: null` when the table has no snapshot or no bound for the
  field. `FieldUpperBound` reads every manifest whose `added_snapshot_id`
  equals the current snapshot id and takes the max `upper_bounds[field]` over
  their live entries. Iterating all of them is load-bearing:
  `SnapshotUpdate::WriteDataManifests`
  (`vendor/iceberg-cpp/src/iceberg/update/snapshot_update.cc:204`) writes
  through a `RollingManifestWriter` bounded by `target_manifest_size_bytes_`
  and dispatches through `WriteManifestGroups` under
  `write_manifest_parallelism_`, so one append may produce several manifests
  and their order is not the append order.
- The spec surface for the same fact is `planTableScan` with `stats-fields`
  (yaml:5170), which returns `upper-bounds`/`lower-bounds` per **data file** on
  each `FileScanTask.data-file` (yaml:5070, typed `ValueMap` at yaml:4940).
  That is O(data files) on the wire against O(manifests) server-side for the
  extension; the spec has no aggregate route.
- Design detail: `markdown/data_eng/irc_catalog_design.md`.
- Open gaps live in `markdown/plans/holes_registry.md`. That file is the
  registry; this file is the map.

## Binaries and what links into them

| Binary | Entry | Notably links |
|---|---|---|
| `primeparts-generate` | `generate.cc` | writer, aligned_writer, schemas, pp_commit, partition_stats |
| `primeparts-catalogd` | `pp_catalogd_main.cc` | pp_catalogd, plan_store, scan planner, lmdb store |
| `primeparts-verify` | `verify_main.cc` | verify, source_scan, scan planner |
| `primeparts-tui` | `tui_main.cc` | tui_*, query_service, materialize, lua_presets, source_scan |
| `pp` | `query/pp_main.cc` | lua_query_module, query_service, materialize, source_scan |
| `primeparts-bench-core`, `primeparts-bench-materialize` | `bench.c`, `materialize_bench.c` | core only |

Consumers of scan plans are `source_scan.cc` (the generic reader) and
`query_service.cc`. Both plan **in-process** from metadata they read off disk.

## Not wired to anything

Start here when deciding what is alive.

- `catalog/rest_scan_plan.{h,cc}` — the four client planning calls and
  `PlanScanOnServer`. Linked only into the e2e test; **no shipped binary calls
  it**.

## ACTIVE WORK HERE ON ...

## Configuration loose ends

- **Two disjoint config surfaces.** `config::Load` (`config.{h,cc}`) reads
  `config.lua`'s `config({...})` into a flat `map<string,string>`; `generate`
  consumes exactly three keys — `rest_uri`, `namespace`, `warehouse`. The TUI
  keeps its own six-field `App::cfg` (log limit, gen threads, default limit,
  log format, autosave, warehouse). Only `warehouse` overlaps. Neither surface
  knows about the other's keys.
- **The warehouse default is hardcoded in five places, and they disagree.**
  `pp_catalogd_main.cc:10`, `verify_main.cc:30`, `pp_main.cc:14` and
  `tui_main.cc:22` use `/media/extssd/research/dioph.pp/data/ib-staging`;
  `generate.cc:303` uses `/media/extssd/research/dioph.pp/data` (as the temp
  root under `FUNBUNS_DATA_DIR`). On a machine without that mount every tool
  fails the same way and the systemd unit crash-loops.
- **Table definitions are C++, not config.** `schemas.cc` holds `PrimesSchema`,
  `PartitionsSchema`, `BucketPartitionSpec`, `AscendingSortOrder`. Adding a
  table means editing and rebuilding. `CreateTableRequest` (yaml:3931) accepts
  `schema`, `partition-spec`, `write-order`, `location` and `properties`, so
  everything a table needs at birth is expressible over the wire; nothing in
  the tree assembles that body from a declaration.
- **Traits are read from table metadata, not declared.** `TableReadTraits::
  FromMetadata` (`scan/table_traits.cc`) derives sort keys from the table's
  sort order, and resolves identity transforms only. So a table's read
  behaviour follows what was committed, which is why the declaration gap below
  bites.

## Immediate Directions

Roughly in dependency order. Each is a starting point, not a spec.

1. **Declare sort order on the existing tables.** Decided REST-only.
   `TableReadTraits::FromMetadata` (`scan/table_traits.cc:13`) derives sort
   keys from the *committed* sort order. With none declared it returns success
   with empty `sort_keys`; the refusal happens in each `traits.sorted()` caller
   2026.08.04 Note: Is this relevant if we're generating all data from scratch?
   It should be assumed that we are at this point.
   - Note we should 
   — `RequireSorted` under `ScanByK` (`query/query_service.cc:229`), windowed
   `GroupCount`, `QueryService::Extent` (`:510`, the table extent the TUI
   reads), `verify_main.cc:104`, `source_scan.cc:102`, `scan_planner.cc:492`
   and `:526`. A declared order using a non-identity transform is instead a
   loud `NotImplemented` (`table_traits.cc:36`).
   `CreateTableRequest` carries `write-order` (yaml:3945), so a table created
   through an init path declares its order at birth and needs no separate step;
   the two existing tables need one `updateTable` migration
   (`assert-table-uuid` + `assert-default-sort-order-id`;
   `update/update_sort_order.cc` in the vendored tree). The domain precondition
   — refuse if primary-key bounds are missing on any committed file — has no
   spec counterpart and still needs a home. It is the same predicate resume
   needs when `field-upper-bound` finds a snapshot but no bound.
   This blocks the query/read paths only. `generate`'s resume frontier reads
   manifest `upper_bounds` directly and never consults sort order.
2. **Bring `generate` to workable.** Two separate reads, and the fragile one is
   the frontier, not the bucket state.
   `LoadAlignedResume` (`aligned_writer.cc:323`) loads each table through the
   catalog and reads bucket fill and per-table file sequence from the table's
   **partition statistics files** via `catalog::LoadPartitionStats`
   (`partition_stats.cc:697`) — frontier bucket, its
   `total_data_file_size_in_bytes`, its `data_file_count` as `next_seq`. No
   filesystem glob is involved. `LoadPartitionStats` calls `SingleSpecOnly`, so
   partition evolution turns resume into a hard error, and `next_seq =
   frontier_files` assumes no gaps in the `_%04d` suffix within a
   `(version, bucket)`.
   The generation frontier is separate: `resolve_start_idx`
   (`generate.cc:605`) calls `FetchFieldUpperBound` for `primes.prime_rank`.
   Every failure path in that route answers `present=false`, and
   `generate.cc:632` reads that as a fresh warehouse and starts at
   `kFreshStartIdx = 2` (rank 2 is the first row, `p=3`). A resume that cannot
   locate the frontier therefore regenerates from the beginning into an
   append-only table rather than refusing. Initialization is a fallback where
   it should be a deliberate act; the three outcomes worth distinguishing are
   table absent (not initialized), table present with a snapshot but no bound
   (hard error), and table present with no snapshot (legitimately empty, start
   at 2).
   `ShapePolicy` (`aligned_writer.h:45`) is one instance across every
   `BoundTable`, and its `file_target_bytes` / `rgs_per_file` shadow spec'd
   Iceberg table properties — `write.target-file-size-bytes`
   (`table_properties.h:245`) and `write.parquet.row-group-size-bytes`
   (`:114`), both settable through `CreateTableRequest.properties` and
   therefore fixable at creation. `bucket_target_bytes` has no spec
   counterpart; bucketing here is an organizational axis, not a spec concept.
3. **Rewrite old pp_graph** The batch materialization pipeline
  (pp_graph.cc, pp_graph_exp.cc, pp_graph_store.{cc,h}) is being replaced. The
  sweep and disk spill used to persist the trie state into derived tables
  (chain_words, node_words, skeletons) was misguided (and costs too much space).
  The partitions table really contains most of what we need, but it might take
  some time to settle on how to use the structure to quickly compute things.
  The first step we've decided on is starting with the basis elements:

  The keystone of our replacement model is seeded by an initial pass starting
  from the $k=0$ primes will gather all Hermite polynomials, then determine the
  basis. That will be saved as an Iceberg table (unpartitioned) within the
  warehouse (fully leveraging the IRC interface). 

  Then composing these bases from a given set, fully compatible with GiNaC
  expression types (which decompose to subexpressions), will allow us to do
  analysis on a given range of prime values.

  One hope is that we will be able to use these as formal expressions for
  evaluation over the dataset (a la expressions in Polars for example; i.e.,
  these should give us constraints for satisfaction over subsets of primes.)

  One consumer of this data I'm hoping to see immediately is a Hasse-diagram
  corresponding to a queried set of primes, and a chain decomposition (what are the
  antichains?). 
4. **Settle consumer server-side planning.** `rest_scan_plan` exists and
   works; it is linked only into the e2e test. Either wire `source_scan` /
   `query_service` to it — which also decides where mode dispatch lives — or
   accept that in-process planning is the real path and the REST client is for
   foreign consumers. `generate` is already a REST client on both resume reads,
   so the producer's REST-ness is not the gap; what it does not use is the
   *spec* planning routes.
5. **Settle configuration.** One surface, one warehouse default.
  However, a qualification and some clarifications: (a) Iceberg tables
  being defined by schemas.cc is fine when they're tightly coupled to a binary.
  What the final shape will likely be is that we'll have a client surface for
  deriving tables from data and committing those. For the user directed approach
  then, we can use a Lua interface.
  (b) By "warehouse default" I'd prefer localhost, the current default port,
  and ~/local/share/pp-data/ as the default. This is fixed via an example config.lua
  which resides in the same directory as the other binaries. (default is ~/.local/bin)
  (c) The Lua config table should have general reusable variables in a config.core table.
  Then binary specific config variables (all flags checked against the config, and
  overidden.). I.e., all default behavior is determined by the example.config.lua.
  (d) The example config should trigger a message on init to switch away from the
  defaults.
  (e) For the time being, this config is separate from any table or schema definitions.
  That can wait. 
  (f) config.tui, config.graph, config.generate are some examples. config.test might be
  appropriate, but consider that we're using gtest and whether we want to wrap around that
  or keep separate.
  (g) This should be enough groundwork to help tie up the loose ends in the previous section.


## Lower priority leftovers: 

1. **Lifecycle.** Snapshot expiry is unwired, orphaned files from failed
   transactions are unreachable, rollback has no surface. All three are small
   against APIs that already exist.
2. **Derived read indexes** for fast number-theoretic reads (approach open).
3. **Views** — `https://raw.githubusercontent.com/apache/iceberg/refs/heads/main/format/view-spec.md`
4. **Janitor** for killed-run `.pp-staging` debris.
6. **Compute `q_k` Dynamically:** The `q_k` column was removed from the `partitions` table. Update all consumers (`query_service.cc`, `verify.cc`, `lua_query_module.cc`) to solve for `q_k` algebraically from `(p, m_k, n_k)` on the fly.
