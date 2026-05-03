"""
Iceberg table definitions and write primitive for the prime-power decomposition
dataset.

Two tables:

    funbuns.primes          — one row per p (including k=0), universe of primes
    funbuns.decompositions  — fact table, one row per (p, m_k, n_k, q_k) with q_k>0

Both are partitioned by truncate(p, 10^10) and sorted by (p) / (p, m_k).
The sort declaration is advisory in PyIceberg 0.11.1 — we enforce it on the
writer side via pre-sort and validation.

``commit_seq`` is a **write-side flush counter**, not a position along the
p-axis. Under multiprocessing ingest (``imap_unordered``) workers finish in
non-deterministic order, so a batch with lower commit_seq may cover a higher
p-range than a later one. Every (p_lo, p_hi) range is still disjoint and
contiguous, but ``ORDER BY commit_seq`` does not walk primes in p-order.
Queries that want p-ordering must sort on ``p`` (or read the manifest's
``p.lower_bound`` per file). Resume state is derived from ``max(p)``, not
from ``max(commit_seq)``, so reorder is safe for the write path.

Public surface:

    open_catalog(warehouse_root=None) -> Catalog
    ensure_tables(cat) -> (primes_tbl, decomp_tbl)
    validate_primes_batch(df, *, commit_seq)
    validate_decomp_batch(df, *, commit_seq)
    validate_cross(primes_df, decomp_df)
    build_file_kv(pa_table, *, table_name, commit_seq, ...) -> dict[bytes, bytes]
    IcebergWriter(cat=None).flush(raw_df) -> WriteBatchResult    # parquet only
    IcebergWriter.commit_pending()                                # catalog commit

Write path does not go through polars.DataFrame.write_iceberg. PyIceberg 0.11.1
ignores row-group sizing, does not sort on write, and does not pass through
parquet key-value metadata. Instead we:

    sort → chunk at p-boundaries → pa.Table with custom KV → pq.write_table
    (hot path ends here; files are durable on disk)
    ... later: tbl.add_files(all_pending, snapshot_properties=...)

``add_files`` is deferred to ``commit_pending`` so ingest runs don't pay
per-batch catalog I/O. One snapshot per run replaces N per-batch
snapshots.

Schema evolution:

    To add a column to either table:

        1. Bump SCHEMA_VERSION here
        2. Add the NestedField to PRIMES_SCHEMA or DECOMP_SCHEMA with the next
           available field_id (never reuse)
        3. Run a one-off migration:
               tbl = cat.load_table(PRIMES_IDENT)
               with tbl.update_schema() as us:
                   us.add_column("new_col", IntegerType(), required=False)
        4. Old files remain readable; Iceberg fills missing columns with NULL
        5. Update build_file_kv / validators as needed

    Never change field_id assignments or types of existing fields. Renames go
    through update_schema().rename_column(); drops through .delete_column().
"""

from __future__ import annotations

import json
import os
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import polars as pl
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
from pyiceberg.catalog import Catalog
from pyiceberg.catalog.hive import HiveCatalog
from pyiceberg.catalog.sql import SqlCatalog
from pyiceberg.exceptions import NoSuchIdentifierError, NoSuchTableError
from pyiceberg.io.pyarrow import schema_to_pyarrow
from pyiceberg.partitioning import PartitionField, PartitionSpec
from pyiceberg.schema import Schema
from pyiceberg.table import Table
from pyiceberg.table.sorting import SortDirection, SortField, SortOrder
from pyiceberg.transforms import IdentityTransform, TruncateTransform
from pyiceberg.types import IntegerType, LongType, NestedField

from . import __version__ as _PACKAGE_VERSION  # noqa: F401  # re-exported via build_file_kv
from . import _patches  # noqa: F401  — applies pyiceberg patches; lifted out of __init__.py so workers don't pay for pyarrow import
from .utils import get_config

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SCHEMA_VERSION = 1
NAMESPACE = "funbuns"
PRIMES_IDENT = f"{NAMESPACE}.primes"
DECOMP_IDENT = f"{NAMESPACE}.decompositions"
_IDENT_TO_NAME = {PRIMES_IDENT: "primes", DECOMP_IDENT: "decompositions"}

PRIMES_SCHEMA = Schema(
    NestedField(1, "p", LongType(), required=False),
    NestedField(2, "k", IntegerType(), required=False),
    NestedField(3, "commit_seq", IntegerType(), required=False),
)

DECOMP_SCHEMA = Schema(
    NestedField(1, "p", LongType(), required=False),
    NestedField(2, "m_k", IntegerType(), required=False),
    NestedField(3, "n_k", IntegerType(), required=False),
    NestedField(4, "q_k", LongType(), required=False),
    NestedField(5, "commit_seq", IntegerType(), required=False),
)

PRIMES_PARTITION_SPEC = PartitionSpec(
    PartitionField(source_id=1, field_id=1000, transform=TruncateTransform(10**10), name="p_trunc")
)
DECOMP_PARTITION_SPEC = PartitionSpec(
    PartitionField(source_id=1, field_id=1000, transform=TruncateTransform(10**10), name="p_trunc")
)

PRIMES_SORT_ORDER = SortOrder(
    SortField(source_id=1, transform=IdentityTransform(), direction=SortDirection.ASC)
)
DECOMP_SORT_ORDER = SortOrder(
    SortField(source_id=1, transform=IdentityTransform(), direction=SortDirection.ASC),
    SortField(source_id=2, transform=IdentityTransform(), direction=SortDirection.ASC),
)

