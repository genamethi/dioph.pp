from sage.all import factor, is_prime_power, Integer, Mod, next_prime
import polars as pl


def floorlog2(n):
    return Integer(n).nbits() - 1


def kzero(p):
    """k(p)=0: no p - 2^m (m>=1, up to floor(log2 p)) is a prime power q^n."""
    p = Integer(p)
    for m in range(1, floorlog2(p) + 1):
        x = p - 2 ** m
        if x >= 2 and is_prime_power(x):
            return False
    return True


def ord2(q):
    q = Integer(q)
    return 0 if q == 2 else int(Mod(2, q).multiplicative_order())


def rows_for(p):
    p = Integer(p)
    out = []
    for m in range(1, floorlog2(p) + 1):
        x = p - 2 ** m
        if x < 2:
            out.append(dict(p=int(p), m=m, pow2=int(2 ** m), x=int(x), omega=0,
                            Omega=0, prime_power=False, factorization=str(int(x)),
                            factors_ord=("unit" if x == 1 else str(int(x)))))
            continue
        fac = factor(x)
        parts = []
        for (q, e) in fac:
            tag = "—" if q == 2 else f"d{ord2(q)}"
            parts.append(f"{q}^{e}[{tag}]" if e > 1 else f"{q}[{tag}]")
        out.append(dict(
            p=int(p), m=m, pow2=int(2 ** m), x=int(x),
            omega=len(list(fac)),
            Omega=int(sum(e for _, e in fac)),
            prime_power=bool(is_prime_power(x)),
            factorization=str(fac),
            factors_ord=" · ".join(parts),
        ))
    return out


p1 = 149
# first k=0 prime with floor(log2 p) = 14  (2^14 <= p < 2^15)
p2 = None
q = next_prime(2 ** 14)
while q < 2 ** 15:
    if kzero(q):
        p2 = int(q)
        break
    q = next_prime(q)

pl.Config.set_tbl_rows(40)
pl.Config.set_fmt_str_lengths(240)
pl.Config.set_tbl_width_chars(260)
pl.Config.set_tbl_hide_dataframe_shape(True)

for p in (p1, p2):
    print(f"\n===== p = {p}   k0={kzero(p)}   floor(log2 p)={floorlog2(p)}   "
          f"positions m = 1..{floorlog2(p)} =====")
    df = pl.DataFrame(rows_for(p))
    print(df.select(["m", "pow2", "x", "omega", "Omega", "prime_power",
                     "factors_ord"]))
