# The graph in Macaulay2

## Status

Every claim marked **[run]** below was executed in M2 1.26.06 while writing this
and its output is quoted. 

## The object

Nodes are the odd primes. For `p` prime, `K(p)` is the collection of length-two
partitions of `p` into prime powers,

    p = 2^m + q^n,    q prime,   m, n >= 1,

and `k(p) = |K(p)|` is the restricted partition function for that length under
those conditions. `k = 0` means no such partition exists.

There is a directed edge `q -> p`, read "`q` is a parent to `p`", whenever
`p = 2^m + q^n`. The edge is labelled `(m, n)`. That is the whole definition.

`k(p)` is the in-degree. The graph is acyclic because `p = 2^m + q^n > q`.

##

The graph is definable in M2 directly from the definition.
The edge relation is recovered by factoring `p - 2^m` (see about caching).

```m2
needsPackage "Graphs"

-- is r a prime power q^n with q prime, n >= 1?  returns (q,n) or null
primePowerData = (r) -> (
    if r < 2 then return null;
    L := toList factor r;
    if #L != 1 then return null;
    q := L#0#0; n := L#0#1;
    if not isPrime q then return null;
    (q, n))

-- K(p), as a list of (m, q, n)
K = memoize((p) -> (
    out := {}; m := 1;
    while 2^m < p do (
        d := primePowerData(p - 2^m);
        if d =!= null then out = append(out, (m, d#0, d#1));
        m = m + 1);
    out))

k = (p) -> #K(p)
```

**[run]** `K(137)` returns the three parents

    137 = 2^4 + 11^2      137 = 2^6 + 73^1      137 = 2^7 + 3^2

so `k(137) = 3`. Below 200 the `k = 0` primes are `{3, 149}`.

Note: 137 is so small that we may want to check others, but in principle this
should scale.

## Where primality lives


**1. Incidence — needs primality, and is not M2's job at scale.**
Deciding whether `q -> p` is an edge requires knowing `p - 2^m` is a prime
power. In M2 this is `factor`, exact and immediate for small `p`, and hopeless
past `1e6` or so (why?). The warehouse already holds this relation to `1e11`. M2
recomputing it is a convenience for small examples and a cross-check, not a
capability.

**2. Interior — no primality at all.**
Once a chain `q_0 -> q_1 -> ... -> q_k` is fixed, the composite
`P = ( ... (x^{n_1} + 2^{m_1}) ... )^{n_k} + 2^{m_k}` is a polynomial in `Z[x]`,
and everything asked of it — its collapse to a canonical word, its Hermite
expansion, its decomposability, its reduction mod `l`, its factorization over a
finite field — is polynomial algebra. No node value enters. This layer is
entirely M2's, and it is where the interesting structure is.

**3. Endpoints — a membership relation, and the seam.**
The chain is pinned to the graph only by `P(q_0) = q_k` with both endpoints
prime. Because runs of `n = 1` edges collapse (below), the intermediate nodes of
a flat run are not part of the object; only the endpoints are. So primality is
neither inert nor pervasive. It is a boundary condition on a purely algebraic
interior, and the questions about how that affects matters are questions about
that boundary.

The flat/higher split in the warehouse schema is the same split. It is not a
storage optimization.

## The collapse, and why the endpoints suffice

An `n = 1` edge is a translation `t -> t + 2^m`. A run of them composes to a
single translation by the sum of its `2^m`, so how the run is traversed does not
matter — only its total shift, which is `q_k - q_0` and is therefore readable
from the endpoints alone. This is the path-conservation principle, and it is why
a span of `n_j = 1` can be counted by the magnitude of the shift rather than
enumerated.

Folding those runs gives the canonical word

    P = ( ... ((x + A_0)^{n_1} + c_1)^{n_2} + c_2 ... )^{n_k} + c_k,   n_i >= 2,

where `A_0` is the leading flat run and each `c_i` is the `2^{m_i}` of the power
edge together with the flat run that follows it. Every constant is a sum of
distinct powers of two, and its binary digits are the `m`-labels of that stretch.

```m2
-- flat run BEFORE the first power edge is A_0; the run AFTER the i-th folds into c_i
wordOf = (c) -> (
    flats := {}; powers := {}; acc := 0;
    for e in c do (
        if e#1 == 1 then acc = acc + 2^(e#0)
        else (flats = append(flats, acc);
              powers = append(powers, (e#1, 2^(e#0)));
              acc = 0));
    flats = append(flats, acc);
    segs := for i from 0 to (#powers)-1 list (powers#i#0, powers#i#1 + flats#(i+1));
    {flats#0, segs})

wordPoly = (w) -> (P := x + w#0; for s in w#1 do P = P^(s#0) + s#1; P)
```

**[run]** At `p = 137`, enumerating every chain from a root:

    37 chains  ->  6 words  ->  6 polynomials  ->  6 Hermite vectors
    word rebuilds the chain polynomial in every case: true

The skeleton-`[2]` words are `A_0 = 0, 2, 8` with `c_1 = 128, 112, 16`, each
solving `(3 + A_0)^2 + c_1 = 137`. The multiplicity-16 class is `A_0 = 134` with
no blocks: sixteen distinct flat routes from 3 to 137, one word. This reproduces
`collapse_findings.md` exactly, including its `(0,128), (2,112), (8,16)`.

Note what the agreement of the last three counts does *not* establish: that
words inject into polynomials in general. It holds here. The Ritt argument for
it in `misc/retired/chain_hermite_primer.md` §4 is not proved, and stage S5
below is how to attack it.

## Hermite as the convenient representation

The reason the Hermite basis is not an arbitrary choice: expanding
`p = 2^m + (2^{m'} + q'^{n'})^n` binomially produces exactly the shape that
`He` diagonalizes, because `He` is an Appell sequence and therefore shifts like
monomials, `He_n(x + a) = sum_j C(n,j) He_j(x) a^{n-j}`. Translations — the
`n = 1` edges — act cleanly on the basis, so the collapse is visible in the
coefficients rather than hidden behind them.

In M2 this is a change of basis in a free module, which is free:

```m2
He = memoize((n) -> (
    if n == 0 then return 1_R;
    if n == 1 then return x;
    a := 1_R; b := x;
    for i from 1 to n-1 do (c := x*b - i*a; a = b; b = c);
    b))

-- He_i is monic, so top-down subtraction terminates and stays in Z
toHermite = (P) -> (
    d := first degree P; Q := P;
    for j from 0 to d list (
        i := d - j;
        c := coefficient(x^i, Q);
        Q = Q - c * He i;
        (i, c)))
```

**[run]** The six classes at 137, with multiplicities:

    mult  1    He4 + 10 He2 + 27 He0        skeleton [2,2]
    mult  3    He2 + 16 He1 + 81 He0        skeleton [2],  A_0 = 8
    mult  4    He3 +  3 He1 + 110 He0       skeleton [3]
    mult  4    He2 +  4 He1 + 117 He0       skeleton [2],  A_0 = 2
    mult  9    He2 + 129 He0                skeleton [2],  A_0 = 0
    mult 16    He1 + 134 He0                skeleton []

The depth-filtration claim `notation.md` E8 makes — that `c_i` first appears at
index `N - N_i` with linear coefficient `(N / N_i) c_i` — holds on the `[2,2]`
class: `N = 4`, `N_1 = 2`, so `c_1` enters at `He_2` with coefficient `2 c_1`
on top of `x^4`'s `C(4,2)·1!! = 6`, giving `6 + 2c_1 = 10`, hence `c_1 = 2`,
which is the word. **[run]**

E8 as written is nonetheless incomplete: it says the coefficients above
`N - N_i` are those of `x^N` and depend on `N` alone, but the three
skeleton-`[2]` classes have `N - N_1 = 0` and differ at `He_1`, where the
coefficient is `2 A_0` — `16, 4, 0` for `A_0 = 8, 2, 0`. The statement does not
account for the leading translation `A_0`. With `A_0 = 0` it holds on this data.
This is the first thing the bench caught, and it is the kind of thing it is for.

