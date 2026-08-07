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
| `primeparts-bench-core`, `primeparts-bench-materialize` | `bench.c`, `materialize_bench.c` | core only |

Consumers of scan plans are `source_scan.cc` (the generic reader) and
`query_service.cc`. Both plan **in-process** from metadata they read off disk.

## Not wired to anything

Start here when deciding what is alive.

- `catalog/rest_scan_plan.{h,cc}` — the four client planning calls and
  `PlanScanOnServer`. Linked only into the e2e test; **no shipped binary calls
  it**.

## ACTIVE WORK HERE ON ...

## Configuration loose end

- Disjoint config surfaces.`config::Load` (`config.{h,cc}`) reads `config.lua`'s
  `config({...})` into a flat `map<string,string>`; `generate` consumes exactly
  three keys — `rest_uri`, `namespace`, `warehouse`. The TUI keeps its own
  six-field `App::cfg` (log limit, gen threads, default limit, log format,
  autosave, warehouse). Only `warehouse` overlaps. Neither surface knows about
  the other's keys.

## Immediate Directions

Roughly in dependency order. Each is a starting point, not a spec.

1. Implement pp-graph for graph analysis of structure leveraging Hermite
   polynomials.

   I want to explore the structure of these solutions. Grouping by p by k is of
   interest when studying classes of solutions. 

   Basic graph shape is given by: q is a parent when p = 2^m + q^n is 
   satisfied. Composing solutions we get chains, and I've noticed the binomial
   expansion yields expressions that are obvious combinations of Hermite
   polynomials. I want to see if we can leverage the structure that emerges.

   To that effect I'd like some reusable data which we can leverage for
   analysis via the IRC interface as it stands today. It should amenable to
   usage with ginac expression types (decomposes to subexpressions). This
   should enable  queries for various algebraic or topological properties given
   a range of prime values.

   First round of consumers for this data:

   - produce Hasse-diagram(s) corresponding to a queried set of primes,
     and chain decompositions
   - want to see how the k-many ways a single p is expressed, so those collections
     can be compared within a k value and between k values.
     Particularly curious about this point because it should give insights
     when considered alongside the congruences.
   - the degree spectrum, and grouping primes by the shape of their coefficient
     sets

   N.B. Everything needs to work scale and be reasonable to work within for a dataset
   covering all primes under 64 bits (and hopefully beyond, really).
   Point is we don't want toy examples that are only workable on a bounded prefix
   or some unmotivated restriction just to hide a design that doesn't scale
   on truly general datasets (over large ranges compared between one another
   or won't allow for taking the projective limit, i.e., doing p/l-adic work)

2. The spectrum of a prime, as something we can ask for by name.

   Each partition edge is a map x -> x^n + 2^m, and composing along a chain
   gives a polynomial which maps q to p. Every prime carries k many solutions.
   Provisionally, I'm referring to it as the spectrum of p.

   We want this to enable compute spectra for a general set, with our common
   predicates.

   The sets I care about first are the ones we can already select: everything
   with a given k in a window, and pairs of twin primes — p and p + 2 with both
   of them prime. For such a pair I want to compare Spec(p) and Spec(p + 2)
   see what falls out.

   First round of consumers for this data:

   - the spectrum of a named p
   - the spectra of a family { p : k(p) = K } over a window
   - the spectra of a pair of twin primes, side by side

   Same constraint as above: this has to be reasonable over the whole dataset,
   not a bounded prefix.

3. The monoid the edge maps generate mod l, and its orbits under translation.

   Reduce the edge maps mod l. They generate a monoid M of maps of the affine
   line over F_l. The translations x -> x + c lie in M and form a group T. T
   acts on M by post-composition, f goes to tau_c composed with f. The action is
   free, and two maps share an orbit exactly when they differ by a constant. So
   T\M is the set of chain maps up to an additive constant.

   Classify each prime by which orbits its chains realize and which it never
   reaches. Do the same for the families in 2.

   First round of consumers for this data:

   - T\M over the dataset, per l
   - the orbits realized by a given p
   - the orbits p misses, and which (m, n) in its ancestry account for that

   l = 2 kills every translation, so odd l is the range.

4. The same classification at l^2, l^3, and in the limit.

   Repeat item 3 over Z/l^e with coefficients in Z_l. Reduction from Z/l^(e+1)
   to Z/l^e carries orbits to orbits, so the classifications form an inverse
   system. Build it so raising e extends the data rather than recomputing it.

   Then ask whether the realized set is constant across stretches of p and
   changes only at isolated primes. That is the condition for these to be
   sections of a sheaf on a stratification rather than a table of answers.

   If it stratifies, cohomology is the next question. I am not asserting there
   is any. The point of building it is to make which solutions exist as p grows
   a computation, twin primes being the ambitious version, rather than another
   count.

   First round of consumers for this data:

   - realized orbits at l and l^2 for the same primes, and the map between levels
   - where they hold constant across p, and where they change
   - a read keyed by p: which orbits does this prime realize, at level e

5. **Settle consumer server-side planning.** `rest_scan_plan` exists and works;
   it is linked only into the e2e test. Either wire `source_scan` /
   `query_service` to it — which also decides where mode dispatch lives — or
   accept that in-process planning is the real path and the REST client is for
   foreign consumers. `generate` is already a REST client on both resume reads,
   so the producer's REST-ness is not the gap; what it does not use is the
   *spec* planning routes.
6. **Settle configuration.** One surface, one warehouse default. However, a
  qualification and some clarifications: (a) Iceberg tables being defined by
  schemas.cc is fine when they're tightly coupled to a binary. What the final
  shape will likely be is that we'll have a client surface for deriving tables
  from data and committing those. For the user directed approach then, we can
  use a Lua interface. (b) By "warehouse default" I'd prefer localhost, the
  current default port, and ~/local/share/pp-data/ as the default. This is fixed
  via an example config.lua which resides in the same directory as the other
  binaries. (default is ~/.local/bin) (c) The Lua config table should have
  general reusable variables in a config.core table. Then binary specific config
  variables (all flags checked against the config, and overidden.). I.e., all
  default behavior is determined by the example.config.lua. (d) The example
  config should trigger a message on init to switch away from the defaults. (e)
  For the time being, this config is separate from any table or schema
  definitions. That can wait. (f) config.tui, config.graph, config.generate are
  some examples. config.test might be appropriate, but consider that we're using
  gtest and whether we want to wrap around that or keep separate. (g) This
  should be enough groundwork to help tie up the loose ends in the previous
  section.


## Lower priority leftovers: 

1. **Lifecycle.** Snapshot expiry is unwired, orphaned files from failed
   transactions are unreachable, rollback has no surface. All three are small
   against APIs that already exist.
2. **Derived read indexes** for fast number-theoretic reads (approach open).
3. **Views** —
   `https://raw.githubusercontent.com/apache/iceberg/refs/heads/main/format/view-spec.md`
4. **Janitor** for killed-run `.pp-staging` debris.
6. **Compute `q_k` Dynamically:** The `q_k` column was removed from the
   `partitions` table. Update all consumers (`query_service.cc`, `verify.cc`,
   `lua_query_module.cc`) to solve for `q_k` algebraically from `(p, m_k, n_k)`
   on the fly.
