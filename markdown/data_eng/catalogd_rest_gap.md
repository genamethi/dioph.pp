# pp-catalogd vs the Iceberg REST spec — gap audit & consolidation plan

Audit of `native/src/catalog/pp_catalogd.cc` route surface against the vendored
OpenAPI spec at `native/vendor/iceberg-refs/rest-catalog-open-api.yaml` (5789
lines; anchors below are line numbers of each `path:` entry in that file).
Written 2026-07-07. Companion docs: `irc_catalog_design.md`,
`iceberg_data_setup.md`, `../misc/HANDOFF.md`.

Motivating observation: every place a tool calls
`SourceTableReader::source_files()` and string-parses `/p_bucket=` out of a
path (`mdiff_main.cc:432`, `tally_main.cc:315`) is client-side scan planning
done by hand. The spec's planning API (`planTableScan` / `fetchScanTasks`)
returns `FileScanTask`s carrying partition values, residual filters, and
delete files from the manifests the catalog already owns — no tool ever needs
a path. The planner is the disease-level fix; the Stage 1 items below clean
the symptoms and give it a stable base.

## Exposed today (13 ops + 1 stub)

`getConfig`, `listNamespaces` (`?parent=`), `createNamespace`,
`loadNamespaceMetadata`, `dropNamespace`, `updateProperties`, `listTables`,
`createTable`, `registerTable`, `loadTable`, `updateTable`, `dropTable`
(`?purgeRequested=`), `renameTable` — `pp_catalogd.cc:209-406`.
`reportMetrics` is a blind 204 stub (`pp_catalogd.cc:405`).

Routes are bare `/v1/...` with no `{prefix}` segment (legal while `getConfig`
returns no prefix; locks that in).

## Tracker

Stages: **S1** = cleanup + atomic commit + direct bucketing (current focus).
**S2** = planner (PAUSED — do not start until S1 is verified and reviewed).
**S3** = later / opportunistic. **N/A** = local-fs deployment makes it moot.

| Operation | Route | Spec | catalogd | Stage | Status |
|---|---|---|---|---|---|
| getConfig | `GET /v1/config` | yaml:65 | exposed | — | done |
| getToken | `POST /v1/oauth/tokens` | yaml:181 | missing (deprecated in spec) | N/A | skip |
| listNamespaces | `GET .../namespaces` | yaml:250 | exposed; no pagination | S3 | partial |
| createNamespace | `POST .../namespaces` | yaml:250 | exposed | — | done |
| loadNamespaceMetadata | `GET .../namespaces/{ns}` | yaml:351 | exposed | — | done |
| namespaceExists | `HEAD .../namespaces/{ns}` | yaml:351 | missing | S1 | todo |
| dropNamespace | `DELETE .../namespaces/{ns}` | yaml:351 | exposed | — | done |
| updateProperties | `POST .../{ns}/properties` | yaml:460 | exposed | — | done |
| listTables | `GET .../{ns}/tables` | yaml:525 | exposed; no pagination | S3 | partial |
| createTable | `POST .../{ns}/tables` | yaml:525 | exposed; no `stage-create` | S1 | partial |
| listFunctions | `GET .../{ns}/functions` | yaml:625 | missing | S3 | todo |
| loadFunction | `GET .../functions/{function}` | yaml:664 | missing | S3 | todo |
| planTableScan | `POST .../tables/{t}/plan` | yaml:707 | missing | S2 | todo |
| fetchPlanningResult | `GET .../plan/{plan-id}` | yaml:796 | missing | S2 | todo |
| cancelPlanning | `DELETE .../plan/{plan-id}` | yaml:796 | missing | S2 | todo |
| fetchScanTasks | `POST .../tables/{t}/tasks` | yaml:919 | missing | S2 | todo |
| registerTable | `POST .../{ns}/register` | yaml:971 | exposed | — | done |
| loadTable | `GET .../tables/{t}` | yaml:1027 | exposed; no `?snapshots=`, no ETag/If-None-Match | S3 | partial |
| updateTable | `POST .../tables/{t}` | yaml:1027 | exposed | — | done |
| dropTable | `DELETE .../tables/{t}` | yaml:1027 | exposed | — | done |
| tableExists | `HEAD .../tables/{t}` | yaml:1027 | missing | S1 | todo |
| unregisterTable | `POST .../tables/{t}/unregister` | yaml:1302 | missing (metadata-only detach; relevant to `primes_k0_sieve` shallow clone) | S3 | todo |
| loadCredentials | `GET .../tables/{t}/credentials` | yaml:1352 | missing | N/A | skip |
| signRequest | `POST .../tables/{t}/sign` | yaml:1398 | missing | N/A | skip |
| renameTable | `POST /v1/tables/rename` | yaml:1432 | exposed | — | done |
| reportMetrics | `POST .../tables/{t}/metrics` | yaml:1496 | 204 stub | S3 | stub |
| commitTransaction | `POST /v1/{prefix}/transactions/commit` | yaml:1540 | exposed (atomic multi-table via `store->RunInTransaction`) | — | done |
| listViews | `GET .../{ns}/views` | yaml:1657 | missing | S3 | todo |
| createView | `POST .../{ns}/views` | yaml:1657 | missing | S3 | todo |
| loadView | `GET .../views/{view}` | yaml:1743 | missing | S3 | todo |
| replaceView | `POST .../views/{view}` | yaml:1743 | missing | S3 | todo |
| dropView | `DELETE .../views/{view}` | yaml:1743 | missing | S3 | todo |
| viewExists | `HEAD .../views/{view}` | yaml:1743 | missing | S3 | todo |
| renameView | `POST /v1/views/rename` | yaml:1956 | missing | S3 | todo |
| registerView | `POST .../{ns}/register-view` | yaml:2020 | missing | S3 | todo |
| `{prefix}` route segment | all | yaml:250+ | not parsed | S3 | todo |

