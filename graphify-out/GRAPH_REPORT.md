# Graph Report - .  (2026-07-07)

## Corpus Check
- Large corpus: 5033 files · ~6,169,330 words. Semantic extraction will be expensive (many Claude tokens). Consider running on a subfolder.

## Summary
- 1290 nodes · 2602 edges · 71 communities (66 shown, 5 thin omitted)
- Extraction: 94% EXTRACTED · 6% INFERRED · 0% AMBIGUOUS · INFERRED: 144 edges (avg confidence: 0.81)
- Token cost: 155,715 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Prime Generator + Lua Core|Prime Generator + Lua Core]]
- [[_COMMUNITY_TUI App State|TUI App State]]
- [[_COMMUNITY_Source Table Reader|Source Table Reader]]
- [[_COMMUNITY_Covering Sieve|Covering Sieve]]
- [[_COMMUNITY_mdiff Table Builder|mdiff Table Builder]]
- [[_COMMUNITY_LMDB Catalog Store|LMDB Catalog Store]]
- [[_COMMUNITY_Iceberg REST Catalog|Iceberg REST Catalog]]
- [[_COMMUNITY_Lua Query Presets|Lua Query Presets]]
- [[_COMMUNITY_Tuple Cluster Analysis|Tuple Cluster Analysis]]
- [[_COMMUNITY_Row Delta  Deletes|Row Delta / Deletes]]
- [[_COMMUNITY_Mersenne Primitive Factors|Mersenne Primitive Factors]]
- [[_COMMUNITY_Tally Bucket Aggregation|Tally Bucket Aggregation]]
- [[_COMMUNITY_Iceberg Schemas + Mersenne|Iceberg Schemas + Mersenne]]
- [[_COMMUNITY_Catalogd HTTP Server|Catalogd HTTP Server]]
- [[_COMMUNITY_TUI Main Render Loop|TUI Main Render Loop]]
- [[_COMMUNITY_Parquet Data File Writer|Parquet Data File Writer]]
- [[_COMMUNITY_Sieve Clone + Catalog CLI|Sieve Clone + Catalog CLI]]
- [[_COMMUNITY_Catalog Seam Design Docs|Catalog Seam Design Docs]]
- [[_COMMUNITY_Writer Config + Partition Spec|Writer Config + Partition Spec]]
- [[_COMMUNITY_TUI Query View|TUI Query View]]
- [[_COMMUNITY_Query Service Scan Control|Query Service Scan Control]]
- [[_COMMUNITY_IntQ Factor Counting|IntQ Factor Counting]]
- [[_COMMUNITY_Prime Factor Stats|Prime Factor Stats]]
- [[_COMMUNITY_TUI Run Query Screen|TUI Run Query Screen]]
- [[_COMMUNITY_Progress Rendering|Progress Rendering]]
- [[_COMMUNITY_Parquet Writer Impl|Parquet Writer Impl]]
- [[_COMMUNITY_QueryPreset Headers|Query/Preset Headers]]
- [[_COMMUNITY_LMDB Smoke Test|LMDB Smoke Test]]
- [[_COMMUNITY_Query Service Facade|Query Service Facade]]
- [[_COMMUNITY_Written File Metadata|Written File Metadata]]
- [[_COMMUNITY_Catalogd Smoke Test|Catalogd Smoke Test]]
- [[_COMMUNITY_Generation Pipeline Core|Generation Pipeline Core]]
- [[_COMMUNITY_TUI Config Screen|TUI Config Screen]]
- [[_COMMUNITY_TUI Generate Screen|TUI Generate Screen]]
- [[_COMMUNITY_Generate CLI Args|Generate CLI Args]]
- [[_COMMUNITY_Parquet Aggregation Pass|Parquet Aggregation Pass]]
- [[_COMMUNITY_Factor Frequency Outputs|Factor Frequency Outputs]]
- [[_COMMUNITY_Batch Group Writer|Batch Group Writer]]
- [[_COMMUNITY_Aggregation Options|Aggregation Options]]
- [[_COMMUNITY_Generation Bucket Options|Generation Bucket Options]]
- [[_COMMUNITY_Arrow Array Builders|Arrow Array Builders]]
- [[_COMMUNITY_Stop Monitor  Keyboard|Stop Monitor / Keyboard]]
- [[_COMMUNITY_Query Service Impl|Query Service Impl]]
- [[_COMMUNITY_Iceberg Writer Test|Iceberg Writer Test]]
- [[_COMMUNITY_TUI + Lua Design Docs|TUI + Lua Design Docs]]
- [[_COMMUNITY_Query Service Validation|Query Service Validation]]
- [[_COMMUNITY_Table Extent Metadata|Table Extent Metadata]]
- [[_COMMUNITY_Progress Counter|Progress Counter]]
- [[_COMMUNITY_Warehouse + Catalog Docs|Warehouse + Catalog Docs]]
- [[_COMMUNITY_Prime Obstruction Theory|Prime Obstruction Theory]]
- [[_COMMUNITY_Query Lua Module + REPL|Query Lua Module + REPL]]
- [[_COMMUNITY_mtuple Statistics (math)|mtuple Statistics (math)]]
- [[_COMMUNITY_Bucket Writer Facade|Bucket Writer Facade]]
- [[_COMMUNITY_Primes Table + Read Paths|Primes Table + Read Paths]]
- [[_COMMUNITY_Additive Decomposition Math|Additive Decomposition Math]]
- [[_COMMUNITY_Sample Row Schema|Sample Row Schema]]
- [[_COMMUNITY_Query Layer + Build Docs|Query Layer + Build Docs]]
- [[_COMMUNITY_Query Literal Bounds|Query Literal Bounds]]
- [[_COMMUNITY_TUI Query Worker|TUI Query Worker]]
- [[_COMMUNITY_Covering Filter + NT Core|Covering Filter + NT Core]]
- [[_COMMUNITY_l-adic Residue Analysis|l-adic Residue Analysis]]
- [[_COMMUNITY_Partition Tuple Schema|Partition Tuple Schema]]
- [[_COMMUNITY_Prime Info Schema|Prime Info Schema]]
- [[_COMMUNITY_Group Count Row|Group Count Row]]
- [[_COMMUNITY_Scan Hit Row|Scan Hit Row]]
- [[_COMMUNITY_Schema Header|Schema Header]]
- [[_COMMUNITY_Configure Script|Configure Script]]
- [[_COMMUNITY_Vendor Patch Script|Vendor Patch Script]]
- [[_COMMUNITY_Bash Completion|Bash Completion]]
- [[_COMMUNITY_Doc Authoring Guidance|Doc Authoring Guidance]]

