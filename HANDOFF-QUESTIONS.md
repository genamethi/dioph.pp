# Fitting items 1-4 to the applied ell-adic cohomology approach

Reference: FKMS, *Lectures on Applied ell-adic Cohomology*,
`/media/extssd/research/library/math/applied-ell-adic.pdf`. Section numbers below
are from it.

Part A is settled and applies as corrections to items 1 through 4 of
`markdown/HANDOFF.md` on `eval-analysis-engine`. Part B is what still has to be
worked out. Part C maps the code on `tui-query` to the mathematics and lists
what is known to be wrong or off-goal. Implementation is the reader's call
throughout.

Code references are `native/src/graph/pp_graph.cc` unless noted, and are line
numbers as of `d77040b` on `tui-query`. They rot on the first edit to that file;
`git log -L` or a grep for the named symbol is the reliable way back to one that
has moved. The items being discussed live on `eval-analysis-engine`, which has
no `native/src/graph/` at all, so the prose and the code it refers to are never
checked out together.

## A. Corrections

**A1. The base is `F_2` and `ell` is the coefficient prime. The tower over the
base is where the `m`-record lives.**

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
Definitions, the operator `T_{q,n}(S) = (S+q)^n - q^n`, the Kummer and Lucas
filters on its coefficients, and the measured tables are in
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

The code already contains the acknowledgment of this gap without acting on it.
`FaithfulDegree` (`:1884`) computes the smallest `d` with `l^d > D` for the
degree bound `D` in play and prints it as "faithful on `GF(l^d)`-points"
(`:2560`), but every computation in the tree evaluates at `d = 1`. Item 3's
request for a drawing is a request to draw the monodromy object, not the orbit
digraph currently emitted at `:2538`.

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

**B1. Which construction attaches a sheaf to a chain.**

Items 1 through 4 produce polynomial maps. A map is not a sheaf and carries no
trace function, so nothing attaches directly. Two constructions are available
and they give different objects.

*Pullback.* A map `P` and a base sheaf give `P^*L_psi` or `P^*L_chi`, rank 1,
trace function `psi(P(x))`. In characteristic 2, Artin-Schreier reduction
identifies `L_psi(f^2)` with `L_psi(f)`, so pulling back along an even-exponent
edge may collapse. Whether that kills the construction or is the content of it
is not clear either way. Note that at level one there is nothing left to pull
back along: `P ≡ x^N` by A1, so the whole family degenerates to the `N`-th power
map and the question is really what `P^*` means over `Z/2^e` for `e > 1`.

*Pushforward.* `P_*Q_ell` has rank `deg P = prod n_i`, is lisse wherever `P` is
etale, and its trace function at `x` is `#P^{-1}(x)(F_q)`, the number of points
in the fiber. Those three are standard for any finite `P`; the stalk is the
permutation representation on the geometric fiber, so the Frobenius trace counts
rational points of the fiber.

Two further statements are standard only for *separable* `P`: that the branch
locus is the set of critical values, and that the monodromy of the permutation
representation is the Galois group of the Galois closure. Over `F_2` an
even-exponent edge is inseparable, `x -> x^2` being Frobenius with identically
vanishing derivative, so neither can be assumed here. That is the same fault
line as the Artin-Schreier collapse above, reached from the other side, and it
is the thing to settle first.

If those two do survive in some form, the pushforward reading is the one to
build on. The fiber count is the `k` structure the project already measures; the
branch locus of a composite of unicritical maps would be its postcritical set,
making `--critical` (`:2331`) a computation of the singular locus rather than
something analogous to it; and A2's monodromy and the Galois-group question
become the same question asked twice. What remains after that is where the
pullback still has a role.

Nothing in the code builds a sheaf, a trace function, a pullback or a
pushforward. `--critical` carries each node's set of critical values mod `l`
through the ascending sweep using a precomputed per-`(l, c, n)` transition
table (`:2333`), which is the nearest existing object either way.

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

