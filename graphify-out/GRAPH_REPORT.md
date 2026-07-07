# Graph Report - .  (2026-07-06)

## Corpus Check
- 61 files · ~61,082 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1194 nodes · 2444 edges · 61 communities
- Extraction: 96% EXTRACTED · 4% INFERRED · 0% AMBIGUOUS · INFERRED: 104 edges (avg confidence: 0.8)
- Token cost: 32,684 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Core C Prime Generator + Lua Query|Core C Prime Generator + Lua Query]]
- [[_COMMUNITY_TUI App State|TUI App State]]
- [[_COMMUNITY_Catalogd Server Options|Catalogd Server Options]]
- [[_COMMUNITY_LMDB Catalog Store|LMDB Catalog Store]]
- [[_COMMUNITY_mdiff Table Builder|mdiff Table Builder]]
- [[_COMMUNITY_Lua Query Presets|Lua Query Presets]]
- [[_COMMUNITY_Tuple Cluster Analysis|Tuple Cluster Analysis]]
- [[_COMMUNITY_Tally Bucket Aggregation|Tally Bucket Aggregation]]
- [[_COMMUNITY_Iceberg Schemas + Mersenne|Iceberg Schemas + Mersenne]]
- [[_COMMUNITY_Iceberg REST Catalog Client|Iceberg REST Catalog Client]]
- [[_COMMUNITY_TUI Main + Preset Wiring|TUI Main + Preset Wiring]]
- [[_COMMUNITY_TUI Config Model|TUI Config Model]]
- [[_COMMUNITY_Parquet Writer Config|Parquet Writer Config]]
- [[_COMMUNITY_Query Service Scan Control|Query Service Scan Control]]
- [[_COMMUNITY_Catalog Module Docs|Catalog Module Docs]]
- [[_COMMUNITY_Covering Sieve Classify|Covering Sieve Classify]]
- [[_COMMUNITY_Bucket Parquet Writer Impl|Bucket Parquet Writer Impl]]
- [[_COMMUNITY_RowDelta Position-Delete|RowDelta Position-Delete]]
- [[_COMMUNITY_TUI Run-Query Flow|TUI Run-Query Flow]]
- [[_COMMUNITY_RowDelta Manifest Write|RowDelta Manifest Write]]
- [[_COMMUNITY_Primitive Factors Aggregates|Primitive Factors Aggregates]]
- [[_COMMUNITY_Manifest Generation IO|Manifest Generation IO]]
- [[_COMMUNITY_Primitive Factors Progress|Primitive Factors Progress]]
- [[_COMMUNITY_Parquet Writer Arrow Impl|Parquet Writer Arrow Impl]]
- [[_COMMUNITY_Query Service Row Types|Query Service Row Types]]
- [[_COMMUNITY_TUI Config Persistence|TUI Config Persistence]]
- [[_COMMUNITY_LMDB Smoke Test|LMDB Smoke Test]]
- [[_COMMUNITY_Source Scan Reader|Source Scan Reader]]
- [[_COMMUNITY_Covering Sieve BuildURI|Covering Sieve Build/URI]]
- [[_COMMUNITY_Query Service Facade|Query Service Facade]]
- [[_COMMUNITY_Written-File Metadata|Written-File Metadata]]
- [[_COMMUNITY_Catalogd Smoke Test|Catalogd Smoke Test]]
- [[_COMMUNITY_TUI Generate View|TUI Generate View]]
- [[_COMMUNITY_Primitive Factors Coverage|Primitive Factors Coverage]]
- [[_COMMUNITY_Factor Frequency Write|Factor Frequency Write]]
- [[_COMMUNITY_Batch Holder + FileGroup|Batch Holder + FileGroup]]
- [[_COMMUNITY_Arrow Init + Source Decode|Arrow Init + Source Decode]]
- [[_COMMUNITY_Primitive Factors Main|Primitive Factors Main]]
- [[_COMMUNITY_Primitive Factors Options|Primitive Factors Options]]
- [[_COMMUNITY_Generate Options|Generate Options]]
- [[_COMMUNITY_Source Scan File Info|Source Scan File Info]]
- [[_COMMUNITY_Generate Arg Parsing|Generate Arg Parsing]]
- [[_COMMUNITY_Covering Sieve Options|Covering Sieve Options]]
- [[_COMMUNITY_Factor Count Pairs|Factor Count Pairs]]
- [[_COMMUNITY_Arrow Array Builders|Arrow Array Builders]]
- [[_COMMUNITY_Generate Stop Monitor|Generate Stop Monitor]]
- [[_COMMUNITY_Query Service Catalog Load|Query Service Catalog Load]]
- [[_COMMUNITY_Table Extent Stats|Table Extent Stats]]
- [[_COMMUNITY_Source Table Reader|Source Table Reader]]
- [[_COMMUNITY_pp-catalog Main Args|pp-catalog Main Args]]
- [[_COMMUNITY_Parquet File Processing|Parquet File Processing]]
- [[_COMMUNITY_Generate Progress|Generate Progress]]
- [[_COMMUNITY_Sieve Clone + URI|Sieve Clone + URI]]
- [[_COMMUNITY_Bucket Writer Facade|Bucket Writer Facade]]
- [[_COMMUNITY_Mersenne Helper Ord2|Mersenne Helper Ord2]]
- [[_COMMUNITY_Factor Sample Row|Factor Sample Row]]
- [[_COMMUNITY_Materialize Int Columns|Materialize Int Columns]]
- [[_COMMUNITY_Source Scan Task Reader|Source Scan Task Reader]]
- [[_COMMUNITY_Writer File Paths|Writer File Paths]]
- [[_COMMUNITY_Mersenne Factor Struct|Mersenne Factor Struct]]
- [[_COMMUNITY_TUI Query Worker|TUI Query Worker]]

