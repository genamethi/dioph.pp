# M2 runbook

    M2 --script f.m2        # --script must be the first argument

`~/.local/bin/M2` wraps `/usr/bin/M2` to preload Debian's `libflint.so.24`; a stale
`libflint.so.24.0.0` in `/usr/local/lib` shadows it and lacks `fmpz_poly_has_real_root`,
which `libeantic.so.3` needs. Retiring that orphan removes the need for the wrapper —
see its comment header.

Docs cited as `path:lines`, relative to `~/fluid/research/markdown/`.

---

## R0 — preamble

```m2
needsPackage "Graphs"
R = ZZ[x]

primePowerData = (r) -> (
    if r < 2 then return null;
    L := toList factor r;
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

Traps: `toString #x` → `toString(#x)`; `(#L)-1` inside ranges; a 1-arg function applied to a
`Sequence` gets it spread — return `List` from constructors.

Baseline **[verified]** `K(137) = {(4,11,2),(6,73,1),(7,3,2)}`; `k=0` below 200 is `{3,149}`;
37 chains into 137 → 6 words → 6 polys → 6 Hermite vectors; skeleton-`[2]` translations
`(A_0,c_1) = (0,128),(2,112),(8,16)` — matches `math/collapse_findings.md:26-33`.

---

# 1. The composition map as a variety

## 1.1 — image ideal of a skeleton

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

`σ: A^{k+1} → A^N`, `(A_0,c_1..c_k) ↦` coefficient vector of the word polynomial (leading
coeff is 1, so `u_N` is dropped). **[verified]**

| skeleton | deg | codim | #gens |
|---|---|---|---|
| `[2,2]` | 4 | 1 | 1 — `u3³ − 4u2u3 + 8u1` |
| `[3,2]` | 6 | 3 | 7 |
| `[2,3]` | 6 | 3 | 17 |
| `[2,2,2]` | 8 | — | large, degree-7 gens |

