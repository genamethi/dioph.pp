#!/usr/bin/env python3
"""
Repair the April 2026 native-writer commit_seq collision.

The bad state is specific:

* legacy files are referenced under data/batch_id=N with commit_seq=N
* a native 1B run was also committed under data/commit_seq=0..499
* later native files may exist locally as uncommitted orphans

This script rewrites the committed native files into the next canonical
commit_seq range, updates Iceberg manifests with an overwrite snapshot, and
moves obsolete/orphan parquet files to a quarantine directory.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

import pyarrow as pa
import pyarrow.parquet as pq
from pyiceberg.manifest import DataFile
from pyiceberg.table import Table, _parquet_files_to_data_files

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

from primeparts.iceberg_schema import (  # noqa: E402
    DECOMP_IDENT,
    PARQUET_WRITER_KWARGS,
    PRIMES_IDENT,
    _local_file_path,
    open_catalog,
    table_local_data_files,
    table_referenced_data_files,
    warehouse_standing,
)

BAD_NATIVE_COUNT = 500
CONTINUATION_START = 500
COMMIT_SEQ_FIELD_ID = {
    PRIMES_IDENT: 3,
    DECOMP_IDENT: 5,
}


@dataclass(frozen=True, slots=True)
class ActiveFile:
    data_file: DataFile
    path: Path
    commit_seq: int
    p_min: int
    p_max: int
    rows: int


@dataclass(frozen=True, slots=True)
class RewritePlan:
    table_name: str
    source: Path
    target: Path
    old_seq: int
    new_seq: int
    p_min: int
    p_max: int
    rows: int
    delete_source_from_manifest: bool


def decode_i32(raw: bytes) -> int:
    return struct.unpack("<i", raw)[0]


def decode_i64(raw: bytes) -> int:
    return struct.unpack("<q", raw)[0]


def path_commit_seq(path: Path) -> int | None:
    parent = path.parent.name
    if not parent.startswith("commit_seq="):
        return None
    try:
        return int(parent.split("=", 1)[1])
    except ValueError:
        return None


def active_files(tbl: Table, ident: str) -> list[ActiveFile]:
    snap = tbl.current_snapshot()
    if snap is None:
        return []
    commit_field_id = COMMIT_SEQ_FIELD_ID[ident]
    out: list[ActiveFile] = []
    for manifest in snap.manifests(tbl.io):
        for entry in manifest.fetch_manifest_entry(tbl.io, discard_deleted=True):
            df = entry.data_file
            path = _local_file_path(str(df.file_path))
            if path is None:
                raise RuntimeError(f"non-local file path is not repairable: {df.file_path}")
            out.append(
                ActiveFile(
                    data_file=df,
                    path=path,
                    commit_seq=decode_i32(df.upper_bounds[commit_field_id]),
                    p_min=decode_i64(df.lower_bounds[1]),
                    p_max=decode_i64(df.upper_bounds[1]),
                    rows=int(df.record_count),
                )
            )
    return out


def footer_int(path: Path, key: str) -> int:
    metadata = pq.read_metadata(path).metadata or {}
    value = metadata.get(key.encode())
    if value is None:
        raise RuntimeError(f"{path}: missing footer key {key}")
    return int(value.decode())


def footer_table(path: Path) -> str:
    metadata = pq.read_metadata(path).metadata or {}
    value = metadata.get(b"funbuns.table")
    if value is None:
        raise RuntimeError(f"{path}: missing footer key funbuns.table")
    return value.decode()


def table_name_for_ident(ident: str) -> str:
    if ident == PRIMES_IDENT:
        return "primes"
    if ident == DECOMP_IDENT:
        return "decompositions"
    raise ValueError(ident)


def table_path(tbl: Table) -> Path:
    location = tbl.location()
    if location.startswith("file://"):
        return Path(location[len("file://") :])
    if "://" in location:
        raise RuntimeError(f"non-local table location is not repairable: {location}")
    return Path(location)


def canonical_path(tbl: Table, table_name: str, commit_seq: int) -> Path:
    return (
        table_path(tbl)
        / "data"
        / f"commit_seq={commit_seq}"
        / f"{table_name}_b{commit_seq:06d}_000.parquet"
    )


def rewrite_parquet_commit_seq(source: Path, target: Path, new_seq: int) -> None:
    pf = pq.ParquetFile(source)
    source_schema = pf.schema_arrow
    commit_idx = source_schema.get_field_index("commit_seq")
    if commit_idx < 0:
        raise RuntimeError(f"{source}: no commit_seq column")

    metadata = dict(source_schema.metadata or {})
    metadata[b"funbuns.commit_seq"] = str(new_seq).encode()
    target_schema = source_schema.with_metadata(metadata)

    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(f".{target.name}.tmp-{os.getpid()}")
    if tmp.exists():
        tmp.unlink()

    try:
        writer = pq.ParquetWriter(
            tmp,
            target_schema,
            compression=PARQUET_WRITER_KWARGS["compression"],
            compression_level=PARQUET_WRITER_KWARGS["compression_level"],
            write_statistics=PARQUET_WRITER_KWARGS["write_statistics"],
            use_dictionary=PARQUET_WRITER_KWARGS["use_dictionary"],
            data_page_size=PARQUET_WRITER_KWARGS["data_page_size"],
        )
        try:
            for row_group in range(pf.num_row_groups):
                table = pf.read_row_group(row_group)
                seq_array = pa.array([new_seq] * table.num_rows, type=pa.int32())
                table = table.set_column(commit_idx, "commit_seq", seq_array)
                table = table.replace_schema_metadata(metadata)
                writer.write_table(table)
        finally:
            writer.close()

        written = pq.read_metadata(tmp)
        if written.num_rows != pf.metadata.num_rows:
            raise RuntimeError(
                f"{target}: row count changed during rewrite: "
                f"{written.num_rows} != {pf.metadata.num_rows}"
            )
        if footer_int(tmp, "funbuns.commit_seq") != new_seq:
            raise RuntimeError(f"{target}: rewritten footer commit_seq mismatch")
        os.replace(tmp, target)
    except Exception:
        if tmp.exists():
            tmp.unlink()
        raise


def move_to_quarantine(path: Path, *, table_root: Path, quarantine_root: Path) -> Path:
    try:
        relative = path.relative_to(table_root)
    except ValueError:
        relative = Path(path.name)
    target = quarantine_root / table_root.name / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        raise RuntimeError(f"quarantine target already exists: {target}")
    shutil.move(str(path), str(target))
    return target


def collect_continuation_orphans(
    tbl: Table,
    table_name: str,
    *,
    old_active: set[Path],
    source_max_p: int,
) -> list[tuple[Path, int, int, int, int]]:
    local = table_local_data_files(tbl)
    candidates = []
    for path in sorted(local - old_active):
        old_seq = path_commit_seq(path)
        if old_seq is None or old_seq < CONTINUATION_START:
            continue
        if footer_table(path) != table_name:
            continue
        p_min = footer_int(path, "funbuns.p_min")
        p_max = footer_int(path, "funbuns.p_max")
        rows = pq.read_metadata(path).num_rows
        if p_min <= source_max_p:
            continue
        candidates.append((path, old_seq, p_min, p_max, rows))

    if not candidates:
        return []

    # Only keep the initial contiguous run of continuation file groups. Any
    # higher-sequence orphan is duplicate/noise until explicitly inspected.
    candidates.sort(key=lambda row: row[1])
    out = []
    expected = candidates[0][1]
    for row in candidates:
        if row[1] != expected:
            break
        out.append(row)
        expected += 1
    return out


def build_plan_for_table(
    tbl: Table,
    ident: str,
    *,
    base_seq: int,
    source_max_p: int,
    include_continuation: bool,
) -> tuple[list[RewritePlan], list[ActiveFile]]:
    table_name = table_name_for_ident(ident)
    active = active_files(tbl, ident)
    bad = [
        f
        for f in active
        if path_commit_seq(f.path) is not None
        and 0 <= path_commit_seq(f.path) < BAD_NATIVE_COUNT
        and f.commit_seq == path_commit_seq(f.path)
    ]
    bad.sort(key=lambda f: f.commit_seq)
    if len(bad) != BAD_NATIVE_COUNT:
        raise RuntimeError(
            f"{ident}: expected {BAD_NATIVE_COUNT} committed native collision files, "
            f"found {len(bad)}"
        )
    if [f.commit_seq for f in bad] != list(range(BAD_NATIVE_COUNT)):
        raise RuntimeError(f"{ident}: bad native commit_seq set is not 0..499")

    plans = [
        RewritePlan(
            table_name=table_name,
            source=f.path,
            target=canonical_path(tbl, table_name, base_seq + 1 + f.commit_seq),
            old_seq=f.commit_seq,
            new_seq=base_seq + 1 + f.commit_seq,
            p_min=f.p_min,
            p_max=f.p_max,
            rows=f.rows,
            delete_source_from_manifest=True,
        )
        for f in bad
    ]

    if include_continuation:
        old_active = {f.path for f in active}
        continuation = collect_continuation_orphans(
            tbl,
            table_name,
            old_active=old_active,
            source_max_p=source_max_p,
        )
        next_seq = base_seq + 1 + BAD_NATIVE_COUNT
        first_old_seq = continuation[0][1] if continuation else None
        for path, old_seq, p_min, p_max, rows in continuation:
            plans.append(
                RewritePlan(
                    table_name=table_name,
                    source=path,
                    target=canonical_path(tbl, table_name, next_seq + old_seq - first_old_seq),
                    old_seq=old_seq,
                    new_seq=next_seq + old_seq - first_old_seq,
                    p_min=p_min,
                    p_max=p_max,
                    rows=rows,
                    delete_source_from_manifest=False,
                )
            )

    return plans, bad


def make_data_files(tbl: Table, paths: Iterable[Path]) -> list[DataFile]:
    return list(
        _parquet_files_to_data_files(
            table_metadata=tbl.metadata,
            file_paths=[str(p) for p in paths],
            io=tbl.io,
        )
    )


def commit_table_repair(
    tbl: Table,
    plans: list[RewritePlan],
    bad_active: list[ActiveFile],
    *,
    snapshot_properties: dict[str, str],
) -> None:
    delete_files = {f.path: f.data_file for f in bad_active}
    append_files = make_data_files(tbl, [p.target for p in plans])

    with tbl.transaction() as tx:
        with tx.update_snapshot(snapshot_properties=snapshot_properties).overwrite() as overwrite:
            for plan in plans:
                if plan.delete_source_from_manifest:
                    overwrite.delete_data_file(delete_files[plan.source])
            for data_file in append_files:
                overwrite.append_data_file(data_file)


def summarize_plan(label: str, plans: list[RewritePlan]) -> dict[str, int | str | None]:
    if not plans:
        return {"table": label, "files": 0}
    return {
        "table": label,
        "files": len(plans),
        "old_seq_min": min(p.old_seq for p in plans),
        "old_seq_max": max(p.old_seq for p in plans),
        "new_seq_min": min(p.new_seq for p in plans),
        "new_seq_max": max(p.new_seq for p in plans),
        "p_min": min(p.p_min for p in plans),
        "p_max": max(p.p_max for p in plans),
        "rows": sum(p.rows for p in plans),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="perform the repair")
    parser.add_argument(
        "--no-continuation",
        action="store_true",
        help="do not relabel/commit unreferenced continuation files after the 1B run",
    )
    parser.add_argument(
        "--quarantine-dir",
        type=Path,
        default=None,
        help="directory for obsolete/orphan parquet files",
    )
    args = parser.parse_args()

    cat = open_catalog()
    tables = {
        PRIMES_IDENT: cat.load_table(PRIMES_IDENT),
        DECOMP_IDENT: cat.load_table(DECOMP_IDENT),
    }
    all_active = {ident: active_files(tbl, ident) for ident, tbl in tables.items()}

    def is_bad_native(f: ActiveFile) -> bool:
        seq = path_commit_seq(f.path)
        return seq is not None and 0 <= seq < BAD_NATIVE_COUNT and f.commit_seq == seq

    base_values = [
        f.commit_seq
        for files in all_active.values()
        for f in files
        if not is_bad_native(f)
    ]
    if not base_values:
        raise RuntimeError("could not determine base commit_seq")
    base_seq = max(base_values)

    prime_source_max_p = max(f.p_max for f in all_active[PRIMES_IDENT] if is_bad_native(f))
    include_continuation = not args.no_continuation

    plans: dict[str, list[RewritePlan]] = {}
    bad_active: dict[str, list[ActiveFile]] = {}
    for ident, tbl in tables.items():
        plans[ident], bad_active[ident] = build_plan_for_table(
            tbl,
            ident,
            base_seq=base_seq,
            source_max_p=prime_source_max_p,
            include_continuation=include_continuation,
        )

    prime_max_p = max(p.p_max for p in plans[PRIMES_IDENT])
    max_commit_seq = max(p.new_seq for p in plans[PRIMES_IDENT])
    snapshot_properties = {
        "funbuns.max_p": str(prime_max_p),
        "funbuns.max_commit_seq": str(max_commit_seq),
    }

    summary = {
        "apply": args.apply,
        "base_commit_seq": base_seq,
        "snapshot_properties": snapshot_properties,
        "plans": [
            summarize_plan("primes", plans[PRIMES_IDENT]),
            summarize_plan("decompositions", plans[DECOMP_IDENT]),
        ],
        "current_warehouse_ok": warehouse_standing(cat)["ok"],
    }
    print(json.dumps(summary, indent=2))

    if not args.apply:
        return 0

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    warehouse_root = table_path(tables[PRIMES_IDENT]).parents[1]
    quarantine = args.quarantine_dir or warehouse_root / f"_repair_quarantine_{timestamp}"
    quarantine.mkdir(parents=True, exist_ok=True)

    # Free target paths that are occupied by orphan duplicate files.
    referenced = {
        ident: table_referenced_data_files(tbl)
        for ident, tbl in tables.items()
    }
    for ident, tbl in tables.items():
        targets = {p.target for p in plans[ident]}
        for target in sorted(targets):
            if target.exists() and target not in referenced[ident]:
                moved = move_to_quarantine(
                    target,
                    table_root=table_path(tbl),
                    quarantine_root=quarantine,
                )
                print(f"quarantined target collision: {target} -> {moved}")

    for ident in (PRIMES_IDENT, DECOMP_IDENT):
        for plan in plans[ident]:
            if plan.target.exists():
                if footer_int(plan.target, "funbuns.commit_seq") != plan.new_seq:
                    raise RuntimeError(f"target exists with wrong commit_seq: {plan.target}")
                continue
            print(f"rewrite {plan.table_name} {plan.old_seq} -> {plan.new_seq}")
            rewrite_parquet_commit_seq(plan.source, plan.target, plan.new_seq)

    # Commit table manifests only after all replacement files are durable.
    for ident in (PRIMES_IDENT, DECOMP_IDENT):
        print(f"commit Iceberg repair for {ident}")
        commit_table_repair(
            tables[ident],
            plans[ident],
            bad_active[ident],
            snapshot_properties=snapshot_properties,
        )
        tables[ident] = cat.load_table(ident)

    # Move every now-unreferenced parquet out of table data directories.
    moved_orphans = []
    for ident, tbl in tables.items():
        table_root = table_path(tbl)
        local = table_local_data_files(tbl)
        active = table_referenced_data_files(tbl)
        for path in sorted(local - active):
            moved_orphans.append(
                (
                    str(path),
                    str(
                        move_to_quarantine(
                            path,
                            table_root=table_root,
                            quarantine_root=quarantine,
                        )
                    ),
                )
            )

    final_state = warehouse_standing(cat)
    print(json.dumps({"quarantine": str(quarantine), "moved_orphans": len(moved_orphans)}, indent=2))
    print(json.dumps(final_state, indent=2))
    return 0 if final_state["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
