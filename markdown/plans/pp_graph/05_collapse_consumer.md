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
- [ ] **Data model (done 2026-07-22).** Trie-DAG word store (`Seg{parent,n,C}` +
      interned words) = the Hopf coproduct; memo = int32 word-id sets; root derived
      (`q=W⁻¹(p)`, unique), count dropped; `node_classes = (node, word)`. Memo is
      now tiny; the persistent **store is the product**, destined for disk. See
      `../../math/hopf_structure.md`.
- [ ] **Optimization backlog — do NOT drop these (discussed, not yet built):**
      1. **Parallel workers on a shared memo** — the core ask. Dispatch nodes (one
         at a time, or a slice per worker) to a thread pool; each does top-down
         `paths_down`, **breaking the moment it hits a node already in the shared
         memo** (membership = reuse). Target: saturate all cores. Needs a
         concurrent (sharded-lock) memo + store.
      2. **Hybrid prune** — memo-and-kill the empirically low-value tails first
         (`n=1` pure translation chains, `k=0` = roots, `k=1`) so the expensive
         traversal only runs on the long-chain core. Candidates are data-driven;
         revisit as the spectrum data grows.
      3. **Flush the store to disk at a set batch size** — batch sized to fill
         memory (>20 GB) + cores, tuned *after* the algorithm is right. Not 10x-B
         jumps — finer steps; large B is not the sole goal.
      4. **Fuse the two-level intern** (seg then word) — the ~2x `Push` slowdown is
         double hashing; one hash or a better key fixes it.
      5. Parallelize the **GiNaC feature pass** (the `pub` bottleneck).
      Termination for incremental: a chain ends at a node iff it is a k=0 prime OR
      below the previous frontier.
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