One candidate answer to test, not to assume: under the pushforward reading the
rank is `prod n_i`, and conductor is at least rank, so a bounded conductor would
mean a fixed chain with `q = 2^d` growing. That would agree with A1's statement
that the growing family is the degree-`d` points. Both halves of that need
checking, the conductor bound against the paper's definition and the agreement
against what the chain data actually contains, before it counts as an answer.

What the rank does is now exact rather than a worry. `N = prod n_i` and a chain
into `p` from a root `q_0 >= 3` has `p > q_0^N`, so `N <= floor(log_3 p)`. That
bound is attained and every value below it occurs: at `e = 1` the class count
equals `floor(log_3 p_max)` in all seven windows measured from 1e5 to 1e10, and
the largest level-one state set has exactly that many elements. So rank does not
grow with the window except logarithmically, and it does not depend on `d` at
all — but the family it is uniform over is a fixed chain, which is not the
family items 1 through 4 range over. That tension is the real content of B2.

Nothing computes a conductor, a rank, or a Swan conductor. Degree is available
from the word layout in `ComputeSpectrumCore` (`:1474`) and is used as a
truncation bound in the orbit pass (`max_degree`, `:2437`); `--critical`'s
bitmask counts critical values mod `l`, which is a count of candidate
singularities and not the conductor.

**B3. Which sum is supposed to exhibit cancellation.**

The paper is organised around sums of trace functions: over `F_q` (§4), short
intervals (§7), primes (§9), bilinear (§10). Items 1 through 4 ask for structure
and never name a sum.

Work out what the sum is and what a bound on it would establish. `k(p)` is a
count over `m <= log_2 p` of an arithmetic condition, not a sum over
`F_q`-points, so if `k` is the target then producing the trace-function sum it
corresponds to is itself the work.

Every output in the tree is a count or a set size (`Family census`, `:2224`;
orbit report, `:2560`). No signed quantity is summed anywhere, so there is
nothing to point cancellation at yet.

## B'. Next direction

Stated in the same register as the rest: these are exact questions with exact
answers, and a fitted growth law is not an answer to any of them. Definitions
and the measured tables behind each are in `markdown/math/notation.md`.

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

**B'3. Read the chain off the Hermite vector.** For a word of degree `N` with
partial degrees `N_i`, the coefficients above index `N - N_1` are those of `x^N`
and depend on `N` alone, and `c_i` first appears at index `N - N_i` with linear
coefficient `(N / N_i) c_i`. So the Hermite grading is a depth filtration of the
chain and the constants are a linear readout of it. Settle whether the support
alone determines the skeleton. `SegKey` is already that support, so this is a
grouping question over data the tree emits today.

**B'4. Wire `--critical` and the orbit pass onto the shared state.** Both still
carry per-node sets keyed by the absolute polynomial. The normalization of A1
applies verbatim, and without it neither runs past about 1e7.

## C. Code to mathematics

### C1. Item 1, the graph, the basis, and the parameterization

