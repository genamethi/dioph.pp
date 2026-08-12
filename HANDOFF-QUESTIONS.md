# Fitting items 1-4 to the applied ell-adic cohomology approach

Reference: FKMS, *Lectures on Applied ell-adic Cohomology*,
`/media/extssd/research/library/math/applied-ell-adic.pdf`. Section numbers below
are from it.

These are the top-line design questions that shape `markdown/dev/HANDOFF.md`.
They are meant to be answered and marked settled, not deleted; deletion is only
for a question that was misguided. Nothing should be added here that is not one
of those questions.

Part A applies as corrections to items 1 through 4 of `markdown/HANDOFF.md` on
`eval-analysis-engine`. Part B is the design questions: B1 and B1' are settled,
B2 and B3 are answered in form but not in implementation. Part C maps the code
on `tui-query` to the mathematics and lists what is known to be wrong or
off-goal. Implementation is the reader's call throughout.

A dated account of the work behind the settled entries is in `2026-08-12-log.md`
at the project root.

Code references are `native/src/graph/pp_graph.cc` unless noted, and are line
numbers as of `c8deb83` on `tui-query`. They rot on the first edit to that file;
`git log -L` or a grep for the named symbol is the reliable way back to one that
has moved. The items being discussed live on `eval-analysis-engine`, which has
no `native/src/graph/` at all, so the prose and the code it refers to are never
checked out together.

## A. Corrections

**A1. `ell` is the coefficient prime and the base is a parameter. The tower over
the base is where the `m`-record lives.**

The heading previously read "the base is `F_2`". No such choice was made; it was
asserted here and then treated as settled. `point_count` takes the
characteristic as an argument, and by B3 the ell-adic content only appears at an
odd base. What follows is about the `F_2` case, which is the one the equation
singles out arithmetically, not the one the geometry is committed to.

FKMS fixes a base `F_q` and a prime `ell != q`; trace functions are
`Qbar_ell`-valued functions on `A^1(F_q)` (§3.1). Two primes, two roles.

`p = 2^m + q^n` makes 2 the base. So: base `F_2`, coefficient prime `ell` odd,
and `l = 2` is not excluded from anything, it is where the base lives. The
growing family is not a range of primes but the degree-`d` closed points of
`A^1_{F_2}`, i.e. `F_{2^d}` with Frobenius acting. Item 3's "l = 2 is
degenerate, so odd l" fixes the base rather than dropping it.

The level-one collapse is not an argument against this and is worth stating
exactly, because it is the base case of the tower rather than an obstruction.
Normalize a chain polynomial by subtracting its target, `S = P - p`. Then mod 2
every `q` is a unit, so `S + q ≡ S + 1`, and induction from `S ≡ x + 1` at the
roots gives

    S ≡ x^N + 1  (mod 2),   equivalently  P ≡ x^N  (mod 2),   N = prod n_i.

Every `2^m` vanishes and the level-one class is the degree alone. That is a
statement about level one, not about the base: the constants are carried in the
tower `Z/2^e` above it, and `q mod 2^e` exposes exactly the exponents `m' < e`,
so **level `e` resolves the exponents below `e`**. Item 4's tower with `l = 2`
is therefore the object that recovers what level one drops, and `ZeroConst`
deleting the constant term is deleting precisely the datum being recovered.
Definitions, the operator `T_{q,n}(S) = (S+q)^n - q^n`, and the Kummer and Lucas
filters on its coefficients are in
`markdown/math/notation.md`.

In the code the single letter `l` plays the base role and only the base role.
`ClassSweep` reduces polynomial coefficients mod `l` with `nmod_poly`. Until
`65bd8bb` the filter in `ParseEllList` rejected 2 outright — it now admits any
small prime, so `--ells 2 --e E` reaches the base. `IsSmallPrime` always
accepted 2; the odd-only loops driven by `--ell-max` at `:788`, `:1346`,
`:1676` and `:2044` still skip it. Nothing in the tree names a coefficient
prime distinct from the reduction modulus.

