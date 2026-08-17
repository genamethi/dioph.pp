# ROADMAP in Macaulay2

`ROADMAP.md` checkpoints C0–C5 in M2. Labels are the ROADMAP's.

Stacks tags resolve with `grep '^0BQ7,' ~/fluid/byo/repos/stacks-project/output/tagged/tags`.
**[run]** = output from M2 1.26.06, quoted verbatim.

M2 parsing: `toString(#x)` not `toString #x`; `(#L)-1` inside ranges; return `List` not `Sequence`
from constructors — a one-argument function applied to a `Sequence` receives it spread.

---

# C0 — the spine

TUs `algebra/ring.cc`, `algebra/polynomial.cc`, `algebra/cyclotomic.cc`, `algebra/adic.cc`,
`lua/userdata.cc`, `check/witness.cc`. Stacks `00AP`, `05E4`.

```m2
R = ZZ[x]
P = (x + 2)^3 + 16
first degree P                                  -- 3
S = ZZ[x]/ideal(32_(ZZ[x])); sub(P, S)          -- Z/2^5 coefficients   [run] builds
needsPackage "Cyclotomic"                       -- [run] loads
needsPackage "Padic"                            -- [run] loads
needsPackage "WittVectors"                      -- [run] loads
```

*Check* (ROADMAP): `truncate(lift(a)) == a` across the tower; cyclotomic norms against known Gauss
sum magnitudes. Not run.

---

# C1 — the equation's objects, off the tables

TUs `pp/cone.cc`, `pp/word.cc`, `pp/sweep.cc`, `query/bind.cc`.

## C1.a — graph, K(p), chains, word, Hermite

```m2
needsPackage "Graphs"
R = ZZ[x]

primePowerData = (r) -> (
    if r < 2 then return null;
    L := toList factor r;                        -- see C1.d
    if #L != 1 then return null;
    q := L#0#0; n := L#0#1;
    if not isPrime q then return null; (q,n))

K = memoize((p) -> (out := {}; m := 1;
    while 2^m < p do (d := primePowerData(p - 2^m);
        if d =!= null then out = append(out,(m,d#0,d#1)); m = m+1); out))
k = (p) -> #K(p)

chainsInto = memoize((p) -> (ps := K p;
    if #ps == 0 then return {{}};
    flatten (for e in ps list (for c in chainsInto(e#1) list append(c,(e#0,e#2,e#1))))))

chainPoly = (c) -> (P := x; for e in c do P = P^(e#1) + 2^(e#0); P)

-- D4: flat run before the first power edge is A_0; the run after the i-th folds into c_i
wordOf = (c) -> (flats := {}; powers := {}; acc := 0;
    for e in c do (if e#1 == 1 then acc = acc + 2^(e#0)
        else (flats = append(flats,acc); powers = append(powers,(e#1,2^(e#0))); acc = 0));
    flats = append(flats,acc);
    {flats#0, for i from 0 to (#powers)-1 list (powers#i#0, powers#i#1 + flats#(i+1))})
wordPoly = (w) -> (P := x + w#0; for s in w#1 do P = P^(s#0) + s#1; P)

He = memoize((n) -> (if n == 0 then return 1_R; if n == 1 then return x;
    a := 1_R; b := x; for i from 1 to n-1 do (c := x*b - i*a; a = b; b = c); b))
toHermite = (P) -> (d := first degree P; Q := P;
    for j from 0 to d list (i := d-j; c := coefficient(x^i,Q); Q = Q - c*He i; (i,c)))
```

**[run]** `K(137) = {(4,11,2),(6,73,1),(7,3,2)}`, `k(137) = 3`. `k = 0` below 200: `{3,149}`.
Digraph on odd primes below 200: 45 vertices, 105 edges, acyclic.

**[run]** `37 chains → 6 words → 6 polynomials → 6 Hermite vectors`;
`wordPoly(wordOf c) == chainPoly c` in all 37.

    mult  1    He4 + 10 He2 + 27 He0        [2,2]
    mult  3    He2 + 16 He1 + 81 He0        [2],  A_0 = 8
    mult  4    He3 +  3 He1 + 110 He0       [3]
    mult  4    He2 +  4 He1 + 117 He0       [2],  A_0 = 2
    mult  9    He2 + 129 He0                [2],  A_0 = 0
    mult 16    He1 + 134 He0                []

*Check* (ROADMAP): word count against a direct path walk at small bounds. Here 37 against 6.

## C1.b — the word as a point on a variety

