# planner-prep review task

Reviewer: you. Goal: decide merge-readiness of `planner-prep` → `pure-local` (14 commits ahead). Not "does it match the plan" (it does; gates green) but "was this the right design." Check boxes / annotate inline.

## two levels — spend your time on level 2

- L1 faithful-to-plan: mechanical, mostly verified (build+test green, per-phase grep gates clean, holes named). Skim.
- L2 was-the-plan-right: the real review. The code can be a faithful implementation of a design you don't want. Everything below is L2.

## method

- [ ] Read the SEAM HEADERS first — they are the contracts; if the interfaces read wrong, impls don't matter. In order: `writer.h`, `scan/table_traits.h`, `scan/scan_plan.h`, `scan/scan_planner.h`, `source_scan.h`, `catalog/pp_iceberg_rest.h` (TableDeclaration), `query/query_service.h`.
- [ ] Then per-phase commits `git show <sha>` 01→08 (`git log --oneline pure-local..planner-prep`); each `NN_*.md` `## notes` flags where impl diverged from plan.
- [ ] Run it: scratch catalogd + `pp` materialize/read, `generate --temp`, confirm the loud failures actually fire (see 09_merge verifiable-e2e list).
- [ ] L2 trace: pick ONE real upcoming task (add a new table? a new query shape?) and trace it through the new seams. Is it actually easier than before, or just differently shaped?

## design decisions to interrogate (each is a fork I picked — push on it)

### core thesis: descriptors
- [ ] Is "caller/catalog DECLARES what machinery assumed" the right seam, vs keeping some knowledge central? The whole branch rests on this. `TableReadTraits`, `WriterConfig::stat_columns`, `TableDeclaration`.
- [ ] Smell — speculative generality: `stat_columns` is a general list but only ever p/prime_rank/q_k. Warranted, or should it be named-but-fixed channels? Same q for the GroupKey derived-key machinery — does it match how you actually query, or is it abstraction for its own sake?

### plan atom (IRC mirroring)
- [ ] `ScanPlan`/`scan::FileScanTask` model a SUBSET of the spec: plan-id/paging/PlanStatus live at the server seam (08 stubs), not in the plan type. Sub-file scoping REWORKED post-review: the private `row_groups` ordinals are gone; tasks carry `iceberg::Split{offset,length}` (spec `split-offsets` vocabulary), one task per surviving split. Remaining question: does the session protocol land cleanly on these types when catalogd serves planTableScan?
- [ ] Is client-side planning the right transitional step at all, or premature vs waiting for server-side? (gap docs said yes — do you still agree?)

### residual / window handling — SUBTLEST, look hardest here
- [ ] `scan_planner.cc:ExtractKeyWindow` folds only ASCENDING PRIMARY-key range conjuncts into a window; `source_scan.cc:SliceToKeyWindow` binary-slices batches to it. Non-range predicates, secondary keys, descending orders → left in the residual.
- [ ] The contract is "executor slices the key window; consumer applies the rest of the residual via `reader->residual()`." Do consumers ACTUALLY apply it? LookupPrime/ScanByK do their own ==/k checks; GroupCount... check each. If a consumer neither slices nor re-checks a predicate, that's a silent-wrong-rows bug. This is the #1 place correctness could be quietly broken.

### row-group pruning correctness
- [ ] `scan_planner.cc:SelectRowGroups` synthesizes a per-row-group `DataFile` from parquet footer stats → `InclusiveMetricsEvaluator`. Verify: bound serialization/types (int32 vs int64), null-count handling, and that "keep on missing/unsupported stats" (inclusive) is the default you want.

### loud-failure philosophy
- [ ] Order-requiring paths ERROR without a declared sort order (vs degrade to full scan). You wanted loud — confirm it's loud in the RIGHT places and the messages are actionable. (Dangling pp-declare-sort refs already fixed.)

### deletions — did I remove something you wanted?
- [ ] pp_commit_smoke + pp_catalogd_smoke, `PublishTable`, `LatestMetadataJson`, `NextFileSeq`, the pp-declare-sort binary, `SourceFileInfo`/`source_files()`. Any load-bearing for something not yet rebuilt?

### the holes — right holes, right places?
- [ ] Existing-table declaration surface (no home — 06). Row-group read path NotImplemented for identity-partition columns (03). catalogd 406 plan stubs (08). Are these the deliberate holes you'd choose, or did I hole something that should have been solved now?

### namespace threading
- [ ] `kCatalogName` (catalog identity) vs `Namespace` (table grouping) split — right distinction? Multi-level ns via `%1F`/path-join — real need, or ceremony you'll never exercise?

## meta — "was the plan what I wanted"

- [ ] Does FileScanTask-as-plan-atom actually serve the EVENTUAL engine (DuckDB pushdown / SourceTableReader), or is it API-shaped for its own sake?
- [ ] Did the branch reduce real instance-coupling pain or just relocate it into more indirection? (the L2 trace above answers this concretely.)
- [ ] Does sort-order-as-catalog-truth hold up now that partitions is (p, m_k) and future tables will differ? Only the PRIMARY key is consumed today — is that a simplification that will need undoing?

## verdict

- [ ] merge as-is
- [ ] merge after: ___
- [ ] rework: ___
