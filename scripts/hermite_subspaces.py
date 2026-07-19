"""Hermite subspace decomposition of the q -> p chain graph.

Reads a bounded slice of primeparts.partitions straight from the Iceberg
warehouse (read-only) using catalogd's server-side scan-planning endpoint, then
reports how the Hermite subspace spanned by chain polynomials decomposes as a
function of the p-range bound B.

Background. Each partition p = 2^m + q^n is a directed edge q --(m,n)--> p,
i.e. the map f(t) = 2^m + t^n applied to the parent q. Composing the edge maps
along a chain writes the leaf prime as a polynomial in the root variable, and
that polynomial expands over the probabilists' Hermite basis He_n. A chain's
degree is the product of its n's, so only the n >= 2 edges ever raise the
degree -- and those are vanishingly rare (~0.02% of rows at p <= 1e8), which is
what makes the whole question tractable at scale.

Two closed forms this script states and checks against the data:

  Hermite coefficients   c_n = [x^n] exp((1/2) d^2/dx^2) P(x)
  Exponent ceiling       max_n(B) = floor(log_3 B)

The second follows from q >= 3 and q^n < p <= B. Since a chain of total degree
D from a root >= 3 already exceeds 3^D, the same bound caps the chain degree,
so the spanned subspace is span{He_0 .. He_D} with D = floor(log_3 B).

Usage:
    pixi run python scripts/hermite_subspaces.py
    pixi run python scripts/hermite_subspaces.py --bound 5e9 --out /tmp/pp
"""

from __future__ import annotations

import argparse
import json
import math
import time
import urllib.request
from pathlib import Path

import polars as pl
import sympy as sp

DEFAULT_REST = "http://127.0.0.1:8181"
NAMESPACE = "primeparts"
X = sp.symbols("x")


def _request(url: str, body: dict | None = None, timeout: int = 60,
             attempts: int = 8) -> dict:
    """POST/GET JSON, retrying transient non-2xx replies.

    A stale catalogd sharing :8181 via SO_REUSEPORT answers a fraction of
    requests with HTTP 406 while still returning a well-formed body, so retry
    rather than trusting the first status line.
    """
    data = None if body is None else json.dumps(body).encode()
    headers = {"Accept": "application/json"}
    if data:
        headers["Content-Type"] = "application/json"
    last = None
    for attempt in range(attempts):
        req = urllib.request.Request(url, data=data, headers=headers,
                                     method="POST" if data else "GET")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read())
        except urllib.error.HTTPError as exc:
            last = exc
            time.sleep(0.2 * (attempt + 1))
    raise RuntimeError(f"{url} failed after {attempts} attempts: {last}")


def plan_files(table: str, bound: int, rest_uri: str = DEFAULT_REST,
               poll_seconds: int = 60) -> list[str]:
    """Iceberg REST server-side scan planning: data files that may hold p <= bound.

    Honours the table's current snapshot, so this never picks up orphaned files
    the way a raw directory glob would.
    """
    base = f"{rest_uri}/v1/namespaces/{NAMESPACE}/tables/{table}"
    plan = _request(f"{base}/plan", {
        "select": ["p", "m_k", "n_k", "q_k"],
        "filter": {"type": "lt", "term": "p", "value": int(bound) + 1},
    })
    status = plan.get("status")
    plan_id = plan.get("plan-id")
    deadline = time.time() + poll_seconds
    while status == "submitted" and time.time() < deadline:
        time.sleep(1)
        plan = _request(f"{base}/plan/{plan_id}")
        status = plan.get("status")
    if status != "completed":
        raise RuntimeError(f"scan planning did not complete: status={status}")
    return [t["data-file"]["file-path"] for t in plan.get("file-scan-tasks", [])]


def load_power_edges(files: list[str], bound: int) -> pl.DataFrame:
    """The n_k >= 2 edges with p <= bound -- the only ones that raise degree."""
    return (pl.scan_parquet(files)
              .filter((pl.col("n_k") >= 2) & (pl.col("p") <= bound))
              .select(["p", "m_k", "n_k", "q_k"])
              .collect()
              .sort("p"))


def hermite_coeffs(poly, x=X) -> dict[int, int]:
    """Expand a polynomial over the probabilists' Hermite basis He_n.

    Closed form: since He_n = exp(-(1/2) d^2/dx^2) x^n, applying the inverse
    operator to P turns its Hermite coefficients into ordinary monomial
    coefficients, so c_n = [x^n] exp((1/2) d^2/dx^2) P(x). The sum terminates
    because P is a polynomial.
    """
    P = sp.expand(poly)
    total = sp.Integer(0)
    k = 0
    term = P
    while term != 0:
        total += term / (2 ** k * sp.factorial(k))
        k += 1
        term = sp.diff(P, x, 2 * k)
    dense = sp.Poly(sp.expand(total), x)
    degree = dense.degree()
    return {d: c for d, c in zip(range(degree, -1, -1), dense.all_coeffs()) if c != 0}


def chain_polynomial(a0: int, powers: list[tuple[int, int]], x=X):
    """Canonical reduced form of a chain: (((x + a0)^n1 + c1)^n2 + c2 ...).

    Runs of n = 1 edges are pure translations, so they collapse into the
    additive constants; a0 is the translation before the first power edge and
    each c_i is that power edge's 2^m plus the translation run following it.
    This is why so many distinct chains share one Hermite expansion: only the
    sums survive, not the routing.
    """
    poly = x + a0
    for n, c in powers:
        poly = poly ** n + c
    return sp.expand(poly)


