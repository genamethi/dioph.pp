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

Items 1 through 4 are one line of enquiry: what algebraic object does a prime's
set of representations p = 2^m + q^n form, once chains are composed into
polynomials — and is which representations exist, rather than how many,
governed by structure that survives reduction mod l? If it is, the existence
questions become computable in finite characteristic, which is the only route
by which something like twin primes could come out of this rather than another
density statement.

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

2. The spectrum of a prime, as something we can ask for by name.

   Each partition edge is a map x -> x^n + 2^m, and composing along a chain
   gives a polynomial in the root, so every prime carries a whole collection of
   these — I've been calling it the spectrum of p. I want to be able to hold
   that collection for a p I name, and for a set of primes I specify out of the
   dataset rather than one at a time.

   The sets I care about first are the ones we can already select: everything
   with a given k in a window, and pairs of twin primes — p and p + 2 with both
   of them prime. For such a pair I want to put Spec(p) and Spec(p + 2) next to
   each other and see whether anything survives the comparison.

   First round of consumers for this data:

   - the spectrum of a named p
   - the spectra of a family { p : k(p) = K } over a window
   - the spectra of a pair of twin primes, side by side

   Same constraint as above: this has to be reasonable over the whole dataset,
   not a bounded prefix.

3. The monoid these maps generate mod l, and what the translations leave behind.

   Reduce the edge maps mod l and they generate a monoid of maps of the affine
   line. The translations x -> x + c live in that monoid and act on it, and I
   want to know what the action leaves behind — the orbits — because that seems
   to be where anything that isn't forced by the action would have to live.

   The part I actually care about is per prime: which of those classes p's own
   ancestry realizes, and which it never does. The ones it misses are the
   interesting half, and I'd like to be able to look at them for a prime or for
   one of the families above.

   First round of consumers for this data:

   - the orbit space itself, per l, over the dataset
   - the classes realized by a given p
   - the classes p misses, and what in its ancestry accounts for that

   l = 2 looks like it sees nothing here, so odd l is the range of interest.

4. The same data at l^2 and beyond, and whether a sheaf falls out.

   I want to climb from l to l^2 to l^3 with coefficients in Z_l, and be able to
   go up a level without starting over. What I'm hoping to see is how the
   realized classes behave as p varies — whether they hold steady over stretches
   of primes and jump somewhere in particular, since that's the shape that would
   make this a sheaf rather than a pile of tables.

   Whether there's cohomology in it is the open question; what I want built is
   the object that lets us test it. If it works, the payoff is a handle on which
   solutions exist as p grows — twin primes are the ambitious version of that —
   rather than another statement about how many.

   First round of consumers for this data:

   - realized classes at l and at l^2 for the same primes, and the map between
     the levels
   - where those classes stay constant across p, and where they change
   - a read keyed by p: which classes does this prime realize, at this level

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
