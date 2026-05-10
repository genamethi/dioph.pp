"""
Measure on-disk B/row for the planned post-repartition schema.

For both `funbuns.primes` and `funbuns.decompositions`, picks one
representative file from the live warehouse (median by row count),
transforms it into the new schema:

  primes:        (p, k, prime_rank, p_bucket_version, p_bucket)
  decompositions:(p, m_k, n_k, q_k, prime_rank, p_bucket_version, p_bucket)

versus the live schema (which currently carries `commit_seq` and a
`p_trunc` partition column), and rewrites both forms with the warehouse's
zstd-3 / large-row-group config to /tmp.

The new B/row figure feeds the bucket planner: rows_per_bucket is just
target_bucket_bytes / bpr_new, so getting this number right matters.

Notes:
  - p_bucket_version and p_bucket are constant within a single output file
    by construction (the new partition spec is identity over both), so
    Parquet RLE_DICTIONARY should drive their per-row cost to ~0. The
    measurement confirms that empirically rather than asserting it.
  - commit_seq is a low-cardinality int32 in the live files. Dropping it
    is the source of most savings; the measurement separates "drop
    commit_seq" from "add bucket cols" so the planner constants are
    transparent.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from primeparts.iceberg_schema import open_catalog  # noqa: E402

ZSTD_LEVEL = 3
# Match the live warehouse: ~256 MiB row-groups (≈170M rows for primes,
# ≈57M rows for partitions at observed B/row). pyarrow caps each group at
# this many rows, so a single big number is fine.
ROW_GROUP_ROWS = 200_000_000

# Synthetic bucket coordinates for the rewritten file. Real values will
# be assigned by the planner; here we just need *some* constant per file
# so we can measure the encoded size of two constant int32 columns.
SYNTH_BUCKET_VERSION = np.int32(1)
SYNTH_BUCKET_ID = np.int32(5)

OUT_DIR = Path("/tmp/prime_rank_bpr")
OUT_DIR.mkdir(exist_ok=True)


def _bound(bounds, fid):
    raw = next((v for f, v in bounds if f == fid), None)
    return int.from_bytes(raw, "little", signed=True) if raw else None


def kv_footer_stats(path: str) -> dict:
    """Sum bytes of all `funbuns.*` KV entries in a parquet file's footer.

    Returns total wire size and a per-key breakdown so we can see whether
    e.g. k_histogram dominates. Excluded from the new schema (analytics
    move to Iceberg MVs).
    """
    md = pq.read_metadata(path).metadata or {}
    funbuns = {
        k.decode("utf-8", "replace"): len(k) + len(v)
        for k, v in md.items()
        if k.startswith(b"funbuns.")
    }
    total = sum(funbuns.values())
    return {"total_bytes": total, "by_key": funbuns}


def _print_kv(label: str, sample_path: str, n_rows: int) -> None:
    stats = kv_footer_stats(sample_path)
    total = stats["total_bytes"]
    bpr_overhead = total / n_rows if n_rows else 0.0
    print(
        f"[{label}] dropped KV footer: {total:,} B  ({bpr_overhead:.6f} B/row amortized)"
    )
    for k, sz in sorted(stats["by_key"].items(), key=lambda kv: -kv[1]):
        print(f"          {k:<32s} {sz:>10,} B")


def files_in_p_order(tbl):
    rows = []
    for f in tbl.inspect.files().to_pylist():
        rows.append(
            {
                "path": f["file_path"].replace("file://", ""),
                "rows": f["record_count"],
                "bytes": f["file_size_in_bytes"],
                "p_min": _bound(f["lower_bounds"], 1),
                "p_max": _bound(f["upper_bounds"], 1),
            }
        )
    rows.sort(key=lambda r: r["p_min"])
    return rows


def write_zstd(table: pa.Table, out: Path) -> int:
    pq.write_table(
        table,
        out,
        compression="zstd",
        compression_level=ZSTD_LEVEL,
        row_group_size=ROW_GROUP_ROWS,
        write_statistics=True,
        use_dictionary=True,
    )
    return out.stat().st_size


def _add_bucket_columns(table: pa.Table) -> pa.Table:
    n = table.num_rows
    ver = pa.array(np.full(n, SYNTH_BUCKET_VERSION, dtype=np.int32), type=pa.int32())
    bkt = pa.array(np.full(n, SYNTH_BUCKET_ID, dtype=np.int32), type=pa.int32())
    return table.append_column("p_bucket_version", ver).append_column("p_bucket", bkt)


def _strip_live_extras(table: pa.Table) -> pa.Table:
    """Drop columns that don't exist in the new schema (p_trunc, commit_seq)."""
    drops = [c for c in ("p_trunc", "commit_seq") if c in table.column_names]
    return table.drop(drops) if drops else table


def _reorder(table: pa.Table, order: list[str]) -> pa.Table:
    return table.select(order)


