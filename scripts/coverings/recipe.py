from sage.all import factor, Integer, Mod, lcm


def floorlog2(n):
    return Integer(n).nbits() - 1


def ord2(q):
    return 0 if Integer(q) == 2 else int(Mod(2, q).multiplicative_order())


def recipe(p):
    p = Integer(p)
    L = floorlog2(p)
    rows = []          # (m, x, spf, d, r)
    for m in range(1, L + 1):
        x = p - 2 ** m
        spf = Integer(x).factor()[0][0]        # smallest prime factor
        d = ord2(spf)
        rows.append((m, int(x), int(spf), d, m % d))

    # authenticated congruences donated by the smallest factor of each position
    congr = sorted(set((d, r) for (_, _, _, d, r) in rows))
    smallest_factors = sorted(set(spf for (_, _, spf, _, _) in rows))
    orders = sorted(set(d for (_, _, _, d, _) in rows))
    P = int(lcm(orders))

    def covers(cs, m):
        return any(m % d == r for (d, r) in cs)

    # 100%-by-construction check over this prime's actual positions [1,L]
    full = all(covers(congr, m) for m in range(1, L + 1))

    # minimise: drop any congruence whose removal still covers every position
    S = list(congr)
    changed = True
    while changed:
        changed = False
        for c in list(S):
            trial = [x for x in S if x != c]
            if all(covers(trial, m) for m in range(1, L + 1)):
                S = trial
                changed = True
                break
    S.sort(key=lambda dr: (dr[0], dr[1]))
    distinct = len(set(d for d, _ in S)) == len(S)

    print(f"\n===== p = {int(p)}   floor(log2)={L} =====")
    print("  smallest factor per position (m: x -> spf[d=ord]):")
    for (m, x, spf, d, r) in rows:
        print(f"    m={m:>2}: {x:>6} -> l={spf:<6} d={d:<3} => m≡{r}(mod {d})")
    print(f"  target set (distinct smallest factors): {smallest_factors}")
    print(f"  their orders: {orders}   LCM(orders) = {P}   primes in LCM = "
          f"{sorted(int(q) for q, _ in factor(P))}")
    print(f"  donated congruences cover all positions [1,{L}]: {full}")
    print(f"  MINIMAL DISTINCT covering set ({len(S)} congruences, "
          f"distinct={distinct}, period={int(lcm([d for d,_ in S]))}):")
    for (d, r) in S:
        print(f"    m ≡ {r} (mod {d})")


for p in (149, 16787):
    recipe(p)
