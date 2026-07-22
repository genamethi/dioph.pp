# 05 — the collapse consumer + derived representation

The first real FileScanTask consumer + the query engine behind it (holes
registry: open ground). Built on the phase-04 substrate. The math is settled in
`../../math/collapse_findings.md`.

- [ ] Port the collapse DP to **native C++** (integer-encoded canonical words,
      shared top-down memo). Read `partitions` via
      `SourceTableReader::OpenMetadata` (`select p,m_k,n_k,q_k`; `filter p<B`;
      sharded) — edges **read**, never `is_prime_power`'d.
- [ ] Incremental frontier passes: `SourceTableReader::OpenIncremental`
      (`from_snapshot_id_exclusive`) + bound-invariance + a **24 GB working-set
      cap**; each pass reads only the new slice, extends the memo, flushes to disk.
- [ ] Graph skeleton: the `q_k -> p` pivot of `partitions` **DuckDB-managed
      (cached, not fully materialized)**; termination points from the
      `primes ⟝ partitions` anti-join (= k=0 roots). A chain terminates at a node
      iff it is a **k=0 prime** OR sits **below the previous frontier**.
- [ ] **Regenerate `primes_k0`** from that anti-join, materialized under the pp
      convention (`primeparts/primes_k0`, bare paths). The existing
      `primeparts.db/primes_k0` (3.87B rows) is a foreign pyiceberg layout
      (`file:` URIs, `.crc` sidecars, `<ns>.db` dir) baked into its manifests;
      pp-graph owns the k=0 roots and writes them fresh rather than migrating the
      foreign metadata. `primes_k0_sieve` is left untouched.
- [ ] Two derived tables via `MaterializeIntColumns` (Iceberg MVs):
      `chain_words(word_id stable/append-only, degree, skeleton, translations,
      hermite, symbolic)` and `node_classes(node_id, root_id, word_id, mult)`.
      Point-lookup via DuckDB; LMDB only for a light index if latency demands it.
- [ ] **Injectivity lemma** (collapse classes = canonical words; Ritt, nonzero
      inter-power constants) written down — underwrites the word representation.
- [ ] **F2P tests** in `native/e2e/e2e_test.cc` (each a registered hole, red
      first): reading==computing (byte-identical to the Sage reference at
      10^3/10^4); incremental idempotence (1e5 → extend to 1e6 == from-scratch 1e6).
