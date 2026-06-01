# Spike: native position-delete write→commit→reopen→read

**Goal.** Prove that the vendored iceberg-cpp (`native/vendor/iceberg-cpp`,
v0.2.0-123-g3fed150) can *write a position-delete file, commit it to a table,
reopen the table, and read back the reduced row set* — the persistence
primitive the covering-sieve rewrite needs (live post-delete rows = current
uncovered set). Green ⇒ build the sieve on MOR position deletes. Red ⇒ fall
back to the append-only covered-set model (FastAppend only).

This is **spike-gated**: a throwaway-table proof, not production sieve code.

## What the vendored tree already provides (verified by reading source)

The handoff under-sold this. Present and usable today:

| Piece | File | State |
|---|---|---|
| `PositionDeleteWriter` | `data/position_delete_writer.{h,cc}` | First-class, tested. `WriteDelete(file_path, pos)` buffers; `Metadata()` returns a `WriteResult` whose `DataFile` has `content=kPositionDeletes`, `file_path`, `referenced_data_file`, `partition_spec_id`. Exercised in `delete_filter_test.cc`. |
| `SnapshotUpdate::WriteDeleteManifests` | `update/snapshot_update.cc:194` | **Implemented** (not a stub). Writes `ManifestContent::kDeletes` manifests via `RollingManifestWriter`. |
| Generic snapshot dispatch | `transaction.cc:167,263` | `ApplyUpdateSnapshot(checked_cast<SnapshotUpdate&>(update))` — any `SnapshotUpdate` subclass commits with no transaction-core change. |
| MOR read path | `data/delete_filter.*`, `delete_loader.*`, `deletes/position_delete_index.*` | Position-delete apply-on-read exists and is tested. |
| `DataOperation::kDelete` | `snapshot.h:380` | `"delete"`. |
| Puffin writer | `puffin/` (#624) | Exists, but deletion-vector blob → delete manifest is unverified. **Out of scope** — position-delete *files* (MOR v2) are the on-ramp. |

## The actual gap

There is **no committable delete update class** and **no `Transaction`
accessor** for one. `Transaction` exposes `NewFastAppend()` (and many others)
but nothing that accumulates *delete* files and calls `WriteDeleteManifests`.
`FastAppend` only handles data files.

So the spike's build work is bounded and mechanical:

### 1. Add a `RowDelta` (or `BaseDelete`) subclass of `SnapshotUpdate`

Mirror `FastAppend` (`update/fast_append.{h,cc}`), substituting the delete path:

- `AddDeleteFile(const std::shared_ptr<DataFile>&)` — accumulate, like
  `AppendFile`. Assert `content == kPositionDeletes`.
- `operation()` → `DataOperation::kDelete`.
- `Apply(metadata, snapshot)`:
  - call `WriteDeleteManifests(delete_files.as_span(), spec)` (the existing
    protected method) instead of `WriteDataManifests`;
  - carry forward parent manifests exactly as `FastAppend::Apply` does
    (the `SnapshotCache(snapshot).Manifests(io)` block, lines 117-123).
- `Summary()` / `CleanUncommitted()` / `CleanupAfterCommit()` — copy
  `FastAppend`'s, adjusting summary fields to deleted-records counts.
- Lives **inside the vendored lib** (needs protected `WriteDeleteManifests`,
  `ManifestPath`, `ctx_`) → vendored rebuild (`make ...-rebuild`, shared libs
  per memory `[[feedback_vendored_iceberg_shared_libs]]`).

### 2. Add `Transaction::NewRowDelta()`

One accessor mirroring `NewFastAppend()` (`transaction.cc:474`):
`RowDelta::Make(name, ctx_)` → `AddUpdate(...)`. Declare in `transaction.h`.

### 3. Throwaway proof harness (primeparts side, not vendored)

A small `main` (or extend `pp-catalog --delete-spike`):

1. Create a throwaway table with a few data rows (known file_path + row count).
2. `PositionDeleteWriter` → `WriteDelete(data_file_path, pos)` for a couple of
   positions → `Close()` → `Metadata()` to get the delete `DataFile`.
3. `txn->NewRowDelta()` → `AddDeleteFile(df)` → `Commit()`.
4. Reopen the table (fresh `LoadTable`); scan; assert the deleted positions are
   gone and the count dropped by exactly the number of deletes.
5. Drop the throwaway.

## Known risk, and why it's acceptable here

`snapshot_update.cc:212` FIXME: `WriteDeleteManifests` calls
`WriteAddedEntry(file)` with **no explicit per-file `data_sequence_number`**, so
the delete entry inherits the new snapshot's sequence number (set on the
`ManifestListWriter`, line 266). Iceberg MOR requires a position delete to have
sequence number ≥ the data it removes. **For the sieve this holds by
construction**: we only ever delete from a *fixed, pre-existing* `primes_k0`
copy, so every delete strictly post-dates all data. The FIXME bites only
compaction/rewrite flows that re-sequence files — which the sieve does not do.
If the spike's read-back count is correct, the inherited sequence number is
fine for our use; note it and move on.

