# %% [markdown]
# # Prime partitions viewer (p = 2^m + q^n)
#
# A small, self-contained Colab script that reproduces the primes + partitions
# table for the first N primes, so you can scroll through them -- **including the
# primes that have no partition at all** (k = 0), which the aggregate views hide.
#
# The compute half mirrors `native/src/core.c` (`process_prime`) exactly, so the
# rows here match what the real `generate` pipeline would write. No native build
# is required -- it is pure Python.
#
# Columns follow the joined Iceberg schema from `native/src/schemas.cc`
# (`PrimesSchema` + `PartitionsSchema`), minus the storage-only bucket columns
# (`p_bucket_version`, `p_bucket`) which are writer internals, not produced by
# core.c:
#
# | column     | meaning                                             |
# |------------|-----------------------------------------------------|
# | prime_rank | 1-based prime index (rank 1 = p=2, rank 2 = p=3, ...) |
# | p          | the prime                                           |
# | k          | number of partitions found for p                    |
# | m_k        | exponent of 2   (the `2^m` term)                    |
# | n_k        | exponent of the prime power q  (the `q^n` term)     |
# | q_k        | the prime base q                                    |
#
# A partition row means:  **p = 2**^**m_k  +  q_k**^**n_k**   (q_k prime).
# Primes with k = 0 appear as a single row with m_k / n_k / q_k left blank.

# %% [markdown]
# ## Config

# %%
N_PRIMES = 1000       # how many primes to generate (first N)
PAGE_LENGTH = 25      # rows per page in the interactive table

# Fixed per-column pixel widths so the narrow numeric columns hug their data
# instead of stretching to fill the page. Tweak any of these to taste.
COL_WIDTHS = {
    "prime_rank": "72px",
    "p":          "72px",
    "k":          "44px",
    "m_k":        "52px",
    "n_k":        "52px",
    "q_k":        "72px",
}

# The production dataset omits rank 1 (p = 2) by convention -- a from-scratch
# build starts at index 2 (p = 3); see `kFreshStartIdx` in native/src/generate.cc.
# We INCLUDE p = 2 by default so you can see its k = 0 row. Set INCLUDE_P2 = False
# to match the production dataset exactly (rows start at rank 2 / p = 3).
INCLUDE_P2 = True

# %% [markdown]
# ## Install the interactive table library (Colab)
#
# `itables` renders a DataTables.js grid with clickable per-column sort, a global
# search box, per-column filter inputs, and page-of-25 pagination.

# %%
# In Colab, uncomment the next line (or run it as its own cell). itables drives
# the interactive table; sympy is used by the Hermite section at the end.
# !pip install -q itables sympy

# %% [markdown]
# ## Compute -- faithful port of native/src/core.c
#
# Everything below the config only depends on the Python standard library, so it
# also runs fine outside a notebook (the pandas / itables imports are deferred to
# the rendering cell).

# %%
def first_n_primes(n):
    """Return the first `n` primes as a list, via a simple growing sieve."""
    if n <= 0:
        return []
    # Upper bound for the n-th prime (n >= 6): n(ln n + ln ln n). Pad for small n.
    import math
    if n < 6:
        limit = 15
    else:
        limit = int(n * (math.log(n) + math.log(math.log(n)))) + 10
    while True:
        sieve = bytearray([1]) * (limit + 1)
        sieve[0:2] = b"\x00\x00"
        for i in range(2, int(limit**0.5) + 1):
            if sieve[i]:
                sieve[i * i : limit + 1 : i] = b"\x00" * len(range(i * i, limit + 1, i))
        primes = [i for i in range(2, limit + 1) if sieve[i]]
        if len(primes) >= n:
            return primes[:n]
        limit *= 2


def prime_power(n):
    """Mirror of `pp_is_prime_power_u64` (core.c:146).

    Return (base, exponent) with n == base**exponent and base prime, or (0, 0)
    when n is not a prime power. n is small here (n < ~8000), so trial division
    is plenty; a number is a prime power iff it has exactly one distinct prime
    factor.
    """
    if n < 2:
        return (0, 0)
    m = n
    base = 0
    exponent = 0
    d = 2
    while d * d <= m:
        if m % d == 0:
            base = d
            while m % d == 0:
                m //= d
                exponent += 1
            break
        d += 1 if d == 2 else 2
    if base == 0:
        # No factor <= sqrt(n) found: n is itself prime.
        return (n, 1)
    if m != 1:
        # A second distinct prime factor remains -> not a prime power.
        return (0, 0)
    return (base, exponent)


