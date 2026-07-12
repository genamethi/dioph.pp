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
# In Colab, uncomment the next line (or run it as its own cell):
# !pip install -q itables

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

# Keep a copy alongside the notebook.
df.to_csv("prime_partitions.csv", index=False)

# %% [markdown]
# ## Interactive table
#
# Sort by clicking a column header; type in the box under a header to filter that
# column; the search box filters across all columns; 25 rows per page.

# %%
from itables import init_notebook_mode, show

init_notebook_mode(all_interactive=True)

show(
    df,
    paging=True,
    pageLength=PAGE_LENGTH,
    lengthMenu=[10, 25, 50, 100, 250],
    column_filters="header",
    order=[[0, "asc"]],          # default sort by prime_rank
    scrollX=True,
)
