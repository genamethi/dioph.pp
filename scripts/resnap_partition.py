#!/usr/bin/env python3
"""Re-sort data files for a single p_trunc partition of a void-normalized source table.

Used after preflight detects in-file sort violations. For the primeparts source
warehouse, the original truncate(1e10) partition values are still on disk as
``p_trunc=N`` subdirs, even though the table's current partition spec is void.
This script:

  1. Identifies data files under <table>/data/p_trunc=N/.
  2. Streams them through a polars sort (streaming engine, spills to SSD).
  3. Writes one new compact parquet with PARQUET:field_id metadata matching
     the table's iceberg schema.
  4. Atomically swaps the manifest: drops the old corrupted files, adds the
     new file. Same commit pattern as scripts/normalize_void_partitions.py.

Dry-run by default. Pass --apply to write the new manifests + metadata and
repoint the SqlCatalog row.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import sqlite3
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from uuid import uuid4

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq
from pyiceberg.io.pyarrow import schema_to_pyarrow
from pyiceberg.manifest import (
    DataFile,
    DataFileContent,
    FileFormat,
    ManifestEntry,
    ManifestEntryStatus,
    read_manifest_list,
    write_manifest,
    write_manifest_list,
)
from pyiceberg.typedef import Record

from primeparts.iceberg_schema import (
    DECOMP_IDENT,
    PRIMES_IDENT,
    get_iceberg_dir,
    open_catalog,
)

AVRO_COMPRESSION = "deflate"


@dataclass(frozen=True)
class ResnapPlan:
    ident: str
    p_trunc: int
    sort_keys: list[str]
    old_files: list[str]      # absolute paths
    new_file: str             # absolute path of the new compact
    old_metadata: str
    new_metadata: str


def _local_path(location: str) -> Path:
    if location.startswith("file://"):
        return Path(location[len("file://") :])
    return Path(location)


def _next_metadata_location(metadata_location: str) -> str:
    old_path = _local_path(metadata_location)
    prefix = 0
    if "-" in old_path.name:
        head = old_path.name.split("-", 1)[0]
        if head.isdigit():
            prefix = int(head) + 1
    return f"file://{old_path.with_name(f'{prefix:05d}-{uuid4()}.metadata.json')}"


def _metadata_dir(table_location: str) -> str:
    return f"{table_location.rstrip('/')}/metadata"


def _data_dir(table_location: str) -> str:
    return f"{table_location.rstrip('/')}/data"


def _sort_keys_for(ident: str) -> list[str]:
    if ident == PRIMES_IDENT:
        return ["p"]
    if ident == DECOMP_IDENT:
        return ["p", "m_k"]
    raise ValueError(f"unknown table {ident}")


def _files_in_partition(table, p_trunc: int) -> list:
    """Return ManifestEntry objects whose file_path lives under p_trunc=N/."""
    snapshot = table.metadata.current_snapshot()
    if snapshot is None:
        raise RuntimeError(f"{table.name()}: no current snapshot")
    needle = f"p_trunc={p_trunc}/"
    out = []
    for mf in read_manifest_list(table.io.new_input(snapshot.manifest_list)):
        for entry in mf.fetch_manifest_entry(table.io, discard_deleted=False):
            if needle in entry.data_file.file_path:
                out.append(entry)
    return out


def _arrow_schema_with_field_ids(table) -> pa.Schema:
    """Iceberg schema → arrow schema with PARQUET:field_id metadata per field."""
    return schema_to_pyarrow(table.schema())


def _sort_and_write(
    old_paths: list[str],
    out_path: str,
    sort_keys: list[str],
    spill_dir: str,
    chunk_rows: int = 25_000_000,
) -> tuple[int, int, int, int]:
    """External merge sort + write. Output schema is the source parquet's
    schema (preserves PARQUET:field_id metadata on each column).

    Phase 1: stream input batches; accumulate up to `chunk_rows` in
    memory, sort with pyarrow, sink each run to a temp parquet.
    Phase 2: k-way merge with slice emission. For each iteration we
    pop the smallest-key run from a heap, find the longest prefix of
    that run's current batch whose keys are all <= the next-smallest
    run's current key, emit that slice, advance the cursor.
    Memory bounded by chunk_rows × row_width.
    """
    import heapq
    import numpy as np

    spill = Path(spill_dir)
    spill.mkdir(parents=True, exist_ok=True)
    run_dir = spill / f"resnap_{uuid4().hex[:8]}"
    run_dir.mkdir()

    # Source schema (with field_id metadata preserved by pyarrow).
    src_schema = pq.ParquetFile(old_paths[0]).schema_arrow

    # Phase 1: produce sorted runs.
    runs: list[Path] = []
    buf_batches: list[pa.RecordBatch] = []
    buf_rows = 0

    def flush_run():
        nonlocal buf_batches, buf_rows
        if not buf_batches:
            return
        tbl = pa.Table.from_batches(buf_batches)
        tbl = tbl.sort_by([(k, "ascending") for k in sort_keys])
        run_path = run_dir / f"run_{len(runs):04d}.parquet"
        pq.write_table(tbl, run_path, compression="zstd", compression_level=3)
        runs.append(run_path)
        buf_batches = []
        buf_rows = 0

    for src in old_paths:
        rdr = pq.ParquetFile(src)
        for batch in rdr.iter_batches(batch_size=1_048_576):
            buf_batches.append(batch)
            buf_rows += batch.num_rows
            if buf_rows >= chunk_rows:
                flush_run()
    flush_run()

    if not runs:
        raise RuntimeError("no input rows")

    print(f"  sorted {len(runs)} run(s); merging")

    # Phase 2: k-way merge with slice emission.
    run_readers = [pq.ParquetFile(p) for p in runs]
    run_iters = [r.iter_batches(batch_size=262144) for r in run_readers]
    cur_batches: list[pa.RecordBatch | None] = [next(it, None) for it in run_iters]
    cur_idx = [0] * len(runs)

    key_col_idx = [cur_batches[0].schema.get_field_index(k) for k in sort_keys]
    # Pre-extract numpy views for key columns, refreshed per-batch.
    key_np: list[list[np.ndarray] | None] = [None] * len(runs)

    def refresh_keys(r: int):
        b = cur_batches[r]
        if b is None:
            key_np[r] = None
            return
        key_np[r] = [b.column(c).to_numpy(zero_copy_only=False) for c in key_col_idx]

    for r in range(len(runs)):
        refresh_keys(r)

    def key_at(r: int, i: int) -> tuple:
        return tuple(int(arr[i]) for arr in key_np[r])

    def find_prefix_le(r: int, start: int, limit: tuple) -> int:
        """Length of [start, end) in run r where rows' keys <= limit (lex)."""
        arrs = key_np[r]
        n = cur_batches[r].num_rows - start
        if n == 0:
            return 0
        if len(arrs) == 1:
            sub = arrs[0][start:]
            # Largest index where sub[i] <= limit[0]; return count.
            return int(np.searchsorted(sub, limit[0], side="right"))
        # 2-key lex: prefix where (p,m_k) <= limit.
        p_sub = arrs[0][start:]
        m_sub = arrs[1][start:]
        # p strictly less → always inside
        # p equal AND m_k <= limit[1] → inside
        # p strictly greater → outside
        less_p = p_sub < limit[0]
        eq_p = p_sub == limit[0]
        le_m = m_sub <= limit[1]
        mask = less_p | (eq_p & le_m)
        if mask.all():
            return n
        # First False index = prefix length.
        return int(np.argmin(mask))

    # Initial heap: (key, run_idx)
    heap: list[tuple] = []
    for r in range(len(runs)):
        if cur_batches[r] is not None:
            heapq.heappush(heap, (key_at(r, 0), r))

    writer = pq.ParquetWriter(
        out_path, src_schema,
        compression="zstd", compression_level=3,
        data_page_size=1 << 20, write_statistics=True,
    )
    row_count = 0
    p_field_idx = src_schema.get_field_index("p")
    p_min = None
    p_max = None

    while heap:
        top_key, r = heapq.heappop(heap)
        # Limit = next-smallest key across other active runs.
        limit = heap[0][0] if heap else None

        b = cur_batches[r]
        start = cur_idx[r]
        if limit is None:
            slice_len = b.num_rows - start
        else:
            slice_len = find_prefix_le(r, start, limit)
            slice_len = max(slice_len, 1)  # at least the row that won

        sliced = b.slice(start, slice_len)
        writer.write_batch(sliced)
        row_count += slice_len
        # Output is globally sorted, so track running min/max from p column.
        p_col = sliced.column(p_field_idx)
        if p_min is None:
            p_min = p_col[0].as_py()
        p_max = p_col[-1].as_py()
        cur_idx[r] = start + slice_len

        if cur_idx[r] >= b.num_rows:
            nb = next(run_iters[r], None)
            cur_batches[r] = nb
            cur_idx[r] = 0
            refresh_keys(r)

        if cur_batches[r] is not None:
            heapq.heappush(heap, (key_at(r, cur_idx[r]), r))

    writer.close()

    # Cleanup runs.
    for p in runs:
        p.unlink(missing_ok=True)
    run_dir.rmdir()

    byte_count = Path(out_path).stat().st_size
    return row_count, byte_count, p_min, p_max