def power_of_three_exponent(q):
    """Mirror of `power_of_three_exponent` (core.c:403).

    On the "killed" parity, 3 | q, so a prime power there can only be 3^n.
    Return the exponent n if q == 3**n (n > 0), else 0.
    """
    e = 0
    while q % 3 == 0:
        q //= 3
        e += 1
    if q == 1 and e > 0:
        return e
    return 0


def process_prime(p):
    """Mirror of `process_prime` (core.c:418).

    Return the list of partitions of p as (m_k, n_k, q_k) tuples, meaning
    p = 2**m_k + q_k**n_k with q_k prime. Faithfully reproduces core.c's
    pruning: the Set-A power-of-three shortcut on the killed parity, and the
    per-prime `exhausted`-base skip (a base is exhausted after its 2nd hit).
    """
    max_m = p.bit_length() - 1          # floor(log2 p)
    killed_parity = 1 if (p % 3 == 2) else 0

    hit_count = {}      # base -> number of times recorded for this p
    exhausted = []      # bases hit >= 2 times; q divisible by one is skipped

    decomps = []
    power = 2                           # 2**m, starting at m = 1
    for m in range(1, max_m + 1):
        q = p - power
        if q >= 2 and not any(q % e == 0 for e in exhausted):
            if (m & 1) == killed_parity:
                # Set A: 3 | q, so a prime power here can only be 3**n.
                exponent = power_of_three_exponent(q)
                base = 3 if exponent > 0 else 0
            else:
                base, exponent = prime_power(q)
            if exponent > 0:
                decomps.append((m, exponent, base))
                # increment_hit (core.c:376): exhaust a base after its 2nd hit.
                hit_count[base] = hit_count.get(base, 0) + 1
                if hit_count[base] == 2 and len(exhausted) < 64:
                    exhausted.append(base)
        power <<= 1
    return decomps


def build_rows(n_primes, include_p2):
    """Build the joined primes+partitions rows for the first `n_primes` primes.

    One row per partition; a prime with k = 0 emits a single row with
    m_k / n_k / q_k = None. `prime_rank` is the true 1-based prime index.
    """
    primes = first_n_primes(n_primes)
    rows = []
    for rank, p in enumerate(primes, start=1):
        if p == 2 and not include_p2:
            continue
        decomps = process_prime(p)
        k = len(decomps)
        if k == 0:
            rows.append({"prime_rank": rank, "p": p, "k": 0,
                         "m_k": None, "n_k": None, "q_k": None})
        else:
            for (m_k, n_k, q_k) in decomps:
                rows.append({"prime_rank": rank, "p": p, "k": k,
                             "m_k": m_k, "n_k": n_k, "q_k": q_k})
    return rows


# %% [markdown]
# ## Self-test (runs anywhere, no pandas needed)
#
# Spot-checks derived by hand from the core.c logic. If these pass, the port
# matches the native algorithm.

# %%
def _selftest():
    assert process_prime(2) == []                    # 2 - 2 = 0  -> k = 0
    assert process_prime(3) == []                    # 3 - 2 = 1  -> k = 0
    assert process_prime(5) == [(1, 1, 3)]           # 5 = 2 + 3
    assert process_prime(7) == [(1, 1, 5), (2, 1, 3)]        # 2+5, 4+3
    assert process_prime(11) == [(1, 2, 3), (2, 1, 7), (3, 1, 3)]  # 2+9, 4+7, 8+3
    assert process_prime(17) == [(2, 1, 13), (3, 2, 3)]     # 4+13, 8+9
    assert prime_power(9) == (3, 2)
    assert prime_power(8) == (2, 3)
    assert prime_power(15) == (0, 0)
    assert prime_power(13) == (13, 1)
    print("self-test OK")


_selftest()

# %% [markdown]
# ## Build the table

# %%
import pandas as pd

COLUMNS = ["prime_rank", "p", "k", "m_k", "n_k", "q_k"]

df = pd.DataFrame(build_rows(N_PRIMES, INCLUDE_P2), columns=COLUMNS)
# Nullable integer dtype so k=0 rows show blank (not NaN floats) for m_k/n_k/q_k.
df = df.astype({
    "prime_rank": "int64", "p": "int64", "k": "int32",
    "m_k": "Int32", "n_k": "Int32", "q_k": "Int64",
})

