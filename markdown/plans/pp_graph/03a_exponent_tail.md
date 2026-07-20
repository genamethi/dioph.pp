# 03a — the exponent tail and the 3|n obstruction

Prompted by the non-monotone tail of the full-dataset grading (n=19,20 → 3
edges each; n=21 → 6; n=24 → 4). Conjecture under test: a subgroup-exclusion
property acting on the exponent `n` of the odd prime power, not on `m`.

## The tail needs no warehouse

For `n >= 17` only `q = 3` can occur, since `5^17 = 7.63e11 > max_p = 6.19e11`.
So the whole tail of the grading is `#{m : 2^m + 3^n prime, p <= max_p}` —
arithmetic, not data. Computed against the census, all eight values agree:

| n | 17 | 18 | 19 | 20 | 21 | 22 | 23 | 24 |
|---|---|---|---|---|---|---|---|---|
| computed | 5 | 6 | 3 | 3 | 6 | 4 | 3 | 4 |
| census | 5 | 6 | 3 | 3 | 6 | 4 | 3 | 4 |

This is an independent check of the whole read path — planner, shards, residual
— against pure arithmetic, and it passes exactly.

## The 3|n effect is real, and it runs backwards

Scale-free window `m in [1, n·log2 3]` (so `2^m <= 3^n`), n = 4..240, against
the sieve density `S(n) = prod_l (1 - [(-3^n) in <2> mod l] / ord_l(2))`.

| class | N | mean hits | mean S | hits/S |
|---|---|---|---|---|
| n ≡ 0 mod 3 | 79 | 4.089 ± 0.219 | 0.1551 | 26.35 |
| n ≡ 1 mod 3 | 79 | 4.025 ± 0.241 | 0.2248 | 17.90 |
| n ≡ 2 mod 3 | 79 | 4.177 ± 0.267 | 0.2245 | 18.60 |

Two facts, and the second is the interesting one:

1. **3|n carries a heavier local obstruction.** S drops from ~0.225 to 0.155,
   a 31% loss. Mechanism confirmed directly: a modulus is *active* (i.e.
   `-3^n in <2> mod l`, so it kills an AP of m) on **70.2%** of (l, n) pairs
   when 3|n, against **61.9%** otherwise. The cause is subgroup exclusion on
   the exponent exactly as conjectured: when 3|n, `3^n` lies in the smaller
   subgroup `<27>` of `<3>` mod l, and a smaller subgroup lands inside `<2>`
   more often.
2. **The prime yield does not drop.** Mean hits are 4.089 / 4.025 / 4.177 —
   flat across the three classes, well within one standard error. So the local
   model predicts a 31% penalty for 3|n that the primes do not pay: the excess
   `hits/S` is 26.35 for 3|n against ~18 for the others, a **~45% surplus**.

This is a local-global discrepancy isolated to a clean one-parameter family —
the same species as the open question in lab-notes (2026-03-18 §5) and the
k-dispersion deficit, but here the sub-family is small enough to attack
directly.

## The 3^k series

| n | 3 | 9 | 27 | 81 |
|---|---|---|---|---|
| hits | 3 | 4 | 6 | 3 |
| S | 0.1333 | 0.1060 | 0.1060 | 0.1060 |
| hits/S | 22.51 | 37.73 | **56.60** | 28.30 |

`n = 27` is the most extreme excess in the whole range: the joint-lowest sieve
density with the highest hit count, m = {4, 14, 20, 25, 32, 40}. S saturates at
0.1060 from n=9 onward — once 9|n the exponent already sits in the small
subgroup and further powers of 3 add no new obstruction, so the variation above
n=9 is pure global behaviour.

## What this costs in storage — and what it does not

Frontier needed to observe exponent n at q=3 (`3^n <= max_p`):

| n | 3^n | vs current frontier |
|---|---|---|
| 24 | 2.82e11 | reached |
| 25 | 8.47e11 | 1.4x |
| 26 | 2.54e12 | 4.1x |
| 27 | 7.63e12 | 12.3x |
| 30 | 2.06e14 | 333x |

`n = 25` is the only near-term target: 1.4x the frontier is ~1.3x the rows,
about +62 GB on top of the current ~206 GB, which fits the 224 GB free on the
external volume. `n = 27` in the warehouse would need ~2.2 TB and is out of
reach on this hardware — but the arithmetic above already answers what n=27
looks like, so generating it is a question about wanting the full graph at that
magnitude, not about the exponent tail.

## Open

- The 45% surplus wants a mechanism. Candidate: the same subgroup structure
  that makes moduli active on 3|n also correlates the surviving m-positions,
  so the independent-moduli assumption in S over-counts the loss.
- Whether the surplus persists at 3^k for k >= 5 (n=243 and up) — cheap to run,
  the window grows linearly.
