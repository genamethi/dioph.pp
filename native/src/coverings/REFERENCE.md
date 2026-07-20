# coverings/ — reference import from `verification-patches`

These sources are **imported for reading**, not wired into the build. They are
the sieve / covering-system tooling written on the `verification-patches`
branch, brought onto `pp-graph-sieve` (based on `pp-graph-exp`) so they can be
viewed alongside the current work. The Makefile does not compile them; they
predate the current `scan/` planner and `source_scan` seam, so building any of
them means rebasing onto the current interfaces first.

Provenance: the code is the user's to reuse; only the original working sessions
were proprietary and were intentionally deleted. This import carries no session
history — just the source.

## What each file is

| file | binary it was | what it does |
|---|---|---|
| `mersenne_main.cc` | `primeparts-mersenne` | builds `mersenne_factors` (d, prime, exponent, ord2, is_primitive) — the obstruction moduli ℓ keyed by ord₂(ℓ) |
| `mdiff_main.cc` | `primeparts-mdiff` | builds `mdiff_k{K}` as `(p, hit_mask)`; one sorted pass over `partitions`, run-detection on p-ordered rows |
| `primitive_factors.{h,cc}` | library | `MersenneHelper`, `HitMaskDiffs` (mask → pairwise index-difference multiset), `GetBackboneMask` / `GetCoverageMask` |
| `covering_sieve_main.cc` | `primeparts-covering-sieve` | iterative merge-on-read delete sieve: one modulus per pass, position-deletes newly fully-covered primes, advances a snapshot |
| `cov_emit_main.cc` | `primeparts-cov-emit` | cap-bounded covering certificate emitter (containerized eval-task artifact; likely a dead end) |
| `primitive_factors_main.cc` | test/driver | exercises the primitive-factor helpers |
| `../catalog/pp_row_delta.{cc,h}` | library | the repo's only committable merge-on-read position-delete implementation (subclasses the vendored SnapshotUpdate) |
| `../catalog/pp_sieve_clone.{cc,h}` | library | shallow v2 MOR clone used to seed `primes_k0_sieve` |
| `../mersenne_sidecar.cc` | sidecar | marked "may be obsolete" in its own header |

## Why it is here (the sieve/null connection)

The `pp-graph` phase 03 "null" question — what survival rate a chain *should*
have from local congruence data alone — is the Eisenstein part of the
Eisenstein/cusp split (see `markdown/math/chain_hermite_primer.md` §7). The
covering-corrected survival probability per residue class is the object these
tools compute at scale. Reusing this machinery (rather than rebuilding a naive
`1/ln` null) is the path to a *meaningful* null; `markdown/math/lab-notes.md`
records the results, and `markdown/math/modular-filter-idea.md` records the
hot-path design pedigree.
