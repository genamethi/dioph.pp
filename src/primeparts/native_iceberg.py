"""
Commit bridge for native Iceberg data files.

The native writer owns the hot path:

    C materialization -> iceberg-cpp Parquet -> native_files.jsonl

This module owns the interim catalog boundary. It reads the JSONL file list,
validates the Parquet footer metadata, and registers files through PyIceberg's
SqlCatalog with one ``add_files`` call per table.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pyarrow.parquet as pq

from .iceberg_schema import (
    DECOMP_IDENT,
    PRIMES_IDENT,
    assert_warehouse_good_standing,
    ensure_tables,
    open_catalog,
    table_manifest_bounds,
    warehouse_standing,
)


_TABLE_IDENTS = {
    "primes": PRIMES_IDENT,
    "decompositions": DECOMP_IDENT,
}


@dataclass(frozen=True, slots=True)
class NativeFile:
    table: str
    path: Path
    commit_seq: int
    rows: int
    p_min: int
    p_max: int
    bytes: int

    @classmethod
    def from_json(cls, obj: dict[str, Any], *, base_dir: Path) -> "NativeFile":
        table = str(obj["table"])
        if table not in _TABLE_IDENTS:
            raise ValueError(f"unknown native file table: {table!r}")

        path = Path(str(obj["path"]))
        if not path.is_absolute():
            path = base_dir / path

        out = cls(
            table=table,
            path=path.resolve(strict=False),
            commit_seq=int(obj["commit_seq"]),
            rows=int(obj["rows"]),
            p_min=int(obj["p_min"]),
            p_max=int(obj["p_max"]),
            bytes=int(obj["bytes"]),
        )
        if out.rows <= 0:
            raise ValueError(f"{out.path}: native manifest rows must be positive")
        if out.p_min <= 0 or out.p_max < out.p_min:
            raise ValueError(f"{out.path}: invalid p range {out.p_min}..{out.p_max}")
        if out.bytes <= 0:
            raise ValueError(f"{out.path}: native manifest bytes must be positive")
        return out


def load_native_manifest(manifest: Path) -> list[NativeFile]:
    """Read ``native_files.jsonl`` emitted by ``primeparts-iceberg-write``."""
    manifest = manifest.resolve(strict=True)
    files: list[NativeFile] = []
    with manifest.open() as fh:
        for line_no, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
                files.append(NativeFile.from_json(obj, base_dir=manifest.parent))
            except Exception as exc:
                raise ValueError(f"{manifest}:{line_no}: invalid native manifest row") from exc

    if not files:
        raise ValueError(f"{manifest}: manifest is empty")
    if not any(f.table == "primes" for f in files):
        raise ValueError(f"{manifest}: manifest has no primes files")

    return sorted(files, key=lambda f: (f.commit_seq, f.table, str(f.path)))


def _kv_string(metadata: dict[bytes, bytes], key: str) -> str | None:
    value = metadata.get(key.encode())
    return value.decode() if value is not None else None


def verify_native_files(files: list[NativeFile]) -> None:
    """Validate file existence, manifest counts, and funbuns footer metadata."""
    seen: set[Path] = set()
    for file in files:
        if file.path in seen:
            raise ValueError(f"duplicate native manifest path: {file.path}")
        seen.add(file.path)

        if not file.path.exists():
            raise FileNotFoundError(file.path)
        stat_size = file.path.stat().st_size
        if stat_size != file.bytes:
            raise ValueError(
                f"{file.path}: manifest bytes={file.bytes}, stat bytes={stat_size}"
            )

        metadata = pq.read_metadata(file.path)
        if metadata.num_rows != file.rows:
            raise ValueError(
                f"{file.path}: manifest rows={file.rows}, parquet rows={metadata.num_rows}"
            )
        kv = metadata.metadata or {}
        expected = {
            "funbuns.table": file.table,
            "funbuns.commit_seq": str(file.commit_seq),
            "funbuns.p_min": str(file.p_min),
            "funbuns.p_max": str(file.p_max),
            "funbuns.n_rows": str(file.rows),
        }
        for key, value in expected.items():
            actual = _kv_string(kv, key)
            if actual != value:
                raise ValueError(f"{file.path}: {key}={actual!r}, expected {value!r}")
        if _kv_string(kv, "funbuns.n_primes") is None:
            raise ValueError(f"{file.path}: missing normalized funbuns.n_primes footer")


def _infer_manifest_warehouse(manifest: Path) -> Path | None:
    """Infer the temp warehouse created by ``--temp`` native writer runs."""
    candidate = manifest.resolve().parent / "warehouse"
    if (candidate / "funbuns").exists():
        return candidate
    return None


def commit_native_manifest(
    manifest: Path,
    *,
    warehouse: Path | None = None,
    temp: bool = False,
    verify_footers: bool = True,
    check_duplicate_files: bool = True,
    dry_run: bool = False,
) -> dict[str, Any]:
    """
    Register native Parquet files in an Iceberg catalog.

    When ``warehouse`` is provided, or ``temp`` can infer a sibling
    ``warehouse`` directory from the manifest path, an isolated SqlCatalog is
    opened beside that warehouse. Otherwise the configured production catalog
    is used.
    """
    manifest = manifest.resolve(strict=True)
    files = load_native_manifest(manifest)
    if verify_footers:
        verify_native_files(files)

    inferred_warehouse = warehouse
    catalog_mode = "configured"
    if inferred_warehouse is None:
        inferred_warehouse = _infer_manifest_warehouse(manifest)
        if inferred_warehouse is not None:
            catalog_mode = "manifest-local"
    else:
        catalog_mode = "explicit-warehouse"

    if temp and warehouse is None:
        inferred_warehouse = inferred_warehouse or _infer_manifest_warehouse(manifest)
        if inferred_warehouse is None:
            raise ValueError(
                "--temp was requested, but no sibling warehouse directory was found"
            )
        catalog_mode = "temp"

    cat = (
        open_catalog(warehouse_root=inferred_warehouse)
        if inferred_warehouse is not None
        else open_catalog()
    )
    primes_tbl, decomp_tbl = ensure_tables(cat)
    pending_paths = {f.path for f in files}
    if not temp:
        assert_warehouse_good_standing(
            cat,
            allowed_new_files=pending_paths,
            allow_stale_summary=True,
        )

    by_table = {
        table: [f for f in files if f.table == table]
        for table in ("primes", "decompositions")
    }
    primes_files = by_table["primes"]
    decomp_files = by_table["decompositions"]

    existing_primes_cs, existing_primes_p = table_manifest_bounds(primes_tbl)
    existing_decomp_cs, _ = table_manifest_bounds(decomp_tbl)
    max_p = max(v for v in (existing_primes_p, max(f.p_max for f in primes_files)) if v is not None)
    max_commit_seq = max(
        v
        for v in (
            existing_primes_cs,
            existing_decomp_cs,
            max(f.commit_seq for f in files),
        )
        if v is not None
    )
    snapshot_properties = {
        "funbuns.max_p": str(max_p),
        "funbuns.max_commit_seq": str(max_commit_seq),
    }

    if not dry_run:
        primes_tbl.add_files(
            [str(f.path) for f in primes_files],
            snapshot_properties=snapshot_properties,
            check_duplicate_files=check_duplicate_files,
        )
        if decomp_files:
            decomp_tbl.add_files(
                [str(f.path) for f in decomp_files],
                snapshot_properties=snapshot_properties,
                check_duplicate_files=check_duplicate_files,
            )

    return {
        "manifest": str(manifest),
        "catalog_mode": catalog_mode,
        "warehouse": str(inferred_warehouse.resolve()) if inferred_warehouse else None,
        "dry_run": dry_run,
        "files": len(files),
        "prime_files": len(primes_files),
        "decomposition_files": len(decomp_files),
        "prime_rows": sum(f.rows for f in primes_files),
        "decomposition_rows": sum(f.rows for f in decomp_files),
        "bytes": sum(f.bytes for f in files),
        "max_p": max_p,
        "max_commit_seq": max_commit_seq,
        "snapshot_properties": snapshot_properties,
    }


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Register native primeparts Parquet files with PyIceberg.",
    )
    parser.add_argument("manifest_pos", nargs="?", help="native_files.jsonl path")
    parser.add_argument("--manifest", help="native_files.jsonl path")
    parser.add_argument(
        "--warehouse",
        type=Path,
        help="Open an isolated SqlCatalog for this warehouse root",
    )
    parser.add_argument(
        "--temp",
        action="store_true",
        help="Require temp warehouse resolution beside the manifest",
    )
    parser.add_argument(
        "--no-verify-footers",
        action="store_true",
        help="Skip footer/manifest consistency checks before add_files",
    )
    parser.add_argument(
        "--skip-duplicate-check",
        action="store_true",
        help="Pass check_duplicate_files=False to PyIceberg add_files",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and summarize without committing to the catalog",
    )
    parser.add_argument(
        "--check-warehouse",
        action="store_true",
        help="Inspect warehouse health and exit without committing",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> None:
    args = _parse_args(argv)
    manifest = args.manifest or args.manifest_pos
    if args.check_warehouse:
        cat = open_catalog(warehouse_root=args.warehouse) if args.warehouse else open_catalog()
        state = warehouse_standing(cat)
        print(json.dumps(state, sort_keys=True))
        raise SystemExit(0 if state["ok"] else 1)

    if manifest is None:
        raise SystemExit("--manifest or a positional manifest path is required")

    summary = commit_native_manifest(
        Path(manifest),
        warehouse=args.warehouse,
        temp=args.temp,
        verify_footers=not args.no_verify_footers,
        check_duplicate_files=not args.skip_duplicate_check,
        dry_run=args.dry_run,
    )
    print(json.dumps(summary, sort_keys=True))


if __name__ == "__main__":
    main()