TARGET_FILE_SIZE_BYTES = 1 << 30        # 1 GiB
ROW_GROUP_SIZE_BYTES = 128 << 20        # 128 MiB
ROW_GROUP_SIZE_ROWS = 1_048_576         # pyarrow takes rows, not bytes

# Observed in step-1 prototype; conservative rounding
BYTES_PER_ROW_PRIMES = 3
BYTES_PER_ROW_DECOMP = 6

TABLE_PROPERTIES: dict[str, str] = {
    "write.parquet.compression-codec": "zstd",
    "write.parquet.compression-level": "3",
    "write.target-file-size-bytes": str(TARGET_FILE_SIZE_BYTES),
    "funbuns.schema_version": str(SCHEMA_VERSION),
    "funbuns.algorithm_version": _PACKAGE_VERSION if isinstance(_PACKAGE_VERSION, str) else "unknown",
}

PARQUET_WRITER_KWARGS: dict[str, Any] = {
    "compression": "zstd",
    "compression_level": 3,
    "row_group_size": ROW_GROUP_SIZE_ROWS,
    "write_statistics": True,
    "use_dictionary": True,
    "data_page_size": 1 << 20,
}

# ---------------------------------------------------------------------------
# Catalog / filesystem layout
# ---------------------------------------------------------------------------

def get_iceberg_dir() -> Path:
    if env := os.getenv("FUNBUNS_ICEBERG_DIR"):
        return Path(env)
    cfg = get_config()
    if d := cfg.get("iceberg_dir"):
        return Path(d)
    raise RuntimeError("iceberg_dir not configured in pixi.toml [tool.funbuns.directories]")


def get_warehouse_dir() -> Path:
    return get_iceberg_dir() / "warehouse"


HMS_URI_DEFAULT = "thrift://localhost:9083"


def open_catalog(warehouse_root: Path | None = None) -> Catalog:
    """
    Return an Iceberg catalog handle.

    Production (no args): SqlCatalog backed by ``<iceberg_dir>/catalog.db``.
    No dependency on Hive/HMS/k8s — core.py ingest and polars reads work
    directly against the filesystem + sqlite. Run ``scripts/sync_hms.py``
    after a flush to publish the new snapshot to Hive Metastore.

    Set ``FUNBUNS_CATALOG_BACKEND=hive`` to instead open a HiveCatalog at
    ``FUNBUNS_HMS_URI`` (default ``thrift://localhost:9083``) — useful for
    scripts that need to round-trip through HMS itself.

    Temp/test (warehouse_root given): isolated SqlCatalog backed by SQLite
    in the same directory — no HMS dependency for throwaway runs.
    """
    if warehouse_root is not None:
        catalog_db = warehouse_root.parent / "catalog.db"
        warehouse_root.mkdir(parents=True, exist_ok=True)
        return SqlCatalog(
            "funbuns",
            uri=f"sqlite:///{catalog_db}",
            warehouse=f"file://{warehouse_root}",
        )
    warehouse = get_warehouse_dir()
    warehouse.mkdir(parents=True, exist_ok=True)
    backend = os.getenv("FUNBUNS_CATALOG_BACKEND", "sqlite").lower()
    if backend == "hive":
        uri = os.getenv("FUNBUNS_HMS_URI", HMS_URI_DEFAULT)
        return HiveCatalog(
            "funbuns",
            uri=uri,
            warehouse=f"file://{warehouse}",
        )
    catalog_db = get_iceberg_dir() / "catalog.db"
    return SqlCatalog(
        "funbuns",
        uri=f"sqlite:///{catalog_db}",
        warehouse=f"file://{warehouse}",
    )


def _partition_dir(tbl: Table, commit_seq: int) -> Path:
    location = tbl.location().replace("file://", "")
    return Path(location) / "data" / f"commit_seq={commit_seq}"


def _load_or_create_table(
    cat: Catalog,
    *,
    identifier: str,
    schema: Schema,
    partition_spec: PartitionSpec,
    sort_order: SortOrder,
    properties: dict[str, str],
) -> Table:
    try:
        return cat.load_table(identifier)
    except (NoSuchIdentifierError, NoSuchTableError):
        return cat.create_table_if_not_exists(
            identifier=identifier,
            schema=schema,
            partition_spec=partition_spec,
            sort_order=sort_order,
            properties=properties,
        )


def ensure_tables(cat: Catalog) -> tuple[Table, Table]:
    cat.create_namespace_if_not_exists(NAMESPACE)
    primes_tbl = _load_or_create_table(
        cat,
        identifier=PRIMES_IDENT,
        schema=PRIMES_SCHEMA,
        partition_spec=PRIMES_PARTITION_SPEC,
        sort_order=PRIMES_SORT_ORDER,
        properties=TABLE_PROPERTIES,
    )
    decomp_tbl = _load_or_create_table(
        cat,
        identifier=DECOMP_IDENT,
        schema=DECOMP_SCHEMA,
        partition_spec=DECOMP_PARTITION_SPEC,
        sort_order=DECOMP_SORT_ORDER,
        properties=TABLE_PROPERTIES,
    )
    return primes_tbl, decomp_tbl


def scan_primes(cat: Catalog | None = None) -> pl.LazyFrame:
    """Lazy scan over ``funbuns.primes`` with predicate pushdown via manifests."""
    cat = cat or open_catalog()
    return pl.scan_iceberg(cat.load_table(PRIMES_IDENT))


def scan_decompositions(cat: Catalog | None = None) -> pl.LazyFrame:
    """Lazy scan over ``funbuns.decompositions`` with predicate pushdown via manifests."""
    cat = cat or open_catalog()
    return pl.scan_iceberg(cat.load_table(DECOMP_IDENT))