def subspace_table(edges: pl.DataFrame, bounds: list[int],
                   offsets: list[int]) -> pl.DataFrame:
    """Degree spectrum and Hermite subspace dimension at each bound."""
    rows = []
    for B in bounds:
        sub = edges.filter(pl.col("p") <= B)
        observed = int(sub["n_k"].max()) if sub.height else 1
        exponents = sorted(sub["n_k"].unique().to_list())
        predicted = math.floor(math.log(B, 3))
        rows.append({
            "bound": B,
            "power_edges": sub.height,
            "max_n_observed": observed,
            "max_n_closed_form": predicted,
            "contiguous": exponents == list(range(2, observed + 1)),
            "dim_closed_form": observed + 1,
            "dim_measured": spanned_dimension(edges, offsets, B),
        })
    return pl.DataFrame(rows)


def sample_translations(files: list[str], bound: int, limit: int = 64) -> list[int]:
    """Distinct 2^m offsets realized by n = 1 edges (the degree-1 chains)."""
    got = (pl.scan_parquet(files)
             .filter((pl.col("n_k") == 1) & (pl.col("p") <= bound))
             .select("m_k").unique().limit(limit).collect())
    return sorted(2 ** int(m) for m in got["m_k"])


def spanned_dimension(edges: pl.DataFrame, offsets: list[int], bound: int) -> int:
    """Rank of the Hermite coefficient vectors of chains realized below `bound`.

    Every power edge (q, m, n, p) is itself a length-1 chain rooted at q, whose
    polynomial in the root variable is x^n + 2^m; every n = 1 edge gives x + 2^m.
    Stacking their Hermite coefficient vectors and taking the rank measures the
    dimension actually spanned, rather than assuming it.
    """
    sub = edges.filter(pl.col("p") <= bound)
    if not sub.height:
        return 0
    polys = []
    for n in sorted(sub["n_k"].unique().to_list()):
        m = int(sub.filter(pl.col("n_k") == n)["m_k"].min())
        polys.append(X ** int(n) + 2 ** m)
    polys.extend(X + off for off in offsets[:2])
    width = max(sp.Poly(p, X).degree() for p in polys) + 1
    rows = []
    for poly in polys:
        coeffs = hermite_coeffs(poly)
        rows.append([coeffs.get(d, 0) for d in range(width)])
    return sp.Matrix(rows).rank()


def grading(edges: pl.DataFrame, bound: int) -> pl.DataFrame:
    """Multiplicity of each graded piece: power edges per exponent n."""
    return (edges.filter(pl.col("p") <= bound)
                 .group_by("n_k").len()
                 .rename({"n_k": "n", "len": "edges"})
                 .sort("n"))


def verify(edges: pl.DataFrame) -> None:
    """Check both closed forms and the canonical chain form against the data."""
    bad = [r for r in edges.iter_rows(named=True)
           if r["p"] != 2 ** r["m_k"] + r["q_k"] ** r["n_k"]]
    assert not bad, f"p = 2^m + q^n failed on {len(bad)} rows, e.g. {bad[:3]}"

    known = chain_polynomial(0, [(2, 2 ** 1), (2, 2 ** 4)])
    assert known == sp.expand(X ** 4 + 4 * X ** 2 + 20), known
    assert hermite_coeffs(known) == {4: 1, 2: 10, 0: 27}, hermite_coeffs(known)
    assert known.subs(X, 3) == 137

    for n in (2, 3, 5, 8):
        poly = chain_polynomial(0, [(n, 0)])
        assert hermite_coeffs(poly) == hermite_coeffs(sp.expand(X ** n))
    print("verified: identity, canonical chain form, Hermite closed form")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bound", type=float, default=5e9,
                    help="upper bound on p (default 5e9)")
    ap.add_argument("--rest-uri", default=DEFAULT_REST)
    ap.add_argument("--out", type=Path, default=None,
                    help="ephemeral directory to write CSVs into")
    args = ap.parse_args()
    bound = int(args.bound)

    t0 = time.time()
    files = plan_files("partitions", bound, args.rest_uri)
    print(f"scan planning: {len(files)} data file(s) for p <= {bound:,} "
          f"({time.time() - t0:.1f}s)")

    t0 = time.time()
    edges = load_power_edges(files, bound)
    print(f"power edges (n >= 2): {edges.height:,} rows ({time.time() - t0:.1f}s)")

    verify(edges)

    bounds = [b for b in (10 ** 3, 10 ** 4, 10 ** 5, 10 ** 6, 10 ** 7,
                          10 ** 8, 10 ** 9, bound) if b <= bound]
    offsets = sample_translations(files, bound)
    table = subspace_table(edges, sorted(set(bounds)), offsets)
    print("\nHermite subspace vs p-range bound")
    print(table)
    assert (table["max_n_observed"] == table["max_n_closed_form"]).all(), \
        "closed form max_n(B) = floor(log_3 B) disagreed with the data"
    assert table["contiguous"].all(), "exponent spectrum was not contiguous"
    assert (table["dim_measured"] == table["dim_closed_form"]).all(), \
        "measured rank disagreed with floor(log_3 B) + 1"
    print("closed form max_n(B) = floor(log_3 B) holds at every bound; "
          "measured rank matches dim = floor(log_3 B) + 1")

    grades = grading(edges, bound)
    print(f"\ngraded pieces at B = {bound:,} (edges per exponent)")
    print(grades)

    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        table.write_csv(args.out / "subspace_by_bound.csv")
        grades.write_csv(args.out / "grading.csv")
        edges.write_csv(args.out / "power_edges.csv")
        print(f"\nwrote CSVs to {args.out}")


if __name__ == "__main__":
    main()
