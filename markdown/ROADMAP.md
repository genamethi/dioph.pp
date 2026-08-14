# ROADMAP: an arithmetic geometry engine over the warehouse

## Premise

Assumed in place: `core` (sieve and partition generation), `generate`, the
Iceberg catalog and commit path, `source_scan` / `query_service` / `ScanPlan`,
the TUI, the `pp` Lua CLI, and the three tables

    primeparts.primes        p, k, prime_rank
    primeparts.flat_parts    p, hit_mask            (n = 1 edges, as a mask over m)
    primeparts.higher_parts  p, m_k, n_k, q_k       (n >= 2 edges)

Each also carries `p_bucket_version` and `p_bucket`. `flat_parts` holds one row
per `p` **that has at least one `n = 1` parent** — no row is written when the mask
would be zero, which is why the count of primes with no `n = 1` representation is
a difference of table cardinalities rather than a scan.

Assumed absent: everything algebraic. No `pp-graph`, no Hermite work, no
varieties, no characters, no fields, no valuations, no sheaves. This document is
what to build in their place, in what order, and what each stage is for.

The object throughout is

    p = 2^m + q^n,   p and q odd primes,

read as a directed graph on the odd primes whose paths compose to polynomials.
Definitions are in `markdown/math/notation.md`.

## Design stance

**Parents and elements.** No bare values. An element carries a pointer to the
structure it belongs to and operations dispatch through the parent, not through
the C++ type. `2 mod 7`, `2 in Z`, and `2 in Z_2` are different elements with
different arithmetic and are never interchangeable by accident. This is the
Sage/Magma idiom and it is what makes coercion, base change, and reduction
checkable rather than conventional.

**Functors are objects.** A functor holds its action on objects and its action
on morphisms, and is written against an interface (`Ring`, `Scheme`, `Sheaf`)
rather than a concrete type. `Spec` applied to `Z/2^e` and to `F_q[t]` is one
implementation. That is what "general enough to apply to subclasses" means
operationally: a new ring gets the geometry for free, and a functor that cannot
be stated on the interface is evidence the interface is wrong.

**Every invariant has two routes.** One exact and structural, one experimental
and brute-force, and the engine runs both and compares. The warehouse supplies
the experimental side at scale; small fields supply it locally. An invariant with
one route is not yet trusted. This is a subsystem (`check/`), not a test suite,
and it is what makes this a laboratory rather than a calculator.

**Anything implemented is exposed to Lua.** This is a rule on every object the
engine defines, not a stage that happens once. An object that cannot be held,
printed, compared and composed from the REPL is not finished. The cost is
front-loaded: the first binding settles identity, printing, equality, and how a
value enters a deferred expression, and every parent after that is cheap. Where
the arithmetic actually happens is an implementation detail the Lua side never
sees — a polynomial is a symbolic object built the same way regardless of which
engine evaluates it.

Two constraints inherited from what the current work established. They are design
constraints, not optimizations:

- The graded structure lives on the `n >= 2` edges alone; `n = 1` edges carry
  reachability and nothing else. Anything materializing one row per `n = 1` edge
  is both mathematically confused and unable to run.
- Chains are indexed by words, not paths. A run of `n = 1` edges from a node `q`
  up to `p` contributes `p - q` however it is routed. Enumerating routes is
  enumerating the wrong object and no cap makes it the right one.

Two symbols are fixed for the whole document and are never exchanged, per
`notation.md`: `l` is the reduction modulus of the tower and nothing else, and
the sheaf coefficient prime is always written out as `ell`. Likewise
"coefficients" unqualified is never used: the polynomials' `Z`-coefficients, the
residue field, and the sheaf coefficient field `Qbar_ell` are three different
things and each is named.

## The categories, and the functors between them

    Spec : CRing^op -> Sch,  Gamma : Sch -> CRing^op
        Mor_Sch(X, Spec R) = Hom_CRing(R, Gamma(X, O_X))          [01I1]
        so Gamma is left adjoint to Spec; on AffSch they are
        quasi-inverse equivalences and the handedness is vacuous

    FEt_K     ~=~        pi_1(K)-Set     finite sets, continuous action,
                                         basepoint a separable closure [0BND, 0BNE]

    Sch       --Sh--->   (f^*, f_*)      f^* -| f_*, projection formula
    FnField   --Pl--->   Set<Place>      places of a function field
    (Sheaf, +)--cond-->  Div             monoid hom; the conductor is additive in
                                         short exact sequences, so the source law
                                         is direct sum, never tensor
    Div       --deg--->  Z
    Sheaf     --L----->  Qbar_ell(T)     Euler product over places [03UX]