def table_manifest_bounds(tbl: Table) -> tuple[int | None, int | None]:
    """Return ``(max_commit_seq, max_p)`` from Iceberg manifest metrics.

    Compacted `p_trunc=...` files can leave ``commit_seq`` bounds null while
    still preserving `p` bounds, so the first element may be ``None`` even for
    a healthy table.
    """
    files = pl.from_arrow(tbl.inspect.files().select(["readable_metrics"]))
    if files.height == 0:
        return None, None
    bounds = files.select(
        pl.col("readable_metrics")
        .struct.field("commit_seq")
        .struct.field("upper_bound")
        .alias("cs"),
        pl.col("readable_metrics")
        .struct.field("p")
        .struct.field("upper_bound")
        .alias("p"),
    ).select(
        pl.col("cs").max().alias("max_cs"),
        pl.col("p").max().alias("max_p"),
    )
    max_cs = bounds["max_cs"].item()
    max_p = bounds["max_p"].item()
    return (
        int(max_cs) if max_cs is not None else None,
        int(max_p) if max_p is not None else None,
    )


def table_filesystem_max_commit_seq(tbl: Table) -> int | None:
    """Return max ``commit_seq`` directory present on the local filesystem."""
    location = _local_table_path(tbl)
    if location is None:
        return None

    data_dir = location / "data"
    if not data_dir.exists():
        return None

    max_commit_seq: int | None = None
    for child in data_dir.glob("commit_seq=*"):
        if not child.is_dir():
            continue
        try:
            value = int(child.name.split("=", 1)[1])
        except (IndexError, ValueError):
            continue
        if max_commit_seq is None or value > max_commit_seq:
            max_commit_seq = value
    return max_commit_seq


class WarehouseStateError(RuntimeError):
    """Raised when a production write would touch an unhealthy warehouse."""


def _local_table_path(tbl: Table) -> Path | None:
    location = tbl.location()
    if location.startswith("file://"):
        return Path(location[len("file://"):])
    if "://" in location:
        return None
    return Path(location)


def _local_file_path(file_path: str) -> Path | None:
    if file_path.startswith("file://"):
        return Path(file_path[len("file://"):]).resolve(strict=False)
    if "://" in file_path:
        return None
    return Path(file_path).resolve(strict=False)


def table_referenced_data_files(tbl: Table) -> set[Path]:
    files = pl.from_arrow(tbl.inspect.files().select(["file_path"]))
    out: set[Path] = set()
    for file_path in files["file_path"].to_list():
        local = _local_file_path(str(file_path))
        if local is not None:
            out.add(local)
    return out


def _referenced_file_frame(tbl: Table) -> pl.DataFrame:
    files = pl.from_arrow(tbl.inspect.files().select(["file_path", "readable_metrics"]))
    if files.height == 0:
        return pl.DataFrame(
            {
                "file_path": [],
                "commit_seq": [],
                "p_min": [],
                "p_max": [],
                "partition_dir": [],
                "partition_name": [],
                "partition_layout": [],
            },
            schema={
                "file_path": pl.Utf8,
                "commit_seq": pl.Int64,
                "p_min": pl.Int64,
                "p_max": pl.Int64,
                "partition_dir": pl.Utf8,
                "partition_name": pl.Utf8,
                "partition_layout": pl.Utf8,
            },
        )
    return (
        files.select(
        pl.col("file_path").cast(pl.Utf8),
        pl.col("readable_metrics")
        .struct.field("commit_seq")
        .struct.field("upper_bound")
        .cast(pl.Int64)
        .alias("commit_seq"),
        pl.col("readable_metrics")
        .struct.field("p")
        .struct.field("lower_bound")
        .cast(pl.Int64)
        .alias("p_min"),
        pl.col("readable_metrics")
        .struct.field("p")
        .struct.field("upper_bound")
        .cast(pl.Int64)
        .alias("p_max"),
    ).with_columns(
        pl.col("file_path")
        .str.replace(r"^file://", "")
        .str.replace(r"/[^/]+$", "")
        .alias("partition_dir")
        )
        .with_columns(
            pl.col("partition_dir").str.extract(r"([^/]+)$", 1).alias("partition_name")
        )
        .with_columns(
            pl.when(pl.col("partition_name").str.starts_with("commit_seq="))
            .then(pl.lit("commit_seq"))
            .when(pl.col("partition_name").str.starts_with("p_trunc="))
            .then(pl.lit("p_trunc"))
            .otherwise(pl.lit("other"))
            .alias("partition_layout")
        )
    )


def table_local_data_files(tbl: Table) -> set[Path]:
    location = _local_table_path(tbl)
    if location is None:
        return set()
    data_dir = location / "data"
    if not data_dir.exists():
        return set()
    return {p.resolve(strict=False) for p in data_dir.rglob("*.parquet") if p.is_file()}


def _summary_int(summary: Any, key: str) -> int | None:
    if summary is None:
        return None
    value = summary.get(key)
    return int(value) if value is not None else None


def _table_summary_bounds(tbl: Table) -> tuple[int | None, int | None]:
    snap = tbl.current_snapshot()
    summary = snap.summary if snap is not None else None
    return (
        _summary_int(summary, "funbuns.max_commit_seq"),
        _summary_int(summary, "funbuns.max_p"),
    )


