# Additive Partitions of Prime Power Remainders

## Motivation

The funbuns project studies the exponential Diophantine equation

    p = 2^m + q^n

where p, q are prime and m, n >= 1. Roughly 17% of primes are *obstructed* -- no such partition exists. The goal of this note is to reformulate the problem in terms of additive representations and polynomial expressions, making it amenable to tools from algebraic geometry and p-adic analysis.

## From exponential to polynomial

### The remainder

Fix a prime p and set X = p - 2^m for some m >= 1. The original question "does q^n = X for some prime q?" becomes a question about the arithmetic structure of X. More generally, we ask: what are the partitions of X into prime powers?

### The epsilon bound

When X = 1, we have p = 2^m + 1, so

    log_2(p) = m + epsilon

where epsilon = log_2(1 + 2^{-m}). Taylor expansion gives:

    epsilon = 2^{-m} / ln(2) - 2^{-2m} / (2 ln 2) + ...
            ~ 1 / ((p - 1) ln 2)
            ~ 1 / (p ln 2)

This is exponentially small in m and goes to zero like 1/p. For general X:

    epsilon = log_2(1 + X / 2^m)

Since we choose m = floor(log_2(p)), we have X < 2^m, so epsilon < 1. The epsilon value measures how far log_2(p) is from an integer, equivalently, how much "room" there is in the gap between p and the nearest power of 2.

### Additive representation over F_2

Write X as a sum of distinct prime powers with binary coefficients:

    X = sum_i a_i * q_i^{e_i},   a_i in {0, 1}

where (q_i, e_i) ranges over pairs (prime, positive integer) with q_i^{e_i} <= X.

Treating the primes as formal indeterminates x_1 = 2, x_2 = 3, x_3 = 5, ..., this becomes a polynomial:

    P(x_1, x_2, ...) = a_1 * x_1^1 + a_2 * x_1^2 + ... + a_k * x_2^1 + ...

and we seek binary vectors (a_1, ..., a_D) in F_2^D such that evaluation at x_j = p_j yields X.

### Dimension bounds

Each prime q contributes a "vector space" of basis elements {q^1, q^2, ..., q^{floor(log_q(X))}}. The dimension:

    dim(V_q) = floor(log_q(X))

For X ~ 10^9 (typical for the funbuns dataset):

| Prime q | dim(V_q) = floor(log_q(X)) |
|---------|----------------------------|
| 2       | 29                         |
| 3       | 18                         |
| 5       | 12                         |
| 7       | 10                         |
| 11      | 8                          |
| 31623+  | 1 (only q^1 fits)          |

The total dimension D(X) = sum_{q prime, q <= X} floor(log_q(X)). Most of this comes from the ~pi(X) primes contributing only q^1 (i.e., primes q > sqrt(X) where only the first power fits). The rich structure is concentrated in small primes with multiple usable powers.

## Greedy vs minimal partition

Two natural algorithms bracket the solution space:

**Greedy** (largest prime power first at each step):
- Absorbs X quickly; each subsequent prime contributes minimally
- Gives an upper bound on partition length
- Establishes the *minimal required contribution* from each subsequent prime
- Converges fast -- most of X is eaten by the first few terms

**Minimal** (smallest valid contribution from each prime):
- Keeps the remainder large; pushes burden forward
- Each subsequent prime must contribute *maximally*
- Gives insight into the *maximal required contribution* from each subsequent prime
- Reveals how many primes must participate

Where greedy and minimal agree on a prime's contribution, that contribution is *forced*. Where they diverge, the variety has positive dimension -- there is genuine freedom in the partition.

## Young tableaux structure

The partition has a natural tableau representation. Arrange a grid with:
- Rows indexed by primes (q_1 = 2, q_2 = 3, q_3 = 5, ...)
- Columns indexed by exponents (e = 1, 2, 3, ...)
- Cell (q, e) is filled if a_{q,e} = 1 (the term q^e is included)

Example for 23 = 2^2 + 2^4 + 3:

```
         e=1  e=2  e=3  e=4
q=2:     [ ]  [x]  [ ]  [x]    4 + 16 = 20
q=3:     [x]  [ ]               3
q=5:     [ ]  [ ]               sum = 23
```

The conjugate tableau swaps the view: for each exponent level e, which primes participate? This distinguishes:
- **Broad partitions**: many primes at low exponents (Goldbach-like, many rows of length 1)
- **Deep partitions**: few primes at high exponents (close to prime power, few long rows)

The `dominant_share` metric in the existing funbuns analysis measures exactly this depth/breadth tradeoff: dominant_share ~ 1 means one prime absorbs almost all of log(X) (deep); dominant_share ~ 1/k means k primes share equally (broad).

## The case p = 23

23 is the first prime that cannot be written as 2^m + 3^n:
- 23 - 2 = 21 = 3 * 7 (not a power of 3)
- 23 - 4 = 19 (prime, not 3)
- 23 - 8 = 15 = 3 * 5 (not a power of 3)
- 23 - 16 = 7 (prime, not 3)

But 23 has length-2 partitions into prime powers:
- 23 = 4 + 19 = 2^2 + 19^1
- 23 = 16 + 7 = 2^4 + 7^1

And it has the standard partition 23 = 16 + 4 + 3 = 2^4 + 2^2 + 3^1 (length 3, reusing base 2).

The distinct prime powers {2, 3, 4, 5, 7, 8, 9, 11, 13, 16, ...} form a *complete sequence* -- every sufficiently large integer can be represented as a sum of distinct elements. So representations always exist for large X. The interesting questions are:
1. What is the minimum length?
2. How does the partition count grow?
3. Which primes are forced to participate?

## Fixed-modulus approach

### Reducing to polynomial congruences

Working in the fixed-modulus ring Z/ell^k Z (SageMath: `Zp(ell, prec=k, type='fixed-mod')`), the exponential equation becomes polynomial:

    p = 2^m + q^n  (mod ell^k)

For a fixed ell, the sets {2^m mod ell^k : m >= 1} and {q^n mod ell^k : q prime, n >= 1} are finite. Their sumset {2^m + q^n mod ell^k} determines which residue classes of p can possibly admit partitions. Primes in the complement are *locally obstructed* at ell.

### Hensel lifting and obstruction depth

Start with solutions mod ell. Attempt to lift to mod ell^2, then mod ell^3, etc. The *obstruction depth* at ell is the smallest k such that no solution exists mod ell^k. Deeper obstructions are more "fundamental" -- they survive more levels of approximation.

The fixed-mod type is efficient for this: elements are just integers mod ell^k, with no precision-tracking overhead. The PARI/NTL backend handles the ring arithmetic.

### Local-global principle

Combining local information across primes via the Chinese Remainder Theorem: if p is locally obstructed at ell_1 and ell_2 with gcd(ell_1^{k_1}, ell_2^{k_2}) = 1, then p is obstructed in a specific residue class mod ell_1^{k_1} * ell_2^{k_2}. Primes in these CRT classes are *structurally obstructed* -- their obstruction is forced by congruence conditions alone.

Primes that are obstructed but NOT in any structurally-obstructed CRT class are more mysterious -- their obstruction has a deeper, non-local origin.

## Connection to existing funbuns analysis

| Existing metric | Connection to this framework |
|-----------------|------------------------------|
| `dominant_share` | Depth vs breadth of partition (single-term approximation quality) |
| `omega(r)` | Number of distinct primes in the factorization of X (related to but different from partition length) |
| `v_ell(r)` | Constrains possible (q, n) pairs: if X = q^n and ell != q, then v_ell(X) = 0 |
| Zipf on q_k | Distribution of which primes appear as bases -- reflects the "broad" structure |
| Obstruction spectrum | Fourier analysis of chi(p); fixed-mod analysis may explain the spectral peaks |

## Underutilized data

The block data {p, m_k, n_k, q_k} contains information we aren't fully exploiting:

- **n_k exponents**: The distribution of exponents n in q^n directly constrains the additive framework. Barely analyzed.
- **Joint (m_k, q_k, n_k)**: For non-obstructed primes, the actual partition structure. Which (m, q) pairs co-occur?
- **Near-unique partitions**: Primes with exactly one valid (m, q, n) triple. These boundary cases may reveal the obstruction mechanism.
- **Residue class correlations**: chi(p) correlated with p mod M for composite M (not just prime moduli).
- **Divisibility constraints from v_ell**: If r = q^n for some prime q != ell, then v_ell(r) must equal n * v_ell(q). The v_ell columns we already compute encode this constraint but we aren't checking it.

## Related projects

- **factorsums** (`../factorsums`): Counts partition representations n = p^j + q^k for primes n. SageMath-based, ~12k primes. Uses `Partitions(n, length=2)` and perfect power detection.
- **pppart_collected** (`../pppart_collected`): Same equation with l-adic extensions, q-adic metric spaces, and topological data analysis. Introduces modular sequence analysis (n mod ell^i for varying i). Observes ~7-15% zero-partition rate with phase transitions.

## Next steps

1. Implement fixed-modulus analysis (`fixed_mod.py`): power residue sets, local solution checks, obstruction depth via Hensel lifting, CRT combination.
2. Compute obstruction depth distribution across the dataset -- a new invariant complementing dominant_share.
3. Identify structurally-obstructed CRT classes and measure what fraction of obstructed primes they explain.
4. Correlate spectral peaks in the obstruction indicator with specific l-adic structures.
5. Explore the greedy/minimal partition bracket for a sample of obstructed primes.
6. Connect partition counting from factorsums/pppart to the additive partition framework.