**A2. Item 3 wants the monodromy group, not the mod-`l` orbits.**

Post-composition by translations acting on maps reduced mod `l` is a degree-1
statement, and none of the paper's structure survives restriction to `d = 1`.
The object is the geometric monodromy group and the exact sequence
`1 -> G^geom -> G^arith -> Gal(Fbar_q/F_q) -> 1` (§3.1, §13), which lives at all
degrees and is where Frobenius acts.

Under the pushforward reading in B1 this is the Galois group of the Galois
closure of the chain polynomial, so item 3's monodromy and the Galois structure
of a chain are one question.

By B1' the uncollapsed `y`-variety supplies the block structure on the fiber for
free, so the containment in the iterated wreath product of the `Z/n_i` is given
rather than proved. Two distinct representations are in play and should not be
conflated: the geometric monodromy `pi_1^{et}(U) -> GL(V)`, which for `P_*Q_ell`
is the rank-`N` permutation representation and for `L_psi(P)` is a rank-1
character; and the arithmetic action of `Gal(Fbar_q/F_q)` on `H^1_c`, whose
Frobenius trace *is* the exponential sum. The first constrains, the second
produces numbers.

The code already contains the acknowledgment of this gap without acting on it.
`FaithfulDegree` (`:2270`) computes the smallest `d` with `l^d > D` for the
degree bound `D` in play and prints it as "faithful on `GF(l^d)`-points"
(`:3109`), but every computation in the tree evaluates at `d = 1`. Item 3's
request for a drawing is a request to draw the monodromy object, not the orbit
digraph currently emitted.

**A3. Item 4's tower over `Z/2^e` is the object, and it is not a stand-in for
`rho mod ell^e`.**

An earlier pass here claimed the tower should be the Galois representation
reduced mod `ell^e` rather than the maps over `Z/l^e`. That is empty under any
reading in which the sheaf is a pushforward: the stalk is then a permutation
representation on the geometric fiber, permutation representations are defined
over `Z`, so `rho mod ell^e` is the same combinatorial datum at every `e` and
raising `e` adds nothing. Uniformity in `ell` is why the conductor bounds are
uniform; it is also why `ell` is not a source of new data.

The tower the code builds is the reduction of the chain maps over `Z/l^e`:
`ClassSweep` called with `mod = l^e`, levels compared by truncating coefficient
keys (`TruncKey`). With `l = 2` that is the deformation of the `F_2`-datum over
`Z_2`, and by A1 it is exactly the filtration that restores the `m`-record one
exponent per level. Keep it. Its truncation check is a real consistency check on
the object it builds.

Measured at `l = 2` on `--k 1-8` under 2e6, family classes by level:
13, 42, 179, 727, 2880, 8894, 24881, 64335 for `e = 1..8`, against 13 realizable
degrees at level one. Under 1e10 with `--k 1-16` and `e = 1..4`: 20, 74, 387,
2247 classes and 244, 1301, 25842, 519082 distinct per-prime sets over 376
million primes.

## B. Open

**B1. Which construction attaches a sheaf to a chain.** **Settled.** They are
not alternatives, and the pullback is the side that carries cohomology.

`pi^* -| pi_*`, and for `pi = P` the projection formula gives

    pi_*(pi^* L_psi)  ~  L_psi (x) pi_* Q_ell
    H^*_c(A^1_x, P^* L_psi)  ~  H^*_c(A^1_p, L_psi (x) P_* Q_ell)

which at trace level is `sum_x psi(P(x)) = sum_p psi(p) * #P^{-1}(p)`. The
pullback computation upstairs and the twisted pushforward downstairs are one
computation. An earlier pass here framed them as competing readings; that was
wrong, and so was a later claim that they are orthogonal to the choice of what
happens to the base 2.

Which one carries cohomology is *not* symmetric:

- `P_* Q_ell` alone. `P` is finite, so `P_*` is exact and
  `H^i_c(A^1_p, P_*Q_ell) = H^i_c(A^1_x, Q_ell)`. The source is the affine line.
  **There is no cohomology there.** Its content — rank `N`, fiber counts,
  monodromy — is pointwise.
- `P^*L_psi = L_psi(P)`. Lisse of rank 1 on `A^1` and nontrivial, so
  `H^0_c = H^2_c = 0` and `dim H^1_c = deg P_red - 1`, with `P_red` the
  Artin-Schreier reduction below. One nontrivial group, and the Weil bound
  `|sum_x psi(P(x))| <= (deg P_red - 1) sqrt(q)`.

So the earlier recommendation to build on the pushforward is backwards for
cohomology and right for monodromy. Both are needed; the projection formula is
how they combine.

*Artin-Schreier, exactly.* `℘(z) = z^p - z` is additive with kernel `F_p`, and
`℘ : A^1 -> A^1` is finite etale Galois with group `F_p`, so
`℘_* Q_ell = (+)_psi L_psi`. Then `L_psi(f) ~ L_psi(g)` iff
`f - g ∈ ℘(k[x])`, because `Tr_{F_q/F_p}` kills `℘`. In characteristic 2,
`℘(c x^j) = c^2 x^{2j} + c x^j`, so

    L_psi(a x^{2j})  ~  L_psi(a^{1/2} x^j)     when a is a square in k

Fold every even-degree monomial down, *adding* into the coefficient already at
degree `j` — cancellation there can drop the degree further than folding alone.
`Swan_inf = deg P_red`.

Over `k = F_2` this never blocks, because `F_2` is perfect. With notation §3.4
(`P ≡ x^N mod 2`), `L_psi(x^N) ~ L_psi(x^{N_odd})`, so
`dim H^1_c = N_odd - 1`, zero exactly when `N` is a power of two. That is the
collapse flagged above, made exact: not total, but down to the odd part.

*Where the formal base variable earns its place.* Promote 2 to an indeterminate
`t`; notation §2.4 already writes `A_0` and each `c_i` as a sum of powers of two. Over
`k = F_2(t)`, `[k : k^2] = 2` with `k^2 = F_2(t^2)`, so `t^m` is a square iff
`m` is even, and the fold blocks at the first even-degree term whose
`t`-coefficient has odd valuation. Imperfection is doing the work and `F_2`
cannot supply it. This is why the stand-in is needed on the `L_psi` side and not
on the `P_*` side: `P_*Q_ell` has rank `N` and fiber-count traces regardless of
what happens to 2; only the *additive* character cares, because `℘` is precisely
what additive characters cannot see.

*The obstruction is generic-only.* Every finite field is perfect, so specializing
`t` to any closed point of the `t`-line restores squares and unfolds the
reduction. The blocked degree is a statement about the generic fiber over
`F_2(t)` and about no closed fiber; excising `t = 0` does not fix this, because
the issue is every `t`. The setting that keeps both is the surface

    A^1_t x A^1_x -> A^1_p,   (t, x) |-> P(t, x)

over `F_q`: generic fiber over `F_q(t)` imperfect, constant field finite so
Frobenius acts. Closed points of the `t`-line are places of `F_2(t)` with residue
field `F_{2^d}` and Frobenius elements, and their traces assemble into an
L-function. Untested lead: `F_q[t]/(t^e)`, the `t`-side analogue of the `Z/2^e`
tower, non-reduced and so keeping squares constrained at finite level.

**B1'. Which variety.** **Settled.** The single-word variety is the fiber; the
useful one is a fiber product.

For a fixed word the `y`-variety
`V = {(y_0..y_j) : y_{i+1} = y_i^{n_i} + t^{m_i}}` is the *graph* of the
composition — each equation solves the next coordinate from the previous — so
`V ~ A^2` via `(t, y_0)`. As a variety it is affine space and carries nothing.