def warehouse_standing(
    cat: Catalog | None = None,
    *,
    allowed_new_files: set[Path] | None = None,
    allow_stale_summary: bool = False,
) -> dict[str, Any]:
    """
    Inspect whether the local warehouse is safe for a production write.

    ``allowed_new_files`` is for the explicit commit bridge: those files have
    been written by the native generator and are expected to be unreferenced
    until ``add_files`` succeeds.
    """
    cat = cat or open_catalog()
    allowed = {p.resolve(strict=False) for p in (allowed_new_files or set())}
    errors: list[str] = []
    tables: dict[str, dict[str, Any]] = {}

    for ident in (PRIMES_IDENT, DECOMP_IDENT):
        table_name = _IDENT_TO_NAME[ident]
        try:
            tbl = cat.load_table(ident)
        except (NoSuchIdentifierError, NoSuchTableError):
            tables[table_name] = {"exists": False}
            continue

        manifest_cs, manifest_p = table_manifest_bounds(tbl)
        summary_cs, summary_p = _table_summary_bounds(tbl)
        referenced = table_referenced_data_files(tbl)
        referenced_frame = _referenced_file_frame(tbl)
        local = table_local_data_files(tbl)
        orphan_files = sorted(local - referenced - allowed)
        reused_commit_seqs = []
        if referenced_frame.height:
            reused_commit_seqs = (
                referenced_frame.filter(
                    pl.col("partition_layout") == "commit_seq",
                    pl.col("commit_seq").is_not_null(),
                )
                .group_by("commit_seq")
                .agg(
                    pl.len().alias("files"),
                    pl.col("partition_dir").n_unique().alias("partition_dirs"),
                    pl.col("p_min").min().alias("p_min"),
                    pl.col("p_max").max().alias("p_max"),
                )
                .filter(pl.col("partition_dirs") > 1)
                .sort("commit_seq")
                .to_dicts()
            )
        layout_counts: dict[str, int] = {}
        if referenced_frame.height:
            layout_counts = {
                str(row["partition_layout"]): int(row["files"])
                for row in referenced_frame.group_by("partition_layout")
                .agg(pl.len().alias("files"))
                .to_dicts()
            }
        has_manifest_commit_seq_metrics = bool(
            referenced_frame.height and referenced_frame["commit_seq"].is_not_null().any()
        )

        tables[table_name] = {
            "exists": True,
            "manifest_max_commit_seq": manifest_cs,
            "manifest_max_p": manifest_p,
            "summary_max_commit_seq": summary_cs,
            "summary_max_p": summary_p,
            "referenced_files": len(referenced),
            "local_data_files": len(local),
            "orphan_files": len(orphan_files),
            "orphan_examples": [str(p) for p in orphan_files[:5]],
            "partition_layout_counts": layout_counts,
            "has_manifest_commit_seq_metrics": has_manifest_commit_seq_metrics,
            "reused_commit_seqs": reused_commit_seqs[:10],
        }

        if orphan_files:
            errors.append(
                f"{ident} has {len(orphan_files)} local parquet file(s) not "
                "referenced by Iceberg manifests"
            )
        if reused_commit_seqs:
            examples = ", ".join(str(row["commit_seq"]) for row in reused_commit_seqs[:5])
            errors.append(
                f"{ident} reuses commit_seq across multiple data directories "
                f"({len(reused_commit_seqs)} value(s); examples: {examples})"
            )

        if manifest_cs is None and has_manifest_commit_seq_metrics:
            errors.append(f"{ident} has commit_seq metrics in manifests but no max commit_seq")

        if manifest_cs is None:
            if summary_cs is not None:
                if has_manifest_commit_seq_metrics:
                    errors.append(
                        f"{ident} has snapshot summary commit_seq but no manifest max_commit_seq"
                    )
            if manifest_p is None:
                continue

        if summary_cs is None or summary_p is None:
            errors.append(f"{ident} is missing funbuns resume snapshot properties")
            continue

        if not allow_stale_summary:
            if table_name == "primes":
                if manifest_cs is not None and summary_cs != manifest_cs:
                    errors.append(
                        f"{ident} summary max_commit_seq={summary_cs} but "
                        f"manifest max_commit_seq={manifest_cs}"
                    )
                if manifest_p is not None and summary_p != manifest_p:
                    errors.append(
                        f"{ident} summary max_p={summary_p} but manifest max_p={manifest_p}"
                    )
            else:
                if manifest_cs is not None and summary_cs < manifest_cs:
                    errors.append(
                        f"{ident} summary max_commit_seq={summary_cs} is below "
                        f"manifest max_commit_seq={manifest_cs}"
                    )
                if manifest_p is not None and summary_p < manifest_p:
                    errors.append(
                        f"{ident} summary max_p={summary_p} is below manifest max_p={manifest_p}"
                    )

    primes = tables.get("primes", {})
    decomps = tables.get("decompositions", {})
    if primes.get("exists") and decomps.get("exists"):
        p_summary_cs = primes.get("summary_max_commit_seq")
        d_summary_cs = decomps.get("summary_max_commit_seq")
        p_summary_p = primes.get("summary_max_p")
        d_summary_p = decomps.get("summary_max_p")
        if p_summary_cs is not None and d_summary_cs is not None and p_summary_cs != d_summary_cs:
            errors.append(
                "primes/decompositions snapshot summaries disagree on "
                f"max_commit_seq: {p_summary_cs} != {d_summary_cs}"
            )
        if p_summary_p is not None and d_summary_p is not None and p_summary_p != d_summary_p:
            errors.append(
                "primes/decompositions snapshot summaries disagree on "
                f"max_p: {p_summary_p} != {d_summary_p}"
            )

    return {"ok": not errors, "errors": errors, "tables": tables}