The target of `L` is not `Q(T)` in general. A pushforward `f_* Qbar_ell` has fiber
counts for traces and does land in `Q(T)`; a Kummer sheaf lands in
`Q(zeta_M)(T)`. State it per sheaf or take the common target.

The last four are the spine: a sheaf's global invariants are sums over places of
local ones, and the global object at the end is that sum taken seriously.

## Layers, and where each one is written down

Each layer implements the content of one or more Stacks chapters and takes its
contracts from there, so a contract has a citable home rather than a house
convention.

### Using the tagged build

The build lives at `~/fluid/byo/repos/stacks-project/output/`, refreshed by
`refresh.sh`.

    tagged/chapters/<stem>.pdf   cite from these — tags printed in the margins
    tagged/tags                  21446 lines, "TAG,full_label"
    tagged/book.pdf              the whole thing, one file
    chapters/ORDER.md            chapter order and stems
    web/                         plastex input (book.tex, my.bib, tags), not a site

**Cite tags, never section or lemma numbers.** A tag is permanent and its meaning
never changes; section and lemma numbers move on every rebuild, so a citation
written today against numbering is wrong after the next `refresh.sh`.

Resolve in either direction:

    grep '^0BQ7,' tagged/tags                 # tag -> label
    grep 'section-ramification' tagged/tags   # topic -> tag
    https://stacks.math.columbia.edu/tag/0BQ7 # tag -> the online page

The label's prefix is the chapter stem, so `0BQ7,pione-section-introduction`
means `tagged/chapters/pione.pdf`. Searching labels is the fast way in: the label
vocabulary is stable even when the prose moves.

Before citing the project for anything analytic, check that it is there —
`grep -ci swan tagged/tags` returns 0, and so do `ogg-shafarevich`, `adele`,
`idele`, `hasse-davenport` and `ritt`. Near-misses that are *not* what they look
like: `conductor` matches only `09N2` and `09NW`, the conductor ideal of a finite
birational extension rather than the Artin or Swan conductor; `gauss` matches only
`0FMN`, Gauss-Manin; `herbrand` matches only `02QE`, a Herbrand quotient in Chow.

### Layer to chapter

| Layer | Chapters | Tags |
|---|---|---|
| `algebra/` | Commutative Algebra; More on Algebra, for valuation rings and completions | `00AP`, `05E4` |
| `field/` | Fields; Brauer Groups for the descent parts. Kummer and Artin-Schreier extensions of fields, finite fields, roots of unity | `09FB`, `073X`, `09I6`, `09I7`, `09HY`, `09HW` |
| `scheme/` | Schemes; Morphisms of Schemes; Etale Morphisms of Schemes; Fundamental Groups of Schemes | `01H9`, `01QM`, `024K`, `0BQ7` |
| `local/` | Valuation rings sit in Commutative Algebra and More on Algebra, not in Divisors; ramification has its own homes; Discriminants and Differents; Algebraic Curves for the function-field dictionary and Riemann-Hurwitz | `00I8`, `0EXQ`, `09E3`, `0BSD`, `09E6`, `0DWI`, `0BW9`, `0BRW`, `0BXX`, `0C1B` |
| `sheaf/` | The **Trace Formula** chapter, not Etale Cohomology, for L-functions, Frobenii, ell-adic sheaves and exponential sums; Etale Cohomology for Kummer and Artin-Schreier theory and curve cohomology | `0F5Q`, `03SJ`, `03SL`, `03UL`, `03UU`, `03UX`, `03UY`, `03VB`, `03PK`, `0A3J`, `03R0`, `05BE` |
| `global/` | No Stacks home. Geometric class field theory, adeles and ideles are outside the project | — |

