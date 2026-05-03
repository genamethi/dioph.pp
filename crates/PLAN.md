# Plan: `primeparts-commit` (Rust) — slice B′

## Status as of 2026-05-02

Implemented and committed in `015a497`:

- `primeparts-commit` crate exists with manifest parsing, footer validation,
  warehouse-standing prompt/settings, SqlCatalog append, snapshot property
  aggregation, and HMS lazy-sync code.
- `primeparts-core` exists with shared parquet footer/DataFile helpers.
- Vendor patch machinery exists and the Rust HMS code is written against
  `get_table_req`.
- `pixi run commit-build` / `cargo test -p primeparts-commit` build the crate.

Current production status:

- The Rust commit path is **not** the production commit path yet. The real
  append path remains `python -m primeparts.native_iceberg`.
- The Rust `SqlCatalog` path now has an explicit `LocalFsStorageFactory`, but
  it still does not interoperate with the current PyIceberg-created SQLite
  catalog schema: it fails with `no such column: iceberg_type` before it can
  append files or run HMS sync.
- The Python path remains the source of truth for registering files into
  `catalog.db`; `scripts/sync_hms.py` remains the sync-only tool for HMS.
- A one-shot `primeparts-compact` crate was added after this plan and used to
  rewrite the production warehouse into `p_trunc=10^10` partitions and ~1 GiB
  ZSTD files. That compact crate is operational but not part of this original
  slice-B′ plan.

Decision points left open:

- Decide whether to adapt Rust to PyIceberg's existing SQLite schema, migrate
  the catalog DB to the Rust SQL catalog schema, or keep Rust commit as a
  future-only path for fresh catalogs.
- Decide whether Rust needs a sync-only HMS subcommand. The current Rust HMS
  implementation is only reached after a successful Rust catalog append.
- Update `warehouse_standing()` for the compacted `p_trunc=...` layout before
  using production native generation again; the old health check assumes
  `commit_seq=...` partition directories.

## Background

**Two-catalog topology.** primeparts maintains two iceberg catalogs in
parallel: a SqlCatalog over `catalog.db` (sqlite) is the source of truth for
metadata, and a HiveCatalog over Hive Metastore (Thrift) is a secondary,
lazy-synced index that HS2/Tez/MR3 query against. Today
`scripts/sync_hms.py` performs the sync after each commit by reading
`metadata_location` from sqlite and altering HMS table parameters. The new
`primeparts-commit` binary supersedes that script: it does the sqlite
write *and* the HMS sync inline, but keeps the lazy-sync semantics — sqlite
is committed first; HMS sync failure is recoverable, never blocking.

**Why slice B′ (and not A′ — eliminate pyiceberg in v1).**
`native/vendor/iceberg-rust/bindings/python/` ships `pyiceberg-core`, but
that crate is pyiceberg's *internal* perf accelerator (exposing only
`manifest`, `transform`, `datafusion_table_provider` modules); it does not
expose `Catalog`, `Table`, `Transaction`, or `add_files`. So pyiceberg can't
be removed simply by switching imports. Removing it requires porting
`warehouse_standing` (and the recovery/repair scripts) directly to Rust.
That's deferred to v2. v1 ships the Rust commit binary; the
warehouse-standing pre-check still subprocesses today's pyiceberg-backed
code at a clean seam.

**Existing pyiceberg patches** (`src/primeparts/_patches.py`). The two
vendor patches in this plan mirror these — same bugs, same fixes, ported to
Rust.

> *`_patch_pyiceberg_sort_order_id`*: pyiceberg 0.11.1 hardcodes
> `sort_order_id=None` in `parquet_file_to_data_file`. Every file
> registered via `Table.add_files(...)` lands in the manifest stamped as
> unsorted even when the writer pre-sorts rows. Downstream SQL engines
> then plan redundant sort steps on scans and MV rebuilds. We stamp each
> new DataFile with the table's `default_sort_order_id` after construction.

> *`_patch_hive_metastore_get_table`*: pyiceberg 0.11.1's `HiveCatalog`
> calls the deprecated Thrift method `get_table(dbname, tbl_name)`. Hive
> Metastore 4 no longer exposes that method and raises
> `TApplicationException: Invalid method name: 'get_table'`. The newer
> `get_table_req(GetTableRequest)` is already present in pyiceberg's
> bundled `hive_metastore` IDL, so we redirect the old call to it.