Secondary risks:
- `next_row_id` / format-version-3 row-lineage path (lines 271-278) — the
  table's `format_version` governs this; create the throwaway as v2 to avoid
  the v3 row-id requirement unless we specifically want v3.
- Hive MOR *read* of these deletes is a separate question (ties to the
  HMS-sync work, now solved via `hive_register.sh`). The spike only needs the
  *iceberg-cpp* read-back to be correct; Hive-side MOR read is a follow-up.

## Definition of done

- Vendored lib rebuilds with `RowDelta` + `NewRowDelta` (shared libs).
- Throwaway harness: write→commit→reopen→read shows the exact reduced row set.
- One-paragraph result in HANDOFF.md flipping the delete spike GREEN/RED.
- If GREEN: covering-sieve rewrite proceeds on position-delete MOR.
  If RED (and unfixable cheaply): fall back to append-only covered-set; record
  why.

## Effort estimate

Small-to-medium. The subclass is a near-mechanical mirror of `FastAppend`
(~150 lines incl. header); the accessor is ~5 lines; the harness ~100 lines.
The cost center is the vendored rebuild + getting the manifest/summary details
exactly right, not algorithmic difficulty.

---

## Build log / findings (2026-05-31)

**Approach revised during build.** Two facts changed the plan from the scope above:

1. **No `Transaction` accessor / vendored rebuild needed for `RowDelta` itself.**
   `PendingUpdate::Commit()` (pending_update.cc:34) handles the "no transaction
   yet" case by making a temp transaction and dispatching through the *generic*
   `ApplyUpdateSnapshot(checked_cast<SnapshotUpdate&>)`. And all the headers
   `RowDelta` needs (`snapshot_update.h` with protected `WriteDeleteManifests`,
   `data_file_set.h`, `position_delete_writer.h`) are installed to
   `/usr/local/include` and `ICEBERG_EXPORT`. So **`RowDelta` lives entirely in
   the primeparts tree** (`native/src/catalog/pp_row_delta.{h,cc}`), subclassing
   the installed `SnapshotUpdate`, committing via `RowDelta::Make(table)` →
   `AddDeleteFile` → `Commit()`. It compiles clean against the installed static
   libs — no lib change for the delete class.

2. **iceberg-cpp could not read Hive-created tables (the real blocker), now
   patched.** The harness seeds its throwaway via beeline (Hive), and Hive writes
   manifest-list paths *with* a `file:` scheme (`file:/media/...`), single-slash
   form. `ArrowFileSystemFileIO::ResolvePath` (arrow_io.cc) only stripped schemes
   when it saw `"://"`, so `file:/path` was passed verbatim to arrow's
   `LocalFileSystem`, which rejects URIs → `PlanFiles` failed with *"Expected a
   local filesystem path, got a URI"*. **This is a general constraint, not a
   delete issue: native iceberg-cpp tools cannot scan ANY Hive-created table
   (incl. the Hive MV `primes_k0` the sieve consumes) until this is fixed.**
   Native-written tables (e.g. `primeparts.primes`) emit scheme-less paths and
   were unaffected, which is why it had not surfaced before.

   **Patch (vendored):** `ResolvePath` now strips the `file:` scheme itself,
   accepting `file:/path`, `file:///path`, and `file://host/path`. Requires a
   vendored rebuild + `sudo make install` (static libs, root-owned in
   `/usr/local/lib`).

   **Vendored rebuild recipe (this host, 2026-05-31):**
   ```
   cd native/vendor/iceberg-cpp
   cmake -S . -B build-patch -DCMAKE_BUILD_TYPE=Release \
     -DICEBERG_BUILD_STATIC=ON -DICEBERG_BUILD_SHARED=OFF \
     -DICEBERG_BUILD_TESTS=OFF -DICEBERG_BUILD_BUNDLE=ON -DICEBERG_BUILD_REST=ON \
     -DCMAKE_INSTALL_PREFIX=/usr/local \
     -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF \
     -DFETCHCONTENT_BASE_DIR=$PWD/.fc-cache
   cmake --build build-patch --target iceberg_static iceberg_data_static \
     iceberg_rest_static iceberg_bundle_static --parallel $(nproc)
   sudo cmake --install build-patch   # static .a's land root-owned in /usr/local/lib
   ```
   `CMAKE_COMPILE_WARNING_AS_ERROR=OFF` is REQUIRED with this GCC: a
   `-Werror=free-nonheap-object` false-positive in `json_serde.cc` (a
   `std::expected<unordered_map,Error>` dtor inline) otherwise kills the build —
   unrelated to the patch.