Two things this layer needs have no home in the project at all and must carry an
outside citation the way `global/` does: Swan conductors and
Grothendieck-Ogg-Shafarevich, and Gauss sums with Hasse-Davenport. Katz (*GKM*,
*ESDE*) and Laumon are where those live. `0C1B` is spelled
`curves-section-riemann-hurewitz` in the label — the misspelling is upstream, so
grep for it as written.

## Translation units

New code under `native/src/<layer>/`, headers under
`native/include/primeparts/<layer>/`, namespaces `primeparts::<layer>`.

### algebra — exact arithmetic and the parent/element spine

| TU | Mathematical purpose |
|---|---|
| `algebra/ring.cc` | The commutative-ring interface and element handles. Coercion, characteristic, base-change maps `R -> S`. Concrete parents: `Z`, `Z/n`, `F_q`, `Z_l`, `R[x]`, `R[t]`. |
| `algebra/polynomial.cc` | `R[x]` over any parent: arithmetic, gcd, resultant, discriminant, derivative, composition, factorization, Ritt decomposition. GiNaC carries the symbolic representation — a polynomial is always built as an expression — and FLINT is the arithmetic engine wherever the parent admits one, since GiNaC has no finite fields. Which of the two evaluates a given operation is an implementation question behind one interface, and never surfaces in Lua. |
| `algebra/cyclotomic.cc` | `Z[zeta_M]` as a ring: exact character values and Frobenius eigenvalues. Multiplication, norm, trace, and the Galois action `zeta -> zeta^s`. Without this, character sums are magnitudes rather than numbers. |
| `algebra/adic.cc` | `Z/l^e` and `Z_l` as a projective system: truncation and lift as morphisms, the valuation `v_l`, Teichmuller lift. Raising `e` extends data rather than recomputing it. |

    struct Ring  { characteristic(); zero(); one(); add(); mul(); ... };
    struct Elem  { const Ring* parent; Handle value; };
    Map     coerce(const Ring& from, const Ring& to);      // partial, checked
    Ring    quotient(const Ring& R, Elem modulus);
    System  tower(const Ring& R, Elem l);                  // e |-> R/l^e, with truncations

### field — where the arithmetic becomes Galois

| TU | Mathematical purpose |
|---|---|
| `field/finite_field.cc` | `F_{r^d}`: construction, Frobenius endomorphism, a multiplicative generator with its discrete-log table, subfield lattice, norm and trace, embeddings `F_{r^d} -> F_{r^{de}}`. The character group `Z/(r^d - 1)` is that table. |
| `field/galois.cc` | Galois group of an extension or a polynomial; the permutation action on roots; subgroup lattice; the Galois correspondence as a functor object. Group computation delegated where a library serves. |
| `field/function_field.cc` | `k(x)` and `k(t)`: the field whose places carry the local theory, and the home of the formal base `t` before it is evaluated at 2. |

    Field    finite_field(int r, int d);
    Endo     frobenius(const Field&);                      // x |-> x^r
    int      order_of(int n, int r);                       // least d with r^d = 1 mod n;
                                                           // requires gcd(n, r) = 1, else undefined
    Group    galois(const Poly& f);                        // acts on roots, has blocks()

### scheme — objects and morphisms

| TU | Mathematical purpose |
|---|---|
| `scheme/affine.cc` | `Spec` of a finitely presented algebra; points, closed points, dimension; `Spec` and `Gamma` as adjoint functor objects. |
| `scheme/morphism.cc` | Morphisms as ring maps read contravariantly: composition, degree, fiber over a point, fiber product as a universal property, base change. |
| `scheme/etale.cc` | Finite etale algebras as `pi_1`-sets: the monodromy functor, the block structure of an iterated cover, and the containment of a composite's group in an iterated wreath product. |

    Scheme    Spec(const Ring&);
    Morphism  morphism(const Scheme& X, const Scheme& Y, RingMap phi);
    Scheme    fiber(const Morphism& f, Point y);           // length == deg f only when f is
                                                           // finite locally free [02NX]
    Scheme    fiber_product(const Morphism& f, const Morphism& g);
    bool      is_finite(const Morphism&), is_flat(const Morphism&), is_etale(const Morphism&);

Finite alone does not give constant fiber length. Everything this engine builds
does satisfy it — a finite morphism of regular integral one-dimensional schemes
is automatically flat — but the invariant belongs on the predicate, not on the
signature.

