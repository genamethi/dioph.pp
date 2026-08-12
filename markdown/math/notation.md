# Notation and exact expressions

Nothing here is a density or a heuristic count; when an argument needs one, that
is a sign the structure has been lost and the argument should be rebuilt, not
patched.

**The labels.** `D` numbers the definitions in §2, `E` the expressions in §3,
`O` the open questions in §4. They exist to be cited from prose and from
`HANDOFF-QUESTIONS.md`, which runs its own unrelated `A`/`B`/`C` scheme for
corrections, open items and the code map. Do not add more prefixes.

Each `E` carries its standing, because the three are not interchangeable:

| tag | meaning |
|---|---|
| **identity** | holds for every chain; proved here or immediate from the definitions |
| **census** | exhaustive *inside* a stated window. **A window is a sample.** Complete enumeration within it settles nothing beyond it, so the general statement is conjectural however large the window |
| **fit** | a form matched against a scanned range and not proved. A conjecture with a stated domain |

The distinction between the last two is only how the evidence was gathered, not
how much it establishes: neither proves anything about all primes. Where a
census entry has a proved half, it says which half — typically an inequality is
an identity while its attainment is sampled.

## 1. Glossary

| Symbol | Kind | Reading |
|---|---|---|
| `p`, `q` | odd prime | node of the graph |
| `q -> p` | relation | `q` is a parent of `p`: there are `m >= 1`, `n >= 1` with `p = 2^m + q^n` |
| `k(p)` | integer | in-degree of `p`, the number of parents counted with their `(m, n)` |
| `m`, `n` | integers | the exponent pair carried by one edge; `m >= 1` always, `n >= 1` |
| `q_0 -> q_1 -> ... -> q_k` | chain | a directed path; `q_0` is its **root**, `q_k` its target |
| `(m_i, n_i)` | edge data | the pair on the edge `q_{i-1} -> q_i`, so `q_i = 2^{m_i} + q_{i-1}^{n_i}` |
| `x` | indeterminate | stands for the root of a chain |
| `t` | indeterminate | formal stand-in for the base 2, evaluated last. **Not** `x`; the two are never the same variable |
| `P_i` | polynomial in `Z[x]` | the chain polynomial truncated at depth `i` |
| `P` | polynomial in `Z[x]` | `P_k`, the chain polynomial; `P(q_0) = q_k` |
| `N_i` | integer | `deg P_i = n_1 n_2 ... n_i` |
| `N` | integer | `N_k = deg P`, the **degree** of the chain |
| `A_0`, `c_i` | integers, or elements of `F_2[t]` | the constants of the **word** form (D4); each is a sum of powers of two, equivalently a sum of powers of `t` before evaluation |
| `l`, `e` | prime, integer | reduction modulus and tower level; the base is `l = 2` |
| `S` | polynomial in `(Z/l^e)[x]` | the **normalized state**, `P - p` (D6) |
| `T_{q,n}` | operator on polynomials | the action of one `n >= 2` edge out of `q` (D7) |
| `C(n, j)` | integer | binomial coefficient `n!/(j!(n-j)!)` |
| `v_2(a)` | integer | the 2-adic valuation of `a` |
| `He_j` | polynomial in `Z[x]` | probabilists' Hermite, `He_0 = 1`, `He_1 = x`, `He_{j+1} = x He_j - j He_{j-1}` |
| `D` | integer | `floor(log_3 p_max)`, the largest chain degree available in a window (E5) |

Two letters that are deliberately *not* reused: `M` is not a degree (partial
degrees are `N_i`), and node values are `q_i`, never `v`. `l` is the reduction
modulus only; it is never a coefficient prime.

`D` is overloaded against the definition prefix. A bare `D` is the degree bound
`floor(log_3 p_max)`; `D` followed by a digit is a definition number.

## 2. Definitions

**D1 (graph).** Nodes are the odd primes. `q -> p` iff `p = 2^m + q^n` for some
`m >= 1`, `n >= 1`. `m >= 1` is forced: `p` and `q^n` are both odd, so `2^m` is
even. A node with `k(p) = 0` is a **root**.