Imposing the target makes it the pushforward's fiber: `V ∩ {y_j = p}` is
zero-dimensional of length `N` and is exactly the geometric fiber of `pi`, the
stalk of `P_*Q_ell`. That is the precise sense in which the two pictures line up.

What the variety has and the composed polynomial does not is the **tower
filtration on that fiber** — blocks of size `n_j` inside blocks of size
`n_j n_{j-1}`, and so on. That imprimitivity system is why the monodromy lands in
the iterated wreath product of the `Z/n_i` rather than in `S_N`. From the
composed `P` that is a decomposability theorem to prove; from the variety it is
given. It is the reason to keep the uncollapsed presentation.

The variety that is *not* a graph is the fiber product of two chains over the
target line:

    C = V x_{A^1_p} V' = { (y, y') : P(y) = P'(y') }   in A^2

a plane curve, generally singular, of positive genus: `C -> A^1_y` is `N':1`,
branched where `P(y)` meets a critical value of `P'`, so on the order of `k' N`
branch points. This is where cohomology of a *curve*, rather than of `A^1`,
becomes available, and it is the first object here with an `H^1` that is not
forced.

Nothing in the code builds a sheaf, a trace function, a pullback or a
pushforward. `--critical` carries each node's set of critical values mod `l`
through the ascending sweep using a precomputed per-`(l, c, n)` transition
table, which is the nearest existing object either way. By notation §2.4 only the `n >= 2`
blocks have critical points, so that pass reads `higher_parts` alone.

**B2. Whether a bounded-conductor family exists.**

Every usable bound in the paper is in terms of the conductor, rank plus number
of singularities plus Swan conductors, and is only worth something when the
conductor stays bounded as `q` grows (§4). Along a chain the degree is the
product of the exponents, so any invariant tracking degree grows with depth.

Work out whether there is a family here with conductor bounded independently of
the growing parameter. If the conductor grows with chain depth then the bounds
go trivial past a short depth, and items 1 through 4 would have to be asking for
something else. This decides whether the approach produces results or only
vocabulary.

Both conductors are now computable rather than conjectural.

*Pullback.* `L_psi(P)` is rank 1, singular only at infinity, `Swan_inf = deg
P_red`, so

    c(L_psi(P)) = 1 + 1 + deg P_red = deg P_red + 2 <= N + 2

*Pushforward.* `P_*Q_ell` has rank `N`. Its separable part has degree `N_odd`,
and ramification indices there divide `N_odd`, which is odd, so characteristic 2
never divides them: **tame, no Swan conductor**. The singularities are the
critical values, and by notation §2.4 only the `n >= 2` blocks have critical points, one
each, so `#sing <= k + 1` with `k <= log_2 N`. Hence

    c(P_*Q_ell) <= N + log_2 N + 2

What the rank does is exact rather than a worry. `N = prod n_i` and a chain into
`p` from a root `q_0 >= 3` has `p > q_0^N`, so `N <= floor(log_3 p)`. That bound
is attained and every value below it occurs: at `e = 1` the class count equals
`floor(log_3 p_max)` in all seven windows measured from 1e5 to 1e10, and the
largest level-one state set has exactly that many elements.

So both conductors are bounded for a fixed chain and **independent of `d`**:
constant-field growth `q = 2^d` does not move them, which is exactly the family
FKMS's bounds are stated over. Both grow like `log_3 p` over the family items 1
through 4 range over. That is B2's tension with numbers attached rather than a
worry — the bounds are usable in `d` and go trivial in chain depth.

Nothing computes a conductor, a rank, or a Swan conductor. Degree is available
from the word layout in `ComputeSpectrumCore` (`:1514`) and is used as a
truncation bound in the orbit pass (`max_degree`, `:2516`); `--critical`'s
bitmask counts critical values mod `l`, which is a count of candidate
singularities and not the conductor.

**B3. Which sum is supposed to exhibit cancellation.**