## God Nodes (most connected - your core abstractions)
1. `App` - 125 edges
2. `LmdbCatalogStore` - 34 edges
3. `Stats` - 33 edges
4. `main()` - 30 edges
5. `WriterConfig` - 27 edges
6. `run_generation()` - 26 edges
7. `BucketParquetWriter::Impl` - 26 edges
8. `WrittenFile` - 23 edges
9. `lua_State` - 22 edges
10. `Catalog` - 22 edges

## Surprising Connections (you probably didn't know these)
- `dioph.pp research engine (project overview)` --references--> `Native build & rootless provisioning (BUILD.md)`  [EXTRACTED]
  markdown/misc/README.md → BUILD.md
- `Modular covering filter (native hot-path optimization)` --conceptually_related_to--> `Number-theory core (primecount/primesieve/FLINT/GMP/PARI)`  [INFERRED]
  markdown/misc/modular-filter-idea.md → native/README.md
- `native/configure provisioning script` --references--> `Number-theory core (primecount/primesieve/FLINT/GMP/PARI)`  [EXTRACTED]
  BUILD.md → native/README.md
- `primeparts TUI application design (tui copy)` --semantically_similar_to--> `primeparts TUI application design (screens, presets, dispatch)`  [INFERRED] [semantically similar]
  markdown/tui/tui_app_design.md → markdown/arch/tui_app_design.md