## God Nodes (most connected - your core abstractions)
1. `App` - 125 edges
2. `LmdbCatalogStore` - 33 edges
3. `Stats` - 33 edges
4. `main()` - 30 edges
5. `WriterConfig` - 27 edges
6. `run_generation()` - 26 edges
7. `BucketParquetWriter::Impl` - 26 edges
8. `lua_State` - 22 edges
9. `Catalog` - 22 edges
10. `WrittenFile` - 22 edges

## Surprising Connections (you probably didn't know these)
- `main()` --calls--> `GetOrd2`  [INFERRED]
  native/src/coverings/covering_sieve_main.cc → native/include/primeparts/coverings/primitive_factors.h
- `RowDelta::Apply()` --calls--> `WriteNewDeleteManifests`  [INFERRED]
  native/src/catalog/pp_row_delta.cc → native/include/primeparts/catalog/pp_row_delta.h
- `config_cfn()` --references--> `lua_State`  [EXTRACTED]
  native/src/tui/lua_presets.cc → native/include/primeparts/query/lua_query_module.h
- `query_cfn()` --references--> `lua_State`  [EXTRACTED]
  native/src/tui/lua_presets.cc → native/include/primeparts/query/lua_query_module.h
- `main()` --references--> `SourceFileInfo`  [INFERRED]
  native/src/coverings/primitive_factors_main.cc → native/include/primeparts/source_scan.h

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **LMDB-backed Local Catalog of Record Stack** — native_src_catalog_readme_sqlcatalog, native_src_catalog_readme_lmdbcatalogstore, native_src_catalog_readme_makelocalcatalog, native_src_catalog_readme_metadata_location_cas [EXTRACTED 1.00]
- **Covering-Sieve MOR Persistence Flow** — native_src_catalog_readme_pp_sieve_clone, native_src_catalog_readme_pp_row_delta, native_src_catalog_readme_primes_k0_sieve, native_src_catalog_readme_covering_sieve_persistence [INFERRED 0.75]

## Communities (61 total, 0 thin omitted)

### Community 0 - "Core C Prime Generator + Lua Query"
Cohesion: 0.06
Nodes (69): materialize_shared, mpz_t, lua_State, QueryService, FILE, main(), monotonic_seconds(), parse_i64() (+61 more)

### Community 1 - "TUI App State"
Cohesion: 0.03
Nodes (67): Focus, ModalKind, App, cfg, cfg_cursor, config_path, confirm_quit, cursor (+59 more)

### Community 2 - "Catalogd Server Options"
Cohesion: 0.05
Nodes (52): E, ErrorKind, json, CatalogdOptions, host, port, warehouse, string (+44 more)

### Community 3 - "LMDB Catalog Store"
Cohesion: 0.11
Nodes (36): Body, CatalogStore, MDB_dbi, MDB_env, MDB_txn, MDB_val, NamespaceProperty, function (+28 more)

### Community 4 - "mdiff Table Builder"
Cohesion: 0.07
Nodes (48): BuildMdiff(), Array, atomic, Builder, FileReader, map, pair, path (+40 more)

### Community 5 - "Lua Query Presets"
Cohesion: 0.08
Nodes (39): string, vector, QueryField, name, value, QueryPreset, accepts, desc (+31 more)

