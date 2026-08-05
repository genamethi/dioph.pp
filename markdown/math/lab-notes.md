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


2026-06-27  k-distribution: full census, distributional fit, magnitude-stationarity
-----------------------------------------------------------------------------------

DATA. Full census of `primeparts.primes` (native warehouse, ib-staging):
N = 21,698,850,257 primes, min p = 3, max p = 564,575,405,239 (~2^39.04).
This is the COMPLETE set up to the frontier, not a sample.

APPROACH. Computed natively through the catalog/query layer — no Python, no
bespoke binary. New reusable primitive `QueryService::GroupCount(table, column,
p_lo, p_hi, threads)` (sharded parallel scan over SourceTableReader), exposed as
Lua `query.hist{col=...}` and run via the `pp` Lua shell. Derived ("virtual")
group keys: `bits` = floor(log2 p) (= max_m), `r` = bits - k (per-prime count of
m where p-2^m is NOT a prime power = "misses"; r >= 0 since k <= max_m). Full
scan ~32 s (8 shards). Distribution fits done in Lua on the 17 aggregated counts.

1. THE k-DISTRIBUTION (the obstruction / connectivity spectrum)

   k :        count          pct
   0 : 3,874,747,523     17.857%   (matches the ~17.3-17.6% obstruction rate)
   1 : 6,176,825,098     28.466%   (mode)
   2 : 5,291,635,631     24.387%
   3 : 3,332,361,926     15.357%
   4 : 1,743,044,997      8.033%
   5 :   796,237,699      3.670%
   6 :   321,601,493      1.482%
   7 :   114,422,288      0.527%
   8 :    35,591,015      0.164%
   9 :     9,612,963      0.044%
   10:     2,236,822      0.010%
   11:       445,727      0.0021%
   12:        75,054      0.00035%
   13:        10,744 / 14: 1,156 / 15: 108 / 16: 13
   mean = 1.88216, var = 2.24859, dispersion var/mean = 1.1947 (OVERDISPERSED).
   Cross-checks: sum k*count = 40,840,689,931 ~= the ~40 B `partitions` rows
   (HANDOFF s4); k=0 count = 3.87 B = `primes_k0`.