Views note: `QueryService::Materialize`'s MV cache (replace-semantics
`primeparts.<name>` tables) is a hand-rolled view mechanism; the spec's view
surface is its principled replacement. Defer until a consumer needs it.

## Stage 1 — cleanup, direct bucketing, atomic commit

Goal: shore up what exists; give the planner a clean base. Three interlocking
moves.

### 1a. Gut bucket logic out of `generate.cc`

Everything below duplicates what `writer.cc` / `schemas.cc` already own, or
feeds a mechanism being retired:

- **Row stamping**: `make_primes_batch` / `make_partitions_batch`
  (`generate.cc:429-477`) materialize `p_bucket_version` / `p_bucket` as
  constant int32 arrays per row; the writer's `Write()` projects by name and
  drops them (`writer.cc:369-374`) because identity-partition values live in
  the manifest partition tuple only (`iceberg_data_setup.md:114-135`).
  Delete the columns and the hand-built `arrow::schema` literals (they
  duplicate `PrimesSchema()` / `PartitionsSchema()` field order by comment).
- **Hive path construction**: `generate.cc:745-751` builds
  `p_bucket_version=N/p_bucket=M`. Move sub-path derivation into the writer
  (it already has `bucket_version`/`bucket` in `WriterConfig` and encodes
  them in filenames); `mdiff` staging follows the same helper.
- **`files.jsonl` manifest**: retire. Commit already flows through in-memory
  `DataFile`s + `CommitFiles` (`irc_catalog_design.md:239-241` calls the JSONL
  "the dead text bridge"). The stale header comment in `generate.cc:4-6`
  claiming the commit step consumes it goes too. `--manifest` flag, manifest
  plumbing in `pp_gen_options`, and `append_manifest_*` all deleted.
- **Boundary JSONL sidecar + `funbuns.*` naming**: retired by 1b. Rename the
  remaining `funbuns` references (`generate.cc:97,262`,
  `generate.h:24,35`) — `funbuns.*` was retired
  (`iceberg_data_setup.md:167-170`).
- **`PRIMEPARTS_BUCKET*` env surface + `resolve_bucket_state`**: retired by 1b
  (bucket becomes derived, not coordinator-supplied). `bucket_is_new` and
  `--bucket-is-new` die with the boundary row.

### 1b. Direct bucketing (byte-driven, partitions-first)