- `WriteTable()` --references--> `DataFile`  [INFERRED]
  native/src/analysis/tally_main.cc → native/include/primeparts/writer.h

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **LMDB-backed Local Catalog of Record Stack** — native_src_catalog_readme_sqlcatalog, native_src_catalog_readme_lmdbcatalogstore, native_src_catalog_readme_makelocalcatalog, native_src_catalog_readme_metadata_location_cas [EXTRACTED 1.00]
- **Covering-Sieve MOR Persistence Flow** — native_src_catalog_readme_pp_sieve_clone, native_src_catalog_readme_pp_row_delta, native_src_catalog_readme_primes_k0_sieve, native_src_catalog_readme_covering_sieve_persistence [INFERRED 0.75]
- **Covering-system obstruction theory (mechanism, scaling, proof)** — markdown_math_lab_notes_covering_system, markdown_math_lab_notes_backbone, markdown_math_lab_notes_mod255255, markdown_math_lab_notes_22_classes, markdown_math_lab_notes_subgroup_exclusion [EXTRACTED 0.95]
- **Native query stack (QueryService, Lua module, pp shell, TUI)** — markdown_arch_tui_query_design_queryservice, markdown_api_lua_query_module, markdown_api_lua_pp_shell, markdown_arch_tui_app_design_app [EXTRACTED 0.85]
- **Iceberg native write/commit path (warehouse, LMDB catalog, CommitFiles, generate)** — markdown_data_eng_iceberg_data_setup_warehouse, markdown_data_eng_irc_catalog_design_native_irc, markdown_data_eng_irc_catalog_design_commitfiles, native_readme_generate [EXTRACTED 0.85]

## Communities (71 total, 5 thin omitted)

### Community 0 - "Prime Generator + Lua Core"
Cohesion: 0.06
Nodes (69): materialize_shared, mpz_t, lua_State, QueryService, FILE, main(), monotonic_seconds(), parse_i64() (+61 more)

### Community 1 - "TUI App State"
Cohesion: 0.03
Nodes (67): Focus, ModalKind, App, cfg, cfg_cursor, config_path, confirm_quit, cursor (+59 more)

### Community 2 - "Source Table Reader"
Cohesion: 0.05
Nodes (46): Expression, FileScanTask, FileScanTaskReader, Raw-SQLite reader migration onto the catalog seam, Impl, string, unique_ptr, RecordBatch (+38 more)

### Community 3 - "Covering Sieve"
Cohesion: 0.06
Nodes (48): string, string_view, StripFileScheme(), GetOrd2, AppliedModuli(), BuildCov(), array, pair (+40 more)

### Community 4 - "mdiff Table Builder"
Cohesion: 0.07
Nodes (49): DataFile, BuildMdiff(), Array, atomic, Builder, FileReader, map, pair (+41 more)

### Community 5 - "LMDB Catalog Store"
Cohesion: 0.12
Nodes (35): Body, CatalogStore, MDB_dbi, MDB_env, MDB_txn, MDB_val, NamespaceProperty, function (+27 more)

### Community 6 - "Iceberg REST Catalog"
Cohesion: 0.11
Nodes (43): string, RestOptions, rest_name, rest_prefix, rest_uri, rest_warehouse, Catalog, FileIO (+35 more)

### Community 7 - "Lua Query Presets"
Cohesion: 0.07
Nodes (41): string, vector, QueryField, name, value, QueryPreset, accepts, desc (+33 more)

### Community 8 - "Tuple Cluster Analysis"
Cohesion: 0.09
Nodes (34): Popcount(), ShapeOf(), ShiftOf(), ofstream, path, shared_ptr, string, vector (+26 more)