n_primes_shown = df["p"].nunique()
n_without = int((df["k"] == 0).sum())
print(f"{n_primes_shown} primes, {len(df)} rows "
      f"({n_without} primes with no partition), columns = {list(df.columns)}")

# %% [markdown]
# ## Interactive table
#
# Sort by clicking a column header; type in the box under a header to filter that
# column; the search box filters across all columns; 25 rows per page.
#
# The table uses a fixed layout with the narrow per-column widths from
# `COL_WIDTHS` above, so it hugs the data instead of stretching to the page
# width. Adjust `COL_WIDTHS` if you want them wider/narrower.

# %%
from IPython.display import HTML, display
from itables import init_notebook_mode, show

init_notebook_mode(all_interactive=True)

TOTAL_WIDTH = sum(int(w.removesuffix("px")) for w in COL_WIDTHS.values())


def render_table(view):
    """Render `view` (any DataFrame with the standard columns) as the compact,
    narrow, sortable/filterable/paginated itables grid used throughout this
    notebook. Call it on `df` or on any filtered subset."""
    # CSS insurance: DataTables recomputes widths on draw and the header filter
    # <input> boxes carry a large default min-width -- both fight the widths
    # passed to show(). These `!important` rules (itables renders inline in the
    # Colab cell, so a page-level <style> reaches the table) pin the layout.
    css = [
        f"table.dataTable {{ table-layout:fixed !important; "
        f"width:{TOTAL_WIDTH}px !important; }}",
        "table.dataTable th, table.dataTable td {"
        " padding:2px 6px !important; overflow:hidden; text-overflow:ellipsis;"
        " white-space:normal; text-align:center; }",
        "table.dataTable thead input {"
        " min-width:0 !important; width:100% !important; box-sizing:border-box; }",
    ]
    for i, w in enumerate(COL_WIDTHS.values(), start=1):
        css.append(
            f"table.dataTable th:nth-child({i}), table.dataTable td:nth-child({i})"
            f" {{ width:{w} !important; }}"
        )
    display(HTML("<style>\n" + "\n".join(css) + "\n</style>"))

    # Documented itables recipe: columnDefs widths need autoWidth=False AND a
    # concrete total width in `style` (width:auto applies nothing).
    show(
        view,
        paging=True,
        pageLength=PAGE_LENGTH,
        lengthMenu=[10, 25, 50, 100, 250],
        column_filters="header",
        order=[[0, "asc"]],                      # default sort by prime_rank
        classes="display compact",               # tighter cell padding
        style=f"table-layout:fixed; width:{TOTAL_WIDTH}px; margin:0",
        autoWidth=False,
        columnDefs=[
            {"targets": i, "width": COL_WIDTHS[c], "className": "dt-center"}
            for i, c in enumerate(view.columns)
        ],
    )


render_table(df)

# %% [markdown]
# ## Export the full table as CSV
#
# The whole table (all `N_PRIMES`, every row including the k = 0 primes -- not
# just the current page of the interactive view). Writes `prime_partitions.csv`,
# triggers a browser download in Colab, and prints the CSV inline so you can copy
# it straight out of the output.

# %%
def export_csv(view, filename="prime_partitions.csv", download=True, echo=True):
    """Write `view` to `filename` as CSV, optionally trigger a Colab download,
    optionally print it inline, and return the CSV text. Works on `df` or any
    filtered subset."""
    csv_text = view.to_csv(index=False)
    with open(filename, "w") as f:
        f.write(csv_text)
    print(f"wrote {filename} ({len(view)} rows)")
    if download:
        try:  # In Colab, pop a download to your machine (no-op elsewhere).
            from google.colab import files
            files.download(filename)
        except Exception:
            pass
    if echo:
        print(csv_text)
    return csv_text


csv_text = export_csv(df)  # full table

# %% [markdown]
# ## Group filters (per-prime predicates)
#
# Some questions are about a prime's **whole set of partitions**, not one row --
# e.g. "primes that have partitions but *none* with `n_k == 1`". You can't get
# that by filtering rows to `n_k != 1` (that would keep the `n_k >= 2` rows of a
# prime that *also* has an `n_k == 1` row). You need a predicate over each
# prime's group of rows.
#
# `prime_filter(df, predicate)` does exactly that: `predicate(g)` receives the
# sub-table `g` of all rows for one prime and returns True/False; every row of
# the matching primes is kept. Write any predicate you like -- a few examples
# are shown.