def _i64_le_bytes(v: int) -> bytes:
    return int(v).to_bytes(8, "little", signed=True)


def _build_data_file(
    table,
    out_path: str,
    row_count: int,
    byte_count: int,
    p_min: int,
    p_max: int,
    spec_id: int,
) -> DataFile:
    """Build a DataFile entry pointing at our new compact parquet."""
    null_partition = Record(*([None] * len(table.specs()[spec_id].fields)))
    df = DataFile.from_args(
        _table_format_version=table.metadata.format_version,
        content=DataFileContent.DATA,
        file_path=f"file://{out_path}",
        file_format=FileFormat.PARQUET,
        partition=null_partition,
        record_count=row_count,
        file_size_in_bytes=byte_count,
        column_sizes={},
        value_counts={},
        null_value_counts={},
        nan_value_counts={},
        lower_bounds={1: _i64_le_bytes(p_min)},
        upper_bounds={1: _i64_le_bytes(p_max)},
        key_metadata=None,
        split_offsets=None,
        equality_ids=None,
        sort_order_id=table.metadata.default_sort_order_id,
    )
    df._spec_id = spec_id
    return df


def _rewrite_manifests(table, drop_paths: set[str], new_data_file: DataFile,
                       *, apply: bool) -> str:
    """Build a new manifest list: keep entries whose file path is not in
    drop_paths, then append a single new EXISTING entry for the new file.
    Returns the new manifest_list path."""
    snapshot = table.metadata.current_snapshot()
    manifests = list(read_manifest_list(table.io.new_input(snapshot.manifest_list)))
    metadata_dir = _metadata_dir(table.location())
    new_manifest_files = []

    # Resolve drop_paths to a set with and without file:// prefix.
    drop_set = set()
    for p in drop_paths:
        drop_set.add(p)
        drop_set.add(f"file://{p}")
        if p.startswith("file://"):
            drop_set.add(p[len("file://"):])

    for index, manifest_file in enumerate(manifests):
        entries = manifest_file.fetch_manifest_entry(table.io, discard_deleted=False)
        kept = [e for e in entries if e.data_file.file_path not in drop_set]
        if not kept:
            continue
        spec = table.specs()[manifest_file.partition_spec_id]
        new_manifest_location = f"{metadata_dir}/{uuid4()}-resnap-m{index}.avro"
        if apply:
            with write_manifest(
                table.metadata.format_version,
                spec,
                table.schema(),
                table.io.new_output(new_manifest_location),
                snapshot.snapshot_id,
                AVRO_COMPRESSION,
            ) as writer:
                for entry in kept:
                    writer.add_entry(entry)
                new_manifest_files.append(writer.to_manifest_file())
        else:
            new_manifest_files.append(manifest_file)

    # Add a new manifest for the new data file.
    new_data_manifest_location = f"{metadata_dir}/{uuid4()}-resnap-new.avro"
    current_spec_id = table.metadata.default_spec_id
    current_spec = table.specs()[current_spec_id]
    if apply:
        with write_manifest(
            table.metadata.format_version,
            current_spec,
            table.schema(),
            table.io.new_output(new_data_manifest_location),
            snapshot.snapshot_id,
            AVRO_COMPRESSION,
        ) as writer:
            entry = ManifestEntry.from_args(
                status=ManifestEntryStatus.ADDED,
                snapshot_id=snapshot.snapshot_id,
                sequence_number=snapshot.sequence_number,
                file_sequence_number=snapshot.sequence_number,
                data_file=new_data_file,
            )
            writer.add_entry(entry)
            new_manifest_files.append(writer.to_manifest_file())

    new_manifest_list = f"{metadata_dir}/snap-{snapshot.snapshot_id}-0-{uuid4()}-resnap.avro"
    if apply:
        with write_manifest_list(
            table.metadata.format_version,
            table.io.new_output(new_manifest_list),
            snapshot.snapshot_id,
            snapshot.parent_snapshot_id,
            snapshot.sequence_number,
            AVRO_COMPRESSION,
        ) as writer:
            writer.add_manifests(new_manifest_files)
    return new_manifest_list