**Auto-loaded memory pointers.** Three feedback memories load automatically
via `~/.claude/projects/.../memory/MEMORY.md`:
`feedback_terminology` (call (m_k,n_k,q_k) tuples *partitions* in prose;
"decompositions" is the table name only), `feedback_user_directed_exploration`
(when user points at files/lines, read those — don't fan-out grep), and
`feedback_python_use_principle` (Python at clean seams; not minimized
dogmatically).

## Scope

Replace `commit_native_manifest` (`src/primeparts/native_iceberg.py:1-255`) with a
Rust binary. JSONL manifest from `primeparts-generate` → iceberg metadata write
via vendored `iceberg-rust` → lazy HMS sync.

Pyiceberg stays at the warehouse-standing pre-check seam (slice B′).
`_patches.py` keeps applying to that path. Removing the conda dep is v2 work,
not v1.

## CLI contract

```
primeparts-commit --manifest <jsonl> --warehouse <root>
                  [--sqlite <uri>] [--hms <thrift-uri>]
                  [--skip-hms] [--warehouse-standing={ask,run,skip}]
                  [--dry-run]
```

Defaults match `sync_hms.py` env vars: `FUNBUNS_CATALOG_URI`,
`FUNBUNS_HMS_URI`, `FUNBUNS_WAREHOUSE`.

Pipeline:

1. Parse JSONL.
2. Verify each parquet's `metadata.num_rows` and footer KV
   (`funbuns.{table,commit_seq,p_min,p_max,n_rows,n_primes}`) against JSONL claims.
3. Warehouse-standing pre-check, gated by setting (see *Settings*).
4. Compute snapshot props: `funbuns.{max_p,max_commit_seq}` per touched table.
5. Group files by table → build DataFiles via `parquet_to_data_file_builder`
   (vendor-patched to `pub`) → stamp
   `sort_order_id = default_sort_order_id` → append via SqlCatalog Transaction
   with snapshot summary properties.
6. Lazy HMS sync: load `metadata_location` from sqlite, alter HMS table
   parameters via `get_table_req` + `alter_table_with_environment_context`.
   Best-effort — exit 5 on failure with resume hint.

Exit codes:
0 ok · 2 verify fail · 3 warehouse-standing fail · 4 catalog conflict ·
5 HMS sync fail (sqlite already committed; rerun with the same args to retry HMS only).

## JSONL contract — reused as-is

Verified `generate.cc:693-701`. `append_manifest` emits identical shape for
both tables:

```json
{"table":"primes","path":"...","commit_seq":N,"rows":N,"p_min":N,"p_max":N,"bytes":N}
```

- One row per parquet *file*, not per data row. A typical generation run
  produces N files (single digits to hundreds), so the JSONL is tiny.
- Both tables share the shape because the rich per-file stats live in the
  parquet footer KV (`funbuns.k_histogram` for partitions,
  `funbuns.n_primes` for primes). JSONL's `(p_min, p_max, rows)` exists
  to serve as a redundant verification target during commit.
- Conclusion: JSONL stays. No format change, no separate partitions emit.

## Workspace layout

```
crates/                            — Cargo workspace at repo root
  Cargo.toml                       — workspace manifest
  PLAN.md                          — this file
  primeparts-commit/
    Cargo.toml
    src/
      main.rs                      — CLI, exit codes, orchestration
      manifest.rs                  — JSONL parse + verify_against_footer
      footer_kv.rs                 — read funbuns.* parquet KV
      catalog.rs                   — SqlCatalog open + add-files transaction
      hms_sync.rs                  — Thrift alter (sync_hms.py port)
      snapshot_props.rs            — max_p / max_commit_seq aggregation
      prompt.rs                    — confirmation prompt + subprocess spawn
      settings.rs                  — load ~/.config/primeparts/config.toml
  primeparts-core/                 — shared lib for commit + future compact
    Cargo.toml
    src/
      lib.rs
      data_file.rs                 — parquet → DataFile w/ sort_order_id
      table_paths.rs               — warehouse path / commit_seq conventions
  vendor-patches/
    iceberg-rust.patch             — checked-in, applied by build system
```

## Vendor patches — applied by build system

Patches live as a checked-in `.patch` file, applied idempotently before
`cargo build`. Each patch is documented inline with its rationale and the
upstream-fix-condition that allows removal.

`crates/vendor-patches/iceberg-rust.patch` contains:

**Patch A** — `crates/iceberg/src/writer/file_writer/parquet_writer.rs:350`.
Flip `pub(crate)` → `pub` on `parquet_to_data_file_builder`; remove
`#[allow(dead_code)]`.

> Why: pyiceberg `_patch_pyiceberg_sort_order_id` reapplies. The Rust
> `DataFileBuilder` chain in `parquet_to_data_file_builder` (lines 399-421)
> never calls `.sort_order_id(...)`, so files registered via the iceberg-rust
> equivalent of `add_files` land in the manifest stamped as unsorted. Same
> downstream consequence as in pyiceberg: SQL engines plan redundant sorts.
> We don't flip the wrapper `parquet_files_to_data_files`; we write our own
> per-file loop in `primeparts-core::data_file` so we can call
> `.sort_order_id(default_sort_order_id)` between builder and `.build()`.
> Remove when upstream lands a builder that sets it from `TableMetadata`.

**Patch B** — `crates/catalog/hms/src/catalog.rs:553,632,669`. Swap
`.get_table(db, tbl)` calls → `.get_table_req(GetTableRequest { ... })?.table`.

> Why: HMS 4 dropped the deprecated `get_table` Thrift method. Same fix as
> `_patch_hive_metastore_get_table` in pyiceberg. Remove when upstream
> migrates to `get_table_req` natively.

Application mechanism — pixi tasks in `pixi.toml`:

```toml
[tasks.vendor-patch]
cmd = "scripts/apply_vendor_patches.sh"

[tasks.commit-build]
cmd = "cargo build --release --manifest-path crates/Cargo.toml"
depends-on = ["vendor-patch"]
```

`scripts/apply_vendor_patches.sh` is idempotent: checks a sentinel file
(`native/vendor/iceberg-rust/.primeparts-patched`) and exits 0 if already
applied. On failure (e.g. patch context drifts after a re-vendor), it surfaces
the upstream commit hash recorded in the patch header so the conflict is
diagnosable.

## Settings — tri-state for warehouse-standing pre-check

File: `~/.config/primeparts/config.toml`. CLI flag overrides file. Env var
`PRIMEPARTS_WAREHOUSE_STANDING` overrides both.

```toml
# warehouse_standing_check: "always-ask" | "always-run" | "never-ask"
warehouse_standing_check = "always-ask"
```

| Setting | Behavior |
|---|---|
| `always-ask` (default) | Prompt every run. Yes → run check; No → skip. |
| `always-run` | Run check without prompting. |
| `never-ask` | Skip check entirely. No prompt, no subprocess. |

CLI flag: `--warehouse-standing={ask,run,skip}` for one-shot override.

When the check runs, the binary spawns
`python -m primeparts.native_iceberg --check-warehouse` and waits for its
JSON exit. Failure → exit code 3.

## Build integration

- Workspace `crates/Cargo.toml` declares the two member crates.
- `pixi.toml` adds `vendor-patch`, `commit-build`, and
  `commit = "crates/target/release/primeparts-commit"` tasks.
- The Python coordinator `run_native_pipeline` can cut over to subprocess-call
  the new binary in place of `commit_native_manifest` once Rust catalog
  compatibility is resolved. TUI status/read integration should use the
  native C/C++ `ui_iceberg` ABI in `native/`; a future TUI Generate screen can
  invoke Rust only for catalog mutation if/when this binary becomes production
  ready.

## Phasing within v1

1. `crates/Cargo.toml` workspace + empty member crate scaffolding.
2. `crates/vendor-patches/iceberg-rust.patch` +
   `scripts/apply_vendor_patches.sh` + smoke-build the patched iceberg-rust.
3. `primeparts-core::data_file` — parquet → DataFile with sort_order_id;
   `footer_kv` reader.
4. `primeparts-commit::manifest` + `verify_against_footer`.
5. `primeparts-commit::catalog` — SqlCatalog open + Transaction + add-files.
6. `primeparts-commit::hms_sync` — Thrift alter.
7. `primeparts-commit::settings` + `prompt`.
8. Wire `main.rs`, exit codes.
9. Cut over `run_native_pipeline` to the new binary.

## Things to verify before phase 2

These claims were established conversationally but not all proven by direct
read; confirm before committing them to a `.patch` file.

- **`default_sort_order_id` for both tables.** Plan assumes `1` (i.e. the
  first non-default sort order, since both tables declare an explicit sort
  order). Verify by loading each table via the SqlCatalog and inspecting
  `tbl.sort_orders()` / `tbl.metadata.default_sort_order_id`.
- **Patch A compile path.** Once `parquet_to_data_file_builder` is `pub`,
  confirm its body at lines 363-421 doesn't surface any private types in
  the public signature. The function returns `Result<DataFileBuilder>` and
  takes `(SchemaRef, Arc<ParquetMetaData>, usize, String, HashMap<i32, u64>)`
  — all public. Internals (`MinMaxColAggregator`, `IndexByParquetPathName`)
  are consumed inside the function and don't leak. Re-read to confirm.
- **Patch B exact diff shape.** Need to read the surrounding code at
  `crates/catalog/hms/src/catalog.rs:553,632,669` to determine the
  `GetTableRequest` field names in the vendored Thrift IDL (`db_name` vs
  `dbname`, `tbl_name` vs `tblName`) and the actual generated method name
  (`.get_table_req()` vs `.get_table_req_async()`, etc.). The volo-thrift
  codegen conventions matter.
- **Generate's partitions JSONL row.** Confirmed via `generate.cc:693-701`
  that `append_manifest` emits the same shape for both tables. No further
  verification needed unless the C generator changes.

## Out of scope for v1

- Tests / golden fixture — deferred to a later session.
- Compaction binary (`primeparts-compact`) — originally out of scope, but now
  implemented as an operational one-shot crate after this plan and used for
  the May 2026 warehouse compaction.
- Porting `warehouse_standing` to Rust — v2.
- Removing pyiceberg conda dep — v2 (porting `warehouse_standing` +
  recovery + repair).
- Snapshot rollback — TUI v2.

## References

Pinned file:line citations for every claim above so a fresh-context Claude
can navigate without searching.

| Citation | Used for |
|---|---|
| `src/primeparts/native_iceberg.py:1-255` | Pipeline being replaced (`commit_native_manifest`). |
| `scripts/sync_hms.py` | HMS sync logic to port to `hms_sync.rs`; lazy-sync semantics. |
| `src/primeparts/_patches.py:12-39` | Existing `sort_order_id` patch — Patch A mirror. |
| `src/primeparts/_patches.py:43-67` | Existing HMS `get_table` patch — Patch B mirror. |
| `src/primeparts/iceberg_schema.py` (`warehouse_standing`) | Pre-check function the binary subprocesses today. |
| `native/src/generate.cc:685-691` | Warehouse path convention: `<warehouse>/funbuns/<table>/data/commit_seq=N/<table>_b00000N_000.parquet`. |
| `native/src/generate.cc:693-701` | JSONL emission shape for both tables. |
| `native/vendor/iceberg-rust/crates/iceberg/src/writer/file_writer/parquet_writer.rs:312-347` | `parquet_files_to_data_files` — the wrapper we *don't* flip. |
| `native/vendor/iceberg-rust/crates/iceberg/src/writer/file_writer/parquet_writer.rs:350-424` | `parquet_to_data_file_builder` — Patch A target; lines 399-421 are the `DataFileBuilder` chain that omits `sort_order_id`. |
| `native/vendor/iceberg-rust/crates/iceberg/src/spec/manifest/data_file.rs:153-154` | Proof `.sort_order_id(i32)` setter exists on `DataFileBuilder` (`#[builder(default, setter(strip_option))] pub(crate) sort_order_id: Option<i32>`). |
| `native/vendor/iceberg-rust/crates/catalog/hms/src/catalog.rs:553,632,669` | Three `.get_table()` call sites for Patch B. |
| `native/vendor/iceberg-rust/bindings/python/project-description.md` | Establishes that `pyiceberg-core` is pyiceberg's accelerator, not a substitute. |
| `native/vendor/iceberg-rust/bindings/python/src/lib.rs:27-33` | `pyiceberg_core_rust` module registration — only `datafusion_table_provider`, `transform`, `manifest` are exposed. |
| `native/include/primeparts/ui_iceberg.h`, `native/src/ui_iceberg.cc` | Existing C ABI over iceberg-cpp reads for TUI/status integration. |
