# coverings/ — SHARED-ANCESTOR code (corrected provenance)

**Correction.** These C++ sources were first imported here as "verification-patches
work." That was wrong. They are **unchanged from the merge-base `bd64b0e`** — the
common ancestor of both `verification-patches` and `tui-query`. `tui-query` later
deleted them in its cleanup; `verification-patches` inherited them without
modifying them. So they predate the branch and are not its contribution. The one
genuinely branch-authored file here is `cov_emit_main.cc` (the cap3 covering
emitter, a containerized eval-task artifact — likely a dead end).

Not wired into the Makefile. Reference only. The code is the user's to reuse.

## The genuine verification-patches covering work is elsewhere

Recovered into `markdown/reference/covering-vp/` and `scripts/coverings/`, because
the human-readable part was **dropped before the branch head** (added `06362e7`,
deleted `1c8b2a5`):

- `markdown/reference/covering-vp/covering-families.md` — the handoff note: the
  per-prime minimal covering systems, the global order distribution, and the
  open "combine the families by density" program with its three obstacles.
- `scripts/coverings/{recipe,recipe2,pm2_table,global_sample}.py` — the Sage
  analysis behind that note.
- `markdown/reference/covering-vp/covering_sieve_main.method3.cc.txt` — the
  `--classify/--overgen/--method3` version of the covering builder (added
  `49d6c5d`, **reverted to base in `bb1100a`**, so absent from the branch head).
- The live generation-time covering filter is on `verification-patches` in
  `native/src/core.c` (commits `5ba7ecc`, `5e7491a`, `ea18498`, `4666468`) — a
  `{3,5,7,11,13,17}` filter applied during generation with k-range and
  count-only modes; not recovered here (it is a diff against `core.c`, best read
  from the commits).

## The methods these shared-ancestor helpers actually provide

`primitive_factors.{h,cc}` (the reusable, data-free library):

- `BuildMersenneHelper(max_d)` / `GetOrd2(ℓ)` — `ord₂(ℓ)`, the period of the
  congruence progression modulus ℓ kills.
- `GetCoverageMask(p, ℓ, d, max_m)` / `GetBackboneMask(p, max_m)` — per-prime
  bitmask of positions `m` killed by ℓ (or the backbone `{3,5,7,11,13,17}`).
- `PrimitiveFactorsForTerm(p, m)` — the primitive Mersenne factors dividing
  `p − 2^m` (which moduli are active at that position).
- `HitMaskDiffs(mask)` — decode a hit-position mask into the pairwise
  index-difference multiset (the k ≥ 2 S-unit constraint geometry).

## Terminology

Per `covering-families.md`'s own convention line — "ℓ is the generator of the
⟨2⟩ discrete-log action, **not a divisibility sieve**" — this is a **covering
system** (Erdős), not a sieve. Congruence: `ℓ | (p − 2^m)`, `d = ord_ℓ(2)`,
phase `r = m mod d`, covering density `1/d`. "Sieve" is not used here and must
not cross into tui-query.
