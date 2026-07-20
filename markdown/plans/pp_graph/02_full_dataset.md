# 02 — full-dataset run

The census and composite-degree measurement over the whole warehouse,
p ≤ 618,760,338,619 (~44.66B rows, 170 files; the n_k ≥ 2 filter prunes no
files, so this is a full read and the first real test of the sharded path).

- [ ] Run the power-edge census at the full bound through the client module
      with sharded readers: edge count, grading, max_n vs ⌊log₃ max_p⌋ = 24,
      identity check, runtime and peak RSS recorded (32GB ceiling; batches
      stream, only power edges are retained).
- [ ] Composite-degree spectrum at the full bound: sources confined to
      q ≤ √max_p ≈ 786,650, reach universe generated arithmetically (flint),
      direct concatenations, realized products, witness words with Hermite
      expansions via GiNaC.
- [ ] Subspace-by-bound table extended to the full range (1e3 … max_p),
      cross-checked against `.ephemeral/subspace_by_bound.csv` where ranges
      overlap; measured rank vs ⌊log₃B⌋ + 1 at each bound.
- [ ] Outputs to `.ephemeral/` as CSV (power edges, grading, composite
      spectrum); numbers recorded in this file when done.

Expectation to test, not assume: the composite spectrum stays full (every
composite ≤ 24 realized) and the odd-product thinning (9, 15 at 5e9) has a
measurable trend.
