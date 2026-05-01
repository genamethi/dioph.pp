"""
Read-side helpers for the iceberg catalog.

Scaled to the full 10B-prime / 19B-decomposition dataset. Polars' default
physical row counter is u32 (cap 4.29B), so a naive
``scan_iceberg().collect()`` over the full primes or decompositions table
blows up before any aggregation runs. This module resolves that by either
reading the answer from metadata (parquet footer KV + iceberg manifest
statistics) or, when the aggregation genuinely needs every row, by
chunking along the identity-transform ``commit_seq`` partition so each
per-chunk lazy plan fits under u32 and the partition filter pushes down
to exactly one partition's files.

Write-side schema, partitioning, and file KV layout are documented in
``markdown/iceberg_data_setup.md`` and implemented in ``iceberg_schema``.

Public surface:

    total_rows(cat, table)            — from snapshot summary
    max_p(cat, table='primes')        — from manifest upper_bound
    file_footers(cat, table)          — per-file KV metadata frame
    k_distribution(cat)               — full k histogram, footer-only
    chunks_by_commit_seq(cat, table)  — yields (commit_seq, lazy_frame)
    collect_chunked(cat, table, build, *, reduce)
                                      — run a per-chunk aggregate and
                                        combine; replaces collect() when
                                        the full scan would exceed u32
"""

from __future__ import annotations

import json
from collections.abc import Callable, Iterator

import polars as pl
import pyarrow.parquet as pq
from pyiceberg.catalog import Catalog

from .iceberg_schema import (
    DECOMP_IDENT,
    PRIMES_IDENT,
    open_catalog,
    scan_decompositions,
    scan_primes,
)

__all__ = [
    "open_catalog",
    "scan_primes",
    "scan_decompositions",
    "total_rows",
    "max_p",
    "file_footers",
    "k_distribution",
    "chunks_by_commit_seq",
    "collect_chunked",
]


_IDENT = {"primes": PRIMES_IDENT, "decompositions": DECOMP_IDENT}


def _resolve_cat(cat: Catalog | None) -> Catalog:
    return cat if cat is not None else open_catalog()


def total_rows(cat: Catalog | None, table: str) -> int:
    """
    Ground-truth row count, computed from manifest per-file record counts.

    """
    tbl = _resolve_cat(cat).load_table(_IDENT[table])
    files = pl.from_arrow(tbl.inspect.files())
    if files.height == 0:
        return 0
    return int(files["record_count"].sum())


def max_p(cat: Catalog | None = None, table: str = "primes") -> int:
    """Max of column ``p`` across all files via manifest upper_bound."""
    tbl = _resolve_cat(cat).load_table(_IDENT[table])
    files = pl.from_arrow(tbl.inspect.files())
    return int(
        files.select(
            pl.col("readable_metrics")
            .struct.field("p")
            .struct.field("upper_bound")
            .max()
        )[0, 0]
    )


def _file_paths(cat: Catalog, table: str) -> list[tuple[int, str]]:
    """Return ``(commit_seq, local_path)`` for every file in the table."""
    tbl = cat.load_table(_IDENT[table])
    files = pl.from_arrow(tbl.inspect.files())
    paths = files["file_path"].to_list()
    commit_seqs = (
        files.select(
            pl.col("readable_metrics")
            .struct.field("commit_seq")
            .struct.field("upper_bound")
        )
        .to_series()
        .to_list()
    )
    out: list[tuple[int, str]] = []
    for cs, fp in zip(commit_seqs, paths):
        if fp.startswith("file://"):
            fp = fp[len("file://") :]
        out.append((int(cs), fp))
    return out