### Community 6 - "Tuple Cluster Analysis"
Cohesion: 0.09
Nodes (34): Popcount(), ShapeOf(), ShiftOf(), ofstream, path, shared_ptr, string, vector (+26 more)

### Community 7 - "Tally Bucket Aggregation"
Cohesion: 0.10
Nodes (32): BucketRange, hi, lo, atomic, pair, path, shared_ptr, string (+24 more)

### Community 8 - "Iceberg Schemas + Mersenne"
Cohesion: 0.11
Nodes (29): PartitionSpec, Schema, Build(), Array, Builder, path, shared_ptr, string (+21 more)

### Community 9 - "Iceberg REST Catalog Client"
Cohesion: 0.26
Nodes (24): Catalog, DataFile, FileIO, Namespace, PartitionSpec, path, Schema, shared_ptr (+16 more)

### Community 10 - "TUI Main + Preset Wiring"
Cohesion: 0.16
Nodes (24): draw_gen_output(), binary_dir(), binary_seed_path(), built_in_presets(), path, Preset, string, vector (+16 more)

### Community 11 - "TUI Config Model"
Cohesion: 0.09
Nodes (23): deque, Field, Config, autosave, default_limit, gen_threads, log_format, log_limit (+15 more)

### Community 12 - "Parquet Writer Config"
Cohesion: 0.09
Nodes (21): vector, PartitionSpec, PartitionValues, RecordBatch, Schema, WriterConfig, bucket, bucket_version (+13 more)

### Community 13 - "Query Service Scan Control"
Cohesion: 0.19
Nodes (21): function, string, vector, ScanControl, cancel, progress, TableRows, cols (+13 more)

### Community 14 - "Catalog Module Docs"
Cohesion: 0.10
Nodes (23): The Catalog Seam, iceberg::sql::CatalogStore Interface, Covering-Sieve Progress Persistence, HANDOFF.md Section 6 (Verified Phase 0/1 State), iceberg-cpp FileIO, IRC Catalog Design Doc (markdown/data_eng/irc_catalog_design.md), Vendored liblmdb, LmdbCatalogStore (+15 more)

### Community 15 - "Covering Sieve Classify"
Cohesion: 0.11
Nodes (21): pair, string, unordered_map, vector, ClassifyBatch(), CovModulus, fm, pat (+13 more)

### Community 16 - "Bucket Parquet Writer Impl"
Cohesion: 0.20
Nodes (21): BatchStats, Literal, BucketParquetWriter::BucketParquetWriter(), BucketParquetWriter::Close(), CloseCurrent, OpenIfNeeded, BucketParquetWriter::Make(), BucketParquetWriter::Write() (+13 more)

### Community 17 - "RowDelta Position-Delete"
Cohesion: 0.11
Nodes (18): DataFileSet, ManifestFile, unordered_map, vector, RowDelta, Apply, CleanUncommitted, has_new_files_ (+10 more)

### Community 18 - "TUI Run-Query Flow"
Cohesion: 0.17
Nodes (20): SaveAll, confirm_modal(), layout(), main(), option_count(), save_current(), string, confirm_dispatch() (+12 more)

### Community 19 - "RowDelta Manifest Write"
Cohesion: 0.15
Nodes (19): WriteNewDeleteManifests, ManifestFile, Result, shared_ptr, Status, string, Table, TableMetadata (+11 more)

### Community 20 - "Primitive Factors Aggregates"
Cohesion: 0.12
Nodes (19): ClearAggregates(), FlushAggregates(), HasAggregateEntries(), PaddedSeq(), Stats, backbone_covered, by_bucket, by_m (+11 more)

### Community 21 - "Manifest Generation IO"
Cohesion: 0.21
Nodes (17): pp_status_message(), append_manifest_boundary(), append_manifest_file(), ofstream, string, string_view, json_escape(), log_line() (+9 more)

### Community 22 - "Primitive Factors Progress"
Cohesion: 0.12
Nodes (17): time_point, ProgressLoop(), ProgressState, backbone_covered, candidate_remainders, done, files, k0_primes (+9 more)

### Community 23 - "Parquet Writer Arrow Impl"
Cohesion: 0.12
Nodes (16): FileOutputStream, FileWriter, BucketParquetWriter::Impl, arrow_schema, closed, config, current_record, done (+8 more)

### Community 24 - "Query Service Row Types"
Cohesion: 0.12
Nodes (15): GroupCountRow, count, value, atomic, PartitionTuple, m_k, n_k, q_k (+7 more)

### Community 25 - "TUI Config Persistence"
Cohesion: 0.23
Nodes (15): SaveConfig, apply_config_kv(), map, string, cfg_adjust(), cfg_count(), cfg_name(), cfg_value() (+7 more)

