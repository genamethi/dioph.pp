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

## Invariants

- Read-only against the live warehouse throughout this plan; live-warehouse
  mutations are user-only.
- Zero code comments; spec names for new types.
- Every phase boundary compiles; one commit per phase, message = the phase
  document.
- Vendor/ untouched.

## Phases

- 01 shared client module, header first
- 02 full-dataset run
- 03 routing multiplicities against the sieve null
- 04 query-engine evaluation (DuckDB candidate)
- 05 permanent-fixture decision