The paper is organised around sums of trace functions: over `F_q` (§4), short
intervals (§7), primes (§9), bilinear (§10). Items 1 through 4 ask for structure
and never name a sum. Two now have a shape.

*The Weil sum.* `sum_{x in F_q} psi(P(x))`, bounded by
`(deg P_red - 1) sqrt(q)` from B1. Over `F_2` with 2 evaluated, `deg P_red =
N_odd` and the bound is identical for every chain of the same odd degree — the
vacuity A1 records, a statement about the base rather than about the chains.
Non-vacuous only under the formal `t` of B1 or an odd base.

*The correlation sum, which is the one that touches `k`.* With
`t(p) = #P^{-1}(p)` and `t'(p) = #P'^{-1}(p)` the fiber counts of two chains,

    sum_{p in F_q} t(p) t'(p) = #C(F_q)

for the fiber product `C` of B1'. Weil for a curve gives `q + O(g sqrt(q))`.
`k(p)` counts the ways `p` is expressed, so this is its two-chain correlation
and the first sum here whose cancellation would say something about `k`. Moving
from a sum over `F_q` to a sum over primes (§9) is a separate step, not done.

Every output in the tree is a count or a set size (`Family census`, `:2632`;
orbit report). No signed quantity is summed anywhere, so there is still nothing
implemented to point cancellation at.

## B'. Next direction

Stated in the same register as the rest: these are exact questions with exact
answers, and a fitted growth law is not an answer to any of them. Definitions
and the definitions behind each are in `markdown/math/notation.md`.

**B'1. Which degree sets occur.** By A1 a prime's level-one state is its set of
realizable chain degrees, a subset of `[1, D]` with `D = floor(log_3 p_max)`.
Measured over `--k 1-16`, the number of distinct such sets is 52, 82, 118, 160,
201, 244 at `D = 10, 12, 14, 16, 18, 20`. At `D = 20` that is 244 subsets out of
`2^20` across 376 million primes. Characterize the 244. This is a level-one
question, so it costs one sweep at `--ells 2 --e 1`.

**B'2. Separation depth against `k`.** At 1e10 and `e = 4`, `k = 14` separates
completely, 6 sets over 6 primes, while `k = 1` gives 248374 sets over 130
million primes. The state set of `p` is a union over its `k` parents, and by A1
level `e` sees the exponents `m' < e`. Give the level at which a given `k`
separates.

**B'3. Read the chain off the Hermite vector.** The premise this was stated with
is false and was cut from `notation.md`: the claim that coefficients above index
`N - N_1` are those of `x^N`, and that `c_i` first appears at `N - N_i` with
linear coefficient `(N / N_i) c_i`, holds only for `A_0 = 0`. Every word in the
`p = 65537` table supporting it had `A_0 = 0`, visible in the absence of odd
Hermite indices, and for `A_0 != 0` the claim fails already at index `N - 1`,
since `(x + A_0)^N` differs from `x^N` there.

What survives is one edge: `x^n + 2^m` puts its whole `m` in `He_0` and every
other coefficient depends on `n` alone. Whether composition turns that into a
depth filtration is open, and is the question. `SegKey` is the support, so the
grouping half can be run over data the tree emits today, but the linear readout
it was supposed to rest on does not exist.

**B'4. Wire `--critical` and the orbit pass onto the shared state.** Both still
carry per-node sets keyed by the absolute polynomial. The normalization of A1
applies verbatim, and without it neither runs past about 1e7.

## C. Code to mathematics

### C1. Item 1, the graph, the basis, and the parameterization

