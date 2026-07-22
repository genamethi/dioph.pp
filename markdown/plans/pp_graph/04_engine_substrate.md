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
      `native/configure` (idempotent presence checks, user-local, no sudo). This
      replaces "vendor DuckDB as a gitlink": the probe (below) shows DuckDB's role
      is an **out-of-process direct reader / relational engine**, not a library
      linked into `pp-graph`. Whether to later vendor+link `libduckdb` for the
      pivot cache / point-lookup is deferred to phase 05 with measurements — open.
- [x] **Conformance probe = spec-surface audit** (2026-07-22) — see results below.
- [ ] Wire the consumer data path: `rest_scan_plan::PlanScanOnServer` →
      FileScanTask → **direct Parquet read** (DuckDB `read_parquet` / Acero /
      `SourceTableReader`); Substrait as the relational plan IR. Data reads never
      cross the server.
- [ ] Decision recorded: what is **bought** (DuckDB relational / point-lookup /
      out-of-core; pivot DuckDB-managed, not a full Iceberg table) vs **built**
      (the bespoke DP), with measurements — not vibes.

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

**Conclusion (informs bought/built, not yet the full decision):** DuckDB earns
its keep as a **direct reader + relational engine over the Parquet/metadata the
scan plan points to** (`read_parquet([paths])`, `iceberg_scan(metadata.json)`) —
which *is* the architecture ("consumers read Parquet directly; data reads never
cross the server"). DuckDB-over-REST is **not** the read path: catalog navigation
against our IRC works (a real interop win — validates `pp-catalogd`'s spec
surface against a client we did not write), but its data-read path assumes cloud
storage and cannot serve our local warehouse. So: catalog = planning; data =
direct. Filed the storage gap as a hole. The probe de-risked the DuckDB bet
before any vendoring.

`partitions` loadTable confirms the phase-05 projection: fields
`p:long, m_k:int, n_k:int, q_k:long, prime_rank:long, p_bucket_version:int,
p_bucket:int`, `scan-planning-mode: server`. `primes_k0` / `primes_k0_sieve`
already exist in the live warehouse — the k=0 anti-join roots may be reusable
rather than recomputed (phase 05 to confirm).