### local — valuations, and ramification as a derived report

Ramification is not a subject here; it is what a valuation extension reports.

| TU | Mathematical purpose |
|---|---|
| `local/valuation.cc` | Discrete valuations on a field: uniformizer, residue field, completion. Extension of a valuation along a finite morphism, yielding `(e, f)` with `sum e_i f_i = deg`. Separability and tameness are predicates on that record, not separate computations. |
| `local/place.cc` | Places of a function field, the closed points of the curve, with residue degrees. The **ramification locus** is the set of places upstairs where `e > 1`; the **branch locus** is its image downstairs. Residue fields here are finite, hence perfect, so `e > 1` and "ramified" coincide — that equivalence is a fact about this setting and should be asserted, not assumed. |
| `local/divisor.cc` | Divisors and conductors as the free commutative monoid on places. The global conductor is the image of the local records under the sum; degree is a homomorphism to `Z`. |

    Valuation   place_at(const FunctionField& K, Point c);
    vector<Ext> extend(const Valuation& v, const Morphism& f);   // {(w, e, f)}
    bool        ramified(const Ext&), wild(const Ext&), separable(const Ext&);
    Divisor     conductor(const Sheaf&);                         // monoid hom
    int64_t     degree(const Divisor&);

Two errors this layer exists to prevent, both easy to ship undetected: reading
ramification off rational-point counts, which conflates `e` with `f`; and reading
it off multiplicity in a characteristic dividing the exponent, where an
inseparable map factors as a perfect power at every point and reports the whole
line as ramified.

### sheaf — rank-1 families, the six operations, cohomology

| TU | Mathematical purpose |
|---|---|
| `sheaf/lisse.cc` | A lisse sheaf on an open: rank, singular places with local monodromy and Swan conductors, and a trace-function evaluator over a chosen field. |
| `sheaf/character.cc` | Characters of the two one-dimensional groups, additive `psi` and multiplicative `chi`. Constructors `L_psi(f)` (Artin-Schreier) and `L_chi(f)` (Kummer). Gauss and Jacobi sums as the pairing between them, valued in `algebra/cyclotomic`. For `chi` and `psi` both nontrivial, `Tr(Frob | H^1_c(G_m, L_chi (x) L_psi)) = -g(chi, psi)` — the Gauss sum is minus the eigenvalue, and `\|g(chi, psi)\| = sqrt(q)` needs that nontriviality; with `chi` trivial the sum is `-1`. |
| `sheaf/operations.cc` | `f^*`, `f_*`, tensor, and the projection formula as an assertable natural isomorphism. The decomposition `[n]_* Qbar_ell = (+)_{chi^n = 1} L_chi`, valid when `gcd(n, r) = 1` and `mu_n` lies in the base field (equivalently `n` divides `q - 1`), and its translated form. Both conditions are preconditions the constructor checks, not background assumptions. |
| `sheaf/cohomology.cc` | `H^i_c`, the Grothendieck-Ogg-Shafarevich Euler characteristic, and Frobenius eigenvalues. GOS computes `chi_c`, not `h^1_c`; reading `h^1_c` off it requires `h^0_c = h^2_c = 0`, which holds for a nontrivial irreducible on an affine open and is checked rather than assumed. |
| `sheaf/lfunction.cc` | `L(T)` as a rational function over the field generated by the traces, assembled as an Euler product over places. |

    Sheaf     kummer(const Character& chi, Elem c);        // L_chi(x - c)
    Sheaf     artin_schreier(const Character& psi, const Poly& f);
    Sheaf     pushforward(const Morphism& f, const Sheaf& F);   // rank *= deg f
    Sheaf     pullback(const Morphism& f, const Sheaf& F);
    Decomp    decompose(const Morphism& power_map, const Sheaf& F);
    int       h1c(const Sheaf&);
    vector<Cyclotomic> frobenius_eigenvalues(const Sheaf&);
    RatFunc   lfunction(const Sheaf&);                     // over Qbar_ell, or the trace field

`decompose` returning "does not split over this field, splits over `F_{q^d}`,
Frobenius orbit size `s`" is a successful outcome as often as a direct sum is.
That failure is what produces an induced representation, and the position along a
chain where it first occurs is an invariant of the word.

