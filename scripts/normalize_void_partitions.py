#!/usr/bin/env python3
"""Rewrite the production source tables' legacy truncate partition to void.

This is a one-time migration aid for the Rust repartition pass. It does not
rewrite data files. It writes new manifests, a new manifest list, and a new
table metadata JSON for each source table, then atomically repoints the local
SqlCatalog row when ``--apply`` is given.
"""

from __future__ import annotations

import argparse
import copy
import json
import sqlite3
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from uuid import uuid4

from pyiceberg.manifest import read_manifest_list, write_manifest, write_manifest_list
from pyiceberg.partitioning import PartitionField, PartitionSpec
from pyiceberg.transforms import VoidTransform
from pyiceberg.typedef import Record

from primeparts.iceberg_schema import DECOMP_IDENT, PRIMES_IDENT, get_iceberg_dir, open_catalog

SOURCE_TABLES = (PRIMES_IDENT, DECOMP_IDENT)
AVRO_COMPRESSION = "deflate"


@dataclass(frozen=True)
class RewriteResult:
    ident: str
    old_metadata: str
    new_metadata: str
    manifest_count: int
    data_file_count: int


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


def _void_spec_from(spec: PartitionSpec) -> PartitionSpec:
    fields = [
        PartitionField(
            source_id=field.source_id,
            field_id=field.field_id,
            transform=VoidTransform(),
            name=field.name,
        )
        for field in spec.fields
    ]
    return PartitionSpec(*fields, spec_id=spec.spec_id)


def _void_json_specs(metadata: dict[str, Any]) -> None:
    for spec in metadata.get("partition-specs", []):
        for field in spec.get("fields", []):
            field["transform"] = "void"


def _current_snapshot_json(metadata: dict[str, Any]) -> dict[str, Any]:
    current_id = metadata.get("current-snapshot-id")
    for snapshot in metadata.get("snapshots", []):
        if snapshot.get("snapshot-id") == current_id:
            return snapshot
    raise RuntimeError(f"current snapshot {current_id!r} not found in metadata")


def _rewrite_manifests(table, *, apply: bool) -> tuple[str, int, int]:
    snapshot = table.metadata.current_snapshot()
    if snapshot is None:
        raise RuntimeError(f"{table.name()}: table has no current snapshot")

    manifests = list(read_manifest_list(table.io.new_input(snapshot.manifest_list)))
    metadata_dir = _metadata_dir(table.location())
    new_manifest_files = []
    data_file_count = 0

    for index, manifest_file in enumerate(manifests):
        entries = manifest_file.fetch_manifest_entry(table.io, discard_deleted=False)
        data_file_count += len(entries)
        spec = table.specs()[manifest_file.partition_spec_id]
        void_spec = _void_spec_from(spec)
        null_partition = Record(*([None] * len(void_spec.fields)))
        new_manifest_location = f"{metadata_dir}/{uuid4()}-void-m{index}.avro"

        if apply:
            with write_manifest(
                table.metadata.format_version,
                void_spec,
                table.schema(),
                table.io.new_output(new_manifest_location),
                snapshot.snapshot_id,
                AVRO_COMPRESSION,
            ) as writer:
                for entry in entries:
                    entry.data_file._data[3] = copy.copy(null_partition)
                    writer.add_entry(entry)
                new_manifest_files.append(writer.to_manifest_file())
        else:
            new_manifest_files.append(manifest_file)

    new_manifest_list = f"{metadata_dir}/snap-{snapshot.snapshot_id}-0-{uuid4()}-void.avro"
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

    return new_manifest_list, len(manifests), data_file_count


def _write_metadata_json(table, new_manifest_list: str, *, apply: bool) -> str:
    old_metadata = table.metadata_location
    new_metadata = _next_metadata_location(old_metadata)
    metadata_path = _local_path(old_metadata)
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    now_ms = int(time.time() * 1000)

    old_log_entry = {"metadata-file": old_metadata, "timestamp-ms": metadata.get("last-updated-ms", now_ms)}
    metadata.setdefault("metadata-log", []).append(old_log_entry)
    metadata["last-updated-ms"] = now_ms
    _void_json_specs(metadata)

    snapshot = _current_snapshot_json(metadata)
    snapshot["manifest-list"] = new_manifest_list
    if isinstance(snapshot.get("summary"), dict):
        snapshot["summary"]["changed-partition-count"] = "1"

    if apply:
        out_path = _local_path(new_metadata)
        out_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    return new_metadata


def _catalog_db() -> Path:
    return get_iceberg_dir() / "catalog.db"


def _commit_catalog_pointer(ident: str, old_metadata: str, new_metadata: str) -> None:
    namespace, table_name = ident.split(".", 1)
    db_path = _catalog_db()
    con = sqlite3.connect(db_path)
    try:
        with con:
            row = con.execute(
                """
                SELECT catalog_name, metadata_location
                FROM iceberg_tables
                WHERE table_namespace = ? AND table_name = ?
                """,
                (namespace, table_name),
            ).fetchone()
            if row is None:
                raise RuntimeError(f"{ident}: no SqlCatalog row in {db_path}")
            catalog_name, current_metadata = row
            if current_metadata != old_metadata:
                raise RuntimeError(
                    f"{ident}: catalog moved while rewriting; expected {old_metadata}, found {current_metadata}"
                )
            updated = con.execute(
                """
                UPDATE iceberg_tables
                SET metadata_location = ?, previous_metadata_location = ?, iceberg_type = COALESCE(iceberg_type, 'TABLE')
                WHERE catalog_name = ? AND table_namespace = ? AND table_name = ? AND metadata_location = ?
                """,
                (new_metadata, old_metadata, catalog_name, namespace, table_name, old_metadata),
            ).rowcount
            if updated != 1:
                raise RuntimeError(f"{ident}: expected to update one catalog row, updated {updated}")
    finally:
        con.close()


def rewrite_table(ident: str, *, apply: bool) -> RewriteResult:
    catalog = open_catalog()
    table = catalog.load_table(ident)
    old_metadata = table.metadata_location
    new_manifest_list, manifest_count, data_file_count = _rewrite_manifests(table, apply=apply)
    new_metadata = _write_metadata_json(table, new_manifest_list, apply=apply)
    if apply:
        _commit_catalog_pointer(ident, old_metadata, new_metadata)
    return RewriteResult(ident, old_metadata, new_metadata, manifest_count, data_file_count)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--table",
        action="append",
        choices=SOURCE_TABLES,
        help="Table to normalize. May be repeated. Defaults to primes and decompositions.",
    )
    parser.add_argument("--apply", action="store_true", help="Write new metadata and repoint SqlCatalog.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    tables = args.table or list(SOURCE_TABLES)
    for ident in tables:
        result = rewrite_table(ident, apply=args.apply)
        mode = "rewrote" if args.apply else "would rewrite"
        print(
            f"{mode} {result.ident}: {result.manifest_count} manifest(s), "
            f"{result.data_file_count} data file(s)"
        )
        print(f"  old: {result.old_metadata}")
        print(f"  new: {result.new_metadata}")

    if not args.apply:
        print("dry run only; rerun with --apply to commit the SqlCatalog metadata pointer")


if __name__ == "__main__":
    main()