def _write_metadata_json(table, new_manifest_list: str, *, apply: bool) -> str:
    old_metadata = table.metadata_location
    new_metadata = _next_metadata_location(old_metadata)
    metadata_path = _local_path(old_metadata)
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    now_ms = int(time.time() * 1000)
    metadata.setdefault("metadata-log", []).append(
        {"metadata-file": old_metadata, "timestamp-ms": metadata.get("last-updated-ms", now_ms)}
    )
    metadata["last-updated-ms"] = now_ms
    # Update current snapshot's manifest-list pointer.
    current_id = metadata.get("current-snapshot-id")
    for snap in metadata.get("snapshots", []):
        if snap.get("snapshot-id") == current_id:
            snap["manifest-list"] = new_manifest_list
            if isinstance(snap.get("summary"), dict):
                snap["summary"]["changed-partition-count"] = "1"
            break
    if apply:
        _local_path(new_metadata).write_text(
            json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
        )
    return new_metadata


def _commit_catalog_pointer(ident: str, old_metadata: str, new_metadata: str) -> None:
    namespace, table_name = ident.split(".", 1)
    db_path = get_iceberg_dir() / "catalog.db"
    con = sqlite3.connect(db_path)
    try:
        with con:
            row = con.execute(
                "SELECT catalog_name, metadata_location FROM iceberg_tables "
                "WHERE table_namespace = ? AND table_name = ?",
                (namespace, table_name),
            ).fetchone()
            if row is None:
                raise RuntimeError(f"{ident}: no SqlCatalog row")
            catalog_name, current_metadata = row
            if current_metadata != old_metadata:
                raise RuntimeError(
                    f"{ident}: catalog moved while rewriting; expected {old_metadata}, got {current_metadata}"
                )
            updated = con.execute(
                "UPDATE iceberg_tables SET metadata_location = ?, "
                "previous_metadata_location = ?, "
                "iceberg_type = COALESCE(iceberg_type, 'TABLE') "
                "WHERE catalog_name = ? AND table_namespace = ? AND table_name = ? "
                "AND metadata_location = ?",
                (new_metadata, old_metadata, catalog_name, namespace, table_name, old_metadata),
            ).rowcount
            if updated != 1:
                raise RuntimeError(f"{ident}: expected 1 row updated, got {updated}")
    finally:
        con.close()