### global — the object everything assembles into

| TU | Mathematical purpose |
|---|---|
| `global/adele.cc` | The restricted product over places, and the idele class group of a function field. |
| `global/class_field.cc` | Geometric class field theory: rank-1 lisse sheaves as characters of the idele class group, separated by local conductor rather than by kind. One direction is an equivalence and the other is not: on `G_m` the tame rank-1 lisse sheaves *are* the Kummer sheaves, but the wild ones are not all Artin-Schreier — higher breaks need Artin-Schreier-Witt, and `L_psi` reaches only the `r`-torsion part. |

    Adeles     adeles(const FunctionField& K);
    Character  class_character(const Sheaf& rank_one);
    Local      local_component(const Character&, const Place&);
    Divisor    conductor(const Character&);                // = sum of local conductors

### pp — the problem layer

Everything above is general. This is the only layer that knows the equation.

| TU | Mathematical purpose |
|---|---|
| `pp/cone.cc` | The ancestral cone of a target, read from `flat_parts` masks and `higher_parts` rows. Mask expansion by subtraction; no root extraction on any path. |
| `pp/word.cc` | Words: canonical form with skeleton `[n_1..n_k]`, constants as sums of powers of the formal base, degree `prod n_i`, and an enumeration indexed by blocks rather than routes. |
| `pp/chain.cc` | The four presentations built out of `scheme/`: intermediates as coordinates, two chains glued over a target, the composed morphism, the partial composites as a diagram. |
| `pp/sweep.cc` | The ascending sweep. `p > q` makes prime order topological, so a node's state is built from its parents'. Normalizing by subtracting the target makes the state constant along `n = 1` edges at every modulus with no arithmetic. |

### check — the two-route rule

| TU | Mathematical purpose |
|---|---|
| `check/brute.cc` | The experimental route: exhaustive point counts, direct factorization over small fields, direct path walks at tiny bounds. Deliberately naive and deliberately slow. |
| `check/witness.cc` | The registry pairing each invariant with its two routes and comparing them. A disagreement names the invariant, both routes, and the inputs. |

| Invariant | Exact | Experimental |
|---|---|---|
| chain count into `p` | word enumeration | path walk at small `p` |
| `(e, f)` at a place | factorization over the residue field | point counts, which conflate them; the pair documents that |
| `chi_c`, and `h^1_c` once the outer groups vanish | Grothendieck-Ogg-Shafarevich | growth of the Weil sum over `F_{q^d}` |
| layer decomposition, at `x != c` | `sum_chi chi(x - c)` | fiber counts by brute force |
| Gauss sum, `chi` and `psi` nontrivial | cyclotomic integer | numeric magnitude against `sqrt(q)` |
| splitting degree of a skeleton, `gcd(n_i, r) = 1` | `lcm_i ord_{n_i}(r)` | ascend `d` until every layer splits |
| conductor | sum over places | sharpness of the Weil bound |

### interface

| TU | Mathematical purpose |
|---|---|
| `lua/userdata.cc` | The binding machinery every layer uses — metatables, userdata, and the identity/printing/equality contract — not the place where binding happens. Each layer binds its own objects against this. |
| `lua/expr.cc` | Deferred expressions, so a question composes before any of it executes — the Polars idiom, not the shell-pipeline one. |
| `query/bind.cc` | Predicate pushdown into `ScanPlan`, and materialization of results back into the warehouse as a table. |

## What depends on what

The checkpoints are ordered for reading. The dependencies are not a line.

**Three roots, none of which waits on the others.** `pp/cone.cc`, `pp/word.cc`
and `query/bind.cc` need no algebra whatsoever — only the existing tables and
`parts_expand.h` — so the whole of C1 can proceed with none of the algebra layer
in place. `algebra/ring.cc` and the first Lua binding need no data.
`algebra/cyclotomic.cc` is self-contained arithmetic that touches neither. Those
branches first meet at C3, where a chain becomes a morphism and needs both a
polynomial carrier and words.