**D2 (chain).** A directed path `q_0 -> ... -> q_k` with `q_0` a root. The graph
is acyclic because `p = 2^m + q^n > q`.

**D3 (chain polynomial).** `P_0 = x`, and for `1 <= i <= k`

    P_i = P_{i-1}^{n_i} + 2^{m_i}.

Then `P_i(q_0) = q_i`, and in particular `P(q_0) = p` for `P = P_k`. Its degree
is `N_i = n_1 ... n_i`, so `N = deg P = prod n_i`.

**D4 (word).** Runs of `n = 1` edges compose to a single addition, so every
chain polynomial equals

    P = ( ... ((x + A_0)^{n_1} + c_1)^{n_2} + c_2 ... )^{n_k} + c_k

with `n_i >= 2`. `A_0` and each `c_i` is a sum of powers of two — the powers
contributed by the `n = 1` edges of that run together with the `2^{m}` of the
`n >= 2` edge that closes it. The **skeleton** is the list `[n_1, ..., n_k]`;
`N = prod n_i` is unchanged, since `n = 1` edges contribute a factor 1.

**D5 (spectrum of `p`).** The set of `(root, word)` pairs over all chains into
`p`, each with the number of chains realizing it.

**D6 (normalized state).** For a chain into `p`,

    S = P - p,

that is, `P` with `p` subtracted from its constant term, read in `(Z/l^e)[x]`.
`S(p)` denotes the set of normalized states over all chains into `p`.

**D7 (edge operator).** With `tau_c(f) = f + c`, an `n >= 2` edge out of `q`
acts on normalized states as

    T_{q,n} = tau_{-q^n} o pow_n o tau_q,     T_{q,n}(S) = (S + q)^n - q^n.

**D8 (tower).** Level `e` is the reduction of everything above to `Z/l^e` with
`l = 2`. Raising `e` refines; truncation `Z/2^{e+1} -> Z/2^e` carries level
`e+1` onto level `e`.

## 3. Expressions

**E1 — identity (telescoping: why `n = 1` edges are free).** For an `n = 1` edge
`q -> p`, the added constant is `2^m = p - q`, so

    S_p = S_q.

The normalized state is constant along `n = 1` edges, for every modulus, with no
arithmetic. This is what makes the state shareable between primes rather than
one copy each, and it is the whole of the scaling result in `pp-graph --shared`.

**E2 — identity (the `n >= 2` step).** Expanding D7,

    S_p = T_{q,n}(S_q) = sum_{j=1}^{n} C(n, j) q^{n-j} S_q^j.

Only `q` enters, never `m`; the `2^m` cancelled against `p` in the
normalization. `q < p^{1/n} <= sqrt(p)`, so every polynomial step is anchored
below the square root of the window.

**E3 — identity (roots).** At a root `r`, `P = x`, hence

    S = x - r.

**E4 — identity (level one is closed form).** Modulo 2 every `q` is a unit `q ≡ 1`, so
`S + q ≡ S + 1`. With `S = x^{N_i} + 1` this gives `S + 1 = x^{N_i}` and
`T(S) ≡ x^{N_i n} + 1`. Since E3 gives `S ≡ x + 1` at every root,

    S ≡ x^N + 1   (mod 2),    equivalently   P ≡ x^N   (mod 2).

The level-one class is the degree and nothing else: not `m`, not `q`, not the
order of the edges. Consequently the number of distinct level-one classes over a
family equals the number of realizable degrees `N`.

**E5 — census (the degree spectrum is a full interval).** A chain into `p` with root
`q_0 >= 3` satisfies `p = P(q_0) > q_0^N`, so `N < log_3 p`, giving
`N <= D = floor(log_3 p_max)`. Measured on `--k 1-16` at `e = 1`, the number of
level-one classes against `D`:

| `p_max` | 1e5 | 1e6 | 2e6 | 1e7 | 1e8 | 1e9 | 1e10 |
|---|---|---|---|---|---|---|---|
| classes | 10 | 12 | 13 | 14 | 16 | 18 | 20 |
| `D` | 10 | 12 | 13 | 14 | 16 | 18 | 20 |

Equal in all seven. Corroborating, the largest level-one state set has size `D`
exactly (20 at 1e10, 18 at 1e9) — some single prime carries the whole interval
as its degree set.

`N <= D` is the identity here. That every degree in `[1, D]` is *realized*, and
that the bound is attained, is sampled: seven windows, the largest 1e10. Nothing
above 1e10 is tested, and agreement in seven nested windows is one observation
repeated, not seven independent ones.

**E6 — identity (which terms survive level `e`).** In E2 the coefficient of `S^j` is
`C(n, j) q^{n-j}`. Two independent filters:

- *Kummer.* `v_2(C(n, j))` is the number of carries when adding `j` and `n - j`
  in base 2, so the `j`-th term survives mod `2^e` only when that carry count is
  less than `e`. At `e = 1` this is Lucas: only `j` whose binary digits are a
  submask of `n`'s survive. That is what collapses E2 into E4.
- *The unit part.* `q` is odd, so `q^{n-j} mod 2^e` depends on `q mod 2^e`.

**E7 — identity (where the `m`-record re-enters).** `q = 2^{m'} + q'^{n'}`, so `q mod 2^e`
exposes exactly the exponents `m' < e`: **level `e` resolves the exponents below
`e`.** The first instance is explicit. At `e = 2`:

- `n = 2`: `T(S) = S^2 + 2qS ≡ S^2 + 2S`, since `2q ≡ 2 (mod 4)` for odd `q` —
  still no dependence on `q`.
- `n = 3`: `T(S) = S^3 + 3qS^2 + 3q^2 S`; `q^2 ≡ 1`, but `3q` is `3` or `1`
  according to `q mod 4`.

So odd exponents are what first admit the parent's residue, and they do it at
level two.

**E8 — identity (Hermite grades the chain by depth).** Expanding a single generator,

    x^n = sum_j C(n, 2j) (2j-1)!! He_{n-2j}(x),

so for one edge `x^n + 2^m` every Hermite coefficient except `He_0` depends on
`n` alone, and the entire `m` of that edge sits in `He_0`. Composition is what
lifts it. For a word `(A_0; (n_1,c_1), ..., (n_k,c_k))` of degree `N`:

- the coefficients at Hermite indices **above** `N - N_1` are those of `x^N`,
  namely `C(N, 2j)(2j-1)!!` at index `N - 2j`, and depend on `N` alone;
- `c_i` first appears at index `N - N_i`, with linear coefficient `(N / N_i) c_i`.

So the Hermite grading is a depth filtration of the chain, read top-down: the
outermost constant `c_k` reaches only `He_0` (`N - N_k = 0`), and the innermost
`c_1` reaches highest.

Checked against `pp-graph --mode spectrum` at `p = 65537`, all words of degree 8:

    [8]      1*He8 + 28*He6 + 210*He4 + 420*He2 + 59081*He0
    [4,2]    1*He8 + 28*He6 + 530*He4 + 2340*He2 + 34121*He0
    [4,2]    1*He8 + 28*He6 + 242*He4 +  612*He2 + 56585*He0
    [4,2]    1*He8 + 28*He6 + 382*He4 + 1452*He2 + 45665*He0
    [4,2]    1*He8 + 28*He6 + 254*He4 +  684*He2 + 55649*He0
    [4,2]    1*He8 + 28*He6 + 470*He4 + 1980*He2 + 38801*He0
    [2,2,2]  1*He8 + 44*He6 + 690*He4 + 3988*He2 + 17673*He0