### Community 9 - "Row Delta / Deletes"
Cohesion: 0.08
Nodes (33): DataFileSet, ManifestFile, unordered_map, RowDelta, Apply, CleanUncommitted, has_new_files_, Make (+25 more)

### Community 10 - "Mersenne Primitive Factors"
Cohesion: 0.10
Nodes (30): n_factor_t, unordered_map, vector, MersenneFactor, d, exponent, q, MersenneHelper (+22 more)

### Community 11 - "Tally Bucket Aggregation"
Cohesion: 0.10
Nodes (32): BucketRange, hi, lo, atomic, pair, path, shared_ptr, string (+24 more)

### Community 12 - "Iceberg Schemas + Mersenne"
Cohesion: 0.13
Nodes (27): Build(), Array, Builder, path, shared_ptr, string, Finish(), main() (+19 more)

### Community 13 - "Catalogd HTTP Server"
Cohesion: 0.16
Nodes (22): E, ErrorKind, json, Namespace, Result, shared_ptr, string, string_view (+14 more)

### Community 14 - "TUI Main Render Loop"
Cohesion: 0.16
Nodes (23): draw_gen_output(), binary_dir(), binary_seed_path(), built_in_presets(), path, string, vector, config_file_path() (+15 more)

### Community 15 - "Parquet Data File Writer"
Cohesion: 0.19
Nodes (22): BatchStats, BucketParquetWriter::BucketParquetWriter(), BucketParquetWriter::Close(), CloseCurrent, OpenIfNeeded, BucketParquetWriter::Make(), BucketParquetWriter::Write(), BuildDataFile() (+14 more)

### Community 16 - "Sieve Clone + Catalog CLI"
Cohesion: 0.13
Nodes (20): CloneSieveOptions, dest_table, source_table, warehouse, string, Args, clone_sieve, register_tables (+12 more)

### Community 17 - "Catalog Seam Design Docs"
Cohesion: 0.10
Nodes (23): The Catalog Seam, iceberg::sql::CatalogStore Interface, Covering-Sieve Progress Persistence, HANDOFF.md Section 6 (Verified Phase 0/1 State), iceberg-cpp FileIO, IRC Catalog Design Doc (markdown/data_eng/irc_catalog_design.md), Vendored liblmdb, LmdbCatalogStore (+15 more)

### Community 18 - "Writer Config + Partition Spec"
Cohesion: 0.10
Nodes (21): vector, PartitionSpec, PartitionValues, RecordBatch, Schema, WriterConfig, bucket, bucket_version (+13 more)

### Community 19 - "TUI Query View"
Cohesion: 0.10
Nodes (21): Field, Config, autosave, default_limit, gen_threads, log_format, log_limit, string (+13 more)

### Community 20 - "Query Service Scan Control"
Cohesion: 0.18
Nodes (20): function, string, vector, ScanControl, cancel, progress, TableRows, cols (+12 more)

### Community 21 - "IntQ Factor Counting"
Cohesion: 0.19
Nodes (16): CountPair, p_count, remainder_count, DefaultWorkerThreads(), FloorLog2(), IntQKey, q, value (+8 more)

### Community 22 - "Prime Factor Stats"
Cohesion: 0.12
Nodes (19): AddPrimitiveFactorHit(), unordered_map, FactorResidual(), ProcessPrime(), Stats, backbone_covered, by_bucket, by_m (+11 more)

### Community 23 - "TUI Run Query Screen"
Cohesion: 0.22
Nodes (18): confirm_modal(), main(), option_count(), string, confirm_dispatch(), cycle_value(), draw_query(), history_back() (+10 more)

### Community 24 - "Progress Rendering"
Cohesion: 0.12
Nodes (17): time_point, ProgressLoop(), ProgressState, backbone_covered, candidate_remainders, done, files, k0_primes (+9 more)