**Components built:** `pp_row_delta.{h,cc}` (RowDelta), `pp_delete_spike.{h,cc}`
(harness, wired as `pp-catalog --delete-spike`). The harness: seed 5 rows via
beeline → IRC load → baseline count (beeline) → `PositionDeleteWriter` deletes
positions 0,2 → `RowDelta::Commit()` → HMS-sync new snapshot via
`hive_register.sh` → recount, expect 3.

## Status: GREEN (verified 2026-05-31)

`pp-catalog --delete-spike` passes end-to-end:

```
[1/6] create + seed primeparts.zz_…_delspike (5 rows) ... OK
[2/6] IRC load ... OK
[3/6] baseline count ... 5
[4/6] write position-delete file (delete 2 rows) ... OK
[5/6] commit RowDelta ... OK
[6/6] HMS-sync new snapshot + read back ... 3
  baseline=5  after-delete=3  expected=3   == PASSED ==
```

So the native position-delete primitive — `PositionDeleteWriter` →
`RowDelta` IRC commit → HMS sync → delete-aware read-back — works. **The
covering-sieve rewrite can proceed on the position-delete MOR persistence
model** (no fallback to append-only needed).

Three vendored-lib fixes were required to get here, all in
`native/vendor/PATCHES.md`:
1. `ResolvePath` accept `file:/` single-slash URIs — so native iceberg-cpp can
   scan Hive-created tables at all (general; not delete-specific).
2. (`CMAKE_COMPILE_WARNING_AS_ERROR` cache honor — pre-existing, enables the
   rebuild on this GCC.)
3. `assert-ref-snapshot-id` requirement field `ref` (not `ref-name`) — so the
   snapshot-advancing IRC commit is accepted by the HMS REST servlet. This was
   the real commit blocker; it gates **all** snapshot-advancing native commits
   (`FastAppend` too), not just deletes. Upstream PR drafted in
   `native/vendor/iceberg-refs/upstream-pr-ref-field.md`.

**Open follow-ups (not blockers for the sieve):**
- The `RowDelta` subclass lives in the primeparts tree
  (`native/src/catalog/pp_row_delta.{h,cc}`); no `Transaction::NewRowDelta`
  accessor was needed (generic `PendingUpdate::Commit` dispatch).
- The spike read-back goes through beeline HMS-sync because that's how the
  sieve's consumers will see deletes; the native delete-aware *scan* path was
  not separately exercised at **runtime** here. But it is **fully wired in code**
  (verified 2026-05-31 by reading all four layers), so this is a runtime
  smoke-test item, NOT implementation work:
  1. `SourceTableReader` (`source_scan.cc:248`) plans via
     `TableScanBuilder<DataTableScan>` and passes each whole `*task` to
     `FileScanTaskReader::Open` (`source_scan.cc:159`) — it does not strip deletes.
  2. `DataTableScan::PlanFiles` (vendored `table_scan.cc:519-545`) loads
     `snapshot_cache.DeleteManifests(io_)` and builds the `ManifestGroup` with
     both data + delete manifests.
  3. `ManifestGroup::PlanFiles` (vendored `manifest_group.cc:205-209`) builds a
     `DeleteFileIndex`, calls `ForEntry(entry)` for matching deletes, and
     constructs each `FileScanTask(data_file, delete_files, residual)` **with**
     them attached.
  4. `FileScanTaskReader::Open` (vendored `file_scan_task_reader.cc:163+`)
     branches: empty `task.delete_files()` → COW path; non-empty → builds a
     `DeleteFilter` + `MergeOnReadStreamSource` that drops dead rows.
  So `SourceTableReader::Next` over a position-delete `primes_k0` copy *should*
  already return the live (uncovered) set. Confirm with one native read-back
  count when wiring the sieve.

  **CLOSED 2026-05-31 — `pp-catalog --mor-verify` (GREEN).** Runtime-confirmed on
  a throwaway v2 `(p, prime_rank)` table, with **no beeline read-back**:
  - `SourceTableReader` now projects the reserved `_pos`/`_file` metadata columns
    (extension in `source_scan.cc`: split them out of the scan `Select`, append to
    the projected schema, expose `current_data_file_path()`). `_pos` came back as
    correct absolute data-file ordinals (`0..9`).
  - Positions to delete were **derived from the scan** (not hardcoded as in the
    original spike); native MOR re-read returned the reduced set with the deleted
    `p`s absent. 7 accumulated delete files (one per commit) all applied.
  - The live set streams p-ascending, so **first hole = front row** (verified: the
    front walked `3,5,11,13,17,19`).
  - **Commit cadence: 7 back-to-back `RowDelta` IRC commits at ~38 ms each, no
    throttling.** The "Hive updates once / 15 min" concern is the engine path, not
    IRC commits — so per-pass commits are fine. (Resolves plan verification #1–#3.)
- `data_sequence_number` FIXME confirmed harmless for the fixed-source model.