The `boundaries` table was an improvisation to keep buckets a consistent
size; a fixed primes-per-bucket constant would be the same row-count estimate
wearing a different hat. Be direct: **byte thresholds, snapped upward to
whole primes, with `partitions` — the byte-dominant table — setting the shape
and `primes` mirroring it.**

Shape hierarchy (one knob: the `partitions` row-group byte target):

- **Row group = the snap unit.** A row group closes at the first prime
  boundary at-or-after the byte target (soft threshold — overshoot preferred
  to undershoot). No `p` ever straddles a row group in `partitions`;
  `primes` cuts on the exact same p-spans, so the two tables' row groups are
  aligned windows over p.
- **File = fixed row-group count.** 4 (or 8 — settle at implementation
  against the existing 240M-row/~256MiB default, `writer.cc:80-82`) row
  groups per file; `primes` files land near 1 GiB, `partitions` files ~2-4x
  that (mean k ≈ 1.9 partition rows per prime, wider schema).
- **Bucket = byte threshold over whole files.** A bucket closes at the first
  file boundary at-or-after its byte threshold; the last file may overshoot.
  Overflow rows spill into the next bucket.

Mechanics — estimate the cuts, calibrate on flush:

- Parquet compressed size is only known when a row group flushes, so cut
  decisions run on a **byte estimate recalibrated from actual flushed bytes**
  (`WrittenFile.bytes` / per-column sizes already flow back via
  `BuildDataFile`). For this schema (fixed-width ints, DELTA_BINARY_PACKED +
  zstd) bytes/row is near-deterministic at any point in the census but drifts
  slowly upward as prime gaps grow (~ln p) — which is exactly why a running
  byte calibration beats any fixed row count.