`[8]` has `N_1 = 8`, so its constant reaches only `He_0`, and indeed its
coefficients `1, 28, 210, 420` are exactly `C(8,2j)(2j-1)!!`. The five `[4,2]`
words have `N_1 = 4`, so they agree with `[8]` at `He_8` and `He_6` and first
separate at `He_4 = N - N_1`. `[2,2,2]` has `N_1 = 2` and separates already at
`He_6`. For `[4,2]`, `f = (x^4 + c_1)^2 + c_2` gives exactly

    He_8 = 1,  He_6 = 28,  He_4 = 210 + 2 c_1,  He_2 = 420 + 12 c_1,
    He_0 = 105 + 6 c_1 + c_1^2 + c_2,

and reading `c_1` off `He_4` gives `160, 16, 86, 22, 130`, each a sum of powers
of two with `3^4 + c_1` equal to `241, 97, 167, 103, 211` — prime in every case.
`c_1 = 16` then predicts `He_2 = 612` and `c_2 = 65537 - 97^2 = 56128`, both of
which the tool reports.

**E9 — census (what `pp-graph --shared` computes).** By E1 and E2 the state depends only
on the normalized set and `p mod 2^e`, so states are interned once and shared;
`n = 1` edges copy a reference. Family class counts and per-prime set counts
agree with the original per-node sweep at `l = 2, 3, 5, 7` on every level
checked, at windows from 2e6 to 1e10.

**E10 — census (both exponent spectra are full intervals, both bounds attained).** For a
single edge `p = 2^m + q^n` with `n >= 2`, `q^n < p` gives `n <= log_q p`.
Enumerated exhaustively at `p_max = 1456195776043` (the 54e9-th prime), by
inverting the edge rather than testing candidates:

| base | realized `n` | bound |
|---|---|---|
| `q >= 5` (Set B) | every value in `[2, 17]` | `floor(log_5 p_max) = 17` |
| `q = 3` (Set A) | every value in `[2, 25]` | `floor(log_3 p_max) = 25` |

`n <= log_q p` is the identity, from `q^n < p`. That every exponent in the
interval is realized, and that the bound is attained, holds inside this one
window and is not established beyond it. The two distributions are unlike: Set B
is crushed at the bottom,
`n = 2` taking 155545 of 158904 edges (97.9%) and `n = 3` a further 1.7%, while
Set A is nearly flat across all of `[2, 25]` at 3 to 13 edges per exponent.

Consequently **Set A carries the top of the degree spectrum**: only `q = 3`
reaches past 17, and E5's `D = floor(log_3 p_max)` is realized by a single
`q = 3` edge of exponent `D`. The branch the parity trick resolves by table
lookup is the one holding the deepest chains.

**E11 — census (the graded layer is a partial function, and it is small).** Over the same
window there are 159065 edges with `n >= 2` in total — 158904 with `q >= 5` and
161 with `q = 3` — against 54e9 primes. Of the 158903 distinct Set B targets,
**exactly one carries two**:

    2213 = 2^2 + 47^2 = 2^4 + 13^3

and seven further primes carry one of each kind. So eight primes below
`1.456e12` have more than one `n >= 2` parent; `p -> (m, n, q)` is a partial
function up to that exception set.

Nothing here is proved. Eight exceptions in one window is not a bound on the
exception set, and a count of eight is small enough that the next window could
change its character entirely. The counts below are likewise window facts.

Direct composition is nearly as rare: 1079 of the 158904 Set B edges (0.68%)
have a base `q` that is itself the target of another `n >= 2` edge. Since by D4
`n = 1` edges contribute a factor 1 to the degree, a composite degree
`N = n_1 n_2` with both factors `>= 2` therefore almost always routes through
`n = 1` edges between the two blocks, and the degree spectrum is a statement
about `n = 1` reachability between the endpoints of these 159065 edges.

Over the 158903 Set B targets, `k` has mean 3.726 and maximum 13, splitting as
433126 parents with `n = 1` against 158911 with `n >= 2`.

The whole enumeration costs 0.23 s and reads no table: `n <= log_5 p_max` bounds
the exponent, and for each `(n, m)` the constraint `lo <= q^n + 2^m <= hi`
inverts to an exact base interval by two integer roots.