## The dictionary

What the `ROADMAP.md` layers become. The point of the table is how much of it is
not work.

| ROADMAP layer | In M2 |
|---|---|
| `algebra/ring`, `algebra/polynomial` | `ZZ[x]`, `ZZ[t][x]`, `GF(r,d)[y]`. Parents and elements are M2's native model; coercion, base change and `sub` are the language. **Not work.** |
| `algebra/cyclotomic` | `Cyclotomic` package. **[run]** loads. |
| `algebra/adic` | `ZZ[x]/2^e` as a quotient **[run]**; `Padic`, `WittVectors` packages present. |
| `field/finite_field` | `GF(r,d)`. Conway polynomials give *coherent* embeddings across degrees — the thing C2 called "the one that bites" — for free. |
| `field/galois` | **Gap.** No Galois-group package. `factor` gives orbit and block data; the abstract group needs Pari/Sage/Magma. |
| `field/function_field` | `frac(K[t])`; `FunctionFieldDesingularization`, `QthPower` present. |
| `scheme/affine`, `scheme/morphism` | Ideals and ring maps. `Spec` is implicit and does not need representing. |
| `scheme/etale` | `factor(P(u) - P(v))` gives the block structure directly. **[run]** on an example. |
| `local/valuation`, `local/place` | `factor(P - c)` yields `(e, f)` in one call, with `sum e_i f_i = deg`. One computation, not three passes — which was C4's stated design risk. **[run]** |
| `local/divisor` | `WeilDivisors`, or a list of (place, multiplicity). |
| `sheaf/*` | **Gap.** See below. |
| `global/*` | **Dropped.** |
| `pp/cone`, `pp/sweep` | Stays in the warehouse. Data at scale, not algebra. |
| `pp/word`, `pp/chain` | Forty lines of M2, above. |
| `check/brute`, `check/witness` | `RationalPoints2` and direct enumeration for the experimental route; M2's `assert` for the registry. |

The eleven-unit "long pole" of `ROADMAP.md` — `ring -> polynomial ->
finite_field -> function_field -> valuation -> place -> divisor` — is the first
twenty lines of a session. Checkpoints C0 through C4 are setup, not stages.

## What has no answer in M2

**C5 is left open, stated as it stands.** There is no package for `l`-adic or
lisse sheaves, Frobenius eigenvalues on `H^1_c`, Swan conductors,
Grothendieck-Ogg-Shafarevich, or `L`-functions of sheaves, and no near-miss
among the installed packages. Two positions are available and this document
takes neither:

- The invariants that are *curve* zeta data are recoverable experimentally from
  point counts over `F_{r^d}`, which M2 does. That reaches Frobenius eigenvalue
  data without any sheaf machinery, bounded by field size.
- Everything genuinely sheaf-theoretic — the Artin-Schreier fold of
  `notation.md` E12, `Swan_inf = deg P_red`, the conductor as a sum over places
  — has no M2 route and would need a different tool.

E12 is the one part of the sheaf layer with measured content behind it, and it
is also the one that most wants the formal base `t`. That it lands outside M2 is
worth knowing before rather than after.

**C6 is dropped.** Adeles, ideles, and geometric class field theory are out of
scope for this bench.

## Both readings of the base, as one object

Layers are built over `ZZ[t]` and specialized, so `t = 2` is a map rather than a
rewrite. This costs one indirection and keeps the family reading available,
which E12 needs and which the `Z/2^e` tower reads from the other side.

```m2
chain  = (w)       -> ...   -- in ZZ[t][x]
chain  = (w, 2)    -> ...   -- in ZZ[x],       the arithmetic object
chain  = (w, r, d) -> ...   -- in GF(r,d)[y],  the geometric object
```

## Stages

Each produces a number or a table. None produces a library. They are ordered by
dependency, not by importance, and S3 onward are independent of each other.

**S1 — the graph.** Done above. `K`, `k`, the digraph, chain enumeration, the
canonical word, the Hermite expansion, verified against 137.
*Output:* the code, and agreement with `collapse_findings.md`.