### Community 26 - "LMDB Smoke Test"
Cohesion: 0.31
Nodes (14): Error, expected, path, Result, Status, T, Check(), IsAlreadyExists() (+6 more)

### Community 27 - "Source Scan Reader"
Cohesion: 0.13
Nodes (15): FileScanTask, FileScanTaskReader, FileIO, TableMetadata, SourceTableReader::Impl, active, current_file_path, cursor (+7 more)

### Community 28 - "Covering Sieve Build/URI"
Cohesion: 0.26
Nodes (14): string, StripFileScheme(), AppliedModuli(), BuildCov(), array, shared_ptr, Table, ClassifyAccum (+6 more)

### Community 29 - "Query Service Facade"
Cohesion: 0.13
Nodes (14): Impl, unique_ptr, QueryService, Extent, GroupCount, impl_, ListTables, LookupPartitions (+6 more)

### Community 30 - "Written-File Metadata"
Cohesion: 0.13
Nodes (15): path, shared_ptr, string, WrittenFile, bucket, bucket_version, bytes, data_file (+7 more)

### Community 31 - "Catalogd Smoke Test"
Cohesion: 0.26
Nodes (14): Array, path, RecordBatch, Schema, shared_ptr, string, T, Check() (+6 more)

### Community 32 - "TUI Generate View"
Cohesion: 0.24
Nodes (14): string, vector, draw_generate(), gen_adjust(), gen_field(), gen_name(), gen_opt_count(), gen_page() (+6 more)

### Community 33 - "Primitive Factors Coverage"
Cohesion: 0.24
Nodes (12): n_factor_t, BuildMersenneHelper(), vector, FactorU64(), GetBackboneMask(), GetCoverageMask(), HitMaskDiffs(), IsBackbone() (+4 more)

### Community 34 - "Factor Frequency Write"
Cohesion: 0.37
Nodes (14): Array, Builder, path, shared_ptr, string, Table, FinishOrThrow(), WriteFactorFrequencies() (+6 more)

### Community 35 - "Batch Holder + FileGroup"
Cohesion: 0.15
Nodes (14): BatchHolder, batch, path, Schema, vector, FileGroup, batches, first_p (+6 more)

### Community 36 - "Arrow Init + Source Decode"
Cohesion: 0.22
Nodes (8): path, Schema, unique_ptr, vector, decode_int64_le(), SourceTableReader::OpenMetadata(), SourceTableReader::source_files(), SourceTableReader::SourceTableReader()

### Community 37 - "Primitive Factors Main"
Cohesion: 0.33
Nodes (11): AddPrimitiveFactorHit(), DefaultWorkerThreads(), FactorResidual(), FloorLog2(), main(), MergeStats(), ParseBucketFromPath(), ParseI64() (+3 more)

### Community 38 - "Primitive Factors Options"
Cohesion: 0.17
Nodes (12): Options, arrow_threads, flush_entries, limit_k0, max_files, out_dir, p_bucket, partials_dir (+4 more)

### Community 39 - "Generate Options"
Cohesion: 0.17
Nodes (12): Options, bucket, bucket_is_new, bucket_version, chunk_primes, manifest, prime_rank_start, rest_uri (+4 more)

### Community 40 - "Source Scan File Info"
Cohesion: 0.18
Nodes (8): Expression, string, RecordBatch, SourceFileInfo, p_max, p_min, path, record_count

### Community 41 - "Generate Arg Parsing"
Cohesion: 0.33
Nodes (8): FILE, default_temp_root(), parse_args(), parse_i32(), parse_i64(), resolve_bucket_state(), usage(), utc_timestamp_compact()

### Community 42 - "Covering Sieve Options"
Cohesion: 0.18
Nodes (11): Options, apply, classify, have_apply, metric, report, rest_uri, show_top (+3 more)

### Community 43 - "Factor Count Pairs"
Cohesion: 0.20
Nodes (9): unordered_map, CountPair, p_count, remainder_count, IntQKey, q, value, IntQKeyHash (+1 more)

### Community 44 - "Arrow Array Builders"
Cohesion: 0.51
Nodes (11): Array, pp_batch_result, RecordBatch, shared_ptr, const_int32_array(), dense_int64_range(), int32_array(), int64_array() (+3 more)

### Community 45 - "Generate Stop Monitor"
Cohesion: 0.22
Nodes (8): atomic, thread, StopMonitor, enabled_, original_, running_, worker_, termios

