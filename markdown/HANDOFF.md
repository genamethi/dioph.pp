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

## Immediate Directions

Roughly in dependency order. Each is a starting point, not a spec.

1. Declare sort order. To fit the spec design shape we need to change it so that
   tables declare their sort order at creation.
   We can leverage the IRC spec.

   Make the table creation process enforce, and give a clear
   return signal when unimplemented, such as NotImplemented.

2. Bringing generate back to a workable state:

   Decided to make initialization deliberate. That is to say,
   if a table doesn't exist, then it's only created when specified with --init.
   Set ShapePolicy.file_target_bytes, rgs_per_file shadow
   write.target-file-size-bytes, etc. at table creation, read from metadata.

3. Implement pp-graph for graph analysis of structure leveraging Hermite
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

4. **Settle consumer server-side planning.** `rest_scan_plan` exists and works;
   it is linked only into the e2e test. Either wire `source_scan` /
   `query_service` to it — which also decides where mode dispatch lives — or
   accept that in-process planning is the real path and the REST client is for
   foreign consumers. `generate` is already a REST client on both resume reads,
   so the producer's REST-ness is not the gap; what it does not use is the
   *spec* planning routes.
5. ~~**Settle configuration.**~~ done

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