def assert_warehouse_good_standing(
    cat: Catalog | None = None,
    *,
    allowed_new_files: set[Path] | None = None,
    allow_stale_summary: bool = False,
) -> dict[str, Any]:
    state = warehouse_standing(
        cat,
        allowed_new_files=allowed_new_files,
        allow_stale_summary=allow_stale_summary,
    )
    if not state["ok"]:
        details = "\n".join(f"- {error}" for error in state["errors"])
        raise WarehouseStateError(
            "Iceberg warehouse is not in good standing; refusing to write.\n"
            f"{details}\n"
            "Use the explicit native commit/repair path for pending files, or "
            "clean orphaned files after verifying they are not needed."
        )
    return state


def catalog_resume_state(cat: Catalog | None = None) -> tuple[int, int]:
    """
    Return ``(next_commit_seq, resume_p)`` for the primes table.

    The warehouse must already be in good standing: snapshot summary
    properties, manifest metrics, and local data files agree. This function
    refuses stale-summary or orphan-file states instead of guessing past them.
    """
    cat = cat or open_catalog()
    try:
        tbl = cat.load_table(PRIMES_IDENT)
    except Exception:
        return 0, 0

    assert_warehouse_good_standing(cat)
    summary_cs, summary_p = _table_summary_bounds(tbl)
    if summary_cs is None or summary_p is None:
        return 0, 0
    return summary_cs + 1, summary_p


def migrate_rename_batch_id_to_commit_seq(cat: Catalog) -> None:
    """
    One-shot schema migration for catalogs created before the rename.

    Renames ``batch_id`` → ``commit_seq`` on both ``funbuns.primes`` and
    ``funbuns.decompositions`` via iceberg's ``update_schema().rename_column``.
    Field-ids are preserved, so historical data files remain readable under the
    new name without rewrite. Safe to run multiple times: if the old name is
    already gone, pyiceberg raises and this function swallows the no-op.

    Call once on the production catalog after upgrading funbuns past the
    rename commit; subsequent opens read the new name transparently.
    """
    for ident in (PRIMES_IDENT, DECOMP_IDENT):
        tbl = cat.load_table(ident)
        if "commit_seq" in tbl.schema().column_names:
            continue
        with tbl.update_schema() as us:
            us.rename_column("batch_id", "commit_seq")


# ---------------------------------------------------------------------------
# KV metadata
# ---------------------------------------------------------------------------

_GIT_SHA_CACHE: str | None = None


def _git_sha() -> str:
    global _GIT_SHA_CACHE
    if _GIT_SHA_CACHE is not None:
        return _GIT_SHA_CACHE
    try:
        sha = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL,
            cwd=Path(__file__).resolve().parent,
        ).decode().strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        sha = "unknown"
    _GIT_SHA_CACHE = sha
    return sha


def build_file_kv(
    pa_table: pa.Table,
    *,
    table_name: str,
    commit_seq: int,
    generator: str = "funbuns-core",
) -> dict[bytes, bytes]:
    """
    Build per-file parquet footer KV metadata for a prepared pa.Table.

    Row-count and per-column min/max are already stored by parquet itself
    (pyiceberg reads them from the footer on add_files); these KV pairs
    carry only the provenance iceberg stats don't cover.
    """
    if table_name not in ("primes", "decompositions"):
        raise ValueError(f"unknown table_name: {table_name}")

    p_col = pa_table.column("p")
    p_min = int(pc.min(p_col).as_py())
    p_max = int(pc.max(p_col).as_py())

    if table_name == "primes":
        n_primes = pa_table.num_rows
    else:
        n_primes = int(pc.count_distinct(p_col).as_py())

    kv: dict[str, str] = {
        "funbuns.schema_version": str(SCHEMA_VERSION),
        "funbuns.algorithm_version": _PACKAGE_VERSION if isinstance(_PACKAGE_VERSION, str) else "unknown",
        "funbuns.algorithm_git_sha": _git_sha(),
        "funbuns.table": table_name,
        "funbuns.commit_seq": str(commit_seq),
        "funbuns.p_min": str(p_min),
        "funbuns.p_max": str(p_max),
        "funbuns.n_rows": str(pa_table.num_rows),
        "funbuns.n_primes": str(n_primes),
        "funbuns.generated_at": datetime.now(timezone.utc).isoformat(),
        "funbuns.generator": generator,
    }

    if table_name == "primes":
        k_hist = (
            pl.from_arrow(pa_table)
            .group_by("k")
            .len()
            .sort("k")
            .to_dict(as_series=False)
        )
        kv["funbuns.k_histogram"] = json.dumps(
            dict(zip(k_hist["k"], k_hist["len"])),
            separators=(",", ":"),
        )

    return {k.encode(): v.encode() for k, v in kv.items()}


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

class ValidationError(AssertionError):
    pass


def validate_primes_batch(df: pl.DataFrame, *, commit_seq: int) -> None:
    if df.schema != {"p": pl.Int64, "k": pl.Int32, "commit_seq": pl.Int32}:
        raise ValidationError(f"primes schema mismatch: {df.schema}")
    if df.null_count().sum_horizontal().item() != 0:
        raise ValidationError("primes contains nulls")
    if df.height == 0:
        raise ValidationError("primes batch is empty")
    p = df["p"]
    if not p.is_sorted(descending=False):
        raise ValidationError("primes.p not ascending")
    if p.n_unique() != df.height:
        raise ValidationError("primes.p has duplicates")
    if not (df["commit_seq"] == commit_seq).all():
        raise ValidationError(f"primes.commit_seq != {commit_seq}")
    if (df["k"] < 0).any():
        raise ValidationError("primes.k has negative values")


