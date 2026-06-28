# dioph.pp — structure of `p = 2^m + q^n`

A native C/C++ research engine exploring the representations of primes `p` as
`p = 2^m + q^n` (`q` prime, `m, n ≥ 1`), at the scale of the full census of
primes up to ~2^39 (21.7 B primes, ~40 B representations).

For a prime `p`, `k(p)` is the number of such representations; the **hit set**
`H(p) = { m : p − 2^m is a prime power }` has size `k`. The programme studies how
these hit sets are structured: covering systems / modular obstructions (`k = 0`),
and the index-difference / S-unit relations between representations (`k ≥ 2`),
where for two representations `2^{m2}·(2^d − 1) = q2^{n2} − q1^{n1}` ties the gaps
`d = m1 − m2` to factors of Mersenne numbers `2^d − 1`.

## Architecture

- **Storage:** Apache Iceberg (Parquet data + an LMDB-backed `SqlCatalog`). All
  reads and writes go through the catalog seam — see
  [`markdown/data_eng/irc_catalog_design.md`](markdown/data_eng/irc_catalog_design.md)
  and [`markdown/data_eng/iceberg_data_setup.md`](markdown/data_eng/iceberg_data_setup.md).
- **Base tables:** `primes` and `partitions`, bucket-partitioned by `p_bucket`.
- **Derived tables:** `mdiff_k{K}` (per-prime hit sets as a single `hit_mask`
  int64 + covering `shape`) and `mersenne_factors` (factorizations of `2^d − 1`
  with `ord2` / primitivity).
- **Compute:** native tools (`primeparts-generate`, `primeparts-mdiff`,
  `primeparts-mersenne`, `primeparts-covering-sieve`, …), a `QueryService` + an
  embedded-Lua `query` shell (`pp`) and a notcurses TUI.

## Build

See [`BUILD.md`](BUILD.md) (canonical). In short: `native/configure` provisions
the C/C++ stack (Arrow, iceberg-cpp, FLINT/PARI, LMDB) rootless into `$HOME/.local`
via git submodules, then `make` in `native/`.

## Where to look

- **[`HANDOFF.md`](HANDOFF.md)** — living state: catalog architecture, tooling
  inventory, roadmap, and open work.
- **[`markdown/math/lab-notes.md`](markdown/math/lab-notes.md)** — dated research log.
- **[`markdown/math/structures.md`](markdown/math/structures.md)** — the research
  roadmap (problem structure, obstruction theory).