| Code | Object |
|---|---|
| `ReadEdges` (`:582`), `ScanPartitionEdges` (`graph/partition_scan.cc`) | the generator set `{sigma_{m,n} : x -> x^n + 2^m}` realized by the queried window. `SolveQ` is **gone**: `flat_parts` gives `q = p - 2^m` by subtraction and `higher_parts` stores `q_k`, so no root extraction remains on any read path |
| `RunHasse` (`:875`), `EmitDot` (`:1247`), DOT at `:1250` | the prime poset, cover relation, Dilworth width, minimum chain decomposition; DOT with Mirsky levels as `rank=same` |
| `RunCompose` (`:1044`), DOT at `:1158` | the composite/ancestor structure with edge labels `(m, n)` |
| `EmitConeDot` (`:93`), reached from `:1729` | the ancestral cone of a single target; roots boxed, target highlighted, `n >= 2` edges thickened |
| DOT at `:998` | the congruence-refinement poset over `l`, from mode `hasse` |
| `ToHermite` (`:773`, `:1207`, `:1708`, `:2551`), `hermite_modl.cc` | `He_i` decomposition of a chain polynomial |
| `SegKey` (`:2326`), `SegKeyModL` (`:2337`) | the `He_i` support of a chain, exact and mod `l`; this is the grouping key item 1's third bullet asks for |
| `HermiteEvalModL`, `HermiteRootsModL` (`hermite_modl.cc`) | roots of `He_n` mod `l`, feeding the congruence refinement |
| `MaterializeColumns` (`:830`, `:3235`) | `<ns>.spectra` with `p, root, degree, chains, skeleton, hermite` |
| `PolyKey` / `KeyToPoly` / `SeedKey` (`:1836`-`:1861`), `nmod_poly` throughout | coefficient vectors over `F_l` as the state datum; replaced the hand-rolled `MulL`/`PowL`, removing the degree-32 and `l < 256` caps |
| `flat_parts` / `higher_parts` (`schemas.cc:42`, `:53`), `parts_expand.h` | the bigrading made physical. By notation §2.4 an `n = 1` edge contributes no degree, so `hit_mask` is the translation layer and `higher_parts` carries the entire `y`-graded structure — 147,103 rows for the whole warehouse |
| `higher_sweep.c`, `pp_higher_sweep` | the `n >= 2` edges enumerated directly from `n <= log_5(hi)` rather than discovered by testing every `p - 2^m`. Makes the graded layer computable without the flat table |
| `config.h` `kFields` table, `config_gen.cc`, `Conf::defaulted` | one declaration drives flags, config keys and the generated `example.config.lua`; a missing key now defaults instead of being fatal. This is what item 1's N.B. asks for when it says moduli and ranges should be parameters |
| `--ells` / `ParseEllList` (`:1895`), used at `:858` | replaced the hardcoded ten-element modulus list in mode `hasse` |
| `pp_catalogd.cc` `/v1/config`, `pp_iceberg_rest.cc` `AdvertisedWarehouse` | warehouse advertisement and a read/write mismatch guard; infrastructure, no mathematical content |
| `pp_graph_exp.cc` deleted | carried a hardcoded warehouse path and read no config |

### C2. Item 2, the spectrum

| Code | Object |
|---|---|
| `ComputeSpectrumCore` (`:1514`), called at `:1668`, `:2497` | item 2's spectrum for one target: ancestor cone, admissible `n >= 2` segment sequences, canonical words with multiplicities. The definition executed literally |
| `SpectrumRow` (`:1952`), family mode `--k` | the spectrum of a whole family `{p : k(p) = K}` off one shared edge read |
| `ClassSweep` (`:1961`) | the same class sets by a single ascending pass; `p = 2^m + q^n > q` makes ascending order topological, so each node's state is built from its parents' |
| `BuildCompactGraph` (`:2020`) | the streaming CSR build that replaced the in-memory edge map; what makes 1e10 reachable |
| `--sweep` agreement check | the sweep against the per-target walk. The only evidence the fast path computes the definition and not something adjacent |

### C3. Item 3, the monoid and its orbits

