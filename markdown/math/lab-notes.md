Lab Notes: Prime Power Partition Analysis (funbuns)
====================================================

This is the permanent research log for the funbuns project.
Append new findings at the end with dates.


2026-03-18  Local-Global Periodicity: q-chains and fixed-modulus obstructions
-----------------------------------------------------------------------------


1. BIPARTITE (n, m) ADJACENCY FOR q=3

   The q=3 chain has 21 distinct n-values and 33 distinct m-values forming
   a bipartite graph.  All nodes belong to a single connected component --
   there are no isolated clusters.  This means every solution is reachable
   from every other by alternating n-steps and m-steps through shared edges.

   Degree distributions: n-degrees (number of m-values per n) vary from ~3
   to ~15.  m-degrees (number of n-values per m) vary from ~2 to ~12.
   Higher-degree nodes tend to have smaller m or n values, reflecting the
   higher prime density for smaller values of 2^m + 3^n.

2. M-PAIR ARITHMETIC PROGRESSIONS

   Among (m1, m2) pairs that share n-values:
   - d=3 gives the longest AP: m = 1, 4, 7, 10, 13, 16, 19, 22 (length 8).
   - d=12 APs (e.g., m = 4, 16, 28 sharing n = {1, 4, 12}) have the highest
     density of shared n-values per pair.
   - Other frequent deltas: d=1, d=2, d=6.

3. OBSTRUCTION CROSS-LINES

   When 2^m + 3^n is composite (a "gap" in the q=3 chain), the composite
   value's smallest prime factor follows rigid patterns:

   - q'=5 causes 31.7% of gaps, with n-hits distributed by n mod 4
     (period = ord_5(3) = 4).
   - q'=7 causes 16.8% of gaps, with period-6 structure
     (period = ord_7(3) = 6).
   - q'=13: the m-values hit are spaced by d=12 = ord_13(2).

