import glob
from collections import defaultdict
import polars as pl
from sage.all import Integer, Mod, lcm

WH = "/media/extssd/research/dioph.pp/data/exp-5m-k0/warehouse/primeparts"
files = sorted(glob.glob(f"{WH}/primes_k0/**/*.parquet", recursive=True))

# small primes up to 50000 (composite k=0 value has smallest factor <= sqrt < 45000)
B = 50000
sieve = bytearray([1]) * (B + 1)
sieve[0] = sieve[1] = 0
for i in range(2, int(B ** 0.5) + 1):
    if sieve[i]:
        sieve[i * i::i] = bytearray(len(sieve[i * i::i]))
small = [i for i in range(3, B + 1, 2) if sieve[i]]   # odd primes only (values odd)
ORD = {l: int(Mod(2, l).multiplicative_order()) for l in small}

p_all = pl.read_parquet(files, columns=["p"])["p"].to_numpy()
# stratified sample across the whole range
stride = max(1, len(p_all) // 120_000)
P = p_all[::stride]
print(f"primes_k0 total={len(p_all)}  sample={len(P)} (stride {stride})")

order_pos = defaultdict(int)      # d -> #positions whose smallest factor has order d
factor_pos = defaultdict(int)     # l -> #positions
phase_by_mod = defaultdict(lambda: defaultdict(int))  # d -> {phase -> count}
npos = 0
frontier = 0                      # positions whose smallest-factor order > 32
CAP = 32
for p in P:
    p = int(p)
    L = p.bit_length() - 1
    for m in range(1, L + 1):
        x = p - (1 << m)
        if x < 2:
            continue
        npos += 1
        l = 0
        for q in small:
            if x % q == 0:
                l = q
                break
        if l == 0:
            frontier += 1            # smallest factor > 50000 (shouldn't happen for k0)
            continue
        d = ORD[l]
        order_pos[d] += 1
        factor_pos[l] += 1
        phase_by_mod[d][m % d] += 1
        if d > CAP:
            frontier += 1

print(f"\npositions scanned: {npos}   frontier (smallest-factor order > {CAP}): "
      f"{frontier} ({100*frontier/npos:.2f}%)")

print("\norder distribution of the smallest factor (top 20 by positions covered):")
for d, c in sorted(order_pos.items(), key=lambda kv: -kv[1])[:20]:
    ls = sorted([l for l in factor_pos if ORD[l] == d])[:4]
    print(f"  d={d:<4} positions={c:<10} ({100*c/npos:5.2f}%)  factors~{ls}")

small_orders = sorted([d for d in order_pos if d <= CAP])
print(f"\ndistinct small orders (<= {CAP}) appearing: {small_orders}")
print(f"LCM of those small orders = {int(lcm(small_orders))}")
print(f"primes in that LCM = {sorted(int(q) for q,_ in Integer(lcm(small_orders)).factor())}")

print("\nphase pile-up per small modulus (why distinctness needs drop/fold):")
for d in sorted(phase_by_mod):
    if d > 12:
        continue
    ph = phase_by_mod[d]
    tot = sum(ph.values())
    dist = {r: f"{100*c/tot:.0f}%" for r, c in sorted(ph.items())}
    print(f"  mod {d:<3}: phases {dist}")
