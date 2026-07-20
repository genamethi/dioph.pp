# Regenerated covering grading (order vs divisibility, k=0 vs k>0)

`cov_emit_main.cc` defines two covers over `x = p - 2^m`:

- `covering_primary` — **divisibility**: all primes `q <= cert_bound` (⌊√p_max⌋),
  residue 0. A k=0 prime is fully covered by certificate (every `x` composite ⇒
  smallest factor ≤ √x ≤ cert_bound). Near-tautological.
- `covering_bleed_min` — **order**: all primes `l` with `ord₂(l) <= order_cap`,
  residue 0. Minimizes bleed into k>0 at the cost of much lower k=0 coverage.

The original grader `tests/grade_covering.cc` is not in the repo (deleted
sessions). This is a Python regeneration of its measurement.

## Result, primes in [3, 2×10⁶] (k=0: 22,111; k>0: 126,821)

| cover | k=0 coverage | k>0 bleed | precision (cov/bleed) |
|---|---|---|---|
| primary (divisibility) | 100.00% | 0.233% | 428 |
| bleed_min (order)      | 36.21%  | 0.0497% | 729 |

Order bleeds 4.7× less; its k=0 coverage is 36% of divisibility's.

## Scale dependence (why the frontier numbers differ)

At the warehouse frontier (p ≤ 2×10⁹) the user measured order coverage ~11% and
bleed 2–3 orders of magnitude below divisibility. Direction confirmed here; the
gap widens with scale because the large-order frontier grows (covering-families:
~10.1% of positions have smallest-factor order > 32), so order coverage of k=0
falls (36% → ~11%) while divisibility bleed rises and order bleed stays ~0.

## What it says about the theta candidates

The order/congruence cover is the **Eisenstein-computable, low-bleed** part —
sharp where it fires, but accounting for a *shrinking minority* of k=0 as scale
grows. So in a theta representation the Eisenstein series captures only that
minority; the **cusp part must carry the growing majority** (frontier k=0 primes
with large-order obstructions). The cusp form is not a small correction here — it
dominates and grows with scale. Divisibility's 100%/high-bleed confirms it is
near-tautological and carries no arithmetic signal.
