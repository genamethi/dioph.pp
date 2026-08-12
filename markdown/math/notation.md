# Notation and exact expressions

Nothing here is a density or a heuristic count; when an argument needs one, that
is a sign the structure has been lost and the argument should be rebuilt, not
patched.

Items are cited by section and number: §2.4 is the fourth definition, §3.1 the
first expression.

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
| `A_0`, `c_i` | integers, or elements of `F_2[t]` | the constants of the **word** form (§2.4); each is a sum of powers of two, equivalently a sum of powers of `t` before evaluation |
| `l`, `e` | prime, integer | reduction modulus and tower level; the base is `l = 2` |
| `S` | polynomial in `(Z/l^e)[x]` | the **normalized state**, `P - p` (§2.6) |
| `T_{q,n}` | operator on polynomials | the action of one `n >= 2` edge out of `q` (§2.7) |
| `C(n, j)` | integer | binomial coefficient `n!/(j!(n-j)!)` |
| `v_2(a)` | integer | the 2-adic valuation of `a` |
| `He_j` | polynomial in `Z[x]` | probabilists' Hermite, `He_0 = 1`, `He_1 = x`, `He_{j+1} = x He_j - j He_{j-1}` |

Two letters that are deliberately *not* reused: `M` is not a degree (partial
degrees are `N_i`), and node values are `q_i`, never `v`. `l` is the reduction
modulus only; it is never a coefficient prime.

## 2. Definitions

**2.1 (graph).** Nodes are the odd primes. `q -> p` iff `p = 2^m + q^n` for some
`m >= 1`, `n >= 1`. `m >= 1` is forced: `p` and `q^n` are both odd, so `2^m` is
even. A node with `k(p) = 0` is a **root**.

**2.2 (chain).** A directed path `q_0 -> ... -> q_k` with `q_0` a root. The graph
is acyclic because `p = 2^m + q^n > q`.

**2.3 (chain polynomial).** `P_0 = x`, and for `1 <= i <= k`

    P_i = P_{i-1}^{n_i} + 2^{m_i}.

Then `P_i(q_0) = q_i`, and in particular `P(q_0) = p` for `P = P_k`. Its degree
is `N_i = n_1 ... n_i`, so `N = deg P = prod n_i`.

**2.4 (word).** Runs of `n = 1` edges compose to a single addition, so every
chain polynomial equals

    P = ( ... ((x + A_0)^{n_1} + c_1)^{n_2} + c_2 ... )^{n_k} + c_k

with `n_i >= 2`. `A_0` and each `c_i` is a sum of powers of two — the powers
contributed by the `n = 1` edges of that run together with the `2^{m}` of the
`n >= 2` edge that closes it. The **skeleton** is the list `[n_1, ..., n_k]`;
`N = prod n_i` is unchanged, since `n = 1` edges contribute a factor 1.

**2.5 (spectrum of `p`).** The set of `(root, word)` pairs over all chains into
`p`, each with the number of chains realizing it.

**2.6 (normalized state).** For a chain into `p`,

    S = P - p,

that is, `P` with `p` subtracted from its constant term, read in `(Z/l^e)[x]`.
`S(p)` denotes the set of normalized states over all chains into `p`.

**2.7 (edge operator).** With `tau_c(f) = f + c`, an `n >= 2` edge out of `q`
acts on normalized states as

    T_{q,n} = tau_{-q^n} o pow_n o tau_q,     T_{q,n}(S) = (S + q)^n - q^n.

**2.8 (tower).** Level `e` is the reduction of everything above to `Z/l^e` with
`l = 2`. Raising `e` refines; truncation `Z/2^{e+1} -> Z/2^e` carries level
`e+1` onto level `e`.

## 3. Expressions

**3.1 (telescoping: why `n = 1` edges are free).** For an `n = 1` edge `q -> p`,
the added constant is `2^m = p - q`, so

    S_p = S_q.

The normalized state is constant along `n = 1` edges, for every modulus, with no
arithmetic.

**3.2 (the `n >= 2` step).** Expanding §2.7,

    S_p = T_{q,n}(S_q) = sum_{j=1}^{n} C(n, j) q^{n-j} S_q^j.

Only `q` enters, never `m`; the `2^m` cancelled against `p` in the
normalization. `q < p^{1/n} <= sqrt(p)`, so every polynomial step is anchored
below the square root of the window.

**3.3 (roots).** At a root `r`, `P = x`, hence

    S = x - r.

**3.4 (level one is closed form).** Modulo 2 every `q` is a unit `q ≡ 1`, so
`S + q ≡ S + 1`. With `S = x^{N_i} + 1` this gives `S + 1 = x^{N_i}` and
`T(S) ≡ x^{N_i n} + 1`. Since §3.3 gives `S ≡ x + 1` at every root,

    S ≡ x^N + 1   (mod 2),    equivalently   P ≡ x^N   (mod 2).

The level-one class is the degree and nothing else: not `m`, not `q`, not the
order of the edges. Consequently the number of distinct level-one classes over a
family equals the number of realizable degrees `N`.

**3.5 (degree and exponent bounds).** A chain into `p` with root `q_0 >= 3`
satisfies `p = P(q_0) > q_0^N`, so

    N < log_3 p.

The same argument on one edge: `p = 2^m + q^n > q^n` gives `n < log_q p`. Since
`q` is an odd prime, either `q = 3` and `n < log_3 p`, or `q >= 5` and
`n < log_5 p`.

**3.6 (which terms survive level `e`).** In §3.2 the coefficient of `S^j` is
`C(n, j) q^{n-j}`. Two independent filters:

- *Kummer.* `v_2(C(n, j))` is the number of carries when adding `j` and `n - j`
  in base 2, so the `j`-th term survives mod `2^e` only when that carry count is
  less than `e`. At `e = 1` this is Lucas: only `j` whose binary digits are a
  submask of `n`'s survive. That is what collapses §3.2 into §3.4.
- *The unit part.* `q` is odd, so `q^{n-j} mod 2^e` depends on `q mod 2^e`.

**3.7 (where the `m`-record re-enters).** `q = 2^{m'} + q'^{n'}`, so `q mod 2^e`
exposes exactly the exponents `m' < e`: **level `e` resolves the exponents below
`e`.** The first instance is explicit. At `e = 2`:

- `n = 2`: `T(S) = S^2 + 2qS ≡ S^2 + 2S`, since `2q ≡ 2 (mod 4)` for odd `q` —
  still no dependence on `q`.
- `n = 3`: `T(S) = S^3 + 3qS^2 + 3q^2 S`; `q^2 ≡ 1`, but `3q` is `3` or `1`
  according to `q mod 4`.

So odd exponents are what first admit the parent's residue, and they do it at
level two.

**3.8 (Hermite carries one edge's `m` in `He_0`).** Expanding a single
generator,

    x^n = sum_j C(n, 2j) (2j-1)!! He_{n-2j}(x),

so for one edge `x^n + 2^m` every Hermite coefficient except `He_0` depends on
`n` alone, and the entire `m` of that edge sits in `He_0`: adding a constant
moves nothing else.

**3.9 (Artin-Schreier equivalence).** `℘(z) = z^2 - z` is additive with kernel
`F_2`, and `L_psi(f) ~ L_psi(g)` whenever `f - g` lies in its image, since
`Tr_{F_q/F_2}` kills that image. Taking `℘(c x^j) = c^2 x^{2j} + c x^j`,

    L_psi(a x^{2j})  ~  L_psi(a^{1/2} x^j)     for a a square in the base

In `F_2` every element is a square. In `F_2[t]` the squares are `F_2[t^2]`, so
`t^m` is a square iff `m` is even.
