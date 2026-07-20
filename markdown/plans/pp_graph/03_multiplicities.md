# 03 — routing multiplicities against the sieve null

The first measurement designed to find mess rather than confirm structure:
collapse-class multiplicities are counts the free algebra says nothing about.

- [x] Turn the reachability DP into a counting DP: number of n=1 routings
      between each feeder endpoint and each power-edge source (universe
      ≤ √max_p, exact counts, 64-bit).
- [~] Sieve null for the same counts — CRUX, needs your steer (see findings).
- [ ] Compare per class and in aggregate (blocked on the null decision).
- [x] Record findings.

## Findings (partial)

Routing-count DP validated exactly: `paths(137) = 16`, matching the away
session's pure-translation collapse class for 3→137, and the 16 routes
enumerate cleanly. The DP is `paths(b) = sum_m paths(b-2^m)` over primes b with
b-2^m prime, root `paths(3)=1` — one streaming pass in prime order.

Raw structure (B=1e6, 1e7):

- Pure-translation routing counts grow explosively: mean ~10-30x per bit,
  var/mean ~1e15 at 1e7, max 3.2e16. This is combinatorial DAG path growth, not
  a statistical signal on its own.
- Mean prime out-degree in the n=1 graph = **1.867**, essentially the k-census
  mean 1.882 (handshake: mean out-degree = mean in-degree = mean k up to the
  n≥2 edges). Per-band out-degree declines 4→1.64 toward 1/ln2, but that decline
  is the finite-B truncation ceiling, not structure. Consistency check, not news.

The crux — the null. A naive independence null `F(b)=dens(b)·sum_m F(b-2^m)`
with `dens=1/ln` gives actual/null decaying to ~1e-5 by bit 13: real all-prime
chains are far rarer than independence. But that gap is dominated by the
covering obstructions (mod-3 parity lock etc.) compounding along the chain —
i.e. mostly **known local structure**, and 1/ln is the wrong null to difference
against. The scientifically meaningful comparison is actual vs a
**covering-corrected** chain null, and that is exactly where the extensive
lab-notes covering work lives. Building it from scratch here risks rederiving
those results and presenting them as findings.

Decision needed from the user before proceeding: what null is meaningful for
chain multiplicities, and does a covering-corrected chain null duplicate prior
work or extend it? This is the same local-global question as lab-notes
2026-03-18 §5, lifted from single primes to chains.
