# 04 — query-engine evaluation (DuckDB candidate)

A measured decision about data pipelining, nothing pre-committed. The
division of labor under test: graph/word/satisfaction stays hand-built;
the question is whether relational pipelining (group-bys, joins, wide
aggregations over the 44B-row tables) is bought or built.

- [ ] Conformance probe: point DuckDB's iceberg extension at catalogd's
      `/v1` as a foreign IRC client (read-only). What works, what 404s/406s,
      whether it honors the advertised endpoints — doubles as a spec-surface
      audit of our server against a client we did not write.
- [ ] Pushdown fidelity: the phase-02 census query (`n_k >= 2 AND p <= B`,
      4-column projection) expressed in DuckDB SQL; verify filter/projection
      pushdown reaches the parquet layer; row counts must match phase 02
      exactly.
- [ ] Throughput comparison on identical hardware: DuckDB full-table
      aggregation (e.g. the k census group-by) vs the sharded native reader;
      wall time, peak RSS against the 32GB ceiling.
- [ ] The sharding question answered with numbers: what the native sharded
      path costs to keep versus what DuckDB gives for free, stated for the
      three workload shapes (filtered slice reads, full-table aggregation,
      joins between primes and partitions).
- [ ] Decision recorded here with the user; criteria are the measurements
      above, not vibes. Outcomes: adopt for pipelining / defer with named
      re-triggers / reject with reasons.
