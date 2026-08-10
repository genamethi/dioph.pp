# A primer on the chain / Hermite picture

Audience: comfortable with modern algebra, less so with Hermite polynomials,
umbral calculus, Ritt's theorem, Jensen polynomials, and the modular-forms
vocabulary. This is a textbook-style exposition of what the pp-graph work has
settled, what is conjectural, and how it lines up with the three guiding
intuitions. Nothing here assumes the analysis background; every term is defined
on first use.

Notation is fixed once: `p, q` are odd primes; an edge is `p = 2^m + q^n`;
`D = d/dx` is the derivative operator; `[x^k] P` means "the coefficient of `x^k`
in the polynomial `P`."

---

## 0. Glossary (read once, refer back)

- **DP (dynamic programming).** Filling a table in a fixed order so each entry
  uses only entries already computed. Not a math object, just an algorithm shape.
- **Appell sequence.** A sequence of polynomials `A_0, A_1, …` with
  `A_n'(x) = n·A_{n-1}(x)` and `deg A_n = n`. Hermite is the example we use.
- **Umbral calculus.** The formal calculus of such sequences: it lets you treat
  "raise the index" and "multiply/shift the variable" as operators and compose
  them. Concretely for us it is the algebra of the shift `x ↦ x + a` acting on a
  polynomial basis.
- **Eisenstein part / cusp part.** A decomposition, borrowed from modular forms,
  of a quantity into a *smooth, closed-form, locally-predictable* piece
  (Eisenstein) plus a *fluctuating residual* (cusp) that no finite amount of
  local data predicts. Used here as an organizing metaphor, made precise in §7.
- **Cokernel.** For a linear map `φ: V → W`, `coker φ = W / im φ`, i.e., "the part of
  the target not reached by the map." Measures what is *newly required* when you
  enlarge a space.
- **Barcode / persistence.** Given a growing family of spaces indexed by a
  parameter (here, the bound `B`), each feature is *born* at some parameter and
  may *die* later; the barcode is the multiset of these birth-death intervals.

---

## 1. The objects

Every partition `p = 2^m + q^n` is a directed **edge** `q --(m,n)--> p`. Read it
as a map applied to the parent value:

```
f_{m,n}(t) = 2^m + t^n ,   so   p = f_{m,n}(q).
```

A **chain** is a directed path from a root prime `x` out to a leaf prime.
Composing the edge maps along the chain, from the root outward, writes the leaf
as a single polynomial in the root variable:

```
3 --(1,2)--> 11 --(4,2)--> 137
P(x) = f_{4,2}(f_{1,2}(x)) = 2^4 + (2^1 + x^2)^2 = x^4 + 4x^2 + 20,   P(3) = 137.
```

Two facts about the edges, both load-bearing:

- **`n = 1` edges are pure translations**: `f_{m,1}(t) = t + 2^m`. Composing
  translations just adds the offsets, in any order. This is why routing does not
  matter (below).
- **`n ≥ 2` edges are the only ones that raise the polynomial's degree.** The
  degree of a chain is the *product* of its `n`-values. In the warehouse the
  `n ≥ 2` edges are vanishingly rare (≈107k out of 44.66 billion rows), which is
  why the whole degree/subspace question is governed by a tiny, enumerable set.

---

## 2. Hermite polynomials, from scratch

We use the **probabilists'** Hermite polynomials `He_n`. Three equivalent
definitions; you only need one, but each earns its keep later.

**(a) Recurrence.**
```
He_0 = 1,   He_1 = x,   He_{n+1}(x) = x·He_n(x) − n·He_{n-1}(x).
```
So `He_2 = x^2 − 1`, `He_3 = x^3 − 3x`, `He_4 = x^4 − 6x^2 + 3`. The alternating
signs are the whole point (they cancel; §3).

**(b) Appell / umbral.** `He_n'(x) = n·He_{n-1}(x)`. This makes `He_n` an Appell
sequence, and gives the **umbral binomial theorem**: the identity that governs
translations:
```
He_n(x + a) = Σ_{k=0}^n  C(n,k)·He_k(x)·a^{n-k}.        (★)
```
Compare ordinary `(x+a)^n = Σ C(n,k) x^k a^{n-k}`: the Hermite basis shifts
*exactly like monomials do*. This is not a coincidence. It is the defining
property of Appell sequences, and it is the algebraic reason the `n=1`
translations act cleanly on the basis.