| Code | Object |
|---|---|
| `ReadEdges` (`:532`), `SolveQ` | the generator set `{sigma_{m,n} : x -> x^n + 2^m}` realized by the queried window; `q` recovered algebraically from `(p, m_k, n_k)` |
| `RunHasse` (`:2717`), `EmitDot` (`:1118`) | the prime poset, cover relation, Dilworth width, minimum chain decomposition; DOT with Mirsky levels as `rank=same` |
| `RunCompose` (`:921`), DOT at `:1021` | the composite/ancestor structure with edge labels `(m, n)` |
| `EmitConeDot` (`:1841`), reached from `:1601` | the ancestral cone of a single target; roots boxed, target highlighted, `n >= 2` edges thickened |
| DOT at `:868` | the congruence-refinement poset over `l`, from mode `hasse` |
| `ToHermite` (`:644`, `:1078`, `:1581`), `hermite_modl.cc` | `He_i` decomposition of a chain polynomial; `hermite_basis` table written at `:702` |
| `SegKey` (`:1940`), `SegKeyModL` (`:1951`) | the `He_i` support of a chain, exact and mod `l`; this is the grouping key item 1's third bullet asks for |
| `HermiteEvalModL`, `HermiteRootsModL` (`:1230`, `:1274`, `:1347`) | roots of `He_n` mod `l`, feeding the congruence refinement |
| `MaterializeColumns` (`:2640`) | `<ns>.spectra` with `p, root, degree, chains, skeleton, hermite` |
| `PolyKey` / `KeyToPoly` / `SeedKey` (`:1699`-`:1735`), `nmod_poly` throughout | coefficient vectors over `F_l` as the state datum; replaced the hand-rolled `MulL`/`PowL`, removing the degree-32 and `l < 256` caps |
| `config.h` `kFields` table, `config_gen.cc`, `Conf::defaulted` | one declaration drives flags, config keys and the generated `example.config.lua`; a missing key now defaults instead of being fatal. This is what item 1's N.B. asks for when it says moduli and ranges should be parameters |
| `--ells` / `ParseEllList` (`:1745`), used at `:779` | replaced the hardcoded ten-element modulus list in mode `hasse` |
| `pp_catalogd.cc` `/v1/config`, `pp_iceberg_rest.cc` `AdvertisedWarehouse` | warehouse advertisement and a read/write mismatch guard; infrastructure, no mathematical content |
| `pp_graph_exp.cc` deleted | carried a hardcoded warehouse path and read no config |

### C2. Item 2, the spectrum

| Code | Object |
|---|---|
| `ComputeSpectrumCore` (`:1387`) | item 2's spectrum for one target: ancestor cone, admissible `n >= 2` segment sequences, canonical words with multiplicities. The definition executed literally |
| `SpectrumRow` (`:1737`), family mode `--k` | the spectrum of a whole family `{p : k(p) = K}` off one shared edge read |
| `ClassSweep` (`:1798`) | the same class sets by a single ascending pass; `p = 2^m + q^n > q` makes ascending order topological, so each node's state is built from its parents' |
| `--sweep` agreement check (`:2425`) | the sweep against the per-target walk. The only evidence the fast path computes the definition and not something adjacent |

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
| `SharedSweep` (`65bd8bb`) | `S = P - p` as the carried state: invariant along `n = 1` edges, `S_p = (S_q + q)^n - q^n` on `n >= 2`. The normalization item 3 should have asked for |
| `ZeroConst` (`:1894`) | the polynomial with its constant term zeroed. Read as the translation quotient it is item 3's orbit representative; read against A1 it discards the exponents and is the wrong normalization for the tower |
| ambient BFS (`:2459`-`:2490`) | the monoid `M_l` generated by the reductions of the edges actually present, closed under the generator action from the seed `x` |
| `realized` / `missed` (`:2435`, `:2501`) | which orbits the queried family attains and which of the ambient it does not |
| `signatures` (`:2451`) | the per-prime realized set, counted by distinct signature. This is the per-prime reading item 3 asks for |
| orbit DOT (`:2538`) | the quotient as a digraph: nodes are orbits labelled by `PolyLabel` (`:1907`), realized ones filled, complement dashed, `rank=same` by degree, arcs the generator action |
| `FaithfulDegree` (`:1884`) | the `d` at which `GF(l^d)`-points separate maps of the degree in play; reported, never used |

### C4. Item 4, the tower

| Code | Object |
|---|---|
| `ClassSweep(mod = l^e)` (`:2609`) | the class sets over `Z/l^e`, keyed by the absolute polynomial |
| `TruncKey` (`:1925`) and the `e`-loop (`:2607`) | levels compared by truncating the top-level answer to each lower modulus; a mismatch is a hard failure |
| per-`e` family-class and per-prime-set counts (`:2628`) | the stratification question as item 4 poses it: whether the realized set changes only at isolated `p` |
| `--shared` (`65bd8bb`) | the same counts off `S = P - p`, interned and shared between primes; `--ells 2` now reaches the base. Agrees with `ClassSweep` at `l = 2, 3, 5, 7` on every level checked, and is what makes 1e10 reachable |
| `--k` taking a list or range (`65bd8bb`) | the stratification read per `k` off one sweep, since the sweep is over nodes and `k` only selects which nodes are reported |