def file_footers(cat: Catalog | None, table: str) -> pl.DataFrame:
    """
    Per-file parquet footer KV metadata as a polars DataFrame.

    Columns: ``commit_seq, file_path, p_min, p_max, n_rows, n_primes,
    k_histogram``. ``n_primes`` is the number of distinct ``p`` values in the
    file; legacy primes footers that lack it fall back to ``n_rows``.
    ``k_histogram`` is a ``dict[int, int]`` for the primes table, ``None`` for
    decompositions (the writer only stamps the histogram into primes files;
    see ``iceberg_schema.build_file_kv``).

    One footer read per file; no data reads.
    """
    cat = _resolve_cat(cat)
    commit_seqs: list[int] = []
    paths: list[str] = []
    p_mins: list[int] = []
    p_maxs: list[int] = []
    n_rows_col: list[int] = []
    n_primes_col: list[int | None] = []
    k_hists: list[dict[int, int] | None] = []

    for cs, fp in _file_paths(cat, table):
        kv = pq.read_metadata(fp).metadata or {}

        def _get(key: str) -> str | None:
            v = kv.get(key.encode())
            return v.decode() if v is not None else None

        k_hist_s = _get("funbuns.k_histogram")
        k_hist = (
            {int(k): int(v) for k, v in json.loads(k_hist_s).items()}
            if k_hist_s
            else None
        )
        commit_seqs.append(cs)
        paths.append(fp)
        p_mins.append(int(_get("funbuns.p_min")))
        p_maxs.append(int(_get("funbuns.p_max")))
        n_rows = int(_get("funbuns.n_rows"))
        n_primes_s = _get("funbuns.n_primes")
        if n_primes_s is None and table == "primes":
            n_primes = n_rows
        else:
            n_primes = int(n_primes_s) if n_primes_s is not None else None
        n_rows_col.append(n_rows)
        n_primes_col.append(n_primes)
        k_hists.append(k_hist)

    # k_histogram goes in as an Object series — dict[int,int] isn't a polars
    # native struct type (struct keys must be strings). The only consumer is
    # k_distribution(), which iterates it Pythonically.
    return pl.DataFrame(
        {
            "commit_seq": commit_seqs,
            "file_path": paths,
            "p_min": p_mins,
            "p_max": p_maxs,
            "n_rows": n_rows_col,
            "n_primes": n_primes_col,
            "k_histogram": pl.Series("k_histogram", k_hists, dtype=pl.Object),
        }
    ).sort("commit_seq")


def k_distribution(cat: Catalog | None = None) -> pl.DataFrame:
    """
    Full k-distribution over ``funbuns.primes``, assembled by summing the
    per-file ``funbuns.k_histogram`` KV blocks. Zero data reads.

    Returns a two-column DataFrame ``(k, count)`` sorted by ``k``.
    """
    footers = file_footers(cat, "primes")
    merged: dict[int, int] = {}
    for hist in footers["k_histogram"].to_list():
        if hist is None:
            continue
        for k, v in hist.items():
            merged[k] = merged.get(k, 0) + v
    return (
        pl.DataFrame(
            {"k": list(merged.keys()), "count": list(merged.values())},
            schema={"k": pl.Int64, "count": pl.Int64},
        )
        .sort("k")
    )


def chunks_by_commit_seq(
    cat: Catalog | None,
    table: str,
) -> Iterator[tuple[int, pl.LazyFrame]]:
    """
    Yield ``(commit_seq, lazy_frame)`` per commit_seq partition.

    Each yielded lazy frame opens only its own partition's files (identity
    partition pruning pushes through ``pl.scan_iceberg``). Every commit_seq
    is visited once; their union is the full dataset (``iceberg_data_setup.md``
    pins the "every (p_lo, p_hi) range is disjoint and contiguous" invariant,
    even though commit_seq order is not p-order).
    """
    cat = _resolve_cat(cat)
    tbl = cat.load_table(_IDENT[table])
    files = pl.from_arrow(tbl.inspect.files())
    commit_seqs = sorted(
        set(
            files.select(
                pl.col("readable_metrics")
                .struct.field("commit_seq")
                .struct.field("upper_bound")
            )
            .to_series()
            .to_list()
        )
    )
    lf = pl.scan_iceberg(tbl)
    for cs in commit_seqs:
        yield int(cs), lf.filter(pl.col("commit_seq") == int(cs))


def collect_chunked(
    cat: Catalog | None,
    table: str,
    build: Callable[[pl.LazyFrame], pl.LazyFrame],
    *,
    reduce: Callable[[pl.DataFrame], pl.DataFrame] | None = None,
    progress: bool = False,
) -> pl.DataFrame:
    """
    Run a per-commit_seq aggregation across the full table and combine.

    ``build(chunk_lf)`` must return a small aggregate lazy frame given a
    per-partition lazy frame. Chunks are materialized one at a time (so the
    in-memory footprint is one chunk's output plus the running concat),
    concatenated, then passed through ``reduce`` which typically performs
    a final group-by-sum to collapse duplicate keys that fell into multiple
    chunks.

    Example — full k-distribution by data scan (not footer KV)::

        collect_chunked(
            cat,
            "primes",
            build=lambda lf: lf.group_by("k").agg(pl.len().alias("count")),
            reduce=lambda df: df.group_by("k")
                                .agg(pl.col("count").sum())
                                .sort("k"),
        )

    Each per-chunk collect stays under polars' u32 row counter because the
    largest commit_seq partition is on the order of 20M primes / 40M
    decompositions. ``progress=True`` prints one line per chunk.
    """
    parts: list[pl.DataFrame] = []
    for cs, chunk in chunks_by_commit_seq(cat, table):
        df = build(chunk).collect(engine="streaming")
        if progress:
            print(f"commit_seq={cs}: {df.height} agg rows", flush=True)
        parts.append(df)
    combined = pl.concat(parts) if parts else pl.DataFrame()
    return reduce(combined) if reduce else combined
