# M2 bench

**[run]** = output from M2 1.26.06, quoted verbatim. Everything else is a command to run.

Tags resolve with `grep '^0BQ7,' ~/fluid/byo/repos/stacks-project/output/tagged/tags`; the label
prefix names the chapter PDF.

M2 parsing: `toString(#x)` not `toString #x`; `(#L)-1` inside ranges; return `List` not `Sequence`
from constructors — a one-argument function applied to a `Sequence` receives it spread.

---

# C0 — the spine

Commutative algebra `00AP`; valuation rings and completions `05E4`.

```m2
R = ZZ[x]
P = (x + 2)^3 + 16
first degree P                                  -- 3
S = ZZ[x]/ideal(32_(ZZ[x])); sub(P, S)          -- Z/2^5 coefficients
```

**[run]** builds. Loads: `Graphs`, `Cyclotomic`, `Padic`, `WittVectors`, `PushForward`,
`RationalPoints2`, `Varieties` **[run]**.

Checks: `truncate(lift(a)) == a` across the tower; cyclotomic norms against known Gauss sum
magnitudes.

---

# C1 — the equation's objects, off the tables

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
Digraph on odd primes below 200: 45 vertices, 105 edges, `isCyclic` false.

**[run]** `37 chains → 6 words → 6 polynomials → 6 Hermite vectors`;
`wordPoly(wordOf c) == chainPoly c` in all 37.

    mult  1    He4 + 10 He2 + 27 He0        [2,2]
    mult  3    He2 + 16 He1 + 81 He0        [2],  A_0 = 8
    mult  4    He3 +  3 He1 + 110 He0       [3]
    mult  4    He2 +  4 He1 + 117 He0       [2],  A_0 = 2
    mult  9    He2 + 129 He0                [2],  A_0 = 0
    mult 16    He1 + 134 He0                []

Skeleton-`[2]` words are `(A_0, c_1) = (0,128), (2,112), (8,16)`, each solving
`(3 + A_0)^2 + c_1 = 137`. The multiplicity-16 class is `A_0 = 134`, no blocks.

`(3 + A_0)^2 + c_1 = 137` also admits `A_0 = 4, 6` with `c_1 = 88, 56`, both even and positive.
Neither is realized.

E8 (`notation.md:155-189`) puts `c_i` at Hermite index `N − N_i` with coefficient `(N/N_i) c_i`.
On `[2,2]`: `N = 4`, `N_1 = 2`, `6 + 2c_1 = 10`, `c_1 = 2` **[run]**. The three skeleton-`[2]`
classes have `N − N_1 = 0` and differ at `He_1`, coefficient `2A_0` — `16, 4, 0` for
`A_0 = 8, 2, 0` **[run]**.

Compare: `notation.md:167-189` lists all degree-8 words at `p = 65537`; `[4,2]` closed forms at
`:181-189`.

Check: word count against a direct path walk at small bounds. Here 37 against 6.

## C1.b — the word as a point on a variety

`σ: A^{k+1} → A^N`, `(A_0, c_1..c_k) ↦` coefficient vector of `wordPoly`.

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

Injectivity, by inverting the elimination per skeleton. `[2,2]`: `u3 → A_0`, then `u2 → c_1`, then
`u0 → c_2` — invertible in characteristic ≠ 2.

Intersection of two equal-degree skeletons: `imageIdeal{3,2} + imageIdeal{2,3}`, then `decompose`.
The `x^a ∘ x^b = x^b ∘ x^a` move needs the intermediate constant to vanish; D4 gives `c_i ≥ 2`
(`notation.md:56-59`).

## C1.c — the `Z/2^e` tower

Normalize `S = P − p`; zeroing the constant term deletes the `m`-record.

E4 (`notation.md:105-113`): mod 2 every chain polynomial is `x^N`.
E7 (`notation.md:140-149`): level `e` resolves the exponents below `e`.

## C1.d — incidence without `factor`

- `3 | p − 2^m` iff `p ≡ (−1)^m (mod 3)`; when it holds a prime power there is `3^n`, a 40-entry
  table lookup — half the `m`-slots (`native/src/core.c:466,481`).
- Same for each small `ℓ` with period `ord_ℓ(2)`; two distinct small `ℓ` on one slot rejects it.
- `q = 3` edges are `p = 3^n + 2^m`: ~1000 `(n,m)` pairs to `1e12` (`notation.md:210-214`).
- The `n = 1` layer is `Θ(π(B) log B)`.

---

# C2 — fields, and the field a chain needs

Fields `09FB`; Kummer and Artin-Schreier extensions `09I6`, `09I7`; finite fields and roots of
unity `09HY`, `09HW`; Brauer and descent `073X`.

`GF` uses Conway polynomials, so `F_{r^d} → F_{r^{de}}` are compatible by construction.

