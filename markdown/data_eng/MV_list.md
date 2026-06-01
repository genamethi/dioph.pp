# Materialized view inventory — primeparts.*

Tracks Hive 4 + MR3 materialized views (and equivalent native-written
derived tables) on the `primeparts` namespace. The MVs in this catalog
are **precomputed indexes feeding native consumers** — they are not the
final analytical surface. See
[markdown/math/modular_filter_more_ideas.md] and
[markdown/math/structures.md] for the consumer side.

For dataset basics (max_p ≈ 5.6×10¹¹, prime count ≈ 21.7 B, ~40 B
partition rows, n=1 ≈ 99.9 % of edges), see
[markdown/data_eng/iceberg_data_setup.md].

---

## Built

| Name | Source | Where | Built | Cardinality | Build time |
|---|---|---|---|---|---|
| `primeparts.primes_k0` | `primes WHERE k=0` | `/iceberg/warehouse/primeparts.db/primes_k0/` (legacy WH — to be moved in cutover) | 2026-05-27 | 3,874,747,523 rows ≈ 17.85 % of primes (k=0 obstructed) | 290 s |
| `primeparts.q_k_freq_lo` | `partitions WHERE q_k < 10⁴ GROUP BY q_k` | `/ib-staging/primeparts.db/q_k_freq_lo/` | 2026-05-27 | 1,226 distinct q values; max freq = 168 (q=3) | 308 s |
| `primeparts.q_k_freq_mid` | `partitions WHERE 10⁴ ≤ q_k < 10⁶ GROUP BY q_k` | `/ib-staging/primeparts.db/q_k_freq_mid/` | 2026-05-27 | 75,492 distinct q; max freq = 18 | 324 s |
| `primeparts.q_k_histogram` | `partitions GROUP BY FLOOR(LOG2(q_k))` | `/ib-staging/primeparts.db/q_k_histogram/` | 2026-05-27 | 39 rows (one per bit-width); columns: `qk_bits, n_edges, qk_min, qk_max` | 547 s |

### Magnitude distribution (from `q_k_histogram`)

| qk_bits | n_edges | Fraction |
|---|---|---|
| ≤ 25 | ~13 M | < 0.04 % |
| 26-30 | ~243 M | ~0.6 % |
| 31-35 | ~10.7 B | ~27 % |
| 36-38 | **34.0 B** | **~84 %** |
| 39 (above max_p) | 0.85 B | ~2 % |

Bit 38 alone holds 18.5 B edges with avg ~1.8 multiplicity per q — q's
do repeat at large magnitudes, just lightly.

> **Do not conflate two different "almost always 1" claims.** The
> load-bearing fact is **`n_k = 1`** for ~99.9 % of edges (verified;
> see [markdown/math/modular_filter_more_ideas.md] and memory
> `[[project_n1_dominance]]`). That is *not* a statement about `q_k`
> multiplicity: distinct `q` values repeat (~1.8× at bit-38), and the
> distinct-`q_k`-**count** is a separate, harder question that only
> indirectly touches the covering work. An earlier pass mis-stated the
> `n_k` fact as "most `q_k` counts are exactly 1" — that is wrong.

---

## Planned (not yet built)

| Name | Source | Notes |
|---|---|---|
| `primeparts.partitions_n_ge_2` | `partitions WHERE n_k >= 2` | Filtered table for the "n≥2 cheat code" lookup (per modular_filter_more_ideas.md). Cardinality bounded by π(141k) × max_n_per_q ≈ small; tractable in a single mapper. Feeds the native forward-seeded table at startup. |
| `primeparts.partitions_low_q` | `partitions WHERE q_k < 100000` | Low-q inverted index `(p, q_k, n_k, m_k, prime_rank)` for the small-q neighborhood the covering-system pipeline cares about. Output row count = number of partitions with q in that range (mostly n=1 hits on small q; bounded order). |

### Considered but rejected / replaced

| Was | Replaced by | Why |
|---|---|---|
| `q_k_freq_hi` (10⁶ ≤ q_k < 10⁸) | `q_k_histogram` | Hash table would be ~5.6 M entries — sat above 4 GiB worker container peak. The histogram answers the underlying question (distribution shape, multiplicity by magnitude) more compactly. |
| `q_k_freq_top` (q_k ≥ 10⁸) | `q_k_histogram` + future `partitions_n_eq_1` summary | Distinct cardinality of q in n=1 region is hundreds of millions; doesn't fit in current 16 GiB single-node setup. The histogram + n=1-magnitude analysis covers the analytical need. |
| Full `q_k_freq` over all q | per-range ladder, then histogram | Same OOM reason; ladder revealed each tier costs ~300-550 s with the cluster at config A (2 workers × 4 GiB × 2 concurrent tasks). |

---

## MVs at wrong on-disk location (pending move)

A subset of MVs were created *before* the `ALTER DATABASE primeparts
SET LOCATION ...` cutover step (2026-05-27 18:33 local). They live at
the **old funbuns warehouse path** instead of the staging warehouse:

| MV | Current on-disk path | Intended path |
|---|---|---|
| `primeparts.primes_k0` | `file:/media/extssd/research/dioph.pp/data/iceberg/warehouse/primeparts.db/primes_k0/` | `file:/media/extssd/research/dioph.pp/data/ib-staging/primeparts.db/primes_k0/` |

The catalog row is correct (table is queryable; iceberg's
metadata_location is absolute and authoritative). The on-disk
parquet + metadata.json files just sit under the wrong directory tree.
This is purely a cleanliness issue — no read/query impact.

### Recovery procedure (run when convenient, e.g. as part of cutover task #15)

The simplest, in-place way to re-locate is **DROP + recreate at the
new default**:

```sql
DROP MATERIALIZED VIEW primeparts.primes_k0;

-- DB location is already correct since 2026-05-27; no explicit
-- LOCATION clause needed.
CREATE MATERIALIZED VIEW primeparts.primes_k0
    STORED BY ICEBERG STORED AS PARQUET
    AS SELECT p, prime_rank FROM primeparts.primes WHERE k = 0;
```

Cost: ~290 s rebuild (matches the original build time). After
verifying the new file path (`DESCRIBE FORMATTED primeparts.primes_k0`
shows the new `Location`), the old directory can be deleted:

```bash
rm -rf /media/extssd/research/dioph.pp/data/iceberg/warehouse/primeparts.db/primes_k0
```

An alternative no-recompute path — `ALTER TABLE` to update the
location pointer + physical file move — is possible but fragile
(iceberg metadata.json contains absolute paths; you'd have to rewrite
the manifests). The DROP+recreate is cheaper to reason about.

### Why this happened

The `primeparts` database row in HMS pre-existed the IRC-based staging
warehouse rollout, pointing at the old funbuns location. IRC's
`RegisterTable` writes the iceberg table row but does **not** update
the database `location_uri`. So Hive-created tables (MVs) inherited
the stale default. Once `ALTER DATABASE ... SET LOCATION` was run, new
MVs land in the right place — but pre-existing MVs are not retroactively
relocated. See memory `[[feedback-irc-vs-hive-db-location]]`.

---

## Conventions

- **Storage**: `STORED BY ICEBERG STORED AS PARQUET`. All MVs are iceberg
  tables, visible via IRC at `http://192.168.1.202:9090/iceberg/v1/`.
- **Database location**: as of 2026-05-27 cutover, the `primeparts`
  database in HMS is set to `file:/media/extssd/research/dioph.pp/data/ib-staging/primeparts.db`
  so new MVs land in staging. `primes_k0` predates that change and still
  lives in the old warehouse path; will move during the cutover sweep
  (task #15).
- **Always prepend** `DROP MATERIALIZED VIEW IF EXISTS` when retrying — failed
  CREATEs leave registered stubs (see memory `[[feedback-mv-stub-zombie]]`).
- **Session settings worth knowing**:
  - `SET hive.vectorized.groupby.flush.percent=0.2` — early hash flush for
    moderately-high-cardinality GROUP BY. Delays OOM but doesn't avoid
    it past the per-task floor.
  - `SET hive.map.aggr=false` — skip map-side aggregation entirely; shifts
    cost to shuffle + reduce. Useful when map-side hash is the bottleneck.
  - `SET hive.mr3.map.task.memory.mb=2048` — per-session override of the
    cluster default; halves concurrency but doubles per-task heap.

---

## Cluster config in effect for these builds (Config A)

| Lever | Value |
|---|---|
| All-In-One ContainerGroup envelope | 4096 MB / 4 vcores |
| Per-Tez-task memory | 2048 MB (2 concurrent per worker) |
| Worker pods | 2 (via `mr3.auto.scale.out.num.initial.containers=2`, max bounded by `mr3.k8s.worker.total.max.memory.gb=8`) |
| `tez.runtime.io.sort.mb` | 384 |
| `tez.runtime.unordered.output.buffer.size-mb` | 128 |
| `hive.mr3.am.task.concurrent.run.threshold.percent` | 80 (pipelining) |

See `hive_mr3_stack.md` for the propagation pattern (configmap re-roll +
mr3master drop + HS2 restart).

---

## Open questions

- **MV REBUILD against externally-committed iceberg snapshots.** Hive's
  incremental MV refresh expects a transactional source; we commit via
  IRC from C++. Untested whether `ALTER MATERIALIZED VIEW ... REBUILD`
  picks up new snapshots or needs explicit full REBUILD. Decide before
  wiring native ingest into a refresh loop.
- **Bucket-map-join verification.** `primes ⨝ partitions ON p` should
  be a zero-shuffle co-located join given the identity-partition
  alignment, but I never ran an `EXPLAIN` to confirm. Worth checking
  before any join-heavy MV.
- **MV sort_order propagation.** `CREATE MATERIALIZED VIEW STORED BY
  ICEBERG` produces an iceberg table with default (unsorted) sort_order
  even when the source is sorted. ORDER BY queries on the MV pay a full
  resort. Worth an `ALTER TABLE ... WRITE ORDERED BY p` follow-up where
  it matters.