def validate_decomp_batch(df: pl.DataFrame, *, commit_seq: int) -> None:
    expected = {
        "p": pl.Int64,
        "m_k": pl.Int32,
        "n_k": pl.Int32,
        "q_k": pl.Int64,
        "commit_seq": pl.Int32,
    }
    if df.schema != expected:
        raise ValidationError(f"decomp schema mismatch: {df.schema}")
    if df.null_count().sum_horizontal().item() != 0:
        raise ValidationError("decomp contains nulls")
    if df.height == 0:
        return
    if (df["q_k"] <= 0).any():
        raise ValidationError("decomp.q_k <= 0 (sentinel rows must be filtered)")
    if (df["m_k"] < 1).any() or (df["n_k"] < 1).any():
        raise ValidationError("decomp.m_k or n_k < 1")
    sort_check = df.select(
        (pl.col("p").shift(1).fill_null(-1) < pl.col("p")).alias("p_up"),
        (
            (pl.col("p").shift(1).fill_null(-1) < pl.col("p"))
            | (
                (pl.col("p").shift(1) == pl.col("p"))
                & (pl.col("m_k").shift(1).fill_null(-1) <= pl.col("m_k"))
            )
        ).alias("ok"),
    )
    if not sort_check["ok"].all():
        raise ValidationError("decomp not sorted by (p, m_k)")
    if not (df["commit_seq"] == commit_seq).all():
        raise ValidationError(f"decomp.commit_seq != {commit_seq}")


def validate_cross(primes_df: pl.DataFrame, decomp_df: pl.DataFrame) -> None:
    k_sum = int(primes_df["k"].sum())
    if k_sum != decomp_df.height:
        raise ValidationError(
            f"sum(primes.k)={k_sum} != len(decomp)={decomp_df.height}"
        )
    k_zero_ps = primes_df.filter(pl.col("k") == 0)["p"]
    if k_zero_ps.len() > 0:
        overlap = decomp_df.filter(pl.col("p").is_in(k_zero_ps.implode())).height
        if overlap != 0:
            raise ValidationError(
                f"{overlap} decomp rows reference k=0 primes"
            )
    missing = decomp_df.join(primes_df.select("p"), on="p", how="anti")
    if missing.height > 0:
        raise ValidationError(
            f"{missing.height} decomp rows reference primes absent from primes batch"
        )


# ---------------------------------------------------------------------------
# Write path
# ---------------------------------------------------------------------------

@dataclass
class WriteBatchResult:
    commit_seq: int
    primes_files: list[Path]
    decomp_files: list[Path]
    primes_rows: int
    decomp_rows: int


@dataclass(slots=True)
class PendingFlush:
    """One flushed batch's parquet outputs plus its prime-rank identity.

    ``start_idx`` and ``processed_count`` mirror ``PPBatchResult`` so the
    writer can reconstruct contiguous-by-rank ordering at commit time
    without reaching back into the manager. Legacy ``flush(raw_df)`` paths
    (gap fills, tests) leave them as ``None``; ``commit_pending`` with
    an anchor refuses to mix tagged and untagged flushes.
    """
    primes_files: list[Path]
    decomp_files: list[Path]
    p_max: int
    commit_seq: int
    start_idx: int | None = None
    processed_count: int | None = None


def _to_iceberg_arrow(df: pl.DataFrame, schema: Schema) -> pa.Table:
    target = schema_to_pyarrow(schema)
    tbl = df.to_arrow()
    return tbl.cast(target)


def _chunk_row_boundaries(
    n_rows: int,
    target_rows: int,
    *,
    p_col: pa.ChunkedArray | None = None,
) -> list[tuple[int, int]]:
    """
    Return [(start, end), ...] row-index pairs splitting a table into chunks of
    approximately target_rows each. If p_col is provided, chunk boundaries are
    snapped forward to the next p-change so no p value straddles two chunks.
    """
    if n_rows <= target_rows:
        return [(0, n_rows)]

    bounds: list[tuple[int, int]] = []
    start = 0
    if p_col is not None:
        p_list = p_col.to_pylist()
        while start < n_rows:
            end = min(start + target_rows, n_rows)
            while end < n_rows and p_list[end] == p_list[end - 1]:
                end += 1
            bounds.append((start, end))
            start = end
    else:
        while start < n_rows:
            end = min(start + target_rows, n_rows)
            bounds.append((start, end))
            start = end
    return bounds


def _write_chunks(
    pa_table: pa.Table,
    *,
    table_name: str,
    commit_seq: int,
    partition_dir: Path,
    target_rows: int,
    snap_to_p_boundary: bool,
) -> list[Path]:
    partition_dir.mkdir(parents=True, exist_ok=True)
    p_col = pa_table.column("p") if snap_to_p_boundary else None
    bounds = _chunk_row_boundaries(pa_table.num_rows, target_rows, p_col=p_col)
    tmp_paths: list[Path] = []
    final_paths: list[Path] = []
    try:
        for i, (start, end) in enumerate(bounds):
            chunk = pa_table.slice(start, end - start)
            kv = build_file_kv(chunk, table_name=table_name, commit_seq=commit_seq)
            merged = dict(chunk.schema.metadata or {})
            merged.update(kv)
            chunk = chunk.replace_schema_metadata(merged)
            name = f"{table_name}_b{commit_seq:06d}_{i:03d}.parquet"
            final = partition_dir / name
            tmp = partition_dir / f".{name}.tmp"
            pq.write_table(chunk, tmp, **PARQUET_WRITER_KWARGS)
            tmp_paths.append(tmp)
            final_paths.append(final)
    except Exception:
        for t in tmp_paths:
            if t.exists():
                t.unlink()
        raise
    for tmp, final in zip(tmp_paths, final_paths):
        os.replace(tmp, final)
    return final_paths