### Community 46 - "Query Service Catalog Load"
Cohesion: 0.18
Nodes (11): path, QueryService, shared_ptr, unique_ptr, QueryService::Impl, catalog, schema_fields, schema_loaded (+3 more)

### Community 47 - "Table Extent Stats"
Cohesion: 0.20
Nodes (10): TableExtent, data_files, file_bytes, max_p, ok, row_count, sequence, snapshot_id (+2 more)

### Community 48 - "Source Table Reader"
Cohesion: 0.20
Nodes (9): Impl, unique_ptr, SourceTableReader, file_count, impl_, Next, OpenMetadata, source_files (+1 more)

### Community 49 - "pp-catalog Main Args"
Cohesion: 0.36
Nodes (9): Args, clone_sieve, register_tables, warehouse, string, main(), ParseArgs(), RunRegister() (+1 more)

### Community 50 - "Parquet File Processing"
Cohesion: 0.20
Nodes (10): AggregateEntryCount(), atomic, FileReader, unique_ptr, vector, OpenParquetFile(), metadata, ProcessFile() (+2 more)

### Community 51 - "Generate Progress"
Cohesion: 0.20
Nodes (7): time_point, count, Progress, enabled_, start_, total_groups_, total_primes_

### Community 52 - "Sieve Clone + URI"
Cohesion: 0.39
Nodes (6): string_view, shared_ptr, string, Table, FindJsonInt(), ScanCounts()

### Community 53 - "Bucket Writer Facade"
Cohesion: 0.25
Nodes (7): BucketParquetWriter, Close, impl_, Make, Write, Impl, unique_ptr

### Community 54 - "Mersenne Helper Ord2"
Cohesion: 0.29
Nodes (7): unordered_map, vector, MersenneHelper, factors, GetOrd2, max_d, ord2_by_q

### Community 55 - "Factor Sample Row"
Cohesion: 0.29
Nodes (7): SampleRow, exponent, m, p, p_bucket, q, remainder

### Community 56 - "Materialize Int Columns"
Cohesion: 0.40
Nodes (5): path, shared_ptr, string, vector, MaterializeIntColumns()

### Community 57 - "Source Scan Task Reader"
Cohesion: 0.47
Nodes (4): RecordBatch, shared_ptr, string, SourceTableReader::Next()

### Community 58 - "Writer File Paths"
Cohesion: 0.50
Nodes (5): path, string_view, FilePathFor(), FileToken(), NextFileSeq()

### Community 59 - "Mersenne Factor Struct"
Cohesion: 0.50
Nodes (4): MersenneFactor, d, exponent, q

### Community 60 - "TUI Query Worker"
Cohesion: 0.50
Nodes (4): time_point, secs_since(), Preset, run_query_worker()

## Knowledge Gaps
- **375 isolated node(s):** `warehouse`, `host`, `port`, `rest_uri`, `rest_name` (+370 more)
  These have ≤1 connection - possible missing edges or undocumented components.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `App` connect `TUI App State` to `TUI Generate View`, `Catalogd Server Options`, `LMDB Catalog Store`, `Lua Query Presets`, `TUI Main + Preset Wiring`, `TUI Config Model`, `TUI Run-Query Flow`, `TUI Config Persistence`, `TUI Query Worker`?**
  _High betweenness centrality (0.156) - this node is a cross-community bridge._
- **Why does `mutex_` connect `LMDB Catalog Store` to `Core C Prime Generator + Lua Query`, `TUI App State`, `TUI Generate View`, `Arrow Init + Source Decode`, `mdiff Table Builder`, `Primitive Factors Main`, `Tally Bucket Aggregation`, `Generate Arg Parsing`, `TUI Config Model`, `Query Service Scan Control`?**
  _High betweenness centrality (0.102) - this node is a cross-community bridge._
- **Why does `WriterConfig` connect `Parquet Writer Config` to `Bucket Parquet Writer Impl`, `Written-File Metadata`, `Parquet Writer Arrow Impl`?**
  _High betweenness centrality (0.043) - this node is a cross-community bridge._
- **What connects `warehouse`, `host`, `port` to the rest of the system?**
  _376 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Core C Prime Generator + Lua Query` be split into smaller, more focused modules?**
  _Cohesion score 0.06015037593984962 - nodes in this community are weakly interconnected._
- **Should `TUI App State` be split into smaller, more focused modules?**
  _Cohesion score 0.029850746268656716 - nodes in this community are weakly interconnected._
- **Should `Catalogd Server Options` be split into smaller, more focused modules?**
  _Cohesion score 0.052403846153846155 - nodes in this community are weakly interconnected._