### Community 25 - "Parquet Writer Impl"
Cohesion: 0.12
Nodes (16): FileOutputStream, FileWriter, BucketParquetWriter::Impl, arrow_schema, closed, config, current_record, done (+8 more)

### Community 26 - "Query/Preset Headers"
Cohesion: 0.21
Nodes (6): CatalogdOptions, host, port, warehouse, string, vector

### Community 27 - "LMDB Smoke Test"
Cohesion: 0.31
Nodes (14): Error, expected, path, Result, Status, T, Check(), IsAlreadyExists() (+6 more)

### Community 28 - "Query Service Facade"
Cohesion: 0.14
Nodes (14): Impl, unique_ptr, QueryService, Extent, GroupCount, impl_, ListTables, LookupPartitions (+6 more)

### Community 29 - "Written File Metadata"
Cohesion: 0.13
Nodes (15): path, shared_ptr, string, WrittenFile, bucket, bucket_version, bytes, data_file (+7 more)

### Community 30 - "Catalogd Smoke Test"
Cohesion: 0.26
Nodes (14): Array, path, RecordBatch, Schema, shared_ptr, string, T, Check() (+6 more)

### Community 31 - "Generation Pipeline Core"
Cohesion: 0.24
Nodes (15): append_manifest_boundary(), append_manifest_file(), ofstream, string, string_view, json_escape(), log_line(), main() (+7 more)

### Community 32 - "TUI Config Screen"
Cohesion: 0.25
Nodes (14): apply_config_kv(), map, string, cfg_adjust(), cfg_count(), cfg_name(), cfg_value(), config_to_kv() (+6 more)

### Community 33 - "TUI Generate Screen"
Cohesion: 0.24
Nodes (14): string, vector, draw_generate(), gen_adjust(), gen_field(), gen_name(), gen_opt_count(), gen_page() (+6 more)

### Community 34 - "Generate CLI Args"
Cohesion: 0.23
Nodes (11): deque, FILE, path, default_temp_root(), parse_args(), parse_i32(), parse_i64(), pp_gen_last_error() (+3 more)

### Community 35 - "Parquet Aggregation Pass"
Cohesion: 0.15
Nodes (14): AggregateEntryCount(), atomic, FileReader, unique_ptr, vector, ClearAggregates(), FlushAggregates(), HasAggregateEntries() (+6 more)

### Community 36 - "Factor Frequency Outputs"
Cohesion: 0.37
Nodes (14): Array, Builder, path, shared_ptr, string, Table, FinishOrThrow(), WriteFactorFrequencies() (+6 more)

### Community 37 - "Batch Group Writer"
Cohesion: 0.17
Nodes (13): BatchHolder, batch, Schema, vector, FileGroup, batches, first_p, last_p (+5 more)

### Community 38 - "Aggregation Options"
Cohesion: 0.17
Nodes (12): Options, arrow_threads, flush_entries, limit_k0, max_files, out_dir, p_bucket, partials_dir (+4 more)

### Community 39 - "Generation Bucket Options"
Cohesion: 0.17
Nodes (12): Options, bucket, bucket_is_new, bucket_version, chunk_primes, manifest, prime_rank_start, rest_uri (+4 more)

### Community 40 - "Arrow Array Builders"
Cohesion: 0.51
Nodes (11): Array, pp_batch_result, RecordBatch, shared_ptr, const_int32_array(), dense_int64_range(), int32_array(), int64_array() (+3 more)

### Community 41 - "Stop Monitor / Keyboard"
Cohesion: 0.24
Nodes (8): atomic, thread, StopMonitor, enabled_, original_, running_, worker_, termios

### Community 42 - "Query Service Impl"
Cohesion: 0.18
Nodes (11): path, QueryService, shared_ptr, unique_ptr, QueryService::Impl, catalog, schema_fields, schema_loaded (+3 more)

