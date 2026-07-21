# Recovered observations from the away session (28eec3a5)

Source: session `28eec3a5-13d5-4911-b2dc-4662af8f703c`, the origin of the
`claude/colab-prime-partitions-t3wtno` (colab) branch. These are the chain /
collapse / exponent observations that were worked out there and never migrated
into `lab-notes.md`. Recorded verbatim in substance so the graph store (phases
04/05) is shaped by them rather than retrofitted. Line references are into that
session's jsonl.

## Roots of the reverse graph = the "no n=1 parent" primes

Each `n=1` partition `p = 2^m + q` is a directed edge `q →(m)→ p`. Over the
first 1000 primes: 994 nodes, 1838 edges; 751 primes reachable from 3 (one huge
component). The **roots** — primes with no `n=1` parent — are exactly the primes
that have partitions but none at `n=1`: `3, 127, 149, 251, 331, 337, 373, 509,
599, 757, …` (8 of the first 1000). Examples `127 = 2¹+5³`, `2213 = 2²+47² =
2⁴+13³`. So "no `n=1` representation" and "graph source" are the same set — a
structural fact, not a coincidence (lines 225, 240).

This is the same set a later probe rediscovered as the `n1_unreach` primes
(~12–15% of primes, growing with scale). They are chain sources, not an
obstruction artifact.

## Immediate collapse: n=1 runs are translations, only sums of 2^m survive

Every chain reduces to a **canonical word** because runs of `n=1` edges are pure
translations `t ↦ t + 2^m`. Composing a run adds the offsets:
`f_{a,1} ∘ f_{b,1} = t + (2^a + 2^b)`. So along a chain only the accumulated
sums survive — an initial translation `A₀` and the inter-power translations
`C₁ … C_r` between successive power-raising (`n ≥ 2`) edges — never the routing
that produced each sum (lines 345, 519).

The surviving data of a chain is therefore:
`(((x + A₀)^{n₁} + C₁)^{n₂} + C₂ … )^{n_r} + C_r`, where each `A₀, C_i` is a
**sum of powers of two** `Σ 2^{m}`. These sum-sequences are the live arithmetic
object: coefficients and modular properties of the He-expansion are functions of
them, and they accumulate as chains lengthen.

## The 137 paradigm

The all-`n=1` chain `3→5→7→11→13→29→37→41→43→59→67→71→73→137` carries the
`m`-sequence `6,1,2,3,4,1,2,3,4,1,2,1,1` (reversed 137→3). There are 37 chains
`3→137` in the full graph. They **collapse to 6 distinct Hermite expansions**:

- 16 chains (all `n=1`) → `He₁ + 134`  (`= 3 + 134 = 137` at x=3)
- one chain → `He₄ + 10·He₂ + 27`  (`= 30 + 80 + 27 = 137` at x=3), the pure
  degree-4 route `3 →(2)→ 11 →(2)→ 137`, i.e. `x⁴ + 4x² + 20`
- four other chains → `He₃ + 3·He₁ + 110`, etc.

The He-vector is a genuine invariant: it depends only on the chain's shape (which
edges raise a power, and the net translation), and is blind to `n=1` routing
(lines 294, 329, 345).

## Exponent / grading structure

- `deg(f_a ∘ f_b) = n_a · n_b` — the chain is graded by the **product** of its
  edge exponents.
- Exponent spectrum is **contiguous** `[2, ⌊log₃B⌋]` at every bound; the subspace
  fills one graded piece per new exponent, so dimension grows `~ ⌊log₃B⌋ + 1`.
- Grading is top-heavy: at `B = 5e9`, `n=2` is 11,658 of 12,549 power edges
  (93%), `n=20` has 3.
- The `n=1` maps form a **closed commutative** translation sub-monoid; every
  composite is **monic** in the He basis; composition is non-commutative in
  general (lines 345, 519).

## Closed forms established there

- Hermite coefficients of a chain polynomial `P`: `c_n = [x^n] exp((1/2) d²/dx²) P`.
- Exponent ceiling: `max_n(B) = ⌊log₃ B⌋`, matched by measured rank
  `dim = ⌊log₃ B⌋ + 1`.

## Bearing on phases 04/05

The store must make first-class: (a) forward `p → partitions/children` and
reverse `p → ancestors/chains`; (b) per-chain the **canonical word** `(A₀; n₁,C₁;
…; n_r,C_r)` with its sum-of-2^m offsets, not the raw routing; (c) the He-vector
as the collapse invariant (dense-id the distinct vectors; keep symbolic strings
for inspection). The routing multiplicity is compressible precisely because it
collapses — the store should hold the invariant, and regenerate routing on demand.
