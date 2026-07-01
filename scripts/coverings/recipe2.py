from sage.all import Integer, Mod, lcm, next_prime

def flog2(n): return Integer(n).nbits() - 1

def ord2(q): return int(Mod(2, q).multiplicative_order())

def kzero(p):
    p = Integer(p)
    for m in range(1, flog2(p) + 1):
        x = p - 2 ** m
        if x >= 2 and Integer(x).is_prime_power():
            return False
    return True

def dlog2(l, a):                       # r in [0,d) with 2^r == a (mod l)
    x, d = 1 % l, ord2(l)
    for r in range(d):
        if x == a: return r
        x = x * 2 % l
    return None

def covering_system(p):
    """Minimal distinct covering set of positions [1,L] using ALL prime factors
    of every p-2^m as candidate congruences (each factor l -> (ord_l 2, dlog))."""
    p = Integer(p); L = flog2(p)
    need = set()                       # positions that require covering
    cand = {}                          # d -> (r, factor l)  best phase per order
    cover_by = {}                      # (d,r) -> set of positions it covers
    for m in range(1, L + 1):
        x = p - 2 ** m
        if x < 2:                      # p = 2^m + 1: value is a unit, trivially obstructed
            continue
        need.add(m)
    for m in list(need):
        x = p - 2 ** m
        for (l, _e) in Integer(x).factor():
            if l == 2:                 # x is odd, won't happen; guard anyway
                continue
            d = ord2(l); r = dlog2(l, int(p % l))
            key = (d, r)
            cover_by.setdefault(key, set())
            # this factor (order d, phase r) divides p-2^m' for every m' == r (mod d)
            for mm in need:
                if mm % d == r:
                    cover_by[key].add(mm)
            cand.setdefault(d, (r, int(l)))
    # greedy distinct-modulus set cover, then prune to minimal
    chosen, used, covered = [], set(), set()
    while covered != need:
        best, bestgain = None, 0
        for (d, r), poss in cover_by.items():
            if d in used: continue
            gain = len(poss - covered)
            if gain > bestgain or (gain == bestgain and gain > 0 and
                                   (best is None or d < best[0])):
                best, bestgain = (d, r), gain
        if bestgain == 0: break
        chosen.append(best); used.add(best[0]); covered |= cover_by[best]
    changed = True
    while changed:
        changed = False
        for c in list(chosen):
            if (covered - cover_by[c]) | set().union(
                    *[cover_by[o] for o in chosen if o != c]) >= need \
               if [o for o in chosen if o != c] else False:
                pass
            rest = need & set().union(*[cover_by[o] for o in chosen if o != c]) \
                if [o for o in chosen if o != c] else set()
            if rest >= need:
                chosen.remove(c); changed = True; break
    chosen.sort()
    ok = need <= set().union(*[cover_by[c] for c in chosen]) if chosen else not need
    period = int(lcm([d for d, _ in chosen])) if chosen else 1
    return L, chosen, cover_by, period, ok, need

# smallest k=0 prime at each bit-length 7..14
targets = []
for L in range(7, 15):
    q = next_prime(2 ** L)
    while flog2(q) == L:
        if kzero(q): targets.append(int(q)); break
        q = next_prime(q)

print(f"{'p':>7} {'L':>3} {'#cong':>5} {'period':>7}  covering system  (m ≡ r (mod d))")
for p in targets:
    L, ch, cb, period, ok, need = covering_system(p)
    sysstr = "  ".join(f"{r}({d})" for d, r in ch)
    maxd = max((d for d, _ in ch), default=0)
    print(f"{p:>7} {L:>3} {len(ch):>5} {period:>7}  {sysstr}   [maxmod={maxd}, covers_all={ok}]")