`σ: A^{k+1} → A^N`, `(A_0, c_1..c_k) ↦` coefficient vector of `wordPoly`. Eliminate the parameters.

```m2
imageIdeal = (skel) -> (
    k := #skel; N := product skel;
    S := QQ[getSymbol "a0", apply(k, i -> getSymbol("c"|toString i)),
            apply(N, i -> getSymbol("u"|toString i)), MonomialOrder => Eliminate (k+1)];
    xr := S[getSymbol "X"]; X := xr_0;
    P := X + (S_0)_xr;
    for i from 0 to k-1 do P = P^(skel#i) + (S_(i+1))_xr;
    ideal selectInSubring(1, gens gb ideal apply(N, i -> S_(k+1+i) - lift(coefficient(X^i,P), S))))
```

**[run]**

| skeleton | deg | codim | gens |
|---|---|---|---|
| `[2,2]` | 4 | 1 | `u3³ − 4u2u3 + 8u1` |
| `[3,2]` | 6 | 3 | 7 |
| `[2,3]` | 6 | 3 | 17 |
| `[2,2,2]` | 8 | — | large, degree-7 |

`P(q_0) = p` is a hyperplane cutting the image. `codim = N − (k+1)` in the three computed cases.

Vary: skeletons with `∏ n_i ≤ 12`.

Injectivity per skeleton, by inverting the elimination. `[2,2]`: `u3 → A_0`, `u2 → c_1`,
`u0 → c_2`, invertible in characteristic ≠ 2.

Intersection of two equal-degree skeletons: `imageIdeal{3,2} + imageIdeal{2,3}`, then `decompose`.
D4 forces `c_i ≥ 2` (`notation.md:56-59`); the `x^a ∘ x^b = x^b ∘ x^a` move needs the intermediate
constant to vanish.

## C1.c — the `Z/l^e` tower

`algebra/adic.cc` and `pp/sweep.cc` only, so available from C1. Normalize `S = P − p`.

`notation.md:105-113` (E4): mod 2 every chain polynomial is `x^N`. Not run.

## C1.d — incidence without `factor`

- `3 | p − 2^m` iff `p ≡ (−1)^m (mod 3)`. When it holds a prime power there is `3^n`, resolved by a
  40-entry table — `native/src/core.c:466,481`. Half the `m`-slots.
- Same for every small `ℓ` with period `ord_ℓ(2)`. Two distinct small `ℓ` on one slot rejects it.
- `q = 3` edges are `p = 3^n + 2^m`: ~1000 `(n,m)` pairs to `1e12`. `notation.md:210-214`.
- `n = 1` layer is `Θ(π(B) log B)`.

---

# C2 — fields, and the field a chain needs

TUs `field/finite_field.cc`, `field/galois.cc`, `field/function_field.cc`.
Stacks `09FB`, `073X`, `09I6`, `09I7`, `09HY`, `09HW`.

`GF` uses Conway polynomials, so embeddings `F_{r^d} → F_{r^{de}}` are compatible by construction.

```m2
F = GF(7,1); S = F[y]
g = y; for e in {(3,2),(5,3)} do g = g^(e#1) + (2_F)^(e#0)
factor g                        -- [run] (y^3+y^2+2y-3)(y^3-y^2+2y+3)
```

*Check* (ROADMAP): group order against degree; block structure against skeleton.

`ROADMAP.md:444-449` gives `splitting_degree` as `d = lcm_i ord_{n_i}(r)`. For `[2,3]` over `F_7`
that is 1; the composite's roots first appear at `d = 3` — **[run]** root counts `0, 0, 6, 0` for
`d = 1..4`.

No Galois-group package. `factor` gives orbits and blocks.

---

# C3 — schemes, and the four presentations

TUs `scheme/affine.cc`, `scheme/morphism.cc`, `scheme/etale.cc`, `pp/chain.cc`.
Stacks `01H9`, `01QM`, `024K`, `0BQ7`; adjunction `01I1`; `FEt_K ≃ π_1-Set` `0BND`, `0BNE`;
fiber length under finite locally free `02NX`.

Uncollapsed to composed, by elimination (`AG_Chains_conv.md:146-158`):

```m2
S = QQ[q0,q1,p, MonomialOrder => Eliminate 1]
I = ideal(q1 - (q0^2 + 8), p - (q1^2 + 16))
eliminate(q1, I)
```

Étale block structure:

```m2
K = GF(7,1); W = K[s,t]
gs = s; gt = t; for e in {(3,2),(5,3)} do (gs = gs^(e#1)+(2_K)^(e#0); gt = gt^(e#1)+(2_K)^(e#0))
factor (gs - gt)                 -- [run] (s+t)(s-t)(s²+3t²-3)(s²-2t²-1)
```