```m2
F = GF(7,1); S = F[y]
g = y; for e in {(3,2),(5,3)} do g = g^(e#1) + (2_F)^(e#0)
factor g
```

**[run]** `g = y^6 + 3y^4 + 3y^2 − 2`; `factor g = (y^3+y^2+2y−3)(y^3−y^2+2y+3)`.

**[run]** roots of `g` over `F_{7^d}`, `d = 1..4`: `0, 0, 6, 0`. For this skeleton
`lcm_i ord_{n_i}(7) = 1`.

Checks: group order against degree; block structure against skeleton.

---

# C3 — schemes, and the four presentations

Schemes `01H9`; morphisms `01QM`; étale morphisms `024K`; fundamental groups `0BQ7`.
`Mor_Sch(X, Spec R) = Hom_CRing(R, Γ(X,O_X))` is `01I1`; `FEt_K ≃ π_1(K)-Set` is `0BND`, `0BNE`;
fiber length equal to degree needs finite locally free, `02NX`.

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
factor (gs - gt)
```

**[run]** `(s+t)(s−t)(s²+3t²−3)(s²−2t²−1)`. `s − t` is the diagonal. Fiber product is
`W/ideal(gs - gt)`.

`AG_Chains_conv.md:468` states `π_* O_{X_0}` is free of rank `N` with the Hermite polynomials a
basis. `pushFwd` of `QQ[p] → QQ[x]`, `p ↦ chainPoly`, returns a module and basis to compare
against `{He_0..He_{N-1}}`.

Checks: last graded level equals the composed morphism; fiber length equals degree at every point.

---

# C4 — places, and ramification as a derived report

Valuation rings `00I8`, `0EXQ`; ramification `09E3`, `0BSD`, `09E6`, `0DWI`; discriminants and
differents `0BW9`, `0BRW`; algebraic curves and the function-field dictionary `0BXX`;
Riemann-Hurwitz `0C1B` (spelled `curves-section-riemann-hurewitz` in the label).

```m2
factor (g - 3_K)
select(toList(0..6), i -> first degree gcd(g - i_K, diff(y,g)) > 0)
discriminant(g, y)
```

**[run]** `(y+3)(y+2)(y−3)(y−2)(y²+2)` — residue degrees `1,1,1,1,2` summing to `6 = deg g`, all
`e_i = 1`. Critical values `{4, 5}`; `disc = −1`; `gcd(g, g') = 1`.

Checks: `Σ e_i f_i = deg f` at every place; inseparable maps do not report as ramified.

In characteristic 2 an even `n` gives `f' ≡ 0` (`AG_Chains_conv.md:419-424`); `n = 2` is 97.9% of
Set B (`notation.md:207-209`).

The place at infinity is not a point of `A^1` and needs the projective closure.

---

# C5 — characters, sheaves, cohomology

Trace formula, L-functions, Frobenii, exponential sums `0F5Q`, `03UX`, `03UU`, `03UL`, `03UY`,
`03VB`; étale cohomology, Kummer and Artin-Schreier theory, curve cohomology `03SJ`, `03SL`,
`03PK`, `0A3J`, `03R0`, `05BE`. Swan and Grothendieck-Ogg-Shafarevich are outside the project —
Katz and Laumon.

Kummer and Artin-Schreier covers as plane curves (`AG_Chains_conv.md:362-366`):

```m2
K = GF(3,1); B = K[x,y,z]
F = y^3*z - y*z^3 - x^4                          -- AS cover of y^3 - y = x^4
genus(B/ideal F)
G = y^3*z - y*z^3 - (x^4 + x^2*z^2 - z^4)        -- AS cover of a chain polynomial
genus(B/ideal G)
H = y^2*z^2 - (x^4 + x^2*z^2 - z^4)              -- Kummer y^2 = f
genus(B/ideal H)

needsPackage "Varieties"
X = Proj(B/ideal F)
rank HH^0(OO_X), rank HH^1(OO_X)
```

**[run]** all three genera 3; `H^0 = 1`, `H^1 = 3`. `(r−1)(d−1)/2 = 3`.

`homogenize` rejects these `GF` rings; homogenize by hand.

E12 (`notation.md:241-285`) predicts `deg P_red = max(N_odd, n_1(n_{2,odd}−1) >> v)` with
`v = min{v_2(e) : e ∈ supp(c_1)}`, and `dim H^1_c = deg P_red − 1`. Largest excess in range is
`(n_1,n_2) = (4,5)`: 16 against `N_odd = 5` (`notation.md:271-274`).

Checks: `Σ_χ χ(x − c)` against brute-force fiber counts at `x ≠ c` only; `|g(χ,ψ)| = √q` for both
characters nontrivial.

Point counts on these covers over `F_{r^d}`, `d = 1..5`: fit the zeta numerator, compare its degree
against `2g`.
