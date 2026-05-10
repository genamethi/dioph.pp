"""
Isolate prime_rank's and p's per-column cost under different parquet
encodings.

prime_rank: synthesized monotone int64 arange (deltas all +1).
p: real values pulled from a live primes file (irregular prime gaps).

For each column, writes a single-column table four ways:

  1. pyarrow defaults (dict + zstd-3)
  2. PLAIN + zstd-3
  3. DELTA_BINARY_PACKED, no compression
  4. DELTA_BINARY_PACKED + zstd-3

Reports column-chunk bytes (what's actually on disk for the column after
page headers and footer are subtracted) and a read-back wall-clock time
across N_READS full-column scans.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from primeparts.iceberg_schema import open_catalog  # noqa: E402

OUT_DIR = Path("/tmp/prime_rank_enc")
OUT_DIR.mkdir(exist_ok=True)

# 200M rows: same magnitude as a live primes file, fast enough for
# repeated experiments.
N_ROWS = 200_000_000
RANK_OFFSET = 11_000_000_000  # plausible mid-table offset

# zstd level matches the warehouse / new-schema rewriter assumption.
ZSTD_LEVEL = 3

# pyarrow caps each row group at this many rows. One big group keeps the
# encoding overhead amortized.
ROW_GROUP_ROWS = N_ROWS

# Number of full-column read-back passes for timing.
N_READS = 3


def column_chunk_bytes(path: Path) -> int:
    """Sum compressed_size of all column chunks (excludes parquet footer)."""
    md = pq.read_metadata(path)
    total = 0
    for rg in range(md.num_row_groups):
        rg_md = md.row_group(rg)
        for col in range(rg_md.num_columns):
            total += rg_md.column(col).total_compressed_size
    return total


def write_one(label: str, table: pa.Table, **kwargs) -> tuple[int, int]:
    out = OUT_DIR / f"{label}.parquet"
    pq.write_table(table, out, **kwargs)
    file_bytes = out.stat().st_size
    chunk_bytes = column_chunk_bytes(out)
    return file_bytes, chunk_bytes


def time_read(path: Path, column: str, n_reads: int) -> float:
    """Best-of-N wall-clock seconds for a full-column scan back to numpy."""
    best = float("inf")
    # Warm-up pass to populate page cache; we want decode time, not IO.
    pq.read_table(path, columns=[column])
    for _ in range(n_reads):
        t0 = time.perf_counter()
        tbl = pq.read_table(path, columns=[column])
        # Force materialization to numpy so we measure decode + copy.
        _ = tbl.column(column).to_numpy(zero_copy_only=False)
        best = min(best, time.perf_counter() - t0)
    return best


def cases_for(column: str):
    return [
        (
            "default",
            "pyarrow defaults (dict + zstd-3)",
            dict(
                compression="zstd",
                compression_level=ZSTD_LEVEL,
                row_group_size=ROW_GROUP_ROWS,
                write_statistics=True,
                use_dictionary=True,
            ),
        ),
        (
            "plain_zstd3",
            "PLAIN + zstd-3",
            dict(
                compression="zstd",
                compression_level=ZSTD_LEVEL,
                use_dictionary=False,
                column_encoding={column: "PLAIN"},
                row_group_size=ROW_GROUP_ROWS,
                write_statistics=True,
            ),
        ),
        (
            "delta_nocomp",
            "DELTA_BINARY_PACKED, no compression",
            dict(
                compression="none",
                use_dictionary=False,
                column_encoding={column: "DELTA_BINARY_PACKED"},
                row_group_size=ROW_GROUP_ROWS,
                write_statistics=True,
            ),
        ),
        (
            "delta_zstd3",
            "DELTA_BINARY_PACKED + zstd-3",
            dict(
                compression="zstd",
                compression_level=ZSTD_LEVEL,
                use_dictionary=False,
                column_encoding={column: "DELTA_BINARY_PACKED"},
                row_group_size=ROW_GROUP_ROWS,
                write_statistics=True,
            ),
        ),
    ]


def measure_column(column: str, table: pa.Table) -> None:
    n = table.num_rows
    print(f"\n=== column: {column} ({n:,} rows) ===")
    print(
        f"\n{'encoding':<40} {'col B':>14} {'B/row':>10} "
        f"{'read s':>10} {'M rows/s':>10}"
    )
    print("-" * 90)
    for slug, label, kwargs in cases_for(column):
        out = OUT_DIR / f"{column}_{slug}.parquet"
        pq.write_table(table, out, **kwargs)
        chunk_b = column_chunk_bytes(out)
        bpr = chunk_b / n
        secs = time_read(out, column, N_READS)
        mrows = n / secs / 1_000_000
        print(f"{label:<40} {chunk_b:>14,} {bpr:>10.6f} {secs:>10.3f} {mrows:>10.1f}")


def real_p_sample() -> pa.Table:
    """Pull N_ROWS of real p values from a live primes file.

    If the median-by-row-count file has < N_ROWS, takes from the largest
    file. Falls back to synthesized arange-spaced "primes" if no live
    catalog is reachable (warns).
    """
    try:
        cat = open_catalog()
        tbl = cat.load_table("funbuns.primes")
        files = []
        for f in tbl.inspect.files().to_pylist():
            files.append((f["file_path"].replace("file://", ""), f["record_count"]))
        files.sort(key=lambda r: -r[1])  # largest first
        for path, rows in files:
            if rows >= N_ROWS:
                src = pq.read_table(path, columns=["p"])
                p = src.column("p").to_numpy(zero_copy_only=False)[:N_ROWS]
                print(f"loaded {N_ROWS:,} real p values from {Path(path).name}")
                return pa.table({"p": pa.array(p, type=pa.int64())})
    except Exception as e:
        print(f"warning: could not load real p values ({e}); using synthetic")

    # Synthetic fallback: spacing ≈ ln(p) at p~2.8e11 is ~26
    base = 280_000_000_000
    gaps = np.random.default_rng(0).integers(2, 60, size=N_ROWS, dtype=np.int64)
    p = base + np.cumsum(gaps)
    return pa.table({"p": pa.array(p, type=pa.int64())})


def main() -> int:
    print(f"building monotone int64 prime_rank: {N_ROWS:,} rows, offset={RANK_OFFSET:,}")
    ranks = np.arange(RANK_OFFSET, RANK_OFFSET + N_ROWS, dtype=np.int64)
    rank_table = pa.table({"prime_rank": pa.array(ranks, type=pa.int64())})
    measure_column("prime_rank", rank_table)

    p_table = real_p_sample()
    measure_column("p", p_table)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