- The dispatch point is early and purely numeric, and it lives on the seam
  side (see "Separation of concerns" below), not in `generate`: the atom
  layer recovers per-prime row counts as run-lengths of the key column in the
  batches it is handed (`prime_k[]` is exactly that encoding — the generator
  carries no knowledge the data doesn't), then cumulative-byte prefix sums ×
  calibrated bytes/row mark every row-group/file/bucket cut. Batches are
  re-chunked at cut ranks with zero-copy `RecordBatch::Slice`. No added
  serialization; overflow rides into the next window.
- `BucketParquetWriter` grows one dumb primitive: an explicit row-group cut.
  Today row groups close by row count inside the parquet writer
  (`max_row_group_length`, `writer.cc:82`); instead set it effectively
  unbounded so the internal cutter never fires, and expose an
  `end_row_group` flag on `Write` (or `CutRowGroup()`) that calls
  `parquet::arrow::FileWriter::NewBufferedRowGroup()` after the batch lands.
  Policy stays out of this class. Memory note: a buffered row group holds its
  compressed pages until flush, so resident-write memory scales with the
  partitions row-group byte target; gutting the stamped bucket columns (1a)
  buys some of that back.
- `p` / `prime_rank` / `q_k` stay DELTA_BINARY_PACKED with dictionary off —
  already wired (`generate.cc:497` delta_columns → `writer.cc:83-87`);
  monotone-within-row-group makes delta near-optimal.
- Parquet files are immutable once the footer is written — no appends. A
  bucket only ever grows by whole new files; nothing is reopened.

Separation of concerns — `AlignedBucketWriter` (seam-side facade):

All shape logic — atom detection, byte accounting, slicing, row-group /
file / bucket cuts, bucket assignment, DataFile accumulation — lives in a
facade in the seam library (beside `writer.h` / `pp_iceberg_rest`), between
clients and `BucketParquetWriter`. `generate` streams batches in prime order
and declares its atom key; it never slices, never prefix-sums, never sees a
bucket number. Specs are data, not subclasses:

```cpp
struct AtomKey   { std::string column; };   // monotone; equal values = one indivisible atom
struct BoundTable {
  std::string name;
  std::shared_ptr<iceberg::Schema> schema;
  std::vector<std::string> delta_columns;
  double bytes_per_row_prior;               // seed; recalibrated from flushed row groups
};
struct ShapePolicy {
  std::string driver;                       // byte-dominant table sets the cuts ("partitions")
  int64_t rg_target_bytes;                  // soft, overshoot-preferred
  int32_t rgs_per_file;                     // 4
  int64_t bucket_target_bytes;              // closes on file boundary
};

class AlignedBucketWriter {
  static Make(warehouse, std::vector<BoundTable>, AtomKey, ShapePolicy, std::string* error);
  bool Append(std::string_view table, const arrow::RecordBatch&, std::string* error);
  bool Finish(CommitPlan* out, std::string* error);  // per-table DataFiles + rg hints → CommitFilesAtomic
};
```

Cut rule, fully generic: *never cut inside an atom; close a row group at the
first atom boundary at-or-after the driver's byte target.* The facade
computes the cut sequence once, on the driver table, and applies the
identical sequence to every bound table — alignment stops being an invariant
to verify and becomes one that cannot be violated.

The sheaf reading: the p-axis is the base space; the cut sequence is a choice
of cover (the rg/file/bucket windows); each `BoundTable` is a sheaf whose
shaped files are its sections over the cover. One cover, many sheaves — the
gluing condition holds by construction, and the gluing data (window → rg
hint, per bucket_version) is exactly what the catalog records and the S2
planner serves. The functors are general (`AtomKey` + `ShapePolicy` fit any
table family sharing a monotone key); all specialization lives in the specs.

Reuse: `mdiff_k{K}` binds as `BoundTable{atom=p}` into the same cover as
primes/partitions — planner assumptions extend for free (one rg hint answers
every bound table over a p-window). `mersenne_factors` binds with
`AtomKey{d}` as its own single-table set — degenerates to a plain bucketed
writer. iceberg-cpp groundwork stays maximal: identity `PartitionSpec`,
`DataFile` partition tuples + stats, FastAppend/commitTransaction transport
everything; the facade only decides numbers.

Catalog side:

- Bucket boundaries are now data-dependent, but their record is the
  manifests: every `DataFile` carries `(p_bucket_version, p_bucket)` in its
  partition tuple plus p/rank min-max stats. `boundaries` is dropped, not
  migrated; a bucket map is a manifest aggregation (later: one
  `planTableScan`). Resume reads the open bucket's fill level the same way —
  sum of its files' bytes.
- New writes go out as `p_bucket_version=2`; v1 files keep their manifest
  tuples and stay readable side by side (`iceberg_data_setup.md:189-197`).
- Anti-join alignment (k0 counts, mdiff-style passes): aligned row groups
  give 1:1 window correspondence between the tables — stats-pruned streaming
  merge, no straddled p, bounded memory. Caveat: iceberg-cpp's table scan
  does not prune row groups (`tui_query_design.md:75-81`), so today only the
  direct-parquet read path exploits the alignment; the S2 planner closes that
  gap (`FileScanTask` can carry row-group `split_offsets`, currently
  unpopulated in `BuildDataFile`).
- Coordinator role shrinks to what it really was: supply `prime_rank_start`
  (resume cursor). No bucket picking, no fill-level table.

### 1c. Atomic two-table commit

Replace the ordered-commit workaround (partitions before primes,
`irc_catalog_design.md:203-209`) with the spec's `commitTransaction`:

- **Server**: `POST /v1/transactions/commit` (yaml:1540) — parse per-table
  `requirements[]`/`updates[]`, apply through the engine, swap **both
  `metadata_location` head pointers in one LMDB write txn**. This is the
  clean fit with LMDB: one txn, one publish, no window where primes are
  committed and partitions are not. `createTable` gains `stage-create`
  handling (staged table participates in the transaction instead of being
  created eagerly).
- **Client**: one wrapped call, `CommitFilesAtomic(catalog, warehouse,
  {table, schema, spec, files}...)` in `pp_iceberg_rest.{h,cc}` beside
  `CommitFiles` — stages all tables' parquet, moves files in, builds one
  transaction body, single POST (REST) or single LMDB txn (in-process
  fallback — both modes keep identical semantics).
- `generate` end-of-run becomes: accumulate `DataFile`s per table (already
  does) → one `CommitFilesAtomic({partitions, primes})`. Ordered-commit rule
  and its resume caveats deleted from docs after landing
  (`HANDOFF.md`, `iceberg_data_setup.md:217-224`).

### Stage 1 verification

Installed binaries live in `~/.local/bin` (Makefile install target default):
`primeparts-generate`, `primeparts-catalogd`, `pp-catalog`, `primeparts-tui`,
etc. Smoke: `primeparts-generate --temp` (files-only), then a real run against
a scratch warehouse via in-process catalog and via `--rest-uri` to a local
`primeparts-catalogd`, verifying (1) no bucket columns in physical parquet,
(2) manifest partition tuples carry `p_bucket_version=2` buckets with
consistent p/rank min-max stats, (3) one snapshot per table sharing one
transaction, (4) no `p` straddles a `partitions` row group, primes/partitions
row-group p-spans identical, ~4 row groups per file, `primes` files ≈ 1 GiB,
(5) resume from the committed frontier still works, reading bucket fill from
manifests.
Existing smokes to update: `generate_smoke`, `pp_catalogd_smoke` (drops its
hand-built `p_bucket_version=1/p_bucket=2` staging path,
`pp_catalogd_smoke.cc:90`).

## Stage 2 — planner (PAUSED)

Do not start until Stage 1 is landed, verified, and reviewed.

`planTableScan` + `fetchPlanningResult` + `cancelPlanning` + `fetchScanTasks`
(yaml:707-970) server-side, feeding manifest-derived `FileScanTask`s
(partition values, residuals, delete files). Client side: retire
`source_files()` consumers and both `ParseTagFromPath` copies; `mdiff` /
`tally` shard from plan tasks instead of paths; point-lookup file selection
comes from a plan instead of `p_min`/`p_max` scraping.

**Graduated depth — the planner may go below the file level.** iceberg-cpp's
scan machinery stops at manifest/file granularity; row-group awareness is not
wired into its read path, and nothing in the IRC spec requires it to be —
*how* a planner computes tasks is intentionally unspecified (a black box left
to the implementer). So the planner is where we hack in the lower-level
parquet operations we forbid everywhere else: the catalog interface touches
data files directly — acceptable precisely because it happens inside the
catalog boundary, so no client ever has to. The seam rule inverts here: tools
never touch files *because* the planner may.

Concretely, graduated in three steps as need appears:
1. **Manifest-only** (day one): tasks from partition tuples + p/rank column
   stats — already enough to kill path parsing.
2. **Row-group hints, write-time**: we write every file, so the writer can
   record per-row-group `(p_min, p_max, rank_min, rank_max, byte_offset)` at
   commit time — populate `DataFile.split-offsets` (spec-shaped, yaml:4998,
   rides inside `FileScanTask`'s data-file, yaml:5176) and keep the rg-level
   stats in catalog-side metadata (LMDB, beside the head pointer). No footer
   re-reads ever; the 1b aligned-row-group layout is what makes these hints
   potent (one rg hint per table pair covers the same p-window).
3. **Footer reads, server-side** (only if 2 proves insufficient for foreign
   files, e.g. v1 buckets): planner opens footers, caches parsed row-group
   metadata in LMDB keyed by file path + length.

Client consumption: arrow's parquet `FileFragment` accepts an explicit
row-group selection, so plan tasks translate directly to
rg-constrained direct reads — the blessed direct-parquet path
(`tui_query_design.md:75-81`) becomes plan-directed instead of hand-aimed.

## Horizon notes (post-S2, unscheduled)

- **ib-staging → production warehouse.** Data shaping (1b) is motivated to a
  degree by read performance alone; pushing much further means committing to
  a genuinely production warehouse posture. Not yet.
- **The 64-bit barrier.** Generation sustains ~5M primes/s and will improve
  (writer speedup + resident-memory cuts in S1 are part of that push);
  p will cross 2^63/2^64 eventually. That forces whole-schema evolution
  (int64 p/q_k → decimal/fixed/hi-lo split), a new `p_bucket_version`, and a
  re-audit of every delta-packed column. Design when approached, not now —
  but every S1/S2 choice should avoid baking int64-p assumptions into
  catalog-side metadata (rg-hint keys, plan filters).

## Stage 3 — the rest

HEAD existence routes if not folded into S1, pagination, `?snapshots=`,
ETag/If-None-Match, `{prefix}` support, real metrics sink, `unregisterTable`,
functions, views (MV replacement). Each is independent; pick up as needed.