**(c) Heat operator.** This is the one that makes coefficient extraction a
one-liner. With `D = d/dx`,
```
He_n(x) = e^{−D^2/2} x^n ,      and inversely     x^n = e^{+D^2/2} He_n(x).
```
`e^{±D^2/2}` means the power series `Σ_k (±1/2)^k D^{2k} / k!`; on a polynomial it
terminates. Since `e^{D^2/2}` sends `He_n ↦ x^n`, applying it to any polynomial
`P = Σ_n c_n He_n` gives `e^{D^2/2} P = Σ_n c_n x^n`. Therefore:
```
c_n  =  [x^n]  ( e^{D^2/2} P(x) ).                        (†)
```
"To read a polynomial's Hermite coefficients, apply the operator `e^{D^2/2}`
(a finite sum of even derivatives) and read off the ordinary coefficients." That
is exactly what the code does, and it is exact, not numerical.

---

## 3. The chain polynomial in the Hermite basis

Take the example. `P = x^4 + 4x^2 + 20`. Apply (†): `e^{D^2/2}P = P + ½P'' +
⅛P'''' = (x^4+4x^2+20) + ½(12x^2+8) + ⅛(24) = x^4 + 10x^2 + 27`, so

```
x^4 + 4x^2 + 20  =  He_4 + 10·He_2 + 27·He_0 ,   value 137 at x = 3.
```

The alternating `−6x^2 + 3` inside `He_4` is annihilated by the `+10 He_2 + 27`.
That cancellation is not luck: it is the operator identity
`e^{−D^2/2}·e^{+D^2/2} = 1` (the two generating functions `e^{-t^2/2}` and
`e^{+t^2/2}` multiply to 1). Every chain polynomial expands over `He` with
integer coefficients; the *vector of those coefficients* is the invariant we
study.

---

## 4. Why chains reduce: Ritt's theorem

Empirically, many distinct chains to the same leaf share one Hermite vector (37
chains `3→137` reduce to 6 vectors). The reason is a normal form. Because runs
of `n=1` edges are translations, every chain reduces to a **canonical word**

```
P(x) = ( … ( (x + A_0)^{n_1} + C_1 )^{n_2} + C_2 … )^{n_r} + C_r ,
```

where each constant (`A_0`, `C_1`, …) is a sum of distinct powers of two. Its
binary digits *are* the `m`-labels of that stretch. Only these sums survive, not
which primes you hopped through. That is the reduction.

Is the word-to-polynomial map **injective** (so the Hermite vector is a complete
label of the shape)? This is where **Ritt's theorem** enters. Ritt (1922)
classifies the ways a polynomial can be written as a composition of lower-degree
polynomials: any two complete decompositions have the same number of factors and
the same list of degrees, and they differ only by a chain of two elementary
moves: absorbing affine (degree-1) factors, and the commuting pairs
`x^a ∘ x^b = x^b ∘ x^a` (and the Chebyshev analogue). Chebyshev cannot occur for
maps of the form `t^n + c`. The power-commuting move requires the constant
*between* the two power steps to vanish. But in our words every `C_i ≥ 2`
(each is a sum of `2^m`, `m ≥ 1`). So the move is blocked, and distinct words
give distinct polynomials.

Status: this is a clean argument but I have written it, not proven it to
publication standard; the affine-absorption case still wants a careful write-up.
If it holds (and the data is consistent with it), **the Hermite vector is a
complete invariant of the chain shape**, and the permanent, lean object to store
is the canonical-word census, with the huge `n=1` routing reduced to
multiplicities that never need to be stored as paths.

---

## 5. Bounded subspaces and the graded picture (intuition 1)

Fix a bound `B` on the leaf prime. Because `q ≥ 3` and `q^n < p ≤ B`, the largest
exponent that can appear is

```
max_n(B) = ⌊log_3 B⌋            (exact at every bound tested, up to 6.19×10^11).
```

