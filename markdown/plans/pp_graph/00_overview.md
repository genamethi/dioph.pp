# pp-graph — plan overview

Base branch: `tui-query`. Working branch: `pp-graph-exp`. The colab branch
contributes mathematics only (scripts portable as needed).

Object: the chain graph of `primeparts.partitions` — edges `q --(m,n)--> p`,
canonical words, Hermite expansions — driven through the client idiom this plan
establishes. The experiment (`native/src/graph/pp_graph_exp.cc`, commit
cda13f4) proved the wiring: expressions → server-side scan planning → reader
over the server's task set → GiNaC → flint, 8s at B=5e9, census exact against
the reference numbers, composite-degree spectrum full.

## Decisions of record

| date | decision |
|---|---|
| 2026-07-19 | consumers plan via the REST routes; the in-process planner is the server's engine and an explicit client-mode fallback, never the consumer default |
| 2026-07-19 | client obtains `TableMetadata` from `loadTable` over REST, not from `metadata.json` on disk (the experiment's disk read is retired in phase 01) |
| 2026-07-19 | interfaces are authored header-first and reviewed before implementation |
| 2026-07-19 | graph/word/satisfaction computation stays hand-built; a query engine is evaluated only for data pipelining (phase 04 decides, nothing pre-committed) |
| 2026-07-20 | scan cost has two regimes: cold full scan of the 181 GiB table is disk-bound on the USB SATA link (~447 MB/s, ~408s, 131% CPU, batch size irrelevant); a cached or small working set is decode-bound, where read.batch-size dominates (4096 to 262144 = 1.8x at fixed shards) and threads scale with file count. read.batch-size is now a SessionOptions knob. The power-edge slice is ~2 MB, so extracting it once puts all downstream work in the fast decode-bound regime |
| 2026-07-21 | 04/05 reworked after the collapse exploration: substrate-first (DuckDB + Acero + Substrait, non-JVM) then the collapse consumer (first FileScanTask consumer + query engine). Edges are read from `partitions`, never `is_prime_power`'d; the `q_k->p` pivot is DuckDB-managed (cached, not fully materialized); termination points come from the `primes ⟝ partitions` anti-join (k=0 roots). Collapse is lossless and bound-invariant (resumable) — see `../../math/collapse_findings.md` |

## Invariants

- Read-only against the live warehouse throughout this plan; live-warehouse
  mutations are user-only.
- Zero code comments; spec names for new types.
- Every phase boundary compiles; one commit per phase, message = the phase
  document.
- Vendor/ untouched.

## Phases

- 01 shared client module, header first (done)
- 02 full-dataset run (done)
- 03 routing multiplicities (done)
- 03a exponent tail and the 3|n obstruction (done)
- 04 engine substrate — DuckDB + Acero + Substrait, non-JVM (substrate-first)
- 05 the collapse consumer + derived representation
- 06 config + merge (bracketed)

The collapse exploration (`../../math/collapse_findings.md`) reshaped 04/05: they
now build the first real FileScanTask consumer + the query engine behind it
(holes registry: open ground), on the committed non-JVM stack.
