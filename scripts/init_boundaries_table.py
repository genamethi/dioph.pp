#!/usr/bin/env python3
"""Create and populate the tiny funbuns.boundaries Iceberg table."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable

import pyarrow as pa
from pyiceberg.catalog import Catalog
from pyiceberg.partitioning import PartitionSpec
from pyiceberg.schema import Schema
from pyiceberg.table.sorting import SortDirection, SortField, SortOrder
from pyiceberg.transforms import IdentityTransform
from pyiceberg.types import IntegerType, LongType, NestedField

from primeparts.iceberg_schema import NAMESPACE, open_catalog

BOUNDARY_VERSION = 1
BOUNDARIES_IDENT = f"{NAMESPACE}.boundaries"
DEFAULT_INPUT = Path("native/bin/boundary_primes.tsv")

BOUNDARIES_SCHEMA = Schema(
    NestedField(1, "p_bucket_version", IntegerType(), required=True),
    NestedField(2, "p_bucket", IntegerType(), required=True),
    NestedField(3, "p_min", LongType(), required=True),
)
BOUNDARIES_SORT_ORDER = SortOrder(
    SortField(source_id=1, transform=IdentityTransform(), direction=SortDirection.ASC),
    SortField(source_id=2, transform=IdentityTransform(), direction=SortDirection.ASC),
)
BOUNDARIES_PROPERTIES = {
    "write.format.default": "parquet",
    "write.parquet.compression-codec": "zstd",
    "write.parquet.compression-level": "3",
}
BOUNDARIES_ARROW_SCHEMA = pa.schema(
    [
        pa.field("p_bucket_version", pa.int32(), nullable=False),
        pa.field("p_bucket", pa.int32(), nullable=False),
        pa.field("p_min", pa.int64(), nullable=False),
    ]
)


def parse_boundary_rows(path: Path, *, version: int = BOUNDARY_VERSION) -> list[dict[str, int]]:
    rows: list[dict[str, int]] = []
    previous_rank: int | None = None
    previous_prime: int | None = None

    with path.open("r", encoding="ascii") as fh:
        for line_no, raw in enumerate(fh, start=1):
            line = raw.strip()
            if not line:
                continue
            try:
                rank_text, prime_text = line.split("\t")
                rank = int(rank_text)
                prime = int(prime_text)
            except ValueError as exc:
                raise ValueError(f"{path}:{line_no}: expected '<rank>\\t<prime>'") from exc
            if previous_rank is not None and rank <= previous_rank:
                raise ValueError(f"{path}:{line_no}: ranks must be strictly increasing")
            if previous_prime is not None and prime <= previous_prime:
                raise ValueError(f"{path}:{line_no}: primes must be strictly increasing")
            rows.append(
                {
                    "p_bucket_version": version,
                    "p_bucket": len(rows),
                    "p_min": prime,
                }
            )
            previous_rank = rank
            previous_prime = prime

    if not rows:
        raise ValueError(f"{path}: no boundary rows found")
    return rows


def rows_to_arrow(rows: Iterable[dict[str, int]]) -> pa.Table:
    return pa.Table.from_pylist(list(rows), schema=BOUNDARIES_ARROW_SCHEMA)


def existing_rows(table) -> list[dict[str, int]]:
    if not hasattr(table, "scan"):
        return []
    try:
        arrow = table.scan().to_arrow()
    except Exception as exc:
        raise RuntimeError("failed to scan existing boundaries table") from exc
    if arrow.num_rows == 0:
        return []
    return sorted(arrow.to_pylist(), key=lambda row: (row["p_bucket_version"], row["p_bucket"]))


def write_boundaries(cat: Catalog, rows: list[dict[str, int]]) -> tuple[object, bool]:
    cat.create_namespace_if_not_exists(NAMESPACE)
    table = cat.create_table_if_not_exists(
        identifier=BOUNDARIES_IDENT,
        schema=BOUNDARIES_SCHEMA,
        partition_spec=PartitionSpec(),
        sort_order=BOUNDARIES_SORT_ORDER,
        properties=BOUNDARIES_PROPERTIES,
    )

    ordered_rows = sorted(rows, key=lambda row: (row["p_bucket_version"], row["p_bucket"]))
    current = existing_rows(table)
    if current:
        if current == ordered_rows:
            return table, False
        raise RuntimeError(
            f"{BOUNDARIES_IDENT} already has {len(current)} row(s); "
            "refusing to append duplicate or conflicting boundaries"
        )

    table.append(
        rows_to_arrow(ordered_rows),
        snapshot_properties={"funbuns.boundary_version": str(BOUNDARY_VERSION)},
    )
    return table, True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--warehouse-root", type=Path, default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    rows = parse_boundary_rows(args.input)
    cat = open_catalog(warehouse_root=args.warehouse_root) if args.warehouse_root else open_catalog()
    _, appended = write_boundaries(cat, rows)
    action = "appended" if appended else "verified existing"
    print(f"{action} {len(rows)} boundary row(s) in {BOUNDARIES_IDENT}")


if __name__ == "__main__":
    main()
