# Chain-reduction: what the exploration established

Reference implementation: `scripts/chain_dag.py` (colab branch), a Sage
prototype validated to B=10^6. Recorded here so the findings survive
compaction.

## The object

Each partition `p = 2^m + q^n` (a row of the `partitions` table) is an edge
`q --(m,n)--> p`. A chain root→…→p composes the edge maps into a polynomial in
the root, expanded over probabilists' Hermite `He_n`. Runs of `n=1` edges are
pure translations `t -> t + 2^m`, so a chain reduces to a **canonical word**
`(A0; (n1,C1); …; (nr,Cr))`, where `A0, Ci` are sums of powers of two.

## Findings (measured)

- **Reduction is lossless and large.** At B=10^6: 8.42e12 maximal chains reduce
  to 1.18M distinct words; the whole structure is ~78 MB parquet. Byte-identical
  to brute enumeration at 10^3/10^4.
  - Scale curve `(B: words, raw chains)`: `1e3: 197, 12234` · `1e4: 4709, 2.29e6`
    · `1e5: 74713, 1.67e9` · `1e6: 1.18e6, 8.42e12`.
- **Bound-invariance → resumable.** `paths_down(v)` depends only on ancestors
  (all `< v`); proven byte-identical at B=1e4 vs 1e5 on boundary nodes. Frontier
  passes reuse all prior work; only the new slice is computed.
- **Coset / lattice structure.** Each word splits into `skeleton` (exponents
  `n_i`) and `translations` (`A0, Ci` = sums of 2^m). Fix a skeleton and the
  translations lie on a slice pinned by "evaluate to target at the root." E.g.
  node 137, skeleton `[2]`: translations `(0,128),(2,112),(8,16)`, each solving
  `(3+A0)^2 + C1 = 137`.
- **Degree ceiling** `max_degree = floor(log_3 B)`, exact: 6,8,10,12 at
  B=1e3…1e6 (since `p = F(root) >= 3^deg`).
- **137 paradigm reproduced**: 37 chains → 6 canonical classes; `He_1+134`
  (mult 16), `He_4+10·He_2+27` (mult 1, the degree-4 route = `x^4+4x^2+20`).

## Profiling truth (corrects an earlier misattribution)

- Primality (`is_prime_power`) was 0.52s of 130s at 10^6. This is **not** the cost, and
  moot anyway since **edges are read from `partitions`, never recomputed**.
- The real cost is the **reduction DP word-merge** (110s in Python) + Hermite
  expansion (18.7s). Native (integer-encoded words, real hash maps) is the lever.
- **Memory-bound, not CPU-bound** at the parallelism level. Per-sink private
  memos (12 workers) blew past 10 GB; a **single shared memo** does the same work
  in 9.1 GB (fits 32 GB). `paths_down(v)` is a pure function of `v` and should be
  computed once, not per worker.

## Termination model (for the native passes)

Graph skeleton = the `q_k -> p` pivot of `partitions` (edges) + the
`primes ⟝ partitions` anti-join (= the k=0 primes, the roots). A chain
terminates at a node iff it is a **k=0 prime** or sits **below the previous
frontier** (a boundary node linking into an already-computed chain).

Related: [[away_session_recovery]], [[hermite_congruences]].
