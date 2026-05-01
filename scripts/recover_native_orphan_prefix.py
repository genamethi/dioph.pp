#!/usr/bin/env python3
"""
Recover a contiguous prefix of native Parquet files left by a killed run.

The native writer writes Parquet before Iceberg metadata. If the process is
killed before the final PyIceberg add_files step, the warehouse has complete
local files that are not visible in the catalog. This script reconstructs a
native JSONL manifest from parquet footer metadata and commits only the
gap-free prefix starting at current manifest max(commit_seq)+1.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import pyarrow.parquet as pq

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

from primeparts.iceberg_schema import (  # noqa: E402
    DECOMP_IDENT,
    PRIMES_IDENT,
    get_warehouse_dir,
    open_catalog,
    table_manifest_bounds,
    table_referenced_data_files,
    warehouse_standing,
)
from primeparts.native_iceberg import commit_native_manifest  # noqa: E402


@dataclass(frozen=True, slots=True)
class FooterFile:
    table: str
    path: Path
    commit_seq: int
    rows: int
    p_min: int
    p_max: int
    bytes: int

    def to_json(self) -> dict[str, Any]:
        return {
            "table": self.table,
            "path": str(self.path),
            "commit_seq": self.commit_seq,
            "rows": self.rows,
            "p_min": self.p_min,
            "p_max": self.p_max,
            "bytes": self.bytes,
        }


def kv_text(metadata: dict[bytes, bytes], path: Path, key: str) -> str:
    value = metadata.get(key.encode())
    if value is None:
        raise RuntimeError(f"{path}: missing footer key {key}")
    return value.decode()


def read_footer_file(table: str, path: Path, commit_seq: int) -> FooterFile:
    md = pq.read_metadata(path)
    kv = md.metadata or {}
    footer_table = kv_text(kv, path, "funbuns.table")
    footer_seq = int(kv_text(kv, path, "funbuns.commit_seq"))
    rows = int(kv_text(kv, path, "funbuns.n_rows"))
    p_min = int(kv_text(kv, path, "funbuns.p_min"))
    p_max = int(kv_text(kv, path, "funbuns.p_max"))
    if footer_table != table:
        raise RuntimeError(f"{path}: footer table={footer_table!r}, expected {table!r}")
    if footer_seq != commit_seq:
        raise RuntimeError(f"{path}: footer commit_seq={footer_seq}, expected {commit_seq}")
    if rows != md.num_rows:
        raise RuntimeError(f"{path}: footer rows={rows}, parquet rows={md.num_rows}")
    if p_min <= 0 or p_max < p_min:
        raise RuntimeError(f"{path}: invalid p range {p_min}..{p_max}")
    if kv.get(b"funbuns.n_primes") is None:
        raise RuntimeError(f"{path}: missing footer key funbuns.n_primes")
    return FooterFile(
        table=table,
        path=path.resolve(strict=True),
        commit_seq=commit_seq,
        rows=rows,
        p_min=p_min,
        p_max=p_max,
        bytes=path.stat().st_size,
    )


def table_file(warehouse: Path, table: str, seq: int) -> Path:
    return (
        warehouse
        / "funbuns"
        / table
        / "data"
        / f"commit_seq={seq}"
        / f"{table}_b{seq:06d}_000.parquet"
    )


def collect_prefix(
    warehouse: Path,
    *,
    start_seq: int,
    current_max_p: int,
) -> list[tuple[FooterFile, FooterFile]]:
    out: list[tuple[FooterFile, FooterFile]] = []
    expected_p_min_floor = current_max_p
    seq = start_seq
    while True:
        p_path = table_file(warehouse, "primes", seq)
        d_path = table_file(warehouse, "decompositions", seq)
        if not p_path.exists() or not d_path.exists():
            break
        primes = read_footer_file("primes", p_path, seq)
        decomps = read_footer_file("decompositions", d_path, seq)
        if (primes.p_min, primes.p_max) != (decomps.p_min, decomps.p_max):
            raise RuntimeError(
                f"commit_seq={seq}: primes p range {primes.p_min}..{primes.p_max} "
                f"!= decompositions range {decomps.p_min}..{decomps.p_max}"
            )
        if primes.p_min <= expected_p_min_floor:
            raise RuntimeError(
                f"commit_seq={seq}: p_min={primes.p_min} does not advance "
                f"past {expected_p_min_floor}"
            )
        expected_p_min_floor = primes.p_max
        out.append((primes, decomps))
        seq += 1
    return out


def write_manifest(path: Path, pairs: list[tuple[FooterFile, FooterFile]]) -> None:
    tmp = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    with tmp.open("w") as fh:
        for primes, decomps in pairs:
            fh.write(json.dumps(primes.to_json(), separators=(",", ":")) + "\n")
            fh.write(json.dumps(decomps.to_json(), separators=(",", ":")) + "\n")
    os.replace(tmp, path)


def quarantine_tmp_files(warehouse: Path, start_seq: int, stop_seq: int, quarantine: Path) -> int:
    moved = 0
    for table in ("primes", "decompositions"):
        data_dir = warehouse / "funbuns" / table / "data"
        for path in sorted(data_dir.glob("commit_seq=*/.*.tmp")):
            try:
                seq = int(path.parent.name.split("=", 1)[1])
            except (IndexError, ValueError):
                continue
            if seq < start_seq or seq > stop_seq + 1:
                continue
            relative = path.relative_to(warehouse)
            target = quarantine / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                raise RuntimeError(f"quarantine target already exists: {target}")
            shutil.move(str(path), str(target))
            moved += 1
            try:
                path.parent.rmdir()
            except OSError:
                pass
    return moved


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="commit recovered files")
    parser.add_argument("--manifest", type=Path, default=None, help="output manifest path")
    parser.add_argument("--quarantine-dir", type=Path, default=None)
    args = parser.parse_args()

    cat = open_catalog()
    primes_tbl = cat.load_table(PRIMES_IDENT)
    decomp_tbl = cat.load_table(DECOMP_IDENT)
    primes_cs, primes_p = table_manifest_bounds(primes_tbl)
    decomp_cs, decomp_p = table_manifest_bounds(decomp_tbl)
    if primes_cs is None or primes_p is None:
        raise RuntimeError("primes table has no committed bounds")
    if decomp_cs != primes_cs or decomp_p != primes_p:
        raise RuntimeError(
            f"table bounds disagree: primes=({primes_cs},{primes_p}) "
            f"decompositions=({decomp_cs},{decomp_p})"
        )

    warehouse = get_warehouse_dir()
    start_seq = primes_cs + 1
    referenced = table_referenced_data_files(primes_tbl) | table_referenced_data_files(decomp_tbl)
    pairs = collect_prefix(warehouse, start_seq=start_seq, current_max_p=primes_p)
    if not pairs:
        raise RuntimeError(f"no complete orphan prefix starting at commit_seq={start_seq}")
    candidate_paths = {f.path for pair in pairs for f in pair}
    if referenced & candidate_paths:
        raise RuntimeError("recovered prefix contains already referenced files")

    last_seq = pairs[-1][0].commit_seq
    max_p = pairs[-1][0].p_max
    summary = {
        "apply": args.apply,
        "warehouse": str(warehouse),
        "start_seq": start_seq,
        "last_seq": last_seq,
        "groups": len(pairs),
        "files": len(pairs) * 2,
        "prime_rows": sum(pair[0].rows for pair in pairs),
        "decomposition_rows": sum(pair[1].rows for pair in pairs),
        "start_p": pairs[0][0].p_min,
        "max_p": max_p,
        "current_warehouse_ok": warehouse_standing(cat)["ok"],
    }
    print(json.dumps(summary, indent=2))

    if not args.apply:
        return 0

    ts = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    manifest = args.manifest or warehouse.parent / f"native_files_recovered_{ts}.jsonl"
    quarantine = args.quarantine_dir or warehouse / f"_recovery_quarantine_{ts}"
    write_manifest(manifest, pairs)
    commit_summary = commit_native_manifest(manifest, warehouse=warehouse)
    moved_tmp = quarantine_tmp_files(warehouse, start_seq, last_seq, quarantine)
    final_state = warehouse_standing(open_catalog())
    print(json.dumps({"manifest": str(manifest), "commit": commit_summary, "moved_tmp": moved_tmp}, indent=2))
    print(json.dumps(final_state, indent=2))
    return 0 if final_state["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