**The long pole is one chain with no shortcuts**: `ring -> polynomial ->
finite_field -> function_field -> valuation -> place -> divisor -> lisse ->
operations -> cohomology -> class_field`. Everything else hangs off it.
`cyclotomic` joins only at `character`; `adic` and the tower join nothing else at
all.

**Delegated rather than written**: Galois group computation, factorization, and
Ritt decomposition. Nothing on the long pole requires Ritt.

**Where the design weight sits**, in order: the parent/element/coercion contract;
the functor representation, since a functor that cannot be stated on the
interface means the interface is wrong; and the valuation-extension record, where
`(e, f)`, tame, and separable have to fall out of one computation or ramification
gets recomputed three incompatible ways.

## Checkpoints

Each lands a deliverable reachable from Lua or the TUI. A checkpoint that only
adds headers is not a checkpoint.

### C0 — The spine

TUs: `algebra/ring.cc`, `algebra/polynomial.cc`, `lua/userdata.cc`,
`check/witness.cc`; `algebra/cyclotomic.cc` and `algebra/adic.cc` are parents
that slot in later against the same interface.

This checkpoint is the contract, not a library. What has to be right is what a
parent is, what an element is, how coercion is decided, how a functor is
represented, and how any of it reaches Lua. Two concrete parents are enough to
prove it — `Z` and `Z/n` — because every parent after that is filling in a
settled interface rather than extending it. Getting the interface wrong is the
one mistake here that cannot be paid off incrementally, which is why this is
almost entirely design and very little code.

Consumer: an exact calculator in the `pp` REPL. The warehouse is not involved
yet.

```lua
local R = pp.ZZ:poly("x")
local P = (R.x + 2)^3 + 16
P:degree()                     --> 3
P:base_change(pp.Zmod(32))     --> element of (Z/32)[x]
P:base_change(pp.Zl(2, 5))     --> element of (Z/2^5)[x], with :lift() and :truncate()
```

Check: `truncate(lift(a)) == a` across the tower; cyclotomic norms against known
Gauss sum magnitudes.

### C1 — The equation's objects, off the tables

TUs: `pp/cone.cc`, `pp/word.cc`, `pp/sweep.cc`, `query/bind.cc`.

Consumer: words for a queried set of primes, with no cap that reports a truncated
answer as an answer.

```lua
local S = pp.primes{ p = {1e9, 1e9 + 1e6}, k = 3 }
for p, words in S:words() do
  words[1].skeleton    --> {2, 3}
  words[1].degree      --> 6
end
S:words():materialize("ns.words")
```

Check: word count against a direct path walk at small bounds. The two disagree by
orders of magnitude and the registry records which is the object.

### C2 — Fields, and the field a chain needs

TUs: `field/finite_field.cc`, `field/galois.cc`, `field/function_field.cc`.

Consumer: the splitting degree of a skeleton, across the whole warehouse off
`higher_parts` alone. `d = lcm_i ord_{n_i}(r)` is a function of the skeleton, so
this is one cheap exact query over the entire range rather than a per-target
computation. It requires `gcd(n_i, r) = 1` for every block: when the
characteristic divides an exponent the order is undefined, and that is exactly
the case where the layer is inseparable and has no Kummer description either. The
query returns that block rather than a number.

```lua
local F = pp.field(3, 4)
w:splitting_degree(3)      --> least d with every n_i dividing 3^d - 1,
                           --  or the first block with gcd(n_i, 3) > 1
w:galois()                 --> group; :order(), :is_abelian(), :blocks()
```

Check: the group's order against the degree; block structure against the
skeleton.

### C3 — Schemes, and the four presentations

TUs: `scheme/affine.cc`, `scheme/morphism.cc`, `scheme/etale.cc`, `pp/chain.cc`.

Consumer: a chain as a morphism, its fibers, and two chains glued over a target.

```lua
local f = w:morphism(F)          -- composed
w:graded()                       -- partial composites; last == composed
w:uncollapsed()                  -- intermediates as coordinates
f:fiber(p):length()              --> deg f
f:times_over(g)                  --> the fiber product curve
```

Check: the last graded level equals the composed morphism; fiber length equals
degree at every point.

### C4 — Places, and ramification as a report

TUs: `local/valuation.cc`, `local/place.cc`, `local/divisor.cc`.