### Community 43 - "Iceberg Writer Test"
Cohesion: 0.27
Nodes (10): Array, RecordBatch, Schema, shared_ptr, string, T, Check(), FinishOrDie() (+2 more)

### Community 44 - "TUI + Lua Design Docs"
Cohesion: 0.22
Nodes (10): Lua API (implemented) — preset DSL + query reader module, pp Lua shell (binds query module to live warehouse), Preset / config DSL (query(id,spec), config(tbl)), query Lua module proposal (api copy), query Lua module proposal (design, not-yet-built extensions), primeparts TUI application design (screens, presets, dispatch), Cancellable, progress-tracked, threaded execution, Schema-field-name value dispatch (the 'c' key, accept-sets) (+2 more)

### Community 45 - "Query Service Validation"
Cohesion: 0.33
Nodes (7): atomic, thread, ncplane, QueryService::ValidatePreset(), time_point, main(), secs_since()

### Community 46 - "Table Extent Metadata"
Cohesion: 0.20
Nodes (10): TableExtent, data_files, file_bytes, max_p, ok, row_count, sequence, snapshot_id (+2 more)

### Community 47 - "Progress Counter"
Cohesion: 0.20
Nodes (7): time_point, count, Progress, enabled_, start_, total_groups_, total_primes_

### Community 48 - "Warehouse + Catalog Docs"
Cohesion: 0.33
Nodes (9): primeparts Iceberg warehouse layout & schemas, Commit ordering — LMDB CAS last, single-writer, CommitFiles seam (create-or-load + FastAppend), LmdbCatalogStore (CatalogStore over liblmdb), Native IRC catalog (LMDB-backed SqlCatalog), pp-catalogd native IRC server (cpp-httplib), HANDOFF living-state doc (catalog, tooling, roadmap), dioph.pp research engine (project overview) (+1 more)

### Community 49 - "Prime Obstruction Theory"
Cohesion: 0.22
Nodes (9): 22 unconditional obstruction classes mod 255255, Mod-255255 obstruction classifier (3.5.7.11.13.17), Subgroup exclusion proof (3^n not in <3> mod blocking prime), Baker bounds on linear forms in logarithms (Matveev), Catalan spine (3^2 - 2^3 = 1, Mihailescu 2002), Evertse S-unit bound (<=2 solutions per (p,q)), Phase 2: multiplicative order jump (double-exponential ejection), Planarity question — can r=1 lattice contain a K_{3,3} minor (+1 more)

### Community 50 - "Query Lua Module + REPL"
Cohesion: 0.39
Nodes (5): query reader module (pget/kget/hist/materialize/read + NT helpers), main(), ReportLuaError(), RunRepl(), Usage()

### Community 51 - "mtuple Statistics (math)"
Cohesion: 0.29
Nodes (8): m-tuple statistics per k (unshifted tuples, shifted shapes), Index-difference / S-unit relation on Mersenne factors, k-distribution fit (negative binomial, overdispersed), Magnitude-stationarity of mean_k (~1.88, band-invariant), Index differences are parity-locked (all d even), r-distribution (r = floor(log2 p) - k, misses per prime), mdiff_k{K} derived tables (p, hit_mask int64), mersenne_factors table (d, prime, exponent, ord2, is_primitive)

### Community 52 - "Bucket Writer Facade"
Cohesion: 0.29
Nodes (7): BucketParquetWriter, Close, impl_, Make, Write, Impl, unique_ptr

### Community 53 - "Primes Table + Read Paths"
Cohesion: 0.29
Nodes (7): p-scan window as predicate pushdown on p, Future sparse prime_rank->(file,rg,offset) index, Two read paths (k-scan early-stop vs direct-parquet point lookup), identity bucket partitioning (p_bucket_version, p_bucket), prime_rank = pi(p) semantics, primeparts.primes_k0 derived base table + MOR sieve clone, primeparts.primes table (p, k, prime_rank)