The monoid structure itself does not appear to carry weight. FKMS attaches its
invariants to one sheaf at a time, computed from one chain polynomial; that the
set of composites is closed under composition is not used anywhere in the
paper. What survives from this pass is a normalization of the constant term, but
**not** `ZeroConst`. Deleting the constant deletes the `m`-record: by A1 the
constants are the only place the exponents survive, and level `e` of the tower
reads them out. The correct normalization subtracts the *target* rather than
zeroing, `S = P - p`, which is invariant along `n = 1` edges and keeps every
exponent. `ZeroConst` and `S` agree only on the sub-question of what a twist by
`L_psi(c)` cannot see, and that sub-question is not item 4's.

| Code | Object |
|---|---|
| `SharedSweep` (`:2100`) | `S = P - p` as the carried state: invariant along `n = 1` edges, `S_p = (S_q + q)^n - q^n` on `n >= 2`. The normalization item 3 should have asked for. notation §3.1 and B1' are the same fact from two sides: `n = 1` edges carry no `y`-degree, which is why `S` is constant along them |
| `ZeroConst` (`:2280`) | the polynomial with its constant term zeroed. Read as the translation quotient it is item 3's orbit representative; read against A1 it discards the exponents and is the wrong normalization for the tower |
| ambient BFS (`:3048`) | the monoid `M_l` generated by the reductions of the edges actually present, closed under the generator action from the seed `x` |
| `realized` (`:2982`), `misses` | which orbits the queried family attains and which of the ambient it does not |
| `signatures` (`:2997`) | the per-prime realized set, counted by distinct signature. This is the per-prime reading item 3 asks for |
| orbit DOT (`:3085`) | the quotient as a digraph: nodes are orbits labelled by `PolyLabel` (`:2296`), realized ones filled, complement dashed, `rank=same` by degree, arcs the generator action |
| `FaithfulDegree` (`:2270`) | the `d` at which `GF(l^d)`-points separate maps of the degree in play; reported (`:3109`), never used |

### C4. Item 4, the tower

| Code | Object |
|---|---|
| `ClassSweep(mod = l^e)` (`:3156`) | the class sets over `Z/l^e`, keyed by the absolute polynomial |
| `TruncKey` (`:2311`) and the `e`-loop (`:3160`) | levels compared by truncating the top-level answer to each lower modulus; a mismatch is a hard failure |
| per-`e` family-class and per-prime-set counts | the stratification question as item 4 poses it: whether the realized set changes only at isolated `p` |
| `--shared` (`:2736`) | the same counts off `S = P - p`, interned and shared between primes; `--ells 2` now reaches the base. Agrees with `ClassSweep` at `l = 2, 3, 5, 7` on every level checked, and is what makes 1e10 reachable |
| `--k` taking a list or range (`65bd8bb`) | the stratification read per `k` off one sweep, since the sweep is over nodes and `k` only selects which nodes are reported |

### C5. Known wrong or off-goal

1. **The ambient monoid is not the ambient monoid, and may not be wanted.** The
   BFS at `:3048` skips any image whose degree exceeds `max_degree`, and
   `max_degree` is taken from the *realized* classes. So the set the complement
   is measured against is a degree-truncated monoid whose truncation level is set
   by the same data being measured. `misses` is therefore not the count of
   unreached ambient orbits. The composition monoid in `F_l[x]` is infinite
   because degrees strictly grow; making it finite this way is the shortcut that
   produces a finite answer rather than the target one. Per C3 the prior question
   is whether the monoid is an object worth computing at all.

2. **`--branches` computes the wrong constant.** The fold starts
   `zero = w[1]`, the initial translation `A_0`. The criterion for the even
   quartic applies in the depressed variable, where the fold starts at `0`. The
   two agree only when `A_0 = 0`. Every number this flag has printed is on the
   shifted quantity.

3. **`--branches` runs inside the per-target walk.** It sits in the
   `ComputeSpectrumCore` loop (`:1514`), not the sweep, so it inherits the path
   that does not scale. The fold has the shape and cost of the `--critical` pass
   and belongs there.