4. CONNECTION TO fixed_mod.py: LOCAL OBSTRUCTIONS EXPLAIN GAP PERIODICITY

   The d=12 AP pattern in m-pair sharing and the d=12 spacing in q'=13
   obstructions are the SAME PHENOMENON.  The multiplicative order ord_{q'}(2)
   governs the periodicity of 2^m mod q', which determines when two m-values
   produce the same residue class and thus share the same local solvability.

   More precisely: for each small prime ell (not 2, not q), the values
   2^m + q^n mod ell depend only on (m mod ord_ell(2), n mod ord_ell(q)).
   This creates an obstruction grid of size ord_ell(2) x ord_ell(q).
   Cells where the sum is 0 mod ell are "killed" -- no prime > ell can
   arise from those (m, n) mod-classes.

   The gap periodicity seen in obstruction cross-lines is exactly this
   grid structure projected onto the m-axis (or n-axis).

5. OPEN QUESTION: LOCAL-GLOBAL DISCREPANCY

   The sieve density -- the product of (1 - killed/total) across small
   primes ell -- predicts the fraction of (m, n) pairs surviving all local
   obstructions.  The empirical density is the fraction that actually yield
   primes.

   If empirical/sieve converges to a stable ratio as ell_max grows, the
   gaps are "explained" by local conditions (up to PNT scaling).  If there
   is a systematic discrepancy or the ratio oscillates, that signals global
   structure not captured by any finite set of moduli.

   Implemented in data_exploration.py: obstruction_grid_report() and
   local_global_comparison().  Run via: funbuns --local-global --explore-q 3


2026-03-19  Additional Observations
------------------------------------

6. q=7 MOD-3 OBSTRUCTION (corrected)

   q=7 only appears at even m.  NOT a parity obstruction (2^m + 7^n
   is always odd).  The actual obstruction is mod 3: since 7 = 1 (mod 3),
   7^n = 1 (mod 3) for all n.  2^m mod 3 cycles {2, 1, 2, 1, ...}.
   For odd m: 2^m + 7^n = 2 + 1 = 0 (mod 3), always divisible by 3.

7. q=5 NEVER CHAINS

   q=5 has 0% consecutive hits (cons% = 0) across all m-values.
   Worth investigating which modulus forbids delta_n = 1 for base 5.


2026-03-29  l-adic analysis: from valuations to power residue symbols
----------------------------------------------------------------------

Profiling revealed --ladic was just delegating to --remainder, which runs
full_profile (GMP factorization + Miller-Rabin + Pollard's rho) on every
remainder r = p - 2^m.  86% of runtime was GMP primality testing -- entirely
irrelevant to l-adic structure.  Separated the code paths: --ladic now uses
Rust v_ell (trial division only), --remainder keeps full_profile.

But the deeper problem: computing v_l(r) is only the first digit of the
l-adic story.  The actual question for obstruction theory is:

  "Is r an n-th power in Z_l?"

This decomposes into two parts:
  (1) v_l(r) ≡ 0 mod n  (valuation divisible by n)
  (2) The unit part r/l^v_l(r) is an n-th power in Z_l*

Part (2) depends on r mod l and the group structure of (Z/lZ)*.
Specifically, u ∈ Z_l* is an n-th power iff u^((l-1)/gcd(n,l-1)) ≡ 1 mod l.
The current code computes (1) and completely misses (2).

This also resolves the "why these primes?" question.  The filtration primes
should NOT be a fixed list {2,3,5,7,11,13}.  They should be EXPONENT-
DEPENDENT: for exponent n, the informative primes are those l where
gcd(n, l-1) > 1, i.e., where the n-th power residue symbol is nontrivial:
  n=2 (square): every odd l works
  n=3 (cube):   need l ≡ 1 mod 3, so l = 7, 13, 19, 31, 37, 43, ...
  n=5 (5th pw): need l ≡ 1 mod 5, so l = 11, 31, 41, 61, 71, ...

The prime l=3 can NEVER detect a cube obstruction because |(Z/3Z)*| = 2.

Reference: Burhanuddin thesis (2007), "Some Computational Problems Motivated
by the Birch and Swinnerton-Dyer Conjecture."  Section 2.2-2.3: the l-adic
algorithm for computing elliptic curve rational torsion.  Key ideas:
  - Choice of prime l is determined by discriminant (good reduction)
  - Hensel lifting provides structured refinement (quadratic convergence)
  - Discriminant formula: v_l(Δ(f_m)) = (m²-3)(m²-1)/24 · v_l(Δ)
Not directly applicable (we don't have an elliptic curve) but the methodology
-- using SageMath's p-adic rings (Zp, Qp) with Hensel lifting for root-
finding rather than naive trial division -- is what the l-adic analysis
should be built on.

SageMath p-adic machinery available: Zp(l), Qp(l), capped relative/absolute
precision, Eisenstein and unramified extensions, Frobenius endomorphisms,
Witt vectors.  The Rust plugin's role becomes preparing (p, m, r, n) tuples
efficiently; algebraic analysis happens in SageMath over p-adic rings.

Next steps:
  - Prototype: for a sample of obstructed primes, compute the n-th power
    residue symbol at exponent-appropriate primes using SageMath Zp
  - Compare local obstruction rates vs naive v_l approach
  - Determine if any obstructed primes are locally unobstructed at all l
    (these would be the "interesting" primes from a local-global perspective)


2026-03-30  Covering-system characterization of obstructed primes
-----------------------------------------------------------------

Key insight: the obstruction mechanism is a covering system in the sense
of Erdős.  For p = 2^m + q^n, if p is obstructed (k=0), then for every
m in [1, floor(log2(p))], the remainder r = p - 2^m has at least two
distinct prime factors (it's never a prime power).

The propagation rule is:
  If q | p - 2^k  and  q | 2^m - 2^k,  then  q | p - 2^m.

Since 2^m - 2^k = 2^k(2^{m-k} - 1), the odd part is always a Mersenne
number M_d = 2^d - 1 where d = m - k.  Prime q divides M_d iff
ord(2, q) | d.  So if q | p - 2^k, then q also divides p - 2^{k+j·ord(2,q)}
for all j -- an arithmetic progression of "poisoned" m-values.

An obstructed prime is one where these progressions COVER all valid m:

  [1, floor(log2(p))] ⊆ ⋃_i { k_i + j · ord(2, q_i) : j ≥ 0 }

where each q_i | p - 2^{k_i}.

EMPIRICAL RESULTS (499 obstructed primes, p up to ~30K):
  - 499/499 (100%) fully explained by covering systems of small primes
  - Covering set size distribution: mode = 5, range = [3, 8]
  - The backbone {3, 5, 7} with orders {2, 4, 3} appears in nearly all covers:
      q=3: 100%, q=5: 84%, q=7: 65%
  - LCM(2, 3, 4) = 12, so the backbone repeats with period 12
  - For small primes, {3, 5, 7} + a few helpers (11, 13, 17, ...) suffice
  - Residue classes mod 105 = 3·5·7 determine which backbone classes apply

STRUCTURAL IMPLICATIONS:
  1. The "dual" of the q_k recurrence chains: those describe which q^n
     appear as solutions; covering systems describe why NO q^n can appear.
  2. The obstruction rate (~17.3%) should be computable as: the density
     of primes p such that the covering conditions are satisfiable,
     i.e., p mod q ≡ 2^{k_i} mod q for enough (q_i, k_i) pairs.
  3. Scalability: as p grows, max_m ~ log2(p), and the covering only
     needs log2(p) positions covered.  Since ord(2,3) = 2 already covers
     half, this grows very slowly in the number of primes needed.

CONNECTION TO PRIOR WORK:
  - The d=12 periodicity noted 2026-03-18 (ord_13(2) = 12 in q'=13
    gap structure) is a special case of this covering mechanism.
  - Fixed-mod analysis (multiplicative orders governing gap periodicity)
    was seeing the same phenomenon from the solution side.

FILES: scripts/build_pow2_diffs.py, scripts/covering_analysis.py
DATA:  data/power_of_two_diffs.parquet (741 rows, regenerable)


2026-03-30  Vectorized mod-105 / mod-255255 obstruction classifier
-------------------------------------------------------------------

Scaled the covering-system analysis to 6.4 billion primes via DuckDB.

FIRST PASS: MOD 105 = 3·5·7 (backbone only)
  Coverage of Z/12Z falls into exactly 4 tiers, 12 residue classes each:
    2 gaps/12 → 34.3% obstruction rate
    3 gaps/12 → 20.3%
    4 gaps/12 → 11.8%
    6 gaps/12 →  4.2%  (q=7 inactive: p mod 7 ∈ {3,5,6})
  Weighted average = 17.6%, matching actual 17.64% exactly.
  The backbone gap count is the primary predictor of obstruction.

SECOND PASS: MOD 255255 = 3·5·7·11·13·17 (backbone + helpers)
  GROUP BY (p%3, p%5, p%7, p%11, p%13, p%17) → 92,160 residue groups.
  Coverage structure of each prime in Z/12Z:
    q=3  (ord 2): one parity — 6/12 positions
    q=5  (ord 4): one mod-4 class — 3/12 positions
    q=7  (ord 3): one mod-3 class — 4/12 positions (or 0 if inactive)
    q=11 (ord 10, gcd(10,12)=2): one parity — 6/12 (same structure as q=3)
    q=13 (ord 12, gcd(12,12)=12): exactly ONE position mod 12
    q=17 (ord 8, gcd(8,12)=4): one mod-4 class — 3/12 positions

  Residual gap distribution after all 6 primes:
    0 gaps: 3.40B primes, 21.96% obstructed
    1 gap:   150M primes, 42.48% obstructed  ← highest rate!
    2 gaps:  976M primes, 20.37%
    6 gaps:  300M primes,  1.29%             ← lowest rate

  KEY: 1-gap class has the HIGHEST obstruction rate (42.5%). When only
  one m escapes the covering, only one shot at a prime power remainder.

100% OBSTRUCTION GROUPS:
  22 residue groups where EVERY prime (1.53M total) is obstructed.
  - All 22 have 0 residual gaps (fully covered)
  - All 22 have q=3 and q=11 covering opposite parities (so {3,11}
    alone cover all of Z/12Z)
  - Zero near-100% groups (99-100%): the jump from ~97% to 100% is sharp
  - This means: for these 22 residue classes mod 255255, the covering
    ensures that p - 2^m always has ≥2 distinct prime factors for every m.
    This is a purely algebraic characterization of unconditional obstruction.

RATE DISTRIBUTION (92K groups):
  - Continuous spectrum from ~1% to ~90%
  - Gap at 90-99% (only 2 groups)
  - Then sharp jump to exactly 100% (22 groups)
  - The bimodality suggests two regimes: probabilistic (1-90%) and
    algebraically forced (100%)

CACHED DATA: /media/extssd/research/dioph.pp/data/covering/
  mod255255_aggregate.parquet (92K rows, 1.5 MB)
  coverage_q{3,5,7,11,13,17}.parquet (lookup tables)
  power_of_two_diffs.parquet (741 rows)

FILES: scripts/mod105_classifier.py, scripts/mod105_second_pass.py


2026-03-30  Subgroup exclusion proof for 22 unconditional obstruction classes
------------------------------------------------------------------------------

THEOREM PROVED: For the 22 residue classes r mod 255255 where the covering
system {3,5,7,11,13,17} fully covers Z/12Z, every prime p ≡ r (mod 255255)
is obstructed (k = 0).  This was verified computationally by a finite check
(no heuristics, no sampling — purely algebraic).

THE PROOF HAS THREE LEMMAS:

Lemma 1 (Multi-coverage): At positions m mod 12 covered by ≥2 covering primes,
  p - 2^m has ≥2 distinct prime factors, so it's not a prime power.  Trivial.

Lemma 2 (Covering completeness): For each of the 22 classes, the covering
  system covers all of Z/12Z.  The 8 multi-coverage positions are handled by
  Lemma 1.  The remaining 4 single-coverage positions (per class) are all
  covered by q=3 alone (q=11 positions are always multi-covered).

Lemma 3 (Subgroup exclusion): At each single-coverage position m ≡ pos (mod 12)
  with covering prime q=3, for EVERY m in this congruence class, there exists
  a blocking prime ℓ such that:
    (p - 2^m) mod ℓ  ∉  ⟨3⟩ ⊂ (Z/ℓZ)*
  Since 3^n mod ℓ ∈ ⟨3⟩ for all n, p - 2^m ≠ 3^n.

MECHANISM: 3 is not a primitive root mod certain primes, so ⟨3⟩ is a proper
subgroup of (Z/ℓZ)*.  The forced residue (p - 2^m) mod ℓ, determined by the
congruence class r mod 255255, falls outside this subgroup — a contradiction
if we assume p - 2^m = 3^n.

BLOCKING PRIMES NEEDED: {13, 37, 41, 61, 67, 73, 181, 193}
  - ℓ=13 handles most cases: ord(3,13) = 3, index 4 (excludes 75% of residues)
  - ℓ=41: ord(3,41) = 8, index 5 (excludes 80%)
  - ℓ=61: ord(3,61) = 10, index 6 (excludes 83%)
  - ℓ=73: ord(3,73) = 12, index 6 (excludes 83%)
  - ℓ=193: ord(3,193) = 16, index 12 (excludes 92%)
  - Maximum blocking prime: 193

HARDEST CASE: Class (1,3,2,7,5,15) at m≡2 (mod 12) required 4 blocking
primes {193, 181, 67, 41} with a combined period of 1320 to close.
Most positions are blocked by ℓ=13 alone (period 1 — all m blocked at once).

STRUCTURAL OBSERVATION: The single-coverage prime is ALWAYS q=3 (never q=11).
All q=11 positions have at least one other covering prime (multi-covered).
This means the proof reduces entirely to showing 3^n can't match at 4
positions per class, using the subgroup structure of ⟨3⟩ in (Z/ℓZ)*.

VERIFICATION STATISTICS:
  22 classes × 4 single positions = 88 position-class pairs
  68/88 blocked by ℓ=13 alone (period 1)
  20/88 need additional primes (periods up to 1320)
  Total finite checks: bounded, all pass

FILES: scripts/subgroup_exclusion.py, scripts/proof_structure.py