`s − t` is the diagonal. Fiber product is `W/ideal(gs - gt)`.

Pushforward: `AG_Chains_conv.md:468` states `π_* O_{X_0}` is free of rank `N` with the Hermite
polynomials a basis. `needsPackage "PushForward"` **[run]** loads; `pushFwd` of `QQ[p] → QQ[x]`,
`p ↦ chainPoly`, returns a module and basis to compare against `{He_0..He_{N-1}}`. Not run.

*Check* (ROADMAP): last graded level equals the composed morphism; fiber length equals degree at
every point (`02NX`, needs finite locally free).

---

# C4 — places, and ramification as a derived report

TUs `local/valuation.cc`, `local/place.cc`, `local/divisor.cc`.
Stacks `00I8`, `0EXQ`, `09E3`, `0BSD`, `09E6`, `0DWI`, `0BW9`, `0BRW`, `0BXX`, `0C1B`
(spelled `curves-section-riemann-hurewitz` upstream).

```m2
K = GF(7,1); S = K[y]
g = y; for e in {(3,2),(5,3)} do g = g^(e#1) + (2_K)^(e#0)
factor (g - 3_K)                 -- [run] (y+3)(y+2)(y-3)(y-2)(y²+2)
```

Residue degrees `1,1,1,1,2` summing to `6 = deg g`; all `e_i = 1`.

```m2
select(toList(0..6), i -> first degree gcd(g - i_K, diff(y,g)) > 0)   -- critical values
discriminant(g, y)
```

*Check* (ROADMAP): `Σ e_i f_i == deg f` at every place — **[run]** above; inseparable maps do not
report as ramified.

In characteristic 2, even `n` gives `f' ≡ 0` (`AG_Chains_conv.md:419-424`); `n = 2` dominates the
graded layer (`notation.md:207-209`).

Place at infinity: not a point of `A^1`, needs the projective closure.

---

# C5 — characters, sheaves, cohomology

TUs `sheaf/lisse.cc`, `sheaf/character.cc`, `sheaf/operations.cc`, `sheaf/cohomology.cc`,
`sheaf/lfunction.cc`, `check/brute.cc`. Stacks `0F5Q`, `03SJ`, `03SL`, `03UL`, `03UU`, `03UX`,
`03UY`, `03VB`, `03PK`, `0A3J`, `03R0`, `05BE`. Swan and Grothendieck-Ogg-Shafarevich have no
Stacks home; the ROADMAP points at Katz and Laumon.

No M2 package for `ell`-adic or lisse sheaves, Frobenius on `H^1_c`, Swan conductors, GOS, or
`L`-functions of sheaves.

Kummer and Artin-Schreier covers as plane curves (`AG_Chains_conv.md:362-366`):

```m2
K = GF(3,1); B = K[x,y,z]
F = y^3*z - y*z^3 - x^4                          -- AS cover of y^3 - y = x^4
genus(B/ideal F)                                  -- [run] 3
G = y^3*z - y*z^3 - (x^4 + x^2*z^2 - z^4)         -- AS cover of a chain polynomial
genus(B/ideal G)                                  -- [run] 3
H = y^2*z^2 - (x^4 + x^2*z^2 - z^4)               -- Kummer y^2 = f
genus(B/ideal H)                                  -- [run] 3

needsPackage "Varieties"
X = Proj(B/ideal F)
rank HH^0(OO_X), rank HH^1(OO_X)                  -- [run] 1, 3
```

`(r−1)(d−1)/2 = 3`, Riemann-Hurwitz `0C1B`. `homogenize` rejects these `GF` rings; homogenize by
hand.

E12 (`notation.md:241-285`) predicts `deg P_red = max(N_odd, n_1(n_{2,odd}−1) >> v)` with
`v = min{v_2(e) : e ∈ supp(c_1)}`, and `dim H^1_c = deg P_red − 1`. Largest excess in range is
`(n_1,n_2) = (4,5)`: 16 against `N_odd = 5` (`notation.md:271-274`).

*Check* (ROADMAP): `Σ_χ χ(x − c)` against brute-force fiber counts **at `x ≠ c` only**;
`|g(χ,ψ)| = √q` for both characters nontrivial; GOS against Weil sum growth once the outer
cohomology is known to vanish. `RationalPoints2` **[run]** loads.

Point counts on the covers above over `F_{r^d}`, `d = 1..5`: fit the zeta numerator, compare its
degree against `2g`.
