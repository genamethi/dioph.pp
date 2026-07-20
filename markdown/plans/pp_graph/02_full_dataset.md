# 02 — full-dataset run

The census and composite-degree measurement over the whole warehouse,
p ≤ 618,760,338,619 (~44.66B rows, 170 files; the n_k ≥ 2 filter prunes no
files, so this is a full read and the first real test of the sharded path).

- [x] Run the power-edge census at the full bound through the client module
      with sharded readers: edge count, grading, max_n vs ⌊log₃ max_p⌋ = 24,
      identity check, runtime and peak RSS recorded (32GB ceiling; batches
      stream, only power edges are retained).
- [x] Composite-degree spectrum at the full bound: sources confined to
      q ≤ √max_p ≈ 786,650, reach universe generated arithmetically (flint),
      direct concatenations, realized products, witness words with Hermite
      expansions via GiNaC.
- [ ] Subspace-by-bound table extended to the full range (1e3 … max_p),
      cross-checked against `.ephemeral/subspace_by_bound.csv` where ranges
      overlap; measured rank vs ⌊log₃B⌋ + 1 at each bound.
- [x] Outputs to `.ephemeral/` as CSV (power edges, grading, composite
      spectrum); numbers recorded in this file when done.

Results, B = 619,000,000,000 (170 files, 44,655,205,838 planned rows, 20 shards).
Raw output: `.ephemeral/pp_graph_full_census.txt`.

- 107,286 power edges, identity clean on every row, max_n = 24 = floor(log_3 B) —
  the closed form holds at the full range.
- Grading is not monotone in the tail: n=19,20 have 3 edges each while n=21 has
  6 and n=24 has 4.
- Composite spectrum is FULL: every composite <= 24 — 4, 6, 8, 9, 10, 12, 14,
  15, 16, 18, 20, 21, 22, 24 — is realized by a multi-power chain. 389 direct
  concatenations, 425 reach starts, sources confined to q <= 786,613.
- Odd products stay thin and the thinning deepens with the odd factor:
  9 -> 1,424 feeder pairs, 15 -> 1,253, 21 -> 532, against ~60,000 for 4/6/8/10.
  24 -> 762 and 22 -> 5,506 are thin too, so thinness tracks the largest prime
  factor rather than parity.
- Runtime 415s read (7 min total) at 198% CPU with 20 shards: 181 GiB moved =
  ~447 MB/s, i.e. the run is IO-bound on the external SSD, not CPU-bound. Peak
  memory never approached the 32GB ceiling (107,286 retained edges).

The IO-bound result is load-bearing for phase 04: a query engine cannot beat a
saturated drive on raw scan, so the engine question is about expressiveness and
join/aggregation work, not scan throughput.