2. DISTRIBUTIONAL FIT (G-test, lower = better; G huge for all at this N, so read
   relative G + closeness, not p-values)
   - Poisson(lambda=1):     G = 1.37e10  — REJECTED. Forces P(0)=P(1)=0.368, but
     P(0)=0.179 < P(1)=0.285; the mode at k=1 with a depressed zero forces
     lambda>1.
   - Poisson(lambda=1.882): G = 3.60e8   — nails the center but UNDER-disperses:
     under-predicts P(0) (0.152 vs 0.179) and the whole tail (k=6: 0.0094 vs
     0.0148).
   - Negative binomial (MoM r=9.667, p=0.163): G = 4.65e6  — ~80x better than
     Poisson, ~3000x better than Poisson(1). Tracks body AND tail (k=0:0.1791,
     k=2:0.2453, k=6:0.0142, k=8:0.00176). WINNER.
     NegBin = Gamma-mixture of Poissons => a Poisson whose rate varies across
     primes; mechanistically this is the covering-class heterogeneity. The
     diagnostic (k+1)p[k+1]/p[k] is NOT flat (would be for pure Poisson) but
     LINEAR-rising ~ 0.163*k + 1.58 (= NegBin's p*k + p*r) through k~7.
   - Zeta / Zipf: REJECTED two ways. (a) not monotone (pmf rises 0->1, interior
     mode); (b) the tail is super-exponential, not power-law: log-log slope
     d ln p / d ln k STEEPENS (-1.1, -2.3, -3.5, ... -34) instead of staying
     constant. The "zeta vibe" is only the peaked-then-decaying silhouette.
   - Far tail (k>=9) is slightly LIGHTER than even NegBin (the (k+1) ratio turns
     over after k~7), consistent with the hard k <= floor(log2 p) ceiling.

3. SAMPLING ERROR (sample-size only)
   - Bulk bins are sample-resolved: every k<=12 bin has >=75k events
     (rel err <=0.4%), k<=8 has millions (<=0.02%). Resolution floor: a
     probability p ~ 4.6e-9 is measurable @10% rel err (count 100), p ~ 4.6e-7
     @1% (count 1e4); a 1e-7 probability currently sits at ~2% err (~2170
     events).
   - The high-k tail (k>=13: 10,744 / 1,156 / 108 / 13) is sample-limited, and
     by the k <= floor(log2 p) ceiling only appears at much larger p.


2026-06-27  The r-distribution, and two bucketing schemes (bit-band vs m_k)
--------------------------------------------------------------------------

DATA. Full census of primeparts.primes: N = 21,698,850,257, max p ~ 2^39.04.
r := floor(log2 p) - k = number of MISSES per prime (m in [1, floor(log2 p)]
where p - 2^m is NOT a prime power). Stats: mean_r = 35.1115, var = 4.4787,
dispersion var/mean = 0.1276 (UNDER-dispersed -- the converse of k, which is
overdispersed at 1.19).

THE r-DISTRIBUTION IS TWO REGIMES.

  Right hump (r >~ 25): the prime BIT-LENGTH CENSUS, nothing more. Consecutive
  count ratio is flat at ~1.94, which is exactly 2*(1 - 1/m) at m ~ 37 -- the
  prime-band doubling (pi(2^{m+1}) - pi(2^m)) / (pi(2^m) - pi(2^{m-1})). The mode
  at r=36 and mean_r=35.11 are just mean_m - mean_k (definitional). The rolloff
  at r=37,38,39 is the data frontier (max p ~ 2^39.04), not structure. This side
  carries no information beyond pi(x); read it on a log axis or not at all.

    r :   count        r :   count          r :   count
    0 :       3        14:     8594         28:    91438791
    1 :       5        15:    16769         29:   176172128
    2 :       7        16:    32171         30:   334412765
    3 :       9        17:    61484         31:   618248874
    4 :      19        18:   119362         32:  1097910342
    5 :      22        19:   231476         33:  1840633318
    6 :      66        20:   448061         34:  2846827500
    7 :      81        21:   868015         35:  3928716708
    8 :     190        22:  1687652         36:  4568757238  (mode, 21.06%)
    9 :     371        23:  3273843         37:  4000818753
    10:     685        24:  6373726         38:  2001834319
    11:    1225        25: 12419385         39:    96209205
    12:    2323        26: 24212734
    13:    4553        27: 47107485

  Left tail (small r): MAXIMALLY-CONNECTED primes, structurally confined to
  small p. r = m - k with observed k <= 16, so r=0 forces k=m<=16, i.e. p < 2^17;
  small r lives only at the bottom of the range. The counts sit ABOVE the
  geometric census extrapolation (extrapolating the right hump predicts ~0.2 at
  r=0; observed 3) -- a genuine excess of connectivity at small p, where
  prime powers are dense. r=0 means H(p) = {1,...,m}: a prime that hits every
  available position.

WHY mean_k SATURATES (the magnitude-stationarity, derived). For one prime,
  E[k] = sum_{m'=1}^{m} P(p - 2^{m'} is a prime power) ~ sum 1/ln(p - 2^{m'})
       ~ m * 1/(m ln 2) = 1/ln 2 ~ 1.443  (+ proper prime powers -> ~1.88).
The m available positions and the ~1/m per-position density CANCEL, so the
expected hit count is band-invariant. Confirmed empirically: mean_k = 1.877 ->
1.882 across bands 2^30..2^39, essentially flat.

TWO BUCKETING SCHEMES (both useful; not yet ranked).

  (A) Bit-band bucketing -- bucket primes by m = floor(log2 p). Within a band,
      k ~ Binomial(m, p_m) with per-position success p_m ~ 1.88/m (falls as 1/m),
      which limits to Poisson(1.88). The GLOBAL k-distribution is the mixture over
      bands plus per-prime rate heterogeneity = a Gamma-mixed Poisson = the
      observed negative-binomial overdispersion. Banding is thus the decomposition
      that explains the overdispersion: it is a family of Poissons, not one.

  (B) m_k bucketing (on the partitions table) -- for each representation exponent
      m_k in [1, floor(log2 max_p)], bucket partitions by m_k. Within a bucket,
      q^n = p - 2^{m_k} = 2^{m_k}(2^{j} - 1), where j := log2(p) - m_k indexes the
      size of q^n relative to its 2^{m_k} anchor on a log2 scale. For the top
      position (m_k = floor(log2 p)) j is the mantissa in [0,1); for lower
      positions j > 1. So j is the log-coordinate of q^n, and binning by j with a
      prime-power-density weight 1/ln(q^n) gives a principled, non-uniform
      per-position hit probability (replacing the uniform 1.88/m). Directly
      checkable against the stored (q, n).

HOW THIS FEEDS THE ANALYSIS. The covering / Mersenne structure lives not in the
scalar r but in the per-prime INDEX-DIFFERENCE structure. For a prime with two
representations (m1 > m2):
    2^{m1} + q1^{n1} = 2^{m2} + q2^{n2}
    => 2^{m2} * M_d = q2^{n2} - q1^{n1},   d = m1 - m2,  M_d = 2^d - 1.
Every primitive factor ell of M_d (those with ord_ell(2) = d) is odd and divides
the right side, giving q2^{n2} ≡ q1^{n1} (mod ell). This is the S-unit relation,
per-prime, keyed on the index difference d. The graduated programme: k=2 gives one
such constraint, k=3 gives three pairwise constraints that must be jointly
consistent, and so on. The j-binning of (B) supplies the null/base measure to test
whether the observed d's are biased toward small ord_ell(2).


2026-06-27  mdiff_k2/k3 built (full census): index differences are parity-locked
--------------------------------------------------------------------------------

Built primeparts.mdiff_k{2,3} over the full census (5,291,635,631 and
3,332,361,926 rows = exact k_freq counts) + primeparts.mersenne_factors (full
factorization of 2^d-1, d<=40). Per-k fixed-width: m_1..m_K sorted, d_i the
canonical pairwise differences. Histogramming d confirms, at full scale, the
parity result already seen in the covering-system triage:

  ALL index differences are EVEN. Odd d has single-digit counts across billions
  of primes (mdiff_k2.d_1: d=2 -> 2.4e8, d=4 -> 6.5e8, ..., d=12 -> 8.2e8 (mode,
  15.5%); but d=3 -> 7, d=5 -> 8, d=7 -> 4, d=9 -> 5). Same for all three k=3
  differences. Equivalently: within one prime every hit position m shares a
  common parity, fixed by p mod 3 -- ord_2(3)=2, so 3 | p-2^m on one parity
  class of m, killing it unless p-2^m is a literal power of 3. The handful of
  odd-d rows are exactly those power-of-3 exceptions. This holds across the
  whole range (not a small-p artifact); it is the mod-3 backbone imposing the
  sieve, the same {3,...} backbone the triage uses.

  The even-d mass tracks M_d small-factor richness: the mode d=12 is
  M_12 = 2^12-1 = 3^2.5.7.13 (the small primes with ord_2 in {2,3,4,12}, all
  dividing it); secondary peaks d=4 (3.5), 8 (3.5.17), 24. So d weights are
  biased toward small ord_ell(2), as conjectured -- the S-unit/covering signal.

CONSEQUENCE FOR REPRESENTATION. Single-parity hit sets => store the hit set as
one int64 bitmask (bit m set; m_max ~ 39 < 64, so this is k-agnostic, not just
low k), drop the d-vector entirely (a deterministic bit-op on the mask) and
prime_rank (= pi(p)). The mask factors as (anchor m_min, translation-invariant
even-gap shape); the shape is low-cardinality (the d-distribution above), so
dictionary+RLE on shape and delta on the anchor compress hard, and group-by-shape
is itself the covering-pattern census.

  IMPLEMENTED. mdiff_k{K} now stores exactly (p, hit_mask int64); the m-vector
  and d-multiset are decoded on demand (HitMaskDiffs). The Mersenne-factor cache
  moved to its own builder (primeparts-mersenne -> mersenne_factors with ord2 +
  is_primitive, is_primitive = ord2==d). Replace is catalog-pure: a drop-purge
  through the catalog seam (no warehouse fs writes from the analysis tools).



2026-08-05  The k distribution: scale, shape, and a per-prime reading of Lambda_p
---------------------------------------------------------------------------------

Measured on a 1e9-prime warehouse (max p = 22,801,763,513; 1.884e9 partition
rows). The mechanism below is the Lambda_p lattice of research_programme.md
section 2 -- this entry is the empirical confirmation and the constants, not a
new mechanism.

1. WHY THE SCALE IS CONSTANT

   k(p) = #{m : p - 2^m prime}. Candidates grow like log_2(p); each is prime
   with probability ~ 2*C_2/ln(p) (singular series has no odd-prime correction
   because the gap 2^m is a pure power of two). The two cancel:

       lambda = log_2(p) * 2*C_2/ln(p) = 2*C_2/ln 2 = 1.90482
       observed mean k                                = 1.88402

   Confirmed independent of p: mean k is flat across 13 doublings,
   1.8713 at lg2(p)=21 to 1.8792 at lg2(p)=33 (0.4% drift).

2. IT IS NOT POISSON -- IT IS A MIXTURE

   variance 2.19294 vs mean 1.88402 (index of dispersion 1.164).
   obs/Poisson(mean) is U-shaped: 1.144 at k=0, 0.918 at k=3, then rising
   monotonically to 3.22 at k=11. Both tails fat.

3. WHAT IT MIXES OVER

   p - 2^m == 0 mod l has a solution in m iff p mod l lies in <2> mod l. When
   <2> is a proper subgroup, primes split into two classes with different
   lambda. At l=7, ord_7(2)=3 and <2>={1,2,4} has index 2:

       p mod 7 in {1,2,4}   mean k = 1.50720
       p mod 7 in {3,5,6}   mean k = 2.26083
       ratio 0.666658       predicted 1 - 1/ord_7(2) = 0.666667
       weighted mean 1.88401 (observed 1.88402)

   The three non-residue classes agree to four decimals. The ratio is the
   predicted 2/3 to five decimals.

4. THE SAME THING AT A SINGLE PRIME

   Every obstruction at l removes an arithmetic progression in m of common
   difference ord_l(2). p = 12699571 (k=11) and p = 12699097 (k=2) are both
   1 mod 3, so l=3 (ord 2) removes all 11 even m from both -- half the pool
   before anything else. The difference is entirely in the odd m:

       12699571  k=11  surviving m = 1,3,5,7,11,13,15,17,19,21,23
                       only further loss: m=9 (smallest factor 23)
       12699097  k=2   l=13 removes m=3,15;  l=29 removes m=19
       12699139  k=2   l=11 (ord 10) removes m=1,11,21;  l=13 removes m=7,19

   So k is not a property of p in isolation: it is what survives the union of
   APs, and the cost of each obstruction is ceil(range/ord_l(2)). Coarse
   obstructions (small ord) are expensive; l=3 alone halves every prime.

5. THE k-MANY HERMITE REPRESENTATIONS OF ONE p

   For n=1 every generator is g_m = 1*He1 + 2^m*He0. The He1 coefficient is
   invariably 1, so a prime's whole representation lives in He0:

       p = 12699571, k=11: He0 coefficients {2^m : m in 1,3,5,...,23}
       p = 12699097, k=2 : {2^7, 2^11}
       p = 12699139, k=2 : {2^5, 2^15}

   High-k signatures are near-complete arithmetic progressions in the exponent;
   low-k signatures are the sparse residue left after several APs are removed.
   The comparison between k values is therefore a statement about AP coverage,
   not about magnitude.

   [RETRACTED 2026-08-05, see the signature-structure entry below. The second
   sentence above was written from three hand-picked primes and never tested.
   Measured: the AP is a property of the ELIGIBLE set, which is forced by l=3
   and is the same for every prime in a residue class; the surviving signature
   inside it is statistically indistinguishable from a random subset. "High-k
   is a near-complete AP" is true only in the trivial sense that high-k fills
   its eligible set. Provenance note: this claim was mine, not an input.]

RESOLVED at 11e9 (below). The residual spread is not "other moduli": it is the
truncation of the removed arithmetic progression against the finite m-range.


2026-08-05  The l=7 low-class residual is an m-range truncation effect
------------------------------------------------------------------------

Measured on the 11e9-prime warehouse (max p = 278,401,257,863; 20.69e9
partition rows). Single pass over primes.p,k grouped by (p mod 7, floor(log2 p)).

    N = 11e9    mean k = 1.881245    var = 2.234091    dispersion = 1.1876

    r7   n              mean k     sd        SE
     1   1,833,329,013  1.523355   1.26761   3.0e-05   in <2>
     2   1,833,337,211  1.478129   1.24246   2.9e-05   in <2>
     4   1,833,327,070  1.513487   1.26314   3.0e-05   in <2>
     3   1,833,339,917  2.257478   1.61328   3.8e-05
     5   1,833,333,859  2.257510   1.61326   3.8e-05
     6   1,833,332,929  2.257509   1.61327   3.8e-05

    low/high ratio 0.666663 vs 2/3 = 0.666667  (diff -4.0e-06)

1. THE HIGH CLASSES HAVE NO RESIDUAL

   {3,5,6} agree to 2.2575 across all three, within +-0.6 SE of their common
   mean. They are outside <2> mod 7, so no AP is removed and there is nothing
   to truncate. The residual lives only where an obstruction exists. This alone
   rules out an explanation from the other moduli, which act on both halves.

2. FIRST ORDER: HOW MANY m THE AP TAKES OUT OF [1,M]

   p == 2^j mod 7 kills exactly m == j mod 3. With M = floor(log2 p) the
   candidate range is m in [1,M], so the kill count is

       r7=1 (j=0):  |{m<=M : m=0 mod 3}| = floor(M/3)
       r7=2 (j=1):  ceil(M/3)
       r7=4 (j=2):  ceil((M-1)/3)

   These differ by at most 1 and the pattern has period 3 in M. Equal-value
   model E[k|low] = E[k|high] * (M - count)/M, checked per octave M=28..37:

       max |obs - pred| = 0.00468,  typical 0.002    (means are ~1.5)

   The per-octave ordering of the three low classes matches the predicted kill
   counts in every octave from M=17 to M=37, no exceptions:

       M = 0 mod 3   counts tie 12,12,12    means tie to ~0.003
       M = 1 mod 3   counts 12,13,12        r7=2 alone low
       M = 2 mod 3   counts 11,12,12        r7=1 alone high

   The aggregate spread 1.5234 / 1.4781 / 1.5135 is just the octave mixture of
   these three regimes, weighted by the prime counts (M=37 alone is 880e6 per
   class, half the data).

3. SECOND ORDER: WHICH m, NOT HOW MANY

   The ~0.002 residual left by the equal-value model is monotone decreasing in
   the LARGEST killed index, in all ten octaves M=28..37 with no exceptions:

       M=30: top killed 28 -> +0.00230,  29 -> +0.00177,  30 -> -0.00285
       M=37: top killed 35 -> +0.00126,  36 -> +0.00058,  37 -> -0.00190

   Index m is not worth 1/M of the total. p - 2^m has ln(p - 2^m) < ln p, and
   for m = M the cofactor p - 2^M is uniform-ish in (0, 2^M), so the top index
   carries measurably more prime density than a generic one. The class whose AP
   reaches m = M always pays the extra, and it is always the one with the
   negative residual.

   So the low-class ordering is fully accounted for: a period-3 count effect
   from truncating the AP, plus a rank effect from the top index being worth
   more. Nothing here needs a second modulus.

4. NOTE ON THE MOMENTS

   mean k drifts 1.88402 (1e9, max p 2.28e10) -> 1.881245 (11e9, max p 2.78e11),
   variance 2.19294 -> 2.23409, dispersion 1.164 -> 1.188. The variance grows
   because the sample spans more octaves and each octave contributes its own
   class-conditional means; the mixture widens even though lambda is flat.


2026-08-05  Signature structure: the He0 coefficient set of a prime
--------------------------------------------------------------------

Measured on the 11e9-prime warehouse (p <= 278,401,257,863). For n=1 the
generator is g_m = 1*He1 + 2^m*He0 with the He1 coefficient invariably 1, so the
signature of p is the set S(p) = {2^m : p - 2^m prime}. This entry tests what
shape S(p) actually has. It corrects the untested claim in section 5 of the
k-distribution entry above.

1. S(p) LIES IN A GEOMETRIC PROGRESSION OF RATIO 4 -- EXACTLY

   All 20,693,695,583 partition rows, by (p mod 3, m parity):

       p=1 mod 3   m odd    10,535,942,413   50.914%
       p=1 mod 3   m even               91
       p=2 mod 3   m even   10,157,753,007   49.086%
       p=2 mod 3   m odd                72

   163 exceptions in 20.7e9 rows, all of them q=3: 3 | p - 2^m forces q^n = 3^n,
   so p = 3^n + 2^m is the only escape. Otherwise

       p = 1 mod 3   He0 in {2, 8, 32, 128, ...}
       p = 2 mod 3   He0 in {4, 16, 64, 256, ...}

   The ratio 4 = 2^ord_3(2). Each further obstruction l removes a coset of a GP
   of ratio 2^ord_l(2). This is the multiplicative reading of the Lambda_p AP.

2. OCCUPANCY INSIDE THE GP IS FLAT, WITH A TOP-INDEX PREMIUM

   Octave M=37, p=1 mod 3, 2.64e9 primes, P(m in S) per index:

       m <= 31    0.101448  (flat to 5 decimals)
       m = 33     0.101611   +0.2%
       m = 35     0.102198   +0.7%
       m = 37     0.107363   +5.8%

   Against 2*C_2/ln(p - 2^m) the ratio is 2.0008 at every m. That 2 is the
   (l-1)/(l-2) singular-series factor at l=3 induced by conditioning on the
   residue class -- a direct measurement of it. The m=37 premium is the same
   effect that drives the residual monotonicity in the l=7 entry above, here
   measured rather than inferred.

3. THE SURVIVING SET IS NOT AN AP (RETRACTION)

   Fraction of signatures forming an exact AP, versus a uniform random k-subset
   of the eligible set. 2.26e6 primes in a window at 2^37:

       k=3   0.08829  null 0.085843   ratio 1.03
       k=5   0.00397  null 0.003380   ratio 1.17
       k=7   0.00073  null 0.000477   ratio 1.53
       k>=8  0        null ~2e-4      ratio 0

   Null. The AP structure is entirely in the eligible set; what survives inside
   it behaves like a random subset. So "high-k = near-complete AP" says only
   "high-k fills its eligible set", which is a restatement of k.

4. THE TAIL IS THE REAL STRUCTURE

   Observed / Binomial(19, 0.1015) at M=37:

       k=0    1.31       k=10      33.2
       k=2    0.85       k=13     424
       k=7    3.94       k=16   17252

   Only 7 primes reach k=16 in 11e9; independence predicts 2e-4 of them.

5. MECHANISM: EVEN-ORDER OBSTRUCTIONS ARE FREE HALF THE TIME

   Obstruction load from 5 <= l <= 43, by k:

       k     count   eligible   killed   survivors
       1    761712    18.483    12.944      5.539
       4    228987    18.535     8.448     10.087
       8      5297    18.618     4.022     14.596
      12         7    18.714     1.429     17.286

   The seven k=16 primes:

       128583586759  elig 19  killed 0        184518828679  elig 19  killed 0
       150645643069  elig 19  killed 0        217034567671  elig 19  killed 2
       171915905929  elig 19  killed 0        275472876047  elig 18  killed 1
       175843304423  elig 18  killed 0

   Five have zero obstructions from every odd l up to 43. The reason is a parity
   coupling: l=5 has ord_5(2)=4 and <2> = all of (Z/5)*, so l=5 obstructs EVERY
   p -- but it kills m = j mod 4, and when j has the parity l=3 already killed,
   it costs nothing. Same free branch at l=17 (ord 8), 13 (ord 12), 43 (ord 14),
   41 (ord 20).

       l=3 picks the parity. Every later l with even ord_l(2) is free with
       probability ~1/2. A k=16 prime took the free branch at all of them.

   Concrete signatures:

       p=128583586759  k=16  holes {19,27}
         S = 1,3,5,7,9,11,13,15,17,21,23,25,29,31,33,35
       p=175843304423  k=16  holes {4,18}          (p=2 mod 3, even GP)
         S = 2,6,8,10,12,14,16,20,22,24,26,28,30,32,34,36

       50 apart in value:
       p=128583586903  k=0   S = {}
       p=128583586877  k=1   S = {30}
       p=128583586853  k=2   S = {4,32}
       p=128583586907  k=4   S = {2,10,18,26}

6. n >= 2 IS NEGLIGIBLE

   73,964 rows of 20.69e9 = 0.00036%, max n = 23. The one-parameter family
   g_m = He1 + 2^m*He0 is essentially the whole object at this scale.