def shape_for_write(
    raw: pl.DataFrame,
    *,
    commit_seq: int,
) -> tuple[pl.DataFrame, pl.DataFrame]:
    """
    Transform a raw partition frame {p, m_k, n_k, q_k} into the (primes, decomp)
    pair that write_batch expects. Filters sentinel rows (q_k == 0) out of
    decomp, and derives per-prime k counts for the primes table. The raw
    frame's primes that are entirely obstructed still land in primes with k=0.
    core.py worker output is deterministic and duplicate-free within a batch;
    validate_cross is the integrity backstop.
    """
    decomp = (
        raw.filter(pl.col("q_k") > 0)
        .with_columns(
            pl.col("p").cast(pl.Int64),
            pl.col("m_k").cast(pl.Int32),
            pl.col("n_k").cast(pl.Int32),
            pl.col("q_k").cast(pl.Int64),
            pl.lit(commit_seq, dtype=pl.Int32).alias("commit_seq"),
        )
        .select(["p", "m_k", "n_k", "q_k", "commit_seq"])
    )
    primes = (
        raw.group_by("p")
        .agg((pl.col("q_k") > 0).sum().cast(pl.Int32).alias("k"))
        .with_columns(
            pl.col("p").cast(pl.Int64),
            pl.lit(commit_seq, dtype=pl.Int32).alias("commit_seq"),
        )
        .select(["p", "k", "commit_seq"])
    )
    return primes, decomp


