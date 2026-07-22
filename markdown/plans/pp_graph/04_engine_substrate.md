# 04 — engine substrate (DuckDB + Acero + Substrait, non-JVM)

Substrate-first (confirmed). Stand up the committed non-JVM compute stack over
the existing native IRC before writing the consumer. Substrait (plan IR) → Acero
(execution, already in vendored Arrow) → DuckDB (DBMS: iceberg/parquet reuse,
out-of-core caching, point lookup), glued by the DuckDB Substrait community
extension. No JVM; Rust only with a killer app (none yet).

- [x] Enable Arrow **Acero + Substrait** — already satisfied: the installed
      `/usr/local` Arrow is 26.0.0-SNAPSHOT and ships `libarrow_acero.a`,
      `libarrow_substrait.a`, `libarrow_compute.a`, `libarrow_dataset.a`. **Flight
      is absent** and stays downstream (phase 06); 04/05 do not need it. Linking
      Acero/Substrait into a target is a link-flag change only, no rebuild.
- [x] Provision **DuckDB CLI + iceberg + substrait(community) extensions** via
      `native/configure` (idempotent presence checks, user-local, no sudo) — the
      CLI is for probing/spec-audit against the IRC.
- [x] Provision **libduckdb (in-process engine)** via `native/configure`: the
      prebuilt bundle (`libduckdb.so` + `libduckdb_static.a` + `duckdb.hpp`/`.h`),
      pinned to the CLI version, installed to `$PREFIX` like arrow/iceberg. Make
      link vars `DUCKDB_CPPFLAGS` / `DUCKDB_LDLIBS` (`-lduckdb`) / `DUCKDB_RPATH`.
      **Decision (2026-07-22): DuckDB is linked in-process, not out-of-process.**
      In-process is the engine the user's design calls for (DuckDB-managed pivot
      cache, point lookup, out-of-core over the plan's Parquet). Smoke-tested:
      shared-link works; the static `.a` needs the extension objects linked
      separately (`ExtensionHelper::LoadAllExtensions` unresolved), so **shared +
      rpath** is the default (matches notcurses); static is a follow-up.
- [x] **Conformance probe = spec-surface audit** (2026-07-22) — see results below.
- [x] Wire the consumer data path (`native/src/graph/pp_graph.cc`, `make
      pp-graph`): `PlanScanOnServer` → FileScanTask paths → **in-process DuckDB**
      `read_parquet([paths])`. Verified live over `partitions`: B=1e8 → 10.8M edges
      in 0.8s read / 0.03s plan, `max_n = floor(log_3 B)` exact (12 at 1e6, 16 at
      1e8). The DuckDB↔static-iceberg/arrow link is clean (no symbol clash).
      Substrait→Acero is only for a plan that needs to cross out of DuckDB — not
      yet needed. Data reads never cross the server.
- [x] Decision recorded — **bought vs built**: *bought* = DuckDB as the
      in-process relational engine / point-lookup / out-of-core cache, reading the
      Parquet the scan plan points to (the pivot is DuckDB-managed, not a full
      Iceberg table); *built* = the bespoke collapse DP + canonical-word encoding.
      DuckDB-over-REST is explicitly **not** the read path (its REST reader assumes
      cloud storage — see probe + hole).

## Probe results (2026-07-22)

Tool: DuckDB 1.5.4 CLI, iceberg + substrait(community) extensions. Targets: a
persistent fixture warehouse (`native/e2e/pp_fixture_warehouse.cc`, 6-row
`primes`) served by an isolated `primeparts-catalogd`, and the **live** daemon on
`:8181` (29 real tables incl. `partitions`, `primes_k0`, `primes_k0_sieve`),
read-only.

| path | verdict |
|---|---|
| `iceberg_scan(metadata.json)`, `iceberg_metadata`, `iceberg_snapshots` (direct on-disk) | **conformant** — rows, manifest (DATA/ADDED), snapshot all correct |
| `ATTACH … (TYPE ICEBERG)` catalog navigation — `/v1/config`, `/v1/namespaces`, list tables, `loadTable` | **conformant** — all 200; DuckDB listed all 29 live tables. Requires `AUTHORIZATION_TYPE 'none'` (no OAuth: `POST /v1/oauth/tokens` → 404) |
| `ATTACH … (TYPE ICEBERG)` **data read** | **diverges** — DuckDB's REST read path is wired for cloud object storage; it demands a region / vended S3 credentials and will not read local-filesystem data files vended by the catalog. `DEFAULT_REGION` is rejected as an unhandled option in 1.5.4 |
| `read_parquet('<data-file-path>')` on a plan's data file | **conformant** — rows + relational aggregation (`GROUP BY`) work directly |

**Conclusion:** DuckDB is the **in-process relational engine over the
Parquet/metadata the scan plan points to** (`read_parquet([paths])`,
`iceberg_scan(metadata.json)`) — which *is* the architecture ("consumers read
Parquet directly; data reads never cross the server"). DuckDB-over-REST is **not**
the read path: catalog navigation against our IRC works (a real interop win —
validates `pp-catalogd`'s spec surface against a client we did not write), but its
data-read path assumes cloud storage and cannot serve our local warehouse. So:
catalog = planning (over `/v1`); data = read directly by the in-process engine.
Filed the storage gap as a hole. The probe settled bought-vs-built before wiring
the consumer.

`partitions` loadTable confirms the phase-05 projection: fields
`p:long, m_k:int, n_k:int, q_k:long, prime_rank:long, p_bucket_version:int,
p_bucket:int`, `scan-planning-mode: server`. `primes_k0` / `primes_k0_sieve`
already exist in the live warehouse — the k=0 anti-join roots may be reusable
rather than recomputed (phase 05 to confirm).