def resnap(ident: str, p_trunc: int, spill_dir: str, *, apply: bool) -> ResnapPlan:
    catalog = open_catalog()
    table = catalog.load_table(ident)
    sort_keys = _sort_keys_for(ident)

    entries = _files_in_partition(table, p_trunc)
    if not entries:
        raise RuntimeError(f"{ident}: no data files at p_trunc={p_trunc}")
    old_paths = [_local_path(e.data_file.file_path).as_posix() for e in entries]

    data_dir = _local_path(_data_dir(table.location()))
    out_dir = data_dir / f"p_trunc={p_trunc}"
    out_dir.mkdir(parents=True, exist_ok=True)
    new_file_name = f"resnap_{uuid4()}.parquet"
    out_path = (out_dir / new_file_name).as_posix()

    print(f"[{ident}] resnap p_trunc={p_trunc}")
    print(f"  reading {len(old_paths)} file(s):")
    for p in old_paths:
        print(f"    - {p}")
    print(f"  writing -> {out_path}")
    print(f"  sort keys: {sort_keys}")

    if apply:
        row_count, byte_count, p_min, p_max = _sort_and_write(
            old_paths, out_path, sort_keys, spill_dir
        )
        print(f"  wrote {row_count} rows, {byte_count / 1e9:.2f} GB, p in [{p_min}, {p_max}]")
        spec_id = table.metadata.default_spec_id
        new_data_file = _build_data_file(
            table, out_path, row_count, byte_count, p_min, p_max, spec_id
        )
        new_manifest_list = _rewrite_manifests(table, set(old_paths), new_data_file, apply=True)
        new_metadata = _write_metadata_json(table, new_manifest_list, apply=True)
        _commit_catalog_pointer(ident, table.metadata_location, new_metadata)
    else:
        row_count = byte_count = 0
        new_manifest_list = "(dry-run)"
        new_metadata = "(dry-run)"

    return ResnapPlan(
        ident=ident,
        p_trunc=p_trunc,
        sort_keys=sort_keys,
        old_files=old_paths,
        new_file=out_path,
        old_metadata=table.metadata_location,
        new_metadata=new_metadata,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--table", choices=(PRIMES_IDENT, DECOMP_IDENT), required=True,
        help="Which source table to resnap.",
    )
    parser.add_argument(
        "--p-trunc", type=int, required=True,
        help="Truncate-partition value (e.g., 250000000000).",
    )
    parser.add_argument(
        "--spill-dir", default="/media/extssd/research/dioph.pp/data/polars_tmp",
        help="Directory for polars streaming-sort spill files.",
    )
    parser.add_argument("--apply", action="store_true",
                        help="Write new parquet, manifests, metadata, and repoint catalog.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    plan = resnap(args.table, args.p_trunc, args.spill_dir, apply=args.apply)
    if not args.apply:
        print("dry run only; rerun with --apply to commit changes")
    else:
        print(f"  old metadata: {plan.old_metadata}")
        print(f"  new metadata: {plan.new_metadata}")


if __name__ == "__main__":
    main()
