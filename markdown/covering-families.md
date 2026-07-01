# Covering-system scripts over positions x = p − 2^m (k=0 set)

Scripts committed under `scripts/coverings/`. Run with Sage+polars:
`pixi run python scripts/coverings/<file>.py` from the repo root (plain
`python3` lacks polars/pyarrow/sage).

Congruence convention throughout: a prime factor `l` of `x = p − 2^m` gives
`d = ord_l(2)` and phase `r = m mod d`; the authenticated congruence is
`m ≡ r (mod d)`, covering density `1/d` of the positions. `l` is the generator
of the ⟨2⟩ discrete-log action, not a divisibility sieve.

## Scripts

- `pm2_table.py` — factor table of `x = p − 2^m` for `p = 149` and the first
  `L=14` k=0 prime `p = 16787`; per position: ω, Ω, is-prime-power, full
  factorization with `ord_l(2)` tags.
- `recipe.py` — per prime: smallest prime factor of each `x`, its order and
  phase; verifies the donated congruences cover every position `[1,L]` by
  construction (each `x` is composite ⇒ always has a factor), then prunes to a
  minimal distinct covering system.
  - `149 → {1(2), 1(3), 2(4)}`, period 12
  - `16787 → {1(2), 0(3), 4(5), 0(10), 2(12), 8(60)}`, period 60
- `recipe2.py` — same, but candidates drawn from **all** prime factors of each
  `x` (not just the smallest); greedy distinct-modulus set-cover then prune.
  Table over the smallest k=0 prime at each bit-length `L = 7..14`.
- `global_sample.py` — stride sample (~120k of 17,020,122) over `primes_k0`:
  smallest-factor order distribution, frontier fraction, per-modulus phase
  populations, LCM of small orders.

## Verified numbers

Per-prime minimal distinct covering systems (`recipe2.py`, all-factor):

```
 p        L  #cong period  system  (m ≡ r (mod d))
 149      7   3     12     1(2) 1(3) 2(4)
 331      8   4    264     0(2) 1(3) 3(8) 5(11)
 701      9   4     24     1(2) 0(3) 0(4) 2(8)
 1087    10   4     12     0(2) 1(3) 1(4) 3(12)
 2293    11   5     84     0(2) 2(3) 3(4) 9(12) 1(28)
 4153    12   5     60     0(2) 1(3) 3(4) 9(10) 5(12)
 8287    13   4     40     0(2) 1(4) 3(8) 7(20)
 16787   14   6     60     1(2) 0(3) 4(5) 0(10) 2(12) 8(60)
```

All `covers_all = True`; per-system `Σ 1/d ≈ 1.08–1.23` (just over the covering
threshold of 1).

Global sample (`global_sample.py`, ~120k primes, ~3.49M positions), smallest
factor per position:

- order distribution: `d2 (l=3) ≈ 50%`, `d4 (l=5) ≈ 20%`, `d3 (l=7) ≈ 7%`,
  then d10, d12, d8, d18, …
- ~90% of positions have smallest-factor order ≤ 32; **10.1% frontier**
  (smallest-factor order > 32).
- distinct small orders (≤32) aggregate `Σ 1/d ≈ 2.68`; LCM ≈ 2.40e9; primes in
  that LCM = {2,3,5,7,11,13,23,29}.
- per-modulus phases are ~uniformly populated: `mod 3 → 33/33/33`,
  `mod 4 → 25/25/25/25`, `mod 2 → 49/51`.

## Data / services

- warehouse: `/media/extssd/research/dioph.pp/data/exp-5m-k0/warehouse`
- `primes_k0`: 17,020,122 rows (k=0), `p_max = 2,038,074,659`
- `primes`: 100M rows, `p_max = 2,038,074,743`
- `mersenne_factors`: `(prime=l, ord2=d, is_primitive = (ord2==d))`
- catalogd REST: `http://127.0.0.1:8181` serves this warehouse
- C++ binary: `native/build/primeparts-covering-sieve`; add
  `--rest-uri http://127.0.0.1:8181 --warehouse <path>`

## C++ `--method3` mode (uncommitted WIP on this branch)

`native/src/coverings/covering_sieve_main.cc` has a `--method3` mode
(`--min-modulus`, default 3; `--period-cap`, default 2520) that builds one
fixed-phase distinct covering system over `[0, period_cap)` and does a realized
recall scan over the full `primes_k0`, all physical cores. A single fixed-phase
system realizes only ~1.7% of k=0 positions by itself — see phase saturation
below. The per-prime families are the object; one frozen system is not.

## Open: combining the families by density

Density-sort combination = pool all authenticated congruences `(d, r)` across
primes, sort by density `1/d`, greedily cover the full position set. Three
concrete obstacles:

1. **Phase saturation.** Across all k=0 primes, every phase class of each small
   modulus is populated (the ~uniform splits above). Combining cannot keep one
   phase per modulus — covering all positions at modulus `d` requires all `d`
   phases, so the combination rebuilds the full phase family (all `d`
   congruences at modulus `d`) rather than collapsing to a distinct system.
   No global DROP. This is why the all-k=0 statement is carried by density over
   the distinct moduli `{d}` (each counted once, `Σ 1/d`), not a single system.
2. **Frontier tail.** The ~10.1% of positions with smallest-factor order > 32
   need large moduli with vanishing `1/d`; density-sort covers the ~90% bulk
   fast, then stalls. Lever: CRT-fold small `l_i` into composite `L = ∏ l_i`
   with `ord_L(2) = lcm(d_i)` to manufacture the large distinct moduli.
3. **Weights not yet computed at scale.** The density weights (positions
   covered per `(d, r)`) exist only from the stride sample, not over the full
   17,020,122. Running the sort needs the full all-factor order spectrum from
   the C++ path.