A chain of total degree `d` from a root `≥ 3` already exceeds `3^d`, so the same
bound caps the chain degree. The single power edges realize *every* exponent
`2, 3, …, ⌊log_3 B⌋` (the spectrum is contiguous as measured), and together with
`He_0` (constants) and `He_1` (translations) they span

```
dim span{ He_0, …, He_D }  =  D + 1 ,    D = ⌊log_3 B⌋.
```

This is your intuition (1) made precise. Selecting the Hermite basis and
imposing a bound isolates a finite-dimensional subspace of the polynomial ring;
raising the bound past each threshold `B = 3^d` adds exactly one new basis
direction `He_d`. The whole system is a **filtration**

```
V_2 ⊂ V_3 ⊂ V_4 ⊂ … ,     V_D = span{He_0,…,He_D},   dim V_D = D+1,
```

a graded object where one graded piece is born per new exponent, and the
"combinatorial system doing umbral calculus" is the action of the translation
operators (★) inside each `V_D`. Dimension grows only logarithmically in `B`.

---

## 6. Congruences over the basis (intuition 2)

Your intuition (2), that congruences over the basis are the meaningful
restriction on prime solutions, is the productive one, and it has two exact
handles already.

**The translation identity (★) is a congruence engine.** `He_n(x+a) = Σ C(n,k)
He_k(x) a^{n-k}`. Reducing modulo a prime `ℓ`, the binomial coefficients `C(n,k)`
simplify by **Lucas' theorem** (base-`ℓ` digit-wise), and `a = 2^m` makes the
offsets powers of two. So "which chains survive mod `ℓ`" becomes a statement
about base-`ℓ` and base-2 digit patterns of `n` and `m`. This is the same digit
arithmetic the covering systems already exploit, now expressed on the basis
rather than on the primes.

**The `3 | n` exponent obstruction is exactly this, measured.** For the tail
exponents (only `q = 3` survives there), whether `2^m + 3^n` is prime is governed
by whether `−3^n` lands in the multiplicative subgroup `⟨2⟩ mod ℓ`. When `3 | n`,
`3^n` sits in the *smaller* subgroup `⟨27⟩ ⊂ ⟨3⟩`, which lands inside `⟨2⟩` more
often (measured: a modulus is "active" on 70% of `(ℓ,n)` pairs when `3|n` vs 62%
otherwise). That is a **subgroup-exclusion congruence on the exponent**, a
purely modular restriction, and it is the cleanest one-parameter example of the
whole phenomenon. What is *not* yet done: the analogous statement for `He_n mod ℓ`
directly (the polynomial congruence rather than the prime-power one). That is a
concrete next computation, and it is where "nice congruence properties of Hermite
at prime powers" would become usable.

---

## 7. Survival, Eisenstein vs cusp, and the cokernel (intuition 3)

Here is where your arithmetic-geometry instinct lines up with what I actually
measured, and where the "null" I keep flagging is really *your* Eisenstein/cusp
split under a different name.

A chain **survives** if every intermediate value is prime. Counting survivors is
what the routing table (the DP) does. Split that count into two parts:

- **The Eisenstein part = the locally-predictable average.** Given only local
  data (the covering congruences of §6, the prime density `1/ln`), you can write
  a closed-form *expected* survival: at each step, multiply by the survival
  probability of that residue class. This is smooth, enumerable, and computable
  without touching the data. It is the exact analogue of the Eisenstein series
  being computable from divisor sums / Bernoulli numbers. In our filtration it
  even has a discrete skeleton: the graded dimension `⌊log_3 B⌋ + 1` and the
  contiguous single-power spectrum are the *rank* data, the part of the tree
  that is forced and countable.

- **The cusp part = the residual the local data cannot predict.** Actual survivor
  counts minus the Eisenstein prediction. Empirically this is *not* zero and not
  noise: at the single-prime level it is the k-dispersion deficit (the covering
  model predicts ~half of the observed over-dispersion); at the exponent level it
  is the ~45% surplus for `3 | n`; at the chain level it is whatever survives
  after dividing routing counts by the covering-corrected expectation. This is
  the "global noise" you named, and pinning it onto a cusp-like object is the
  aspirational bridge to modular forms.