4. **The spectrum walk enumerates paths, and D5 is over words.** `kCap = 400000`
   at `:1085` and `:1568` was read as a truncation to be raised. It is not:
   paths into `p = 987391` number 50450299988 against 6361 words, so the cap
   sits between two different objects and raising it changes nothing. The
   collapse is telescoping — an `n = 1` run from `v` to `p` adds `p - v`
   whatever route it takes — so words are indexed by where the `n >= 2` edges
   sit. `EnumerateWords` in `graph/words.cc` computes the word set from the
   cone. `SpectrumCore::capped` (`:1510`) still reports a truncated answer as an
   answer; `kMonoidBound = 4000000` (`:1866`) reports and skips instead.

5. **The odd-only modulus filter excludes the base.** Partly fixed:
   `ParseEllList` (`:1895`) no longer rejects 2, so `--ells 2 --e E` reaches the
   base and produces the tables quoted in A1 and A3. `IsSmallPrime` always
   accepted 2. Still open: the `ell = 3; ell += 2` loops driven by `--ell-max`
   at `:1471`, `:1801` and `:2451` skip it, so anything reached only through `--ell-max`
   still never sees the base.

6. **`FaithfulDegree` reports a gap it does not close.** It prints the `d` at
   which the degree-bounded maps become separated by `GF(l^d)`-points, and every
   evaluation in the tree stays at `d = 1`. Per A2 that is the whole distance
   between item 3 as implemented and item 3 as it should be.

7. **`--critical`'s ramification criterion is asserted, not established.** The
   report at `:2842` states that `l` ramifies for `p` iff some critical value of
   a chain into `p` is congruent to `p` mod `l`. Checked on one case
   (`p = 137`) and not in general. B1 now says what the pass *should* compute:
   critical values as elements of `F_2[t]`, which by notation §2.4 depend only on the
   `n >= 2` blocks and so read `higher_parts` alone.

8. **`root` as a column in `spectra`**. If the initial translation only shifts
   the variable, `root` is redundant against the edge sequence for anything
   Galois-theoretic. Not verified either way; it is stored as a key today.

9. **`--format dot` for the spectrum family is only wired to the orbit space.**
   The format is refused without `--orbits`, and refused for more than one
   modulus. The other family outputs have no drawing.

10. ~~**No sheaf-side quantity exists anywhere in the tree.**~~ **Settled.**
    `graph/point_count.cc` computes the trace function `#P^{-1}(c)` over
    `F_{p^d}`, its correlation and moments; `graph/as_fold.cc` computes
    `deg P_red` and `dim H^1_c`; `graph/ramification.cc` computes places,
    ramification indices, residue degrees and the branch locus;
    `graph/characters.cc` computes Gauss and Jacobi sums exactly.
    `graph/chain_forms.cc` builds the fiber product. Reachable as
    `pp-graph --mode chain`. What remains missing is the monodromy group of A2,
    and by A2 item 3's orbit pass is still not a partial step toward it.

11. **`--critical` and the orbit pass still carry absolute per-node sets.** Both
    predate the normalization in A1 and neither runs much past 1e7. `--shared`
    covers only the tower. Per B'4 the fix is mechanical: they carry the same
    kind of state and `S = P - p` applies to them verbatim.

12. **`--ells 2` at `e = 1` is a correct but empty computation.** By A1 it
    returns the degree and nothing else. It is worth running only as the base of
    a tower or as the degree-spectrum readout of B'1; a single `e = 1` sweep at
    `l = 2` reported on its own says only `floor(log_3 p_max)`.

13. **Nothing in `src/graph/` reads a table.** `ComputedMask` and
    `ComputedHigher` recompute from FLINT what `flat_parts` and `higher_parts`
    store. `FlatCone`'s `MaskFn` callback exists so a table-backed mask can be
    supplied and nothing supplies one. None of it threads, while `Session`
    already shards. Whether a chain computation should become a second plan type
    beside `ScanPlan` or a stage over `ScanStream` is undecided.