class IcebergWriter:
    """
    Live-path writer: owns a catalog handle and a monotonic commit_seq counter,
    initialized from existing iceberg manifest state.

    ``flush(raw_df)`` is the hot-path call — it shapes, validates, and
    writes parquet files into the canonical partition dir, but does NOT
    touch the catalog. Pending file paths accumulate on the writer.

    ``commit_pending()`` registers every pending file with one
    ``add_files`` call per table. Callers run it once after ingest
    completes (and once more after each crash-bounded checkpoint if
    desired).

    Deferring catalog commits:
      - eliminates 13% of hot-path wall (content hashing, count_distinct)
      - eliminates repeated cat.load_table + OCC + manifest-list rewrite
      - collapses N per-batch snapshots into one snapshot per run,
        which keeps manifest-list growth linear in runs, not batches.

    Crash semantics: on mid-run crash, parquet files are sitting on disk
    as orphans under ``data/commit_seq=K/``. Startup is unaffected (they
    aren't referenced by any manifest); a reconcile script can either
    clean them or register them after the fact.

    Attributes:
        cat: active Catalog handle.
        next_commit_seq: next commit_seq to be assigned on flush().
        resume_p: max prime already committed or pending, 0 if empty.
        pending: per-batch ``PendingFlush`` records accumulated since last
            ``commit_pending``; carries the parquet paths plus the
            (start_idx, processed_count) rank identity used for
            contiguous-prefix commit.
    """

    def __init__(self, cat: Catalog | None = None):
        self.cat = cat if cat is not None else open_catalog()
        ensure_tables(self.cat)
        self.next_commit_seq, self.resume_p = self._init_state()
        # Table handles and partition roots are stable for the writer's
        # lifetime; resolve once.
        self._primes_tbl = self.cat.load_table(PRIMES_IDENT)
        self._decomp_tbl = self.cat.load_table(DECOMP_IDENT)
        self.pending: list[PendingFlush] = []

    def _init_state(self) -> tuple[int, int]:
        return catalog_resume_state(self.cat)

    def _flush_tables(
        self,
        primes_df: pl.DataFrame,
        decomp_df: pl.DataFrame,
        *,
        commit_seq: int,
        start_idx: int | None = None,
        processed_count: int | None = None,
    ) -> WriteBatchResult:
        """Validate and write already-shaped primes/decompositions frames."""
        primes_df = primes_df.sort("p")
        batch_max_p = int(primes_df["p"][-1])
        self.resume_p = max(self.resume_p, batch_max_p)

        validate_primes_batch(primes_df, commit_seq=commit_seq)
        validate_decomp_batch(decomp_df, commit_seq=commit_seq)
        validate_cross(primes_df, decomp_df)

        primes_at = _to_iceberg_arrow(primes_df, PRIMES_SCHEMA)

        primes_target = TARGET_FILE_SIZE_BYTES // BYTES_PER_ROW_PRIMES
        decomp_target = TARGET_FILE_SIZE_BYTES // BYTES_PER_ROW_DECOMP

        primes_files = _write_chunks(
            primes_at,
            table_name="primes",
            commit_seq=commit_seq,
            partition_dir=_partition_dir(self._primes_tbl, commit_seq),
            target_rows=primes_target,
            snap_to_p_boundary=False,
        )
        if decomp_df.height > 0:
            decomp_at = _to_iceberg_arrow(decomp_df, DECOMP_SCHEMA)
            decomp_files = _write_chunks(
                decomp_at,
                table_name="decompositions",
                commit_seq=commit_seq,
                partition_dir=_partition_dir(self._decomp_tbl, commit_seq),
                target_rows=decomp_target,
                snap_to_p_boundary=True,
            )
        else:
            decomp_files = []

        self.pending.append(PendingFlush(
            primes_files=primes_files,
            decomp_files=decomp_files,
            p_max=batch_max_p,
            commit_seq=commit_seq,
            start_idx=start_idx,
            processed_count=processed_count,
        ))
        self.next_commit_seq += 1

        return WriteBatchResult(
            commit_seq=commit_seq,
            primes_files=primes_files,
            decomp_files=decomp_files,
            primes_rows=primes_df.height,
            decomp_rows=decomp_df.height,
        )

    def flush(self, raw_df: pl.DataFrame) -> WriteBatchResult:
        """
        Hot path compatibility: shape raw sentinel rows, validate, write parquet.
        No catalog I/O.
        """
        commit_seq = self.next_commit_seq
        primes_df, decomp_df = shape_for_write(raw_df, commit_seq=commit_seq)
        # primes goes through group_by (no order guarantee); decomp comes
        # out of filter on already-ascending raw, so it's already sorted
        # by (p, m_k).
        return self._flush_tables(primes_df, decomp_df, commit_seq=commit_seq)

    def flush_shaped(
        self,
        primes_df: pl.DataFrame,
        decomp_df: pl.DataFrame,
        *,
        start_idx: int | None = None,
        processed_count: int | None = None,
    ) -> WriteBatchResult:
        """
        Hot path for core.py shaped worker output. The frames arrive without
        commit_seq; this method stamps, validates, and writes parquet.

        ``start_idx`` and ``processed_count`` are stored on the PendingFlush
        so ``commit_pending(anchor_start_idx=...)`` can later commit only
        the contiguous prime-rank prefix on interrupted shutdown.
        """
        commit_seq = self.next_commit_seq
        primes_df = (
            primes_df.with_columns(
                pl.col("p").cast(pl.Int64),
                pl.col("k").cast(pl.Int32),
                pl.lit(commit_seq, dtype=pl.Int32).alias("commit_seq"),
            )
            .select(["p", "k", "commit_seq"])
        )
        decomp_df = (
            decomp_df.with_columns(
                pl.col("p").cast(pl.Int64),
                pl.col("m_k").cast(pl.Int32),
                pl.col("n_k").cast(pl.Int32),
                pl.col("q_k").cast(pl.Int64),
                pl.lit(commit_seq, dtype=pl.Int32).alias("commit_seq"),
            )
            .select(["p", "m_k", "n_k", "q_k", "commit_seq"])
        )
        return self._flush_tables(
            primes_df,
            decomp_df,
            commit_seq=commit_seq,
            start_idx=start_idx,
            processed_count=processed_count,
        )

    @staticmethod
    def _contiguous_prefix(
        pending: list[PendingFlush],
        *,
        anchor_start_idx: int,
    ) -> tuple[list[PendingFlush], list[PendingFlush]]:
        """Split ``pending`` into (kept, dropped) by prime-rank contiguity.

        Sorted by start_idx, a flush glues iff its start equals the
        running ``expected`` rank — anchor_start_idx for the first, and
        ``prev.start_idx + prev.processed_count`` thereafter. A partial
        last batch (processed_count < the next batch's stride) naturally
        terminates the prefix because no successor can glue.
        """
        if any(f.start_idx is None or f.processed_count is None for f in pending):
            raise ValueError(
                "commit_pending(anchor_start_idx=...) requires every flush to "
                "carry start_idx/processed_count; legacy flush(raw_df) paths "
                "cannot be mixed in"
            )
        ordered = sorted(pending, key=lambda f: f.start_idx)
        kept: list[PendingFlush] = []
        expected = anchor_start_idx
        for f in ordered:
            if f.start_idx != expected:
                break
            kept.append(f)
            expected = f.start_idx + f.processed_count
        kept_ids = {id(f) for f in kept}
        dropped = [f for f in ordered if id(f) not in kept_ids]
        return kept, dropped

    def commit_pending(self, *, anchor_start_idx: int | None = None) -> None:
        """
        End-of-run (or checkpoint): register pending parquet files with one
        ``add_files`` call per table. Snapshot summary carries
        ``funbuns.max_p`` / ``funbuns.max_commit_seq`` so the next writer
        reads resume state in O(1).

        ``anchor_start_idx`` enables contiguous-prefix mode: pending flushes
        are sorted by prime rank, only the gap-free prefix anchored at
        ``anchor_start_idx`` is committed, and post-gap orphan parquet files
        are deleted from disk. Without an anchor, every pending flush is
        committed (legacy / clean-exit behavior).

        Idempotent: no-op if there's nothing pending.
        """
        if not self.pending:
            return

        if anchor_start_idx is not None:
            kept, dropped = self._contiguous_prefix(
                self.pending, anchor_start_idx=anchor_start_idx,
            )
            for f in dropped:
                for path in (*f.primes_files, *f.decomp_files):
                    Path(path).unlink(missing_ok=True)
        else:
            kept = list(self.pending)

        if not kept:
            self.pending.clear()
            return

        primes_paths = [str(p) for f in kept for p in f.primes_files]
        decomp_paths = [str(p) for f in kept for p in f.decomp_files]
        max_p = max(f.p_max for f in kept)
        max_cs = max(f.commit_seq for f in kept)
        snap_props = {
            "funbuns.max_p": str(max_p),
            "funbuns.max_commit_seq": str(max_cs),
        }
        self._primes_tbl.add_files(
            primes_paths,
            snapshot_properties=snap_props,
        )
        if decomp_paths:
            self._decomp_tbl.add_files(
                decomp_paths,
                snapshot_properties=snap_props,
            )
        self.pending.clear()
        # Refresh cached handles so subsequent partition_dir() calls see
        # the new snapshot.
        self._primes_tbl = self.cat.load_table(PRIMES_IDENT)
        self._decomp_tbl = self.cat.load_table(DECOMP_IDENT)