# %%
def prime_filter(df, predicate):
    """Keep every row of the primes whose partition group satisfies `predicate`.
    `predicate(g)` gets the sub-DataFrame of all rows for one prime."""
    return df.groupby("p", sort=False).filter(predicate)


# The one you asked for: prime HAS partitions, and none of them has n_k == 1
# (i.e. every partition is p = 2^m + q^n with n >= 2).
def no_exponent_one(g):
    return g["k"].iloc[0] > 0 and (g["n_k"] != 1).all()


matches = prime_filter(df, no_exponent_one)
print(f"{matches['p'].nunique()} of {df['p'].nunique()} primes match "
      f"({len(matches)} rows)")
render_table(matches)

# Export the matches too (no auto-download by default; flip download=True):
# export_csv(matches, "prime_partitions_no_exponent_one.csv", download=False)

# More predicates to copy/adapt -- each receives one prime's rows `g`:
#   exactly one partition:      lambda g: g["k"].iloc[0] == 1
#   a base q used twice:        lambda g: g["q_k"].duplicated().any()
#   biggest exponent >= 3:      lambda g: g["k"].iloc[0] > 0 and g["n_k"].max() >= 3
#   all q bases equal:          lambda g: g["k"].iloc[0] > 0 and g["q_k"].nunique() == 1
# e.g.:  render_table(prime_filter(df, lambda g: g["k"].iloc[0] == 1))

# %% [markdown]
# ## Chains: the q --(m)--> p graph
#
# Each partition `p = 2^m + q^n` gives a directed edge **q --(m)--> p** (the base
# `q` is the parent of `p`). With the default `n = 1` slice, a path through this
# graph "resolves" into a sequence of `m` edge-labels -- e.g.
# `3 -> 5 -> 7 -> 11 -> 13 -> 29 -> 37 -> 41 -> 43 -> 59 -> 67 -> 71 -> 73 -> 137`
# reads `1,1,2,1,4,3,2,1,4,3,2,1,6` (or `6,1,2,3,4,1,2,3,4,1,2,1,1` read 137->3).
#
# Note: the **roots** of the n=1 graph (primes with no `n_k == 1` parent) are
# exactly the "no n_k == 1" primes from the group filter above.

# %%
from collections import defaultdict


def build_graph(df, n=1):
    """Directed graph from the partitions with n_k == n: an edge
    q --(m)--> p for each p = 2^m + q^n. Returns (children, parents, edges_df)
    where children[q] / parents[p] are lists of (m, other_node)."""
    sel = df[df["n_k"] == n][["p", "m_k", "q_k"]].dropna().astype(int)
    children, parents, rows = defaultdict(list), defaultdict(list), []
    for p, m, q in sel.itertuples(index=False):
        children[q].append((m, p))
        parents[p].append((m, q))
        rows.append({"parent_q": q, "m": m, "child_p": p})
    edges = (pd.DataFrame(rows, columns=["parent_q", "m", "child_p"])
             .sort_values(["parent_q", "m"]).reset_index(drop=True))
    return children, parents, edges


def chains(src, dst, children):
    """All simple directed paths src -> dst, each a list of (q, m, p) edges.
    Edges strictly increase the node value, so bounding by `dst` keeps this
    finite; still, wide targets can have exponentially many paths."""
    out = []

    def dfs(node, trail):
        if node == dst:
            out.append(trail[:])
            return
        for m, p in sorted(children.get(node, [])):
            if p <= dst:
                dfs(p, trail + [(node, m, p)])

    dfs(src, [])
    return out


def labels(path):
    """The m-sequence (edge labels) of a path returned by `chains`."""
    return [m for _, m, _ in path]


def nodes_of(src, path):
    """The node sequence of a path: [src, then each child]."""
    return [src] + [p for _, _, p in path]


children, parents, edges = build_graph(df, n=1)
print(f"n=1 graph: {len(set(edges.parent_q) | set(edges.child_p))} nodes, "
      f"{len(edges)} edges")

# Your example: all chains 3 -> 137 and their m-sequences.
paths = chains(3, 137, children)
print(f"\n{len(paths)} chains 3 -> 137:")
for path in paths:
    print("  ", "->".join(map(str, nodes_of(3, path))), " m:", labels(path))

# The edge list is a plain DataFrame -- export it for networkx/gephi/etc.:
# export_csv(edges, "nk1_edges.csv", download=False, echo=False)

