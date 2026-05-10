"""
Diagnostic: distribution of k (decompositions-per-prime) per 100M-prime window.

Correctness-first design (after streaming-groupby-with-row-index failed):

1. pyiceberg enumerates referenced primes parquet files in p-sorted order.
2. We iterate files sequentially. Per file we walk row groups via pyarrow
   (each ~1-10M rows; tiny memory) and maintain a Python global rank counter.
3. Window assignment is plain numpy: w = (rank_offset + local_idx) // WINDOW.
4. Per-batch we update a {w: {k: count}} histogram and a {w: [p_lo, p_hi, c]}
   bounds dict — both stay small (~217 windows × ~50 distinct k = ~10K entries).
5. Median/p99/min/max derived exactly from the per-window histogram.

Output: JSONL on stdout (one record per window) + final SUMMARY on stderr.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from statistics import mean, pstdev

import numpy as np
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from primeparts.iceberg_schema import open_catalog  # noqa: E402

WINDOW = 100_000_000


def planned_files(table) -> list[str]:
    schema = table.schema()
    p_field_id = schema.find_field("p").field_id
    rows = []
    for t in table.scan().plan_files():
        df = t.file
        lo_bytes = df.lower_bounds.get(p_field_id) if df.lower_bounds else None
        lo = int.from_bytes(lo_bytes, "little", signed=True) if lo_bytes else 0
        part = df.partition
        ptrunc = getattr(part, "p_trunc", None)
        if ptrunc is None:
            try:
                ptrunc = part[0]
            except Exception:
                ptrunc = 0
        rows.append((int(ptrunc), lo, df.file_path))
    rows.sort(key=lambda r: (r[0], r[1]))
    return [r[2] for r in rows]


def update_window(
    hist: dict[int, dict[int, int]],
    bounds: dict[int, list[int]],
    w: int,
    seg_k: np.ndarray,
    seg_p: np.ndarray,
) -> None:
    uniq, cnt = np.unique(seg_k, return_counts=True)
    wh = hist.setdefault(w, {})
    for ku, cu in zip(uniq.tolist(), cnt.tolist()):
        wh[ku] = wh.get(ku, 0) + cu
    p_lo = int(seg_p.min())
    p_hi = int(seg_p.max())
    n = int(seg_k.size)
    wb = bounds.get(w)
    if wb is None:
        bounds[w] = [p_lo, p_hi, n]
    else:
        if p_lo < wb[0]:
            wb[0] = p_lo
        if p_hi > wb[1]:
            wb[1] = p_hi
        wb[2] += n


def process_file(
    path: str,
    rank_offset: int,
    hist: dict[int, dict[int, int]],
    bounds: dict[int, list[int]],
) -> int:
    local = path.replace("file://", "")
    pqf = pq.ParquetFile(local)
    pos = 0
    for rg_idx in range(pqf.num_row_groups):
        tbl = pqf.read_row_group(rg_idx, columns=["p", "k"])
        n = tbl.num_rows
        if n == 0:
            continue
        ps = tbl.column("p").to_numpy(zero_copy_only=False)
        ks = tbl.column("k").to_numpy(zero_copy_only=False).astype(np.int64, copy=False)
        # window per row
        global_idx = np.arange(rank_offset + pos, rank_offset + pos + n, dtype=np.int64)
        ws = global_idx // WINDOW
        # window changes within row group are rare (≤ a couple per RG)
        change = np.flatnonzero(np.diff(ws)) + 1
        seg_starts = np.concatenate(([0], change))
        seg_ends = np.concatenate((change, [n]))
        for s, e in zip(seg_starts, seg_ends):
            w = int(ws[s])
            update_window(hist, bounds, w, ks[s:e], ps[s:e])
        pos += n
    return pos


def derive_stats(kc: dict[int, int]) -> dict:
    items = sorted(kc.items())
    ks = np.fromiter((k for k, _ in items), dtype=np.int64)
    cs = np.fromiter((c for _, c in items), dtype=np.int64)
    n = int(cs.sum())
    cum = cs.cumsum()
    k_sum = int((ks * cs).sum())
    return {
        "count": n,
        "k_sum": k_sum,
        "k_mean": k_sum / n,
        "k_median": int(ks[int(np.searchsorted(cum, (n + 1) // 2))]),
        "k_p99": int(ks[int(np.searchsorted(cum, int(np.ceil(0.99 * n))))]),
        "k_min": int(ks[0]),
        "k_max": int(ks[-1]),
    }


def main() -> int:
    cat = open_catalog()
    tbl = cat.load_table("funbuns.primes")
    paths = planned_files(tbl)
    print(f"# {len(paths)} referenced primes parquet files", file=sys.stderr, flush=True)

    hist: dict[int, dict[int, int]] = {}
    bounds: dict[int, list[int]] = {}
    rank = 0
    for i, p in enumerate(paths):
        rows = process_file(p, rank, hist, bounds)
        rank += rows
        print(
            f"# {i+1}/{len(paths)} rows+={rows:,} rank={rank:,} windows={len(bounds)}",
            file=sys.stderr,
            flush=True,
        )

    means = []
    for w in sorted(bounds.keys()):
        wb = bounds[w]
        stats = derive_stats(hist[w])
        rec = {"window": int(w), "p_lo": wb[0], "p_hi": wb[1], **stats}
        print(json.dumps(rec), flush=True)
        means.append(stats["k_mean"])

    if means:
        m = mean(means)
        sd = pstdev(means) if len(means) > 1 else 0.0
        ratio = sd / m if m else 0.0
        print(
            f"SUMMARY: windows={len(means)} mean_of_means={m:.6f} "
            f"stddev_of_means={sd:.6f} stddev_over_mean={ratio:.4%}",
            file=sys.stderr,
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