**E12 — fit (Artin-Schreier reduction, and where the formal base earns its keep).**
`L_psi(f) ~ L_psi(g)` iff `f - g` lies in the image of `℘(h) = h^2 - h`, so in
characteristic 2

    L_psi(a x^{2j})  ~  L_psi(a^{1/2} x^j)     when a is a square

Fold every even `x`-degree monomial down, adding into the coefficient already at
degree `j`; `P_red` is the fixed point, `Swan_inf = deg P_red`, and
`dim H^1_c(A^1, L_psi(P)) = deg P_red - 1`. In `F_2[t]` the squares are
`F_2[t^2]`, so the fold blocks on odd `t`-exponents; over `F_2` itself nothing
ever blocks, because `F_2` is perfect.

*One block.* `deg P_red = N_odd` for every word tested. The formal `t` changes
nothing at depth one: the leading coefficient is 1 and folds unobstructed to
`x^{N_odd}`. Since `n = 2` is 97.9% of Set B by E10, most single edges give
`N_odd = 1` and `dim H^1_c = 0`.

*Two blocks* `(A_0; (n_1, c_1), (n_2, c_2))`. With
`v = min { v_2(e) : e in supp(c_1) }`,

    deg P_red = max( N_odd,  n_1 (n_{2,odd} - 1) >> v )

Checked on 16500 words with `n_i` in `[2,6]`, `A_0 = t^a` for `a` in `[0,3]`,
and `c_1` a sum of three powers of `t` with exponents in `[1,11]`; and on 3936
words with `n_i` in `[2,8]` and `c_1` a single power. Zero disagreements. It is
empirical over that range, not proved.

Three things the formula does *not* contain: `A_0`, `c_2`, and the value of
`c_1`. Only the **exponent set** of `c_1` enters, and only through the least
2-adic valuation in it — that is, through the `m`-multiset of the `n = 1` run
that D4 folds into `c_1`. The excess over `N_odd` is nonzero only when `n_1` is
even and `n_{2,odd} >= 3`; the largest in range is
`(n_1, n_2) = (4, 5)` with `v = 0`, giving `deg P_red = 16` against `N_odd = 5`,
so `dim H^1_c` is 15 rather than 4.

Over `F_2` with `t` evaluated the answer is `N_odd` in every case. **The excess
is exactly what the formal base variable sees, and it is a function of the
`m`-record.** E7 recovers that record one exponent per tower level; here the
`t`-grading carries all of it at once, and E12 says which part of it the
cohomology actually reads — not the `m`'s themselves but the 2-adic valuations
of their positions.

By E11 a two-block chain almost always routes through `n = 1` edges between its
blocks, and D4 absorbs exactly those into `c_1`. So the flat layer sets `v` and
therefore the cohomological dimension of the chain.

## 4. Open, in the same register

**O1 (which degree sets occur).** By E4 a prime's level-one state set is its set
of realizable chain degrees, a subset of `[1, D]`. Measured over `--k 1-16`:

| `D` | 10 | 12 | 14 | 16 | 18 | 20 |
|---|---|---|---|---|---|---|
| distinct sets | 52 | 82 | 118 | 160 | 201 | 244 |

At `D = 20` that is 244 subsets out of `2^20`, over 376 million primes. First
differences `30, 36, 42, 41, 43` — quadratic then flattening, so the low degrees
saturate and only the top ones keep contributing. The question is which 244, as
an exact characterization; a fitted growth law would not be an answer.

**O2 (separation depth against `k`).** At 1e10, `e = 4`, `k = 14` gives 6
distinct sets over 6 primes — fully separated — while `k = 1` gives 248374 over
130 million. `|S(p)|` grows with `k` because the set is a union over parents. The
exact statement wanted is the level `e` at which a given `k` separates, in terms
of E7's "level `e` sees `m' < e`".

**O3 (the Hermite support as a grouping key).** E8 makes the coefficient at
`N - N_i` a linear readout of `c_i`. Whether the support alone — which indices
are nonzero — already determines the skeleton, and what it fails to see, is not
settled here.
