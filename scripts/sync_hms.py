#!/usr/bin/env python3
"""
Sync HMS's metadata_location pointers to match the local sqlite catalog.

Funbuns writes new snapshots via :class:`pyiceberg.catalog.sql.SqlCatalog`
(see ``IcebergWriter.flush`` in ``src/primeparts/iceberg_schema.py``). HMS is
unaware of those commits until this script runs, at which point Hive's
``HiveIcebergStorageHandler`` sees the fresh snapshot on its next query.

Run after a core.py ingest session (or wire into a post-flush hook). No
parquet is rewritten; only HMS table properties change.

Idempotent: if HMS is already in sync, each table prints "in sync" and exits 0.

Env overrides:
  FUNBUNS_CATALOG_URI   sqlite URI  (default: catalog.db on extssd)
  FUNBUNS_HMS_URI       thrift URI  (default: thrift://localhost:9083)
  FUNBUNS_WAREHOUSE     warehouse   (default: file:/// on extssd)
"""

from __future__ import annotations

import argparse
import os
import sys

import primeparts # applies pyiceberg + HMS Thrift patches  # noqa: F401

from pyiceberg.catalog.hive import HiveCatalog
from pyiceberg.catalog.sql import SqlCatalog


SQLITE_URI = os.environ.get(
    "FUNBUNS_CATALOG_URI",
    "sqlite:////media/extssd/research/dioph.pp/data/iceberg/catalog.db",
)
HMS_URI = os.environ.get("FUNBUNS_HMS_URI", "thrift://localhost:9083")
WAREHOUSE = os.environ.get(
    "FUNBUNS_WAREHOUSE",
    "file:///media/extssd/research/dioph.pp/data/iceberg/warehouse",
)

TABLES: list[tuple[str, str]] = [
    ("funbuns", "primes"),
    ("funbuns", "decompositions"),
]


def _short(path: str) -> str:
    return path.rsplit("/", 1)[-1] if path else "<none>"


def _strip_scheme(path: str) -> str:
    """HMS stores bare filesystem paths; sqlite stores ``file:///...`` URIs."""
    if path.startswith("file://"):
        return path[len("file://") :]
    return path


def _hms_get_table(hive_cat: HiveCatalog, db: str, tbl: str):
    with hive_cat._client as cl:
        return cl.get_table(dbname=db, tbl_name=tbl)


def _hms_alter_metadata_location(
    hive_cat: HiveCatalog, db: str, tbl: str, new: str, prev: str
) -> None:
    from hive_metastore.ttypes import EnvironmentContext

    with hive_cat._client as cl:
        htbl = cl.get_table(dbname=db, tbl_name=tbl)
        htbl.parameters["metadata_location"] = new
        htbl.parameters["previous_metadata_location"] = prev
        cl.alter_table_with_environment_context(
            dbname=db,
            tbl_name=tbl,
            new_tbl=htbl,
            environment_context=EnvironmentContext(properties={}),
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="report diffs, don't alter HMS",
    )
    args = ap.parse_args()

    sql_cat = SqlCatalog("funbuns", uri=SQLITE_URI, warehouse=WAREHOUSE)
    hive_cat = HiveCatalog("hms", uri=HMS_URI, warehouse=WAREHOUSE)

    for db, tbl in TABLES:
        t = sql_cat.load_table((db, tbl))
        want = _strip_scheme(t.metadata_location)
        htbl = _hms_get_table(hive_cat, db, tbl)
        have = _strip_scheme(htbl.parameters.get("metadata_location", ""))

        if have == want:
            print(f"{db}.{tbl}: in sync at {_short(want)}")
            continue

        print(f"{db}.{tbl}: hms={_short(have)} -> sqlite={_short(want)}")
        if args.dry_run:
            continue
        _hms_alter_metadata_location(hive_cat, db, tbl, want, have)
        print(f"  updated")

    return 0


if __name__ == "__main__":
    sys.exit(main())