### Community 54 - "Additive Decomposition Math"
Cohesion: 0.29
Nodes (7): factorsums (related SageMath project), Additive decomposition of prime power remainders, pppart_collected (related l-adic/TDA project), Young tableaux decomposition (broad vs deep), Hit set H(p) and representation count k(p), Prime partition equation p = 2^m + q^n, Bit-weight filtration (bound r*log2 p, not r or m)

### Community 55 - "Sample Row Schema"
Cohesion: 0.29
Nodes (7): SampleRow, exponent, m, p, p_bucket, q, remainder

### Community 56 - "Query Layer + Build Docs"
Cohesion: 0.33
Nodes (6): native/configure provisioning script, Native build & rootless provisioning (BUILD.md), TUI data-query layer design (QueryService, two read paths), QueryService (LookupPrime/LookupPartitions/ScanByK/GroupCount), TUI data-query layer design (tui copy), Native binaries inventory (generate, sieve, catalogd, tui, pp)

### Community 57 - "Query Literal Bounds"
Cohesion: 0.33
Nodes (4): Literal, map, vector, PutBound()

### Community 58 - "TUI Query Worker"
Cohesion: 0.33
Nodes (6): Preset, time_point, fval(), secs_since(), Preset, run_query_worker()

### Community 59 - "Covering Filter + NT Core"
Cohesion: 0.60
Nodes (5): Backbone {3,5,7} with orders {2,4,3}, period 12, Covering-system characterization of obstructed primes, Modular covering filter (native hot-path optimization), Forward-seeded table / backward-search oracle, Number-theory core (primecount/primesieve/FLINT/GMP/PARI)

### Community 60 - "l-adic Residue Analysis"
Cohesion: 0.67
Nodes (4): Burhanuddin thesis (2007) — l-adic BSD computations, l-adic power-residue symbol analysis, Fixed-modulus / Hensel-lifting obstruction depth, Algebraic-geometry / etale-cohomology formalization (speculative)

### Community 61 - "Partition Tuple Schema"
Cohesion: 0.50
Nodes (4): PartitionTuple, m_k, n_k, q_k

### Community 62 - "Prime Info Schema"
Cohesion: 0.50
Nodes (4): PrimeInfo, k, p, prime_rank

### Community 63 - "Group Count Row"
Cohesion: 0.67
Nodes (3): GroupCountRow, count, value

### Community 64 - "Scan Hit Row"
Cohesion: 0.67
Nodes (3): ScanHit, p, prime_rank

## Knowledge Gaps
- **396 isolated node(s):** `warehouse`, `host`, `port`, `rest_uri`, `rest_name` (+391 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **5 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `App` connect `TUI App State` to `Source Table Reader`, `Generate CLI Args`, `Lua Query Presets`, `Query Service Validation`, `TUI Query View`?**
  _High betweenness centrality (0.002) - this node is a cross-community bridge._
- **Why does `WriterConfig` connect `Writer Config + Partition Spec` to `Written File Metadata`?**
  _High betweenness centrality (0.001) - this node is a cross-community bridge._
- **Why does `BucketParquetWriter::Impl` connect `Parquet Writer Impl` to `Query Literal Bounds`, `Writer Config + Partition Spec`, `Written File Metadata`, `Parquet Data File Writer`?**
  _High betweenness centrality (0.001) - this node is a cross-community bridge._
- **What connects `warehouse`, `host`, `port` to the rest of the system?**
  _398 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Prime Generator + Lua Core` be split into smaller, more focused modules?**
  _Cohesion score 0.06254272043745727 - nodes in this community are weakly interconnected._
- **Should `TUI App State` be split into smaller, more focused modules?**
  _Cohesion score 0.029850746268656716 - nodes in this community are weakly interconnected._
- **Should `Source Table Reader` be split into smaller, more focused modules?**
  _Cohesion score 0.05117845117845118 - nodes in this community are weakly interconnected._