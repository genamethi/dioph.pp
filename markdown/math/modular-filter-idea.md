# Modular Covering Filter Idea

This note records a possible native C optimization for the primeparts hot path.
It is intended for a later branch in the native fork, not as a current
implementation commitment.

## Current Hot Path

For each prime `p`, native generation currently tests candidates

```text
r = p - 2^m,  1 <= m <= floor(log2(p))
```

and calls the general prime-power predicate when the candidate has not already
been eliminated by the existing repeated-base exhaustion logic.

The expensive part is not generating `p` or iterating `m`. The expensive part is
the general predicate on surviving `r`: primality testing first, then perfect
power handling if `r` is composite.

## Covering Observation

For an odd prime `q`, if

```text
q | p - 2^k
```

and

```text
ord_q(2) | (m - k),
```

then

```text
q | p - 2^m.
```

Reason:

```text
2^m - 2^k = 2^k(2^(m-k) - 1),
```

and `q | 2^(m-k) - 1` exactly when `ord_q(2) | (m-k)`.

So a small divisor of one remainder propagates along an arithmetic progression
of `m` values. This creates a cheap modular covering map for the candidate
remainders before running the general FLINT predicate.

## Proposed Native Filter

Use a small fixed set of covering primes, initially:

```text
3, 5, 7, 11, 13, 17
```

with orders:

```text
ord_3(2)  = 2
ord_5(2)  = 4
ord_7(2)  = 3
ord_11(2) = 10
ord_13(2) = 12
ord_17(2) = 8
```

Since the main backbone repeats naturally against `Z/12Z`, precompute coverage
bitmasks by residue:

```text
mask[q][p % q] -> 12-bit mask of m mod 12 positions where q divides p - 2^m
```

For a given `p`, combine the per-prime masks and track the covering
multiplicity for each `m mod 12`.

Then, for each candidate `r = p - 2^m`:

```text
if two or more distinct covering primes divide r:
    r is not a prime power; skip the general predicate.

if exactly one covering prime q divides r:
    r can only be a prime power if r = q^e.
    Check this with repeated division by q.
    If exact, record q^e.
    Otherwise skip the general predicate.

if no covering prime divides r:
    fall back to pp_is_prime_power_u64(r).
```

The single-covered case is important. If `q | r` and `r` is a prime power, the
base must be `q`. There is no need to call the general predicate to discover
that. A small exact-power check is enough:

```text
e = 0
while r % q == 0:
    r /= q
    e += 1

exact iff r == 1 and e > 0
```

For this workload, `q` is odd and `p` is prime greater than the small primes, so
the even-prime edge case should not matter in the normal native path.

## Correctness Boundary

The broad per-candidate filter does not require the full covering-system proof.
It only uses direct divisibility facts.

Safe facts:

- If two distinct primes divide `r`, then `r` is not a prime power.
- If exactly one known prime `q` divides `r`, any prime-power representation of
  `r` must have base `q`.
- If the modular mask says `q | p - 2^m`, that divisibility follows from the
  multiplicative-order argument above.

This means the filter can be implemented as a semantics-preserving shortcut:
it avoids general work only when the result is already forced by modular
divisibility.

The stronger 22-class obstruction theorem is a separate optimization. It could
provide a whole-prime early reject for certain residue classes mod `255255`, but
that has narrower coverage and should be treated as a second step.

## Expected Payoff

The expected win is reducing calls to `pp_is_prime_power_u64()`, especially on
candidates that are cheaply known to have multiple small prime factors.

The effect should be measured with counters before judging the branch:

```text
processed primes
total m candidates
multi-covered skips
single-covered exact-power checks
single-covered hits
single-covered rejects
general FLINT fallbacks
general FLINT hits
elapsed time
```

The useful comparison is against the current native hot path on the same rank
range and thread count. Median, mean, and standard deviation should be reported,
because the expected gain depends on how many candidates fall into the covered
positions for the tested prime distribution.

## Implementation Notes

Keep the filter local to the worker and local to `process_prime`. Avoid shared
cross-thread caches or IPC.

A plausible implementation shape:

```text
1. Build static lookup tables for q in {3,5,7,11,13,17}.
2. At the start of process_prime(p), compute p % q for each q.
3. Build per-position coverage for positions 0..11.
4. In the m loop, inspect coverage[m % 12] before calling pp_is_prime_power_u64.
5. Fall back to the existing predicate only for uncovered positions.
```

This should compose with the existing dynamic chunking model. The filter does
not need global state, and it does not change the scheduling shape.

## Risks

The main risk is implementation error in the mask table or in `m mod 12`
handling, not the algebraic shortcut itself.

Specific things to test:

- `m % 12` convention, especially `m = 12` mapping to position `0`.
- Small `p` values where `max_m < 12`.
- Cases where the covering prime equals the candidate remainder.
- Agreement with the current `pp_is_prime_power_u64()` path over a large sample.
- Agreement across rank ranges, not only the first few primes.

If the branch is implemented, add a debug or benchmark mode that runs both paths
and asserts identical partitions before trusting the optimized path.