**The cokernel, concretely.** Enlarging the bound from `B` to `B'` gives an
inclusion `V_D ↪ V_{D'}`; the cokernel `V_{D'}/V_D` is spanned by the new
directions `He_{D+1}, …, He_{D'}`: literally "which basis vectors you are forced
to add." The *rank* of that cokernel is Eisenstein data (`D' − D`, closed form).
The *filling* of each new direction (which chains, with what multiplicities,
populate `He_d` when it is first reachable near `B = 3^d`) is cusp data. So your
question "what part is Eisenstein-ish, discretely enumerated, and what is the
cokernel" has a clean first answer: **the graded dimension and the contiguous
spectrum are the Eisenstein skeleton and are already enumerated; the cusp/cokernel
is the multiplicity structure filling each graded piece, which is exactly what
phase 03 measures once the null is the covering-corrected one rather than the
naive `1/ln` one.**

**Barcodes.** Treat `B` as a growing parameter. Each `He_d` is *born* at
`B ≈ 3^d` (first reachable degree-`d` chain) and, in this simple nested
filtration, never dies. So the barcode is a staircase, which is just the
dimension formula drawn sideways. The barcode becomes *informative* when you
filter by something that can kill features, e.g., filter chains by their
covering-survival weight, or by `q`, so that a direction can appear and later be
saturated. That is the genuinely topological version of the question and is
open; it needs the survival weighting of §7 in hand first.

---

## 8. Why Hermite at all: Jensen polynomials (the deep cut)

You flagged Jensen polynomials, and the connection is real and worth stating
carefully, because it explains why Hermite is not an arbitrary basis choice.

Given a real sequence `α_0, α_1, …`, its **Jensen polynomial** of degree `d` and
shift `n` is
```
J^{d,n}(X) = Σ_{j=0}^d  C(d,j)·α_{n+j}·X^j .
```
A sequence is "in the Laguerre-Pólya class" (roughly, the good-spectral case,
tied to all-real-roots / *hyperbolicity*) iff all its Jensen polynomials have
only real roots. Griffin-Ono-Rolen-Zagier (PNAS 2019) proved that for the
Riemann `ξ`-function and a broad class of sequences, the **renormalized Jensen
polynomials converge, as the shift grows, to the Hermite polynomials `He_d`**.
Hermite is the *universal limiting shape*: it is the degree-`d` model whose roots
match the Gaussian/GUE profile, and hyperbolicity of all Jensen polynomials is
equivalent to the Riemann Hypothesis for `ξ`.

For us this says: expressing chain data in the Hermite basis is the same move as
asking whether an associated sequence is hyperbolic / has a spectral
interpretation. Whether the chain-survival sequence *is* such a sequence is
unproven and speculative, but it is the precise sense in which "Hermite basis +
subspace decomposition" points at spectral theory, and it is the honest content
behind the modular-forms aspiration in `research_programme.md`. Treat that
document's theta-series lemmas as a research program, not established results;
this section is the part with a real theorem under it.

---

## 9. What each tool computes (so "evaluation" is unambiguous)

Three different things get called "expressions"; keep them separate.

- **GiNaC `ex`**: symbolic polynomial algebra over `ℚ`/`ℤ`: `expand`, `diff`,
  `subs`, series. This is the layer that builds chain polynomials, applies the
  `e^{D^2/2}` operator, and produces Hermite vectors. Exact and modular in the
  software sense (one small translation unit, no data dependency).
- **FLINT**: fast integer / finite-field / `mod ℓ` arithmetic. The right tool
  for §6: reducing `He_n mod ℓ`, testing subgroup membership, power-residue
  symbols, primality. GiNaC is *not* a finite-field CAS; congruence evaluations
  belong in FLINT (or Sage for exploration).
- **Iceberg predicate expressions**: the `n_k ≥ 2 ∧ p ≤ B` filter pushed to the
  scan. Purely a data-plumbing language; unrelated to the math despite the shared
  word "expression."

So "how modular is the GiNaC/expression work for evaluations": the *symbolic*
evaluations (Hermite expansion, umbral shifts, canonical-word reduction) are
already isolated behind small functions and portable. The *arithmetic* evaluations
(mod `ℓ`, residues) are a separate FLINT-backed module we have not written yet,
and §6 is exactly what would motivate it.
