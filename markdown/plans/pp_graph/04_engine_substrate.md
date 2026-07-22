# 04 — engine substrate (DuckDB + Acero + Substrait, non-JVM)

Substrate-first (confirmed). Stand up the committed non-JVM compute stack over
the existing native IRC before writing the consumer. Substrait (plan IR) → Acero
(execution, already in vendored Arrow) → DuckDB (DBMS: iceberg/parquet reuse,
out-of-core caching, point lookup), glued by the DuckDB Substrait community
extension. No JVM; Rust only with a killer app (none yet).

- [ ] Enable Arrow **Acero + Substrait + Flight** via vendored-Arrow build flags
      (vendor/ untouched — flags/CMake options are in scope per holes invariants).
- [ ] Vendor **DuckDB + the Substrait community extension** as gitlinks under
      `native/vendor/` (no source patches).
- [ ] **Conformance probe = spec-surface audit**: DuckDB's iceberg extension →
      `pp-catalogd` `/v1` read-only; record what serves vs 404/406 against a
      client we did not write. Register gaps as holes.
- [ ] Wire the consumer data path: `rest_scan_plan::PlanScanOnServer` →
      FileScanTask → **direct Parquet read** (DuckDB / Acero / `SourceTableReader`);
      Substrait as the relational plan IR. Data reads never cross the server.
- [ ] Decision recorded: what is **bought** (DuckDB relational / point-lookup /
      out-of-core; pivot DuckDB-managed, not a full Iceberg table) vs **built**
      (the bespoke DP), with measurements — not vibes.
