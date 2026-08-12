# HANDOFF for agents

## Guidance for agents

Please excise things once they're done. No need to have running commentary. No
progress tracker. No narrating.

Furthermore, don't add high level summaries. Keep it grounded, technical and
actionable.

I don't want any comments in the code.

---

## Tables

- `primeparts.primes`: one row per prime, `p`-ordered. $\min(p)=3$ (p=2 absent);
  no primes skipped between 3 and $\max(p)$.
- `primeparts.partitions`: length-two integer partitions of odd `p` into prime
  power summands — by parity a power of two plus an odd prime power. Each row's
  `(m_k, n_k)` **characterizes** one such partition for that `p`. Most edges are
  `n=1`.

- Any other tables referenced have been retired or are deprecated (and any
dependent code should be fixed.)

## Binaries and what links into them

| Binary | Entry | Notably links |
|---|---|---|
| `primeparts-generate` | `generate.cc` | writer, aligned_writer, schemas, pp_commit, partition_stats |
| `primeparts-catalogd` | `pp_catalogd_main.cc` | pp_catalogd, plan_store, scan planner, lmdb store |
| `primeparts-verify` | `verify_main.cc` | verify, source_scan, scan planner |
| `primeparts-tui` | `tui_main.cc` | tui_*, query_service, materialize, lua_presets, source_scan |
| `pp` | `query/pp_main.cc` | lua_query_module, query_service, materialize, source_scan |
| `pp-graph` | `graph/pp_graph.cc` | pp_graph_store, session, rest_scan_plan, source_scan, materialize |
| `primeparts-bench-core`, `primeparts-bench-materialize` | `bench.c`, `materialize_bench.c` | core only |

Consumers of scan plans are `source_scan.cc` (the generic reader),
`query_service.cc` — both plan **in-process** from metadata they read off disk —
and `client/session.cc`, which dispatches on the advertised `scan-planning-mode`
and is what `pp-graph` reads through.

## ACTIVE WORK HERE ON ...

## Immediate Directions

Roughly in dependency order. Each is a starting point, not a spec.

1. Implement pp-graph for graph analysis of structure leveraging Hermite
   polynomials.

   I want to explore the structure of these solutions based on properties like
   the number of solutions per prime. q is a parent when p = 2^m + q^n is 
   satisfied. Composing solutions we get chains, and I've noticed the binomial
   expansion yields expressions that are obvious combinations of Hermite
   polynomials. I want to see if we can leverage any resulting structure.
   (more on this: chain_hermite_primer.md, collapse_findings.md,
   research_programme.md.)

   We want to have some reusable data which we can leverage for analysis via the
   IRC interface as it stands today.

   The resulting data to be amenable to usage with ginac expression types
   (which decompose to subexpressions). This should allow us to do analysis on a
   given range of prime values.

   One hope is that we will be able to use these as formal expressions for
   evaluation over the dataset (a la expressions in Polars for example; i.e.,
   these should give us constraints for satisfaction over subsets of primes.)

   First round of consumers for this data:

   - produce Hasse-diagram(s) corresponding to a queried set of primes,
     and chain decompositions
   - want to see how the k-many ways a single p is expressed, so those collections
     can be compared within a k value and between k values.
     Particularly curious about this point because it says something along with
     the congruences about how the density of solutions might behave (although
     this is one thing we know fairly well).
   - the degree spectrum, and grouping primes by the shape of their coefficient
     sets

   One thing I'm considering is how the arithmetic properties of congruences over
   the coefficients or exponents affect the number of solutions as they appear.
  
   Anything we can figure out about de Polignac numbers may be fun here.

   Everything needs to work scale and be reasonable to work within for a dataset
   covering all primes under 64 bits (and hopefully beyond, really).
   Point is we don't want toy examples that are only workable on a bounded prefix.

2. **Settle consumer server-side planning.** `rest_scan_plan` exists and works;
   `client::Session` dispatches on the advertised mode and `pp-graph` reads
   through it. `source_scan` / `query_service` still plan in-process from a local
   metadata path and never consult the advertisement — wire them to `Session`,
   or accept that in-process planning is the real path for them. `generate` is
   already a REST client on both resume reads, so the producer's REST-ness is
   not the gap; what it does not use is the *spec* planning routes.
3. ~~**Settle configuration.**~~ done

4. **One runner, one declared config.** Three parts of one thread.

   (a) Too many binaries with their own entry points. Prefer two interfaces:
   TUI and CLI. A runner is the front door; per-binary entry points may stay
   behind it. C/C++ with Lua embedded, so the runner stays user-editable and
   scriptable — Lua for lightness and speed, and threading is not a runner
   concern. Python has better interactive-progress libraries (tqdm); a Lua
   equivalent is unknown and may have to be built.

   (b) Declare config in a header: one struct carrying the Lua key name beside
   the field, with an optional validated-field struct alongside. Generate
   `example.config.lua` from that declaration on the default `make` target
   instead of authoring it. Same spirit as argparse or Doxygen — one
   declaration drives the surface. The generator has to run ahead of the embed
   step that builds `build/example_config.cc`.

   Pain point this must fix: a key absent from an existing `config.lua` is a
   hard error today. It should be appended dynamically.

   (c) TUI stays separate — reconsider grouping it under the CLI's
   `config.lua` at all. The settings screen may come back; keep the `$EDITOR`
   route either way. Screen was replaced in `310528f`.

   Drop `return conf` unless something principled needs it. It buys `require`
   today and nothing uses that. Sequence is check, then `load`, then execute.
   The check wants an LPEG Lua grammar verifying the parsed AST is a subset of
   the valid ones: whitespace-permissive, little custom code, and another
   argument for a Lua runner. LPEG is not vendored. The loader calls
   `luaL_dofile` today, which is `luaL_loadfile` + `lua_pcall` in one — split
   it so the check lands between them.

## Lower priority leftovers: 

1. **Lifecycle.** Snapshot expiry is unwired, orphaned files from failed
   transactions are unreachable, rollback has no surface. All three are small
   against APIs that already exist.
2. **Derived read indexes** for fast number-theoretic reads (approach open).
3. **Views** —
   `https://raw.githubusercontent.com/apache/iceberg/refs/heads/main/format/view-spec.md`
4. **Janitor** for killed-run `.pp-staging` debris.
6. ~~**Compute `q_k` Dynamically**~~ done
7. **Screening candidates before `pp_is_prime_power_u64`.** Set B in
   `process_prime` hands every `q = p - 2^m` to FLINT. A screen over small
   bases resolves most of them without it: one base dividing `q` means `q` is a
   prime power only if it is a power of that base, two means it cannot be one.
   Carrying `p mod b` and `2^m mod b` makes the per-`m` test a comparison and
   the step a shift plus conditional subtract, so no division is involved.
   Measured 25% off a rank-1..1e6 / 5e7 / 1e9 workload at 6 bases, with the
   width having a clear optimum since the screen costs `O(max_m * N)` per prime
   while base `b` only rejects a further `1/b`.

   Not taken, because it presumes FLINT's `n_is_prime` does not already do this
   better internally, and that was never checked. Before building it, read what
   `n_is_prime` actually does and compare against how Sage wraps the same
   libraries — `src/sage/rings/integer.pyx`, `is_prime_power` around :5306 and
   the perfect-power path near :5437.
