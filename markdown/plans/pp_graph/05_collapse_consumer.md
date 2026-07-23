# 05 — the collapse consumer + derived representation

The first real FileScanTask consumer + the query engine behind it (holes
registry: open ground). Built on the phase-04 substrate. The math is settled in
`../../math/collapse_findings.md`; the algebraic backbone in
`../../math/hopf_structure.md`.

## Done

- **Collapse DP, native** (`native/src/graph/pp_graph.cc`). Reads `partitions`
  via the server plan (`PlanScanOnServer`) + in-process DuckDB `read_parquet` —
  edges are read, never `is_prime_power`'d. Forward sweep in p-order (every parent
  `q < p`), no recursion. Validated exact against the Sage reference; the 1e6 run
  reproduces the documented spectrum.
- **Trie-DAG word store** = the Hopf coproduct: a word is `(a0, tail)` over
  interned segments `Seg{parent, n, C}`, so shared prefixes are shared. The memo
  is a set of int32 word-ids; the root is derived (`q = W⁻¹(p)`, unique because
  `W` is monotone), and count is a comodule multiplicity — both dropped from the
  core. `node_classes = (node, word)`; the store itself is the product, destined
  for disk.
- **Derived tables** via `MaterializeColumns` (typed int/long/string, stat
  bounds) published over the REST catalog (`MakeCatalog({rest_uri}, warehouse)`)
  through the daemon — parquet on the warehouse FS, metadata CAS by catalogd:
  `chain_words(word_id, degree, skeleton, translations, hermite, symbolic)` (GiNaC
  Hermite features) and `node_classes`. `word_id` is dense-by-sort; a
  stable/append-only id across bounds is a scaling concern.
- **Parallel sweep** (`--workers`): a Kahn topological sweep over a sharded
  concurrent store. Contention-limited by a single queue lock; a work-stealing or
  sharded queue is the lever, tuned after the sliced algorithm settles.

## Sliced sweep (in progress) — the RAM bound

Process contiguous p-range slices (default width 1e6, 12 workers per slice) and
dump each slice independently — no frontier memo carried across the boundary.

- Trace normally within a slice. When a chain reaches a node **below the current
  range**, that node is the meeting point (a k=0 root, or a node an earlier slice
  computed): stop, record the **connection** (the node id), do not re-trace. The
  trie makes this fall out — a word crossing the boundary is `(upper segments,
  connection → node q)`, `q` an id into the earlier slice's dump. The connection
  is opaque during compute, so no prior-slice read happens mid-sweep, and each
  slice gets a fresh store (connections reference nodes, not words).
- Per-slice output: `(node, partial-word, connection)` + the slice's trie. The
  whole graph is the union of slice files stitched at connection nodes.
- **The connection algebra is validated**: deferring below-range parents as
  connections and reassembling by composition (replay the upper word's
  push-sequence onto each connected chain) reproduces the single-pass result
  exactly, serial and parallel, for any slicing.

Remaining:

- **Per-slice disk dump/reload** — the sweep is proven but still in-memory;
  dumping each slice to a raw append-only log and freeing is what delivers the
  24 GB/slice bound. Slices read via `OpenIncremental` (p-range).
- **Reassembly is lazy per-query.** The bulk recursive expansion is near-cartesian
  across boundary crossings and does not scale; full words / Hermite features are
  assembled on demand by following connections into earlier slice files. Where a
  bulk pass is unavoidable (compaction), it must be bottom-up and memoized —
  expand each boundary node's chains once and cache.

## Persist the trie/coproduct

Save the coproduct as a first-class table `words(word_id, parent, n, C, slice,
conn)`; `chain_words` becomes a materialized view over it. Write a raw
append-only log per slice during the sweep, then **compact into 1–2 GB parquet
with ~128–256 MB row groups** (sorted: `node_words` by node_id, `words` by
word_id) to avoid a small-file swamp.

## Deferred

- Fuse the two-level `Push` intern (one hash instead of seg-then-word).
- Parallelize the GiNaC feature pass.
- **Injectivity lemma** (collapse classes = canonical words; Ritt, nonzero
  inter-power constants) written down — underwrites the word representation.
  Empirically the Hermite vector is a unique fingerprint per word.
- **Regenerate `primes_k0`** from the `primes ⟝ partitions` anti-join under the pp
  convention, obsoleting the foreign `primeparts.db/primes_k0`. Ties into
  re-wiring `generate.cc` (see the stats-less-tables hole).
- **F2P tests** in `e2e_test.cc`: reading == computing (byte-identical to the Sage
  reference); incremental idempotence once the sliced disk path exists.