### C5. Known wrong or off-goal

1. **The ambient monoid is not the ambient monoid, and may not be wanted.** The
   BFS at `:2478` skips any image whose degree exceeds `max_degree`, and
   `max_degree` is taken from the *realized* classes at `:2437`. So the set the
   complement is measured against is a degree-truncated monoid whose truncation
   level is set by the same data being measured. `misses` at `:2503` is
   therefore not the count of unreached ambient orbits. The composition monoid
   in `F_l[x]` is infinite because degrees strictly grow; making it finite this
   way is the shortcut that produces a finite answer rather than the target one.
   Per C3 the prior question is whether the monoid is an object worth computing
   at all.

2. **`--branches` computes the wrong constant.** At `:2115` the fold starts
   `zero = w[1]`, the initial translation `A_0`. The criterion for the even
   quartic applies in the depressed variable, where the fold starts at `0`. The
   two agree only when `A_0 = 0`. Every number this flag has printed is on the
   shifted quantity.

3. **`--branches` runs inside the per-target walk.** It sits in the
   `ComputeSpectrumCore` loop, not the sweep, so it inherits the path that does
   not scale. The fold has the shape and cost of the `--critical` pass and
   belongs there.

4. **`kCap = 400000`** at `:956` and `:1441` truncates the compose BFS and the
   admissible-sequence enumeration. `SpectrumCore::capped` sets a flag and
   carries on, so a truncated answer is reported as an answer. `kMonoidBound`
   (`:1739`) does report and skip rather than truncate silently.

5. **The odd-only modulus filter excludes the base.** Partly fixed in
   `65bd8bb`: `ParseEllList` no longer rejects 2, so `--ells 2 --e E` reaches
   the base and produces the tables quoted in A1 and A3. `IsSmallPrime` always
   accepted 2. Still open: the `ell = 3; ell += 2` loops driven by `--ell-max`
   at `:788`, `:1346`, `:1676`, `:2044` skip it, so anything reached only
   through `--ell-max` still never sees the base.

6. **`FaithfulDegree` reports a gap it does not close.** It prints the `d` at
   which the degree-bounded maps become separated by `GF(l^d)`-points, and every
   evaluation in the tree stays at `d = 1`. Per A2 that is the whole distance
   between item 3 as implemented and item 3 as it should be.

7. **`--critical`'s ramification criterion is asserted, not established.** The
   report at `:2352` states that `l` ramifies for `p` iff some critical value of
   a chain into `p` is congruent to `p` mod `l`. Checked on one case
   (`p = 137`) and not in general.

8. **`root` as a column in `spectra`** (`:2657`). If the initial translation
   only shifts the variable, `root` is redundant against the edge sequence for
   anything Galois-theoretic. Not verified either way; it is stored as a key
   today.

9. **`--format dot` for the spectrum family is only wired to the orbit space.**
   `:1974` refuses the format without `--orbits` and `:2046` refuses more than
   one modulus. The other family outputs have no drawing.

10. **No sheaf-side quantity exists anywhere in the tree.** No trace function, no
    conductor, no rank, no monodromy group. B1 through B3 are entirely
    unimplemented, and by A2 item 3's current implementation is not a partial
    step toward the object it should produce.

11. **`--critical` and the orbit pass still carry absolute per-node sets.** Both
    predate the normalization in A1 and neither runs much past 1e7. `--shared`
    covers only the tower. Per B'4 the fix is mechanical: they carry the same
    kind of state and `S = P - p` applies to them verbatim.

12. **`--ells 2` at `e = 1` is a correct but empty computation.** By A1 it
    returns the degree and nothing else. It is worth running only as the base of
    a tower or as the degree-spectrum readout of B'1; a single `e = 1` sweep at
    `l = 2` reported on its own says only `floor(log_3 p_max)`.
