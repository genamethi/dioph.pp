# 05 — the collapse consumer + derived representation

The first real FileScanTask consumer + the query engine behind it (holes
registry: open ground). Built on the phase-04 substrate. The math is settled in
`../../math/collapse_findings.md`.

- [x] Port the collapse DP to **native C++** (`native/src/graph/pp_graph.cc`).
      Reads `partitions` via the server plan (`PlanScanOnServer`) + in-process
      DuckDB `read_parquet` — edges **read**, never `is_prime_power`'d. Forward
      sweep in p-order (every parent `q<p`), no recursion. Validated exact vs the
      Sage reference at 1e3/1e4; 1e6 matches the documented findings (1.18M words,
      8.42e12 chains); ~29x faster / 2.4x lighter than the Python prototype.
- [x] Two derived tables via `MaterializeColumns` (typed int/long/string, stat
      bounds): `chain_words(word_id, degree, skeleton, translations, hermite,
      symbolic)` + `node_classes(node_id, root_id, word_id, mult)`. Published over
      the **REST catalog** (`MakeCatalog({rest_uri}, warehouse)`) through the
      daemon; parquet on the warehouse FS, metadata CAS by catalogd. Verified live
      at B=1e3 (197 words / 201 classes, round-tripped via DuckDB `iceberg_scan`).
      GiNaC `He`/`ToHermite` for the Hermite features. `word_id` is dense-by-sorted
      today; stable/append-only across bounds is a scaling concern.
- [ ] **Scaling / incremental machinery (active).** Incremental frontier passes
      (`OpenIncremental` `from_snapshot_id_exclusive` + bound-invariance) under a
      **24 GB working-set cap**; the `q_k->p` pivot **DuckDB-managed** (out-of-core,
      per-pass in-memory window via range-query); the native memo **flushed to
      parquet between passes**. Termination: a chain ends at a node iff it is a k=0
      prime OR sits below the previous frontier. Feature computation to parallelize
      (currently serial); DP sweep to parallelize per-sink.
- [ ] **Injectivity lemma** (collapse classes = canonical words; Ritt, nonzero
      inter-power constants) written down — underwrites the word representation.
- [ ] **Regenerate `primes_k0`** — **deferred TODO, out of scope for now.** From
      the `primes ⟝ partitions` anti-join, materialized under the pp convention
      (`primeparts/primes_k0`, bare paths), obsoleting the foreign
      `primeparts.db/primes_k0` (3.87B rows, `file:`/`.crc`/`<ns>.db`).
      `primes_k0_sieve` untouched. Ties into re-wiring `generate.cc` (see the
      stats-less-tables hole).
- [ ] **F2P tests** — **deferred to last.** The scoped capability (reading==
      computing at 1e3/1e4) already holds, so these are regression tests, not
      fail-to-pass; land them in `e2e_test.cc` once the scaling machinery settles.
      Incremental idempotence (1e5→1e6 == from-scratch 1e6) becomes a real F2P once
      incremental passes exist.