PRIMES_NEW_ORDER = ["p", "k", "prime_rank", "p_bucket_version", "p_bucket"]
PARTITIONS_NEW_ORDER = [
    "p",
    "m_k",
    "n_k",
    "q_k",
    "prime_rank",
    "p_bucket_version",
    "p_bucket",
]


def _report(label: str, n: int, b_live: int, b_drop_seq: int, b_new: int) -> None:
    bpr_live = b_live / n
    bpr_drop = b_drop_seq / n
    bpr_new = b_new / n
    print(f"[{label}] live (with commit_seq):     {b_live:>14,}  ({bpr_live:.3f} B/row)")
    print(
        f"[{label}] drop commit_seq:              {b_drop_seq:>14,}  ({bpr_drop:.3f} B/row)  "
        f"Δ_vs_live={bpr_drop - bpr_live:+.3f}"
    )
    print(
        f"[{label}] new schema (+bucket cols):    {b_new:>14,}  ({bpr_new:.3f} B/row)  "
        f"Δ_vs_live={bpr_new - bpr_live:+.3f}  Δ_from_bucket_cols={bpr_new - bpr_drop:+.3f}"
    )


def measure_primes(cat) -> None:
    tbl = cat.load_table("funbuns.primes")
    files = files_in_p_order(tbl)
    files_by_size = sorted(files, key=lambda f: f["rows"])
    sample = files_by_size[len(files_by_size) // 2]
    print(
        f"[primes] sample {Path(sample['path']).name} rows={sample['rows']:,} "
        f"orig_bytes={sample['bytes']:,} ({sample['bytes']/sample['rows']:.3f} B/row) "
        f"p=[{sample['p_min']:,}..{sample['p_max']:,}]"
    )

    src = pq.read_table(sample["path"])
    if "p_trunc" in src.column_names:
        src = src.drop(["p_trunc"])
    n = src.num_rows

    if "prime_rank" not in src.column_names:
        rank_offset = sum(f["rows"] for f in files if f["p_max"] < sample["p_min"])
        ranks = pa.array(
            np.arange(rank_offset, rank_offset + n, dtype=np.int64), type=pa.int64()
        )
        src = src.append_column("prime_rank", ranks)
        print(f"[primes] synthesized prime_rank starting at {rank_offset:,}")

    # Three forms: live (drop only p_trunc), drop commit_seq, new schema
    live = src
    drop_seq = src.drop(["commit_seq"]) if "commit_seq" in src.column_names else src
    new = _reorder(_add_bucket_columns(drop_seq), PRIMES_NEW_ORDER)

    b_live = write_zstd(live, OUT_DIR / "primes_live.parquet")
    b_drop = write_zstd(drop_seq, OUT_DIR / "primes_drop_seq.parquet")
    b_new = write_zstd(new, OUT_DIR / "primes_new.parquet")
    _report("primes", n, b_live, b_drop, b_new)
    _print_kv("primes", sample["path"], n)


def measure_partitions(cat) -> None:
    tbl = cat.load_table("funbuns.decompositions")
    files = files_in_p_order(tbl)
    files_by_size = sorted(files, key=lambda f: f["rows"])
    sample = files_by_size[len(files_by_size) // 2]
    print(
        f"[partitions] sample {Path(sample['path']).name} rows={sample['rows']:,} "
        f"orig_bytes={sample['bytes']:,} ({sample['bytes']/sample['rows']:.3f} B/row) "
        f"p=[{sample['p_min']:,}..{sample['p_max']:,}]"
    )

    src = pq.read_table(sample["path"])
    if "p_trunc" in src.column_names:
        src = src.drop(["p_trunc"])
    n = src.num_rows

    if "prime_rank" not in src.column_names:
        # Fall back to synthesized rank (file is p-sorted, distinct p gets
        # consecutive rank). Should not be hit on current live data.
        p_arr = src.column("p").to_numpy(zero_copy_only=False)
        pmin = float(p_arr[0])
        rank_offset_est = int(pmin / np.log(pmin)) if pmin > 2 else 0
        starts = np.zeros(p_arr.size, dtype=np.bool_)
        starts[0] = True
        starts[1:] = p_arr[1:] != p_arr[:-1]
        ranks = (rank_offset_est + (np.cumsum(starts) - 1)).astype(np.int64)
        src = src.append_column("prime_rank", pa.array(ranks, type=pa.int64()))

    live = src
    drop_seq = src.drop(["commit_seq"]) if "commit_seq" in src.column_names else src
    new = _reorder(_add_bucket_columns(drop_seq), PARTITIONS_NEW_ORDER)

    b_live = write_zstd(live, OUT_DIR / "partitions_live.parquet")
    b_drop = write_zstd(drop_seq, OUT_DIR / "partitions_drop_seq.parquet")
    b_new = write_zstd(new, OUT_DIR / "partitions_new.parquet")
    _report("partitions", n, b_live, b_drop, b_new)
    _print_kv("partitions", sample["path"], n)


def main() -> int:
    cat = open_catalog()
    measure_primes(cat)
    print()
    measure_partitions(cat)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