# %% [markdown]
# ## Chains -> Hermite polynomials
#
# Your substitution, made explicit. Each edge `q --(m,n)--> p` is the partition
# `p = 2^m + q^n`, i.e. the map `f(t) = 2^m + t^n` applied to the parent `q`.
# Composing the edge maps along a chain, from the **root outward**, writes the
# leaf prime as a polynomial in the root variable `x` -- and that polynomial is
# an integer combination of probabilists' Hermite polynomials `He_n`. Each
# `He_n` alternates in sign; in the combination the negative terms cancel,
# leaving the (positive) binomial expansion. Example, root `3`:
#
# ```
# 3 --(1,2)--> 11 --(4,2)--> 137
# p(x) = 16 + (2 + x^2)^2 = x^4 + 4x^2 + 20 = He_4 + 10 He_2 + 27,   p(3) = 137
# ```
#
# `He_4 = x^4 - 6x^2 + 3`; its `-6x^2 + 3` is exactly cancelled by `10 He_2 + 27`.
# Enumerating the chains to a target gives you all such combinations; distinct
# chains can collapse to the same `He` expansion. Needs `sympy` (in Colab already).

# %%
import sympy as sp
from collections import defaultdict

X = sp.symbols("x")


def He(n, x=X):
    """Probabilists' (monic) Hermite polynomial He_n."""
    return sp.hermite_prob(n, x)


def build_graph_mn(df):
    """Full graph over ALL exponents n: edge q --(m,n)--> p for p = 2^m + q^n.
    children_mn[q] = list of (m, n, p)."""
    sel = df.dropna(subset=["n_k"])[["p", "m_k", "n_k", "q_k"]].astype(int)
    children_mn = defaultdict(list)
    for p, m, n, q in sel.itertuples(index=False):
        children_mn[q].append((m, n, p))
    return children_mn


def chains_mn(src, dst, children_mn):
    """All simple paths src -> dst in the full (m, n) graph; each a list of
    (q, m, n, p) edges. Edges strictly increase value, so `dst` bounds it."""
    out = []

    def dfs(node, trail):
        if node == dst:
            out.append(trail[:])
            return
        for m, n, p in sorted(children_mn.get(node, [])):
            if p <= dst:
                dfs(p, trail + [(node, m, n, p)])

    dfs(src, [])
    return out


def chain_poly(path, x=X):
    """Compose the edge maps f(t) = 2^m + t^n from the root outward; return the
    leaf prime as a sympy polynomial in x (x = the root / base value)."""
    poly = x
    for _q, m, n, _p in path:
        poly = 2 ** m + poly ** n
    return sp.expand(poly)


def to_hermite(expr, x=X):
    """Integer coefficients c_n with expr = sum_n c_n He_n (monic He basis)."""
    P = sp.Poly(sp.expand(expr), x)
    out = {}
    while P.as_expr() != 0:
        d = P.degree()
        c = P.LC()
        out[d] = out.get(d, 0) + c
        P = sp.Poly(sp.expand(P.as_expr() - c * He(d, x)), x)
        if d == 0:
            break
    return dict(sorted(out.items(), reverse=True))


def hermite_str(hd):
    """Pretty-print a {degree: coeff} Hermite expansion."""
    return " + ".join((f"{c}*He_{d}" if d else f"{c}") for d, c in hd.items())


children_mn = build_graph_mn(df)


def expand_chains(src, dst):
    """Enumerate every chain src -> dst, compose it to a polynomial in the root,
    and expand in the He_n basis. One row per chain."""
    rows = []
    for path in chains_mn(src, dst, children_mn):
        nodes = [src] + [p for *_, p in path]
        poly = chain_poly(path)
        hd = to_hermite(poly)
        assert poly.subs(X, src) == dst          # the chain rebuilds dst
        rows.append({
            "chain": "->".join(map(str, nodes)),
            "m_seq": [m for _, m, _, _ in path],
            "n_seq": [n for _, _, n, _ in path],
            "poly": str(poly),
            "hermite": hermite_str(hd),
            "degree": max(hd),
            "he_degrees": tuple(sorted(hd)),
        })
    return pd.DataFrame(rows)


# Your example, fully enumerated and expanded:
tab = expand_chains(3, 137)
present = sorted({d for degs in tab["he_degrees"] for d in degs})
print(f"{len(tab)} chains 3 -> 137 | distinct basis elements present: "
      f"{['He_%d' % d for d in present]}")
tab.sort_values("degree", ascending=False).head(12)
