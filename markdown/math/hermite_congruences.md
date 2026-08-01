# Hermite congruences mod ℓ: a handle on the cusp orthogonal to covering

Phase-03 result. The covering system (the Eisenstein part) lives entirely in the
⟨2⟩ subgroup mod ℓ: it is about `ord₂(ℓ)` and the progressions `2^m ≡ p (mod ℓ)`.
It breaks down at the frontier (large `ord₂`), which carries the growing majority
of k=0 primes (the cusp). The question was whether the polynomial algebra gives
an independent grip there. It does.

## 1. He_p ≡ x^p (mod p)

Exactly, for every prime p (checked p = 3..13, and structurally: every subleading
Hermite coefficient of `He_p` carries a factor of p, e.g., `He_4 = x⁴−6x²+3 ≡ x⁴
(mod 3)`). So mod p the umbral operator `e^{D²/2}` reduces to the identity at
degree p: the Hermite cancellation structure vanishes and `He_p` is pure
Frobenius `x^p`. This is the clean prime-power congruence.

## 2. Hermite factorization mod ℓ is a power-residue condition, NOT ord₂

`He_3 = x(x²−3)` has nonzero roots in `F_ℓ` **iff 3 is a quadratic residue mod ℓ**, verified at **100% agreement over 93 primes** (`He_3` roots ⟺ `(3/ℓ)=1`). The
root counts of higher `He_n` over `F_ℓ` are governed likewise by the residues of
the `He_n` discriminants. Crucially this is **independent of `ord₂(ℓ)`**: e.g.
`ℓ=31` (ord 5) and `ℓ=41` (ord 20) both have `(3/ℓ)=−1` and one root; `ℓ=11`
(ord 10) and `ℓ=13` (ord 12) both have `(3/ℓ)=1` and three roots.

So two different arithmetics sit on the same primes ℓ:

| structure | lives in | governs |
|---|---|---|
| covering (Eisenstein) | `⟨2⟩ ⊂ F_ℓ*`, via `ord₂(ℓ)` | which m-positions are killed |
| Hermite factorization | power residues, via `(disc/ℓ)` | which chain degrees have roots mod ℓ |

They are orthogonal. Where the covering breaks down (large `ord₂`, the frontier /
cusp), the Hermite factorization still provides a determinate, computable
condition, a handle the covering cannot see.

## 3. Why this is the cusp handle

- It is the concrete, verified form of the l-adic n-th-power-residue direction
  in `lab-notes.md` (2026-03-29): "is r an n-th power in Z_ℓ", governed by
  `u^((ℓ-1)/gcd(n,ℓ-1)) ≡ 1`. The Hermite polynomial factorization mod ℓ *is*
  that power-residue structure in polynomial form, so `gcd(n, ℓ-1)` (not
  `ord₂(ℓ)`) is the relevant invariant.
- It is exactly the object `research_programme.md` Lemma 4 speculated about
  ("the path is valid iff `He_n(2^{m'})` avoids the roots of the cyclotomic
  extension `F_ℓ(ζ_{Δm})`"). This computation confirms the roots of `He_n mod ℓ`
  are a real, power-residue-governed set, so Lemma 4's condition is well-defined
  and checkable rather than aspirational.

## Open next steps

- Characterize `He_n mod ℓ` factorization type for `n ≥ 4` by the higher power
  residues (cubic for n with `3 | gcd(n, ℓ-1)`, etc.), which are the exponent-dependent
  filtration primes of the lab-notes l-adic entry.
- Evaluate `He_n(2^{m'}) mod ℓ` along realized chains and test whether the cusp
  (frontier k=0 primes) is distinguished by hitting Hermite roots, forming the direct
  bridge from this congruence structure to the k=0 residue.