Motivated by: `math/collapse_findings.md:26-29` ("fix a skeleton and the translations lie on a
slice"). That slice is this variety; `P(q_0)=p` is a hyperplane cutting it.

Vary: every skeleton with `∏n_i ≤ 12` — `[2],[3],[4],[5],[2,2],[3,2],[2,3],[2,2,2],[4,2],[2,4],[3,3]`.
Note `x^4+c = (y^2+c)∘x^2`, so compare `[4]` against `[2,2]` restricted to `c_1 = 0`.

Compare: `codim` should be `N − (k+1)` when `σ` is generically finite. `[2,2]`: `4−3 = 1` ✓.
`[3,2]`,`[2,3]`: `6−3 = 3` ✓. A skeleton where it isn't is a fiber-dimension jump — look there.

## 1.2 — Ritt injectivity, per skeleton, as a proof

```m2
-- is sigma a closed immersion onto its image? invert the elimination.
-- [2,2]: u3 = 4A_0 -> A_0;  u2 = 2B+4A_0^2 -> B = A_0^2+c_1 -> c_1;  u0 = B^2+c_2 -> c_2.
-- so injective over any field of char != 2.
S = QQ[a0,c1,c2,b0,b1,b2]
sig = (A,c,d) -> (A2 := A^2+c; {4*A, 2*A2+4*A^2, 4*A*A2, A2^2+d})
I = ideal apply(4, i -> (sig(a0,c1,c2))#i - (sig(b0,b1,b2))#i)
J = saturate(I, ideal(a0-b0))     -- is the diagonal the only component?
print(J == ideal(1_S))
```

Motivated by: `misc/retired/chain_hermite_primer.md:141-158` — the Ritt argument is written, not
proved, and the affine-absorption case is open. This settles it per skeleton by Gröbner instead.

Vary: run for `[3,2]`, `[2,3]`, `[2,2,2]`. Also over `ZZ/p` for small `p` — char 2 and char
dividing an `n_i` are where it should break.

Open: does injectivity fail for any skeleton once `c_i` is allowed to be 0? Every real `c_i ≥ 2`
(`math/notation.md:56-59`), so a failure at `c_i = 0` is harmless — confirm it is the only one.

## 1.3 — do two skeletons of equal degree ever meet?

```m2
I = imageIdeal {3,2}; J = imageIdeal {2,3};
Z = I + J;
print("dim: " | toString dim Z)
print("empty: " | toString(Z == ideal(1_(ring I))))
decompose Z
```

Motivated by: Ritt's second theorem; and `misc/retired/hopf_structure.md:20` records `[3,2] ≠ [2,3]`
empirically (105K vs 58K words at 1e6) with no reason given. The reason is that these are two
different codim-3 varieties in `A^6`; this computes whether they share anything.

Vary: `[4,2]` vs `[2,4]` (deg 8), `[2,2,2]` vs `[4,2]` vs `[2,4]` vs `[8]`. The `x^a∘x^b = x^b∘x^a`
commuting move needs the intermediate constant to vanish, so the intersection should sit inside
`{c_i = 0}` — check that, don't assume it.

## 1.4 — is σ triangular in the Hermite basis?

Rebuild `imageIdeal` in Hermite coordinates and read the shape of the Jacobian of `σ`.

Motivated by: `math/notation.md:155-189`. E8 is stated for `x^n` coefficients and **omits `A_0`**:
the three skeleton-`[2]` classes at 137 have `N − N_1 = 0` yet differ at `He_1`, where the
coefficient is `2A_0` — `16, 4, 0` for `A_0 = 8, 2, 0` **[verified]**. Restate E8 with `A_0` and
check triangularity as a property of the Jacobian, not of sampled coefficients.

Compare: `math/notation.md:167-189` lists all degree-8 words at `p = 65537`. Reproduce that table
and check the `[4,2]` closed forms at `:181-189`.

## 1.5 — elimination from the uncollapsed system

```m2
S = QQ[q0,q1,p, MonomialOrder => Eliminate 1]
I = ideal(q1 - (q0^2 + 8), p - (q1^2 + 16))
eliminate(q1, I)
```

Motivated by: `math/AG_Chains_conv.md:146-158`. Confirms the 1D chain polynomial is the projection
of the 2D system, and gives the "intermediates as coordinates" presentation for free.

Vary: 3-step chains; keep `q1` to get the partial-composite diagram. Must agree with `wordPoly`.

---

# 2. Covers, ramification, cohomology

## 2.1 — the Artin-Schreier and Kummer covers

```m2
K = GF(3,1); B = K[x,y,z]
F = y^3*z - y*z^3 - x^4                              -- AS cover  y^3-y = x^4
print genus(B/ideal F)                                -- 3
G = y^3*z - y*z^3 - (x^4 + x^2*z^2 - z^4)             -- AS cover of a chain polynomial
print genus(B/ideal G)                                -- 3
H = y^2*z^2 - (x^4 + x^2*z^2 - z^4)                   -- Kummer  y^2 = f
print genus(B/ideal H)                                -- 3

needsPackage "Varieties"
X = Proj(B/ideal F)
print(rank HH^0(OO_X), rank HH^1(OO_X))               -- (1, 3)
```

**[verified]** all four. `(r−1)(d−1)/2 = 3` matches Riemann-Hurwitz.

Motivated by: `math/AG_Chains_conv.md:362-366` (the cover is `k[x][y]/(y^n − f)`) and
`math/notation.md:241-285` (E12: `Swan_inf = deg P_red`, `dim H^1_c = deg P_red − 1`).
`dim H^1` here is coherent cohomology on an explicit curve — no étale machinery needed.

Vary: `f` = the chain polynomial of each word at a target `p`; `r ∈ {3,5,7}`; Kummer `n` over the
divisors of `r−1`. Homogenize by hand — `homogenize` rejects these `GF` rings.

Compare: E12 predicts `deg P_red = max(N_odd, n_1(n_{2,odd}−1) >> v)` with
`v = min{v_2(e) : e ∈ supp(c_1)}` (`math/notation.md:258-266`, empirical over 16500 words,
unproved). Check against `dim H^1` directly, at the `(n_1,n_2) = (4,5)` case of
`math/notation.md:271-274` where the excess is largest (`deg P_red = 16` vs `N_odd = 5`).

## 2.2 — ramification and the branch locus

```m2
K = GF(3,1); A = K[x]
g = (x^2 + 2_K)^2 + 16_K
diff(x,g)
discriminant(g,x)
select(toList(0..2), i -> first degree gcd(g - i_K, diff(x,g)) > 0)   -- critical values
```

Motivated by: `math/AG_Chains_conv.md:160-169, 476-491`, which also predicts that over `F_2` the
ramified places on the base are exactly `{c_1, c_0^{n_1} + c_1}` (`:487-491`) — a two-point claim,
easy to falsify.

Vary: characteristic. `math/AG_Chains_conv.md:419-424` claims any even `n_i` makes the cover purely
inseparable in char 2 (`f' ≡ 0`); `math/notation.md:209` says `n = 2` is 97.9% of Set B. So the
generic edge is inseparable in char 2. Confirm `f' == 0`, then decide whether char 2 is usable or
whether `ROADMAP.md`'s odd-base requirement stands.

Compare: `(e,f)` by `factor(g - c)`, `Σ e_i f_i = deg` — **[verified]** at `deg 6` over `F_7`:
`(y+3)(y+2)(y−3)(y−2)(y²+2)`.

## 2.3 — π_* O as a free module; is Hermite a basis?

```m2
needsPackage "PushForward"
A = QQ[p]; B = QQ[x]
f = map(B, A, {(x^2+8)^2+16})
(M, g, h) = pushFwd f
print(M, numgens M)          -- expect free of rank N = 4
```

Motivated by: `math/AG_Chains_conv.md:468` — "π_* O is free of rank N, and **your Hermite
polynomials are explicitly a choice of basis**." Checkable, not metaphorical: compare the basis
`pushFwd` returns with `{He_0..He_{N-1}}` and ask whether the change of basis is invertible over
`QQ[p]`.

Open: if Hermite is not a basis, does the obstruction depend on the skeleton or only on `N`?

## 2.4 — point counts as the experimental route to Frobenius

**[verified]** for the word `{(3,2),(5,3)}` over `F_7`: root counts `0, 0, 6, 0` for `d = 1..4` —
roots first appear at `d = 3`, while `lcm_i ord_{n_i}(7) = 1`. Two different invariants, and
`ROADMAP.md:444-449` runs them together in prose: splitting of the *layer decomposition* is not the
splitting field of the composite.

Motivated by: `math/AG_Chains_conv.md:171-187` (the Hasse-Weil error term *is* the cohomology) and
`:216-235` (Jacobi sums are the eigenvalues).

Vary: count points on the §2.1 curves over `F_{r^d}`, `d = 1..5`; fit the zeta numerator; read
eigenvalues. Compare degree of the numerator against `2g` from §2.1, and magnitudes against `√q`.

---

# 3. Congruences

## 3.1 — Hermite mod ℓ

```m2
for l in {3,5,7,11,13,31,41} do (
    F := GF(l,1); S := F[t];
    print(l, toString factor (t^3 - 3*t)))
```

Motivated by: `math/hermite_congruences.md:17-23` — `He_3` has nonzero roots in `F_ℓ` iff `3` is a QR
mod `ℓ`, claimed at 100% over 93 primes; and `:9-15`, `He_p ≡ x^p (mod p)`. Neither re-run here.
The doc's own examples to check first: `ℓ = 31, 41` (one root) against `ℓ = 11, 13` (three roots).

Vary: `He_n`, `n = 4..12`, all `ℓ < 200`. `math/hermite_congruences.md:51-53` names the open step —
factorization type of `He_n mod ℓ` by higher power residues (cubic when `3 | gcd(n, ℓ−1)`).
`math/structures.md:233-238` says which `ℓ` can detect which `n`: `ℓ ≡ 1 (mod n)`, and `ℓ = 3` can
never detect a cube.

Open (`math/hermite_congruences.md:54-56`): evaluate `He_n(2^{m'}) mod ℓ` along realized chains and
test whether frontier `k=0` primes are distinguished by hitting Hermite roots.

## 3.2 — subgroup exclusion, mechanised

```m2
sub = (a,l) -> set apply(toList(0..l-2), i -> (a^i) % l)   -- <a> in (Z/l)^*
```

Motivated by: `math/structures.md:203-210` — 22 unconditional obstruction classes mod 255255,
blocking primes `{13,37,41,61,67,73,181,193}`, most positions blocked by `ℓ = 13` alone
(`ord(3,13) = 3`, excluding 75% of residues).

Vary: build `⟨2⟩` and `⟨3⟩` for all `ℓ < 500` and tabulate index. Small `|⟨3⟩|` relative to `ℓ−1`
marks the strong blockers — the list should regenerate those eight and nothing else.

Open: `math/structures.md:216-221` — do locally-unobstructed-but-globally-obstructed primes exist?
Finite search per prime; the warehouse has the `k=0` set.

---

# 4. Filtration and Hermite grading

## 4.1 — V_D and the cokernel

```m2
coker map(ZZ^13, ZZ^11, id_(ZZ^11) || 0)      -- V_10 -> V_12
```

Motivated by: `misc/retired/chain_hermite_primer.md:163-191` (`dim V_D = D+1`, `D = ⌊log_3 B⌋`) and
`:251-262` (rank of the cokernel is Eisenstein data; the *filling* of each new direction is cusp
data). `math/notation.md:117-128` gives measured class count against `D` — equal in all seven
windows.

Vary: which words populate `He_d` when `d` first becomes reachable near `B = 3^d`. That multiplicity
structure is the stated open object.

## 4.2 — degree-set census

Motivated by: `math/notation.md:289-299` (O1) — at `D = 20` only 244 of `2^20` subsets of `[1,D]`
occur as a prime's degree set, over 376M primes; first differences `30,36,42,41,43`. Wanted: an
exact characterization of *which* 244.

M2 side: enumerate all skeletons with `∏n_i ≤ D` and ask which degree sets the §1.1 variety
constraints alone permit, ignoring primality. If that already cuts `2^20` to near 244 the
obstruction is geometric; if not, the gap is the arithmetic layer.

---

# 5. Warehouse values worth pulling, and what to do with them

1. **The 8 primes below `1.456e12` with more than one `n ≥ 2` parent** — `math/notation.md:216-226`,
   including `2213 = 2^2 + 47^2 = 2^4 + 13^3`. The only places image varieties of *different*
   skeletons provably meet at a real target. Run §1.3 on `[2]` vs `[3]`, specialize at 2213.
2. **The 1079 Set B edges (0.68%) whose base `q` is itself the target of an `n ≥ 2` edge** —
   `math/notation.md:228-232`. The only genuinely 2-block chains; everything else routes through
   flats. This is the entire test corpus for §1.1/§1.3 at `k = 2`.
3. **The `q = 3` edges with exponent near `D = ⌊log_3 p_max⌋`** — `math/notation.md:203-214`. Only
   `q = 3` reaches past 17, so these are the highest-degree words available.
4. **A prime carrying the full degree interval `[1,D]`** — `math/notation.md:126-128` says one exists
   at 1e10 with all 20. Best single target for a full §1.1 sweep.
5. **The `k = 14` primes at 1e10** — `math/notation.md:301-306` (O2): 6 primes, fully separated at
   `e = 4`. Small enough to enumerate every chain in M2 and compare state sets directly.
6. **`(n_1,n_2) = (4,5)` two-block words** — `math/notation.md:271-274`, where E12's excess is
   largest. Direct input to §2.1.
7. **`k = 0` primes at the frontier (large `ord_2(ℓ)`)** — `math/hermite_congruences.md:36-38` calls
   these the cusp, where covering breaks down. Input to §3.1's open step.

---

# 6. Questions this bench can attack

- Is `codim(image σ) = N − (k+1)` for every skeleton, or does one drop? (§1.1)
- Is `σ` a closed immersion for every skeleton in char 0 — Ritt injectivity as a theorem here rather
  than an observation? (§1.2)
- Do two equal-degree skeletons ever meet outside `{c_i = 0}`? (§1.3)
- Restate E8 with `A_0`, prove triangularity from the Jacobian. (§1.4)
- Is `{He_0..He_{N-1}}` a basis of `π_* O`? (§2.3)
- Does E12's `deg P_red` agree with `dim H^1` on the explicit cover? (§2.1)
- Does §1.1's geometry alone explain O1's 244 degree sets, or is the cut arithmetic? (§4.2)
- Is char 2 usable at all, given `n = 2` dominates and forces inseparability? (§2.2)

---

# 7. Not attempted

`ROADMAP.md` Étale
coefficients are absent, but §2.1 shows the invariants that matter are reachable as coherent
cohomology on explicit covers.
