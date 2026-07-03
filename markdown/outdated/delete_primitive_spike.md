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

A small throwaway `main`:

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

Secondary risk:
- `next_row_id` / format-version-3 row-lineage path (lines 271-278) — the
  table's `format_version` governs this; create the throwaway as v2 to avoid
  the v3 row-id requirement unless we specifically want v3.

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

## Status: GREEN (verified 2026-05-31)

The native position-delete primitive — `PositionDeleteWriter` → `RowDelta`
commit → delete-aware read-back — works end to end.

**`RowDelta` lives entirely in the primeparts tree**
(`native/src/catalog/pp_row_delta.{h,cc}`). No `Transaction::NewRowDelta`
accessor and no vendored lib change were needed: `PendingUpdate::Commit()`
handles the "no transaction yet" case by making a temp transaction and
dispatching through the generic `ApplyUpdateSnapshot(checked_cast<SnapshotUpdate&>)`,
and the headers `RowDelta` needs (`snapshot_update.h` with protected
`WriteDeleteManifests`, `data_file_set.h`, `position_delete_writer.h`) are
exported. So `RowDelta` subclasses the built `SnapshotUpdate` and commits via
`RowDelta::Make(table)` → `AddDeleteFile` → `Commit()`. (Rebuild of the vendored
stack is now `native/configure` + git submodules into `$HOME/.local` — see
`BUILD.md`; iceberg-cpp pinned at `v0.3.0`.)

**Delete-aware native scan is fully wired** across four layers, runtime-verified:
1. `SourceTableReader` (`source_scan.cc`) plans via
   `TableScanBuilder<DataTableScan>` and passes each whole `*task` to
   `FileScanTaskReader::Open` — it does not strip deletes.
2. `DataTableScan::PlanFiles` loads `snapshot_cache.DeleteManifests(io_)` and
   builds the `ManifestGroup` with both data + delete manifests.
3. `ManifestGroup::PlanFiles` builds a `DeleteFileIndex` and constructs each
   `FileScanTask(data_file, delete_files, residual)` **with** deletes attached.
4. `FileScanTaskReader::Open` branches: empty deletes → COW path; non-empty →
   `DeleteFilter` + `MergeOnReadStreamSource` that drops dead rows.

Runtime confirmation (on a throwaway v2 `(p, prime_rank)` table):
- `SourceTableReader` projects the reserved `_pos`/`_file` metadata columns;
  `_pos` came back as correct absolute data-file ordinals (`0..9`).
- Positions to delete derived from the scan; native MOR re-read returned the
  reduced set with the deleted `p`s absent. 7 accumulated delete files (one per
  commit) all applied. The live set streams p-ascending, so first hole = front
  row (verified front walked `3,5,11,13,17,19`).
- **Commit cadence: 7 back-to-back `RowDelta` commits at ~38 ms each, no
  throttling** — so per-pass commits are fine.
- `data_sequence_number` FIXME confirmed harmless for the fixed-source model.

So **the covering-sieve rewrite can proceed on the position-delete MOR
persistence model** (no fallback to append-only needed). The commit path is now
re-exercised against the local catalog by `primeparts-catalogd-smoke` and the
`generate` commit; the original throwaway harness (and its `pp-catalog
--delete-spike` / `--mor-verify` subcommands) has been removed.

> The one active vendored patch this relied on is the `ResolvePath` `file:/`
> single-slash URI fix (so native iceberg-cpp can scan tables whose manifests
> carry `file:` paths) — see `native/vendor/PATCHES.md`. The earlier
> `assert-ref-snapshot-id` `ref`-field patch was upstreamed in iceberg-cpp
> `v0.3.0` and retired.