Consumer: the local record at every place of a chain, and a conductor that is a
sum rather than a formula.

```lua
for _, v in ipairs(f:places()) do
  print(v.place, v.e, v.f, v.tame, v.separable)
end
f:branch_locus()
f:conductor():degree()
```

Check: `sum e_i f_i == deg f` at every place; inseparable maps do not report as
ramified.

### C5 — Characters, sheaves, cohomology

TUs: `sheaf/lisse.cc`, `sheaf/character.cc`, `sheaf/operations.cc`,
`sheaf/cohomology.cc`, `sheaf/lfunction.cc`, `check/brute.cc`.

Consumer: one layer decomposed, with exact Frobenius eigenvalues, and the
composite's failure located.

```lua
local L = pp.kummer(F:mult_character(a), c)      -- L_chi(x - c)
L:conductor(); L:h1c(); L:frobenius()            --> exact cyclotomic integers
w:layer(1):decompose(F)                          --> characters, or an orbit of size s
w:first_obstruction()                            --> the block where splitting fails
pp.check("layer_sum", w:layer(1), F)             --> one integer, two routes
```

Check: `sum_chi chi(x - c)` against brute-force fiber counts, **at `x != c` only**
— with the convention `chi(0) = 0` the sum vanishes at the center while the fiber
there is a single point, and the center is precisely the place the obstruction
argument turns on, so the exclusion has to be explicit rather than incidental.
Then `|g(chi, psi)| = sqrt(q)` for both characters nontrivial, and GOS against
Weil sum growth once the outer cohomology is known to vanish.

Characteristic 2 is excluded here and the exclusion is stated by the code rather
than implied. Every constant is a sum of `2^m` and vanishes, so all centers
collapse and the decomposition is unblocked and empty; and `y -> y^2` is
Frobenius rather than a cover, while `n = 2` is the overwhelming majority of the
graded layer. An odd base is required, and the base is a parameter.

### C6 — The global object

TUs: `global/adele.cc`, `global/class_field.cc`.

Consumer: additive and multiplicative sheaves presented as one kind of object,
distinguished by local conductor, with the global invariant assembled from the
local ones.

```lua
local A = pp.adeles(K)
local chi = A:class_character(L)       -- rank-1 sheaf as an idele class character
chi:local_at(v):conductor()
chi:conductor():degree()               -- == sum over places
L:lfunction()                          -- Euler product
```

Check: the global conductor equals the sum of the local conductors computed at
C4; the L-function's degree against GOS.

## Where the additive and multiplicative sides meet

The two rank-1 families are the two ways a one-dimensional group is covered:
`℘(z) = z^r - z` with group `F_r`, giving Artin-Schreier sheaves, and `z -> z^n`
with group `mu_n`, giving Kummer sheaves. The first is wild at infinity with
`Swan = deg f_red`; the second is tame everywhere.

They are paired by the Gauss sum, `g(chi, psi) = sum_x chi(x) psi(x)`, the sum
over the group of one character of each kind, which is the Frobenius eigenvalue
on `H^1_c(G_m, L_chi (x) L_psi)`. Structurally that pairing is the additive
Fourier transform on `A^1` against the Mellin transform on `G_m`, with
Hasse-Davenport describing how it moves under field extension — the same
statement as Frobenius acting as the field grows.

The frame holding both is C6. Over a function field, rank-1 lisse sheaves
correspond to characters of the idele class group, and the two families are that
one object seen at different local conductors. That is why C4 precedes C5 and C5
precedes C6: the local records are the content and the global object is their
sum.

## The `Z/l^e` tower

The tower sits on the polynomials' `Z`-coefficients rather than on the residue
field or on the sheaf coefficient field `Qbar_ell`, which is why it wants base 2
while C5 and C6 want an odd base. That is not a tension. `q mod 2^e`
exposes exactly the exponents `m' < e`, so level `e` resolves the exponents below
`e`, and it is the only place the `m`-record survives at all: modulo 2 every
chain polynomial is `x^N` and the class is the degree and nothing else.
`algebra/adic.cc` and `pp/sweep.cc` are all it needs, so it is available from C1
and does not wait on the sheaf layer. Normalize by subtracting the target;
zeroing the constant term deletes the datum the tower exists to recover.
