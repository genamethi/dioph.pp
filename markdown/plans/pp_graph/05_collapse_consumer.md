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
- [x] **Parallel sweep (done 2026-07-22, `e22d874`).** `--workers W` runs a Kahn
      topological sweep over a 256-shard concurrent `ConcStore`. ~2.9x at 12–20
      workers, contention-limited by the single `qmu`. A batched-queue contention
      fix was tried and reverted (liveness bug); revisit with a proper work-stealing
      deque or sharded queue, tuned after the sliced algorithm is right.
- [ ] **Sliced sweep — CONFIRMED DESIGN (the main remaining build).** Bound RAM to
      a 24 GB-per-slice cap by processing contiguous p-ranges and **dumping each
      slice independently — no frontier memo carried across the boundary.**
      - Within a slice, trace normally. When a chain reaches a node **below the
        current range**, that node is the meeting point: it is either a k=0 root
        (chain ends) or a node an earlier slice already computed. Either way
        **stop, record the connection (the node id), do not re-trace, hold nothing
        from the prior slice.** This is "membership against known chains."
      - The trie makes it fall out: a word crossing the frontier is
        `(upper segments, connection -> node q below)`, `q` an id into the earlier
        slice's on-disk dump. Within-slice `Push` extends these partial words and
        dedups them; the connection is **opaque during the slice**, so *no read of
        the prior slice happens during compute*.
      - A below-range parent acts as a **local seed tagged `q`** (like a root, but
        connected). Every node, once referenced by a later slice, is just a
        connection-seed — memos are never reloaded/carried across slices.
      - Per-slice output: `(node, partial-word, connection)` + the slice's trie.
        The whole graph = the union of slice files **stitched at connection nodes**.
      - **Full word / Hermite features are assembled at reassembly / query time**
        by following connections into earlier slice files — never in RAM mid-sweep.
      - Slices read via `OpenIncremental` (p-range); finer steps, not 10x-B jumps.
- [ ] **Deferred optimizations:** fuse the two-level `Push` intern (kills the ~2x
      trie slowdown); parallelize the GiNaC feature pass (the `pub` bottleneck);
      fix the parallel contention (work-stealing).
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