**S2 — the graph as a bounded object.** `k(p)` and the word census over odd
primes below `B`, for `B` as large as `factor` allows. Compare the `k`
distribution and the `k = 0` rate against the warehouse's, which is the
cross-check that M2's independent construction agrees with the C++ one.
*Output:* one table; a disagreement is a bug in one of the two.

**S3 — the filtration.** `V_D = span(He_0, ..., He_D)` with `D = floor(log_3 B)`,
as a free module in M2, and `V_{D'}/V_D` as a literal `coker`. Which words
populate each graded piece when it is first reachable.
*Output:* the graded dimensions, and the multiplicity filling each new
direction. `misc/retired/chain_hermite_primer.md` §5 and §7 are the source; both
are unverified.

**S4 — Hermite congruences.** `He_n` factored over `GF(l)` for a range of `l`,
against the power-residue prediction. `math/hermite_congruences.md` claims
`He_3` has nonzero roots in `F_l` iff `3` is a QR mod `l`, at 100% over 93
primes, and `He_p = x^p mod p`. Both are two lines in M2 and neither has been
re-run here.
*Output:* a confirmation or a counterexample, and the factorization type of
`He_n mod l` for `n >= 4`, which is the stated open next step.

**S5 — decomposability.** `factor(P(u) - P(v))` over `ZZ` and over `GF(r)`, for
the words of S2. This is the direct handle on whether distinct words give
distinct polynomials — the Ritt injectivity question of §4 of the primer — and
on where a composite's decomposition first fails.
*Output:* either a word pair with equal polynomials, which kills injectivity and
is the more valuable outcome, or evidence over a real range.

**S6 — the endpoint layer.** For the words of S2, which have prime endpoints and
which do not: the membership relation as a predicate on an algebraic object.
This is the seam, and the only stage where layers 2 and 3 meet.
*Output:* the survival rate of a word as a function of its skeleton, computed
rather than modelled.

**S7 — open.** The Hopf structure of `misc/retired/hopf_structure.md`
(deconcatenation coproduct, antipode as compositional inverse, so the root is
derivable rather than stored). M2 has `AssociativeAlgebras` but no Hopf support;
whether this is buildable or belongs elsewhere is not settled here.

## Verified while writing this

- M2 1.26.06 runs; the graph, `K(p)`, `k(p)`, the digraph, chain enumeration.
- `K(137) = {(4,11,2), (6,73,1), (7,3,2)}`; `k = 0` primes below 200 are `{3, 149}`.
- 37 chains into 137, collapsing to 6 words / 6 polynomials / 6 Hermite vectors.
- The canonical word rebuilds the chain polynomial in all 37 cases.
- Skeleton-`[2]` translations `(A_0, c_1) = (0,128), (2,112), (8,16)`.
- E8's `c_1` readout at `He_{N-N_1}` on the `[2,2]` class.
- `factor(P - c)` gives `(e,f)` with `sum e_i f_i = deg`; `factor(P(u)-P(v))`
  gives block structure; `Cyclotomic`, `Padic`, `PushForward`, `RationalPoints2`
  load; `ZZ[x]/2^e` builds.

## Not verified, and flagged

- Injectivity of word to polynomial beyond `p = 137`.
- E8 as stated, which omits `A_0` (see above); the corrected statement is not
  written down here.
- Every count, rate and distribution quoted in the retired notes and in
  `notation.md` §3-4. None were re-run.

## Environment

M2 is installed from Debian at `/usr/bin/M2`, version 1.26.06. It fails to start
with

    libeantic.so.3: undefined symbol: fmpz_poly_has_real_root

because `libflint.so.24` resolves to the locally built FLINT in `/usr/local/lib`
rather than Debian's. Preload the packaged one:

    LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libflint.so.24 M2 --script file.m2

`--script` must be the first argument. Two M2 parsing traps that cost time here:
`toString #x` mis-parses and needs `toString(#x)`, and a function of one
variable applied to a `Sequence` receives it spread as multiple arguments — so
the word must be a `List`, not a `Sequence`.
