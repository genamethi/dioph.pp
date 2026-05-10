"""
Calibrate primes-table BPR by rewriting representative samples in the
locked post-repartition encoding (DELTA_BINARY_PACKED on p / prime_rank,
zstd-3, dict on for the low-cardinality columns).

Stops once the last K samples lie within T of their running mean, or
after MAX_SAMPLES. Reports the mean BPR and the corresponding rpb
(rows-per-bucket) it implies.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from primeparts.iceberg_schema import open_catalog  # noqa: E402

# Locked encoding (must match the rewriter)
ZSTD_LEVEL = 3
ROW_GROUP_ROWS = 200_000_000
DELTA_COLS = ["p", "prime_rank"]
DICT_COLS = ["k", "p_bucket_version", "p_bucket"]
PRIMES_NEW_ORDER = ["p", "k", "prime_rank", "p_bucket_version", "p_bucket"]

# Bucket-sizing constants
F = 4
TARGET_FILE_BYTES = 1 << 30
TARGET_BUCKET_BYTES = F * TARGET_FILE_BYTES

# Calibration policy
INITIAL_SAMPLES = 5 
MAX_SAMPLES = 10
CONVERGENCE_K = 3
TOLERANCE = 0.02

# Synthetic partition coords (constant per file by construction; their
# encoded cost is what we want to measure, not their real values)
SYNTH_BUCKET_VERSION = np.int32(1)
SYNTH_BUCKET_ID = np.int32(5)

OUT_DIR = Path("/tmp/primes_bpr_calibration")
OUT_DIR.mkdir(exist_ok=True)


def _bound(bounds, fid):
    raw = next((v for f, v in bounds if f == fid), None)
    return int.from_bytes(raw, "little", signed=True) if raw else None


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


def select_candidates(files: list[dict], n: int) -> list[dict]:
    """Roughly even spacing across the p-range. files are pre-sorted by p_min."""
    if len(files) <= n:
        return list(files)
    return [files[int(i * len(files) / n)] for i in range(n)]


def synthesize_prime_rank(
    src: pa.Table, sample: dict, all_files: list[dict]
) -> pa.Table:
    if "prime_rank" in src.column_names:
        return src
    rank_offset = sum(f["rows"] for f in all_files if f["p_max"] < sample["p_min"])
    n = src.num_rows
    ranks = pa.array(
        np.arange(rank_offset, rank_offset + n, dtype=np.int64), type=pa.int64()
    )
    return src.append_column("prime_rank", ranks)


def add_bucket_columns(table: pa.Table) -> pa.Table:
    n = table.num_rows
    ver = pa.array(np.full(n, SYNTH_BUCKET_VERSION, dtype=np.int32), type=pa.int32())
    bkt = pa.array(np.full(n, SYNTH_BUCKET_ID, dtype=np.int32), type=pa.int32())
    return table.append_column("p_bucket_version", ver).append_column("p_bucket", bkt)


def to_new_schema(src: pa.Table, sample: dict, all_files: list[dict]) -> pa.Table:
    drops = [c for c in ("p_trunc", "commit_seq") if c in src.column_names]
    if drops:
        src = src.drop(drops)
    src = synthesize_prime_rank(src, sample, all_files)
    src = add_bucket_columns(src)
    return src.select(PRIMES_NEW_ORDER)


def write_pinned(table: pa.Table, out: Path) -> int:
    pq.write_table(
        table,
        out,
        compression="zstd",
        compression_level=ZSTD_LEVEL,
        row_group_size=ROW_GROUP_ROWS,
        use_dictionary=DICT_COLS,
        column_encoding={c: "DELTA_BINARY_PACKED" for c in DELTA_COLS},
        write_statistics=True,
    )
    return out.stat().st_size


def measure_one(sample: dict, all_files: list[dict], idx: int) -> tuple[int, int, float]:
    src = pq.read_table(sample["path"])
    new = to_new_schema(src, sample, all_files)
    out = OUT_DIR / f"sample_{idx:02d}.parquet"
    nbytes = write_pinned(new, out)
    return new.num_rows, nbytes, nbytes / new.num_rows


def converged(bprs: list[float]) -> bool:
    if len(bprs) < CONVERGENCE_K:
        return False
    window = bprs[-CONVERGENCE_K:]
    m = sum(window) / CONVERGENCE_K
    return all(abs(b - m) / m <= TOLERANCE for b in window)


def main() -> int:
    cat = open_catalog()
    tbl = cat.load_table("funbuns.primes")
    all_files = files_in_p_order(tbl)
    n_total = sum(f["rows"] for f in all_files)
    print(f"manifest: {len(all_files)} files, total rows = {n_total:,}")

    candidates = select_candidates(all_files, MAX_SAMPLES)
    if len(candidates) < INITIAL_SAMPLES:
        print(f"only {len(candidates)} files; need at least {INITIAL_SAMPLES}")
        return 1

    print(
        f"\npolicy: initial={INITIAL_SAMPLES}, max={MAX_SAMPLES}, "
        f"k={CONVERGENCE_K}, T={TOLERANCE:.1%}\n"
    )
    print(
        f"{'i':>2}  {'file':<32}  {'p_min':>14}  {'p_max':>14}  "
        f"{'rows':>14}  {'B/row':>8}  {'dev_run':>8}"
    )
    print("-" * 108)

    bprs: list[float] = []
    converged_at: int | None = None
    for idx, sample in enumerate(candidates):
        rows, _nbytes, bpr = measure_one(sample, all_files, idx)
        bprs.append(bpr)
        running = sum(bprs) / len(bprs)
        dev = (bpr - running) / running
        print(
            f"{idx:>2}  {Path(sample['path']).name:<32}  "
            f"{sample['p_min']:>14,}  {sample['p_max']:>14,}  "
            f"{rows:>14,}  {bpr:>8.4f}  {dev:>+7.2%}"
        )

        if idx + 1 >= INITIAL_SAMPLES and converged(bprs):
            converged_at = idx + 1
            break

    final = bprs[-CONVERGENCE_K:] if len(bprs) >= CONVERGENCE_K else bprs
    mean_bpr = sum(final) / len(final)
    spread = (max(final) - min(final)) / mean_bpr
    rpb = int(TARGET_BUCKET_BYTES / mean_bpr)
    n_buckets = -(-n_total // rpb)

    print()
    if converged_at is not None:
        print(
            f"converged after {converged_at} samples "
            f"(last {CONVERGENCE_K} within ±{TOLERANCE:.1%} of their mean)"
        )
    else:
        print(f"did not converge within {MAX_SAMPLES} samples")
    print(
        f"BPR (mean of last {len(final)}) = {mean_bpr:.4f}  "
        f"spread = {spread:.2%}"
    )
    print(
        f"rpb = floor(F * 2^30 / BPR) = floor({TARGET_BUCKET_BYTES:,} / {mean_bpr:.4f}) "
        f"= {rpb:,}"
    )
    print(f"B   = ceil(N / rpb)             = ceil({n_total:,} / {rpb:,}) = {n_buckets}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
