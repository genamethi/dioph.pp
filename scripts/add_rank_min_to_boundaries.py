#!/usr/bin/env python3
"""Evolve funbuns.boundaries: add `rank_min: long` and populate it.

`rank_min[bk]` is the table-local prime_rank (rank 0 = smallest present prime,
p=3; p=2 is absent) of the first prime in bucket `bk`. The rewriter consumes
it as the starting prime_rank for each bucket; cumsum-of-p-changes within the
bucket fills the rest, producing a globally contiguous prime_rank 0..N-1.

Computed by reading `funbuns.primes` manifest stats: files entirely below
`p_min` contribute their full record_count; files entirely above are skipped;
at most one file straddles each boundary and its `p` column is decoded and
binary-searched to count exactly.

For buckets 0..2 the answer matches the idealized `bk * RPB`. Buckets 3..5
come in 1M lower — that is the expected, known-good signal of the
dedup of 1M duplicated primes inside p ∈ [254.9e9, 254.95e9] (the bad
flint run). Using the observed count for rank_min — not bk*RPB — is what
keeps prime_rank contiguous across the gap.

Idempotent: a re-run reads the existing rank_min column (if present) and
exits clean when values match. Pass --apply only if you actually need to
populate or repair rank_min; a stale run on an already-populated table is
otherwise a no-op write you can avoid.
"""

from __future__ import annotations

import argparse

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from pyiceberg.types import LongType

from primeparts.iceberg_schema import NAMESPACE, PRIMES_IDENT, open_catalog

BOUNDARIES_IDENT = f"{NAMESPACE}.boundaries"
RPB = 3_855_446_405  # rows-per-bucket; mirrors crates/primeparts-compact/src/planner.rs


def count_primes_lt(primes_tbl, p_threshold: int) -> int:
    files = primes_tbl.inspect.files()
    record_count = files["record_count"]
    file_path = files["file_path"]
    metrics_p = files["readable_metrics"].combine_chunks().field("p")
    p_lo = metrics_p.field("lower_bound")
    p_hi = metrics_p.field("upper_bound")

    total = 0
    straddling: list[str] = []
    for i in range(files.num_rows):
        lo = p_lo[i].as_py()
        hi = p_hi[i].as_py()
        n = record_count[i].as_py()
        if hi < p_threshold:
            total += n
        elif lo >= p_threshold:
            continue
        else:
            straddling.append(file_path[i].as_py())

    for path in straddling:
        local = path[len("file://"):] if path.startswith("file://") else path
        p_col = pq.read_table(local, columns=["p"])["p"]
        p_arr = p_col.combine_chunks().to_numpy(zero_copy_only=False)
        # Primes are sorted ASC within a file (sort_order on funbuns.primes).
        idx = int(np.searchsorted(p_arr, p_threshold, side="left"))
        total += idx
    return total


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Evolve the schema and overwrite the table if needed; without it, only computes and prints.",
    )
    args = parser.parse_args()

    cat = open_catalog()
    bnd = cat.load_table(BOUNDARIES_IDENT)
    existing = bnd.scan().to_arrow().to_pylist()
    rows = sorted(existing, key=lambda r: (r["p_bucket_version"], r["p_bucket"]))
    column_present = "rank_min" in {f.name for f in bnd.schema().fields}
    already_populated = column_present and all(r.get("rank_min") is not None for r in rows)

    if already_populated:
        primes_tbl = cat.load_table(PRIMES_IDENT)
        print(f"{BOUNDARIES_IDENT} already has rank_min — verifying against primes:")
        print(f"  {'bucket':>6}  {'p_min':>15}  {'rank_min':>15}  status")
        mismatches = 0
        for row in rows:
            observed = count_primes_lt(primes_tbl, row["p_min"])
            status = "ok" if observed == row["rank_min"] else f"DRIFT (primes={observed:,})"
            mismatches += int(status != "ok")
            print(f"  {row['p_bucket']:>6}  {row['p_min']:>15}  {row['rank_min']:>15}  {status}")
        if mismatches:
            raise SystemExit(
                f"{mismatches} bucket(s) drifted from stored rank_min; "
                "the primes data changed since boundaries was populated. "
                "Investigate before forcing an overwrite."
            )
        print("\nrank_min matches actual primes-table counts on every bucket; nothing to do.")
        return

    primes_tbl = cat.load_table(PRIMES_IDENT)
    print(f"{BOUNDARIES_IDENT} — populating rank_min for {len(rows)} bucket(s):")
    print(f"  {'bucket':>6}  {'p_min':>15}  {'rank_min':>15}")
    for row in rows:
        row["rank_min"] = count_primes_lt(primes_tbl, row["p_min"])
        print(f"  {row['p_bucket']:>6}  {row['p_min']:>15}  {row['rank_min']:>15}")

    if not args.apply:
        print("\ndry-run; pass --apply to evolve the schema and overwrite the table")
        return

    if not column_present:
        with bnd.update_schema() as us:
            us.add_column("rank_min", LongType(), required=False)
        bnd = cat.load_table(BOUNDARIES_IDENT)
        print("added rank_min column to schema")

    arrow = pa.Table.from_pylist(
        rows,
        schema=pa.schema([
            pa.field("p_bucket_version", pa.int32(), nullable=False),
            pa.field("p_bucket", pa.int32(), nullable=False),
            pa.field("p_min", pa.int64(), nullable=False),
            pa.field("rank_min", pa.int64(), nullable=True),
        ]),
    )
    bnd.overwrite(arrow)
    print(f"overwrote {BOUNDARIES_IDENT} with rank_min populated")


if __name__ == "__main__":
    main()
