Please just excise things once they're done. No need to have running commentary about progress.

Furthermore, don't add high level summaries or try to describe the task. Keep it grounded.
Don't exposit or narrate. Leave that to the user. A lot of false assertions keep being added,
in particular the objectives have been rewritten by agents ruining the original message.
We are NOT primarily concerned with k = 0, nor are we solely concerned with covering systems.
These are tools, and they will revolve in and out of the project without me necessarily
stating as much. The point is: Leave the high level stuff to the user.

---

## Dataset basics

Numbers drift as `generate` extends the census; re-query the live snapshot
(`SELECT MIN, MAX, COUNT(*)`) rather than trusting these.

- `primeparts.primes`: $\min(p)=3$ (p=2 absent), $\max(p) \approx 6.19\times10^{11}$,
  $\mathrm{count} \approx 23.7\text{ B}$. No primes skipped between 3 and $\max(p)$.
- `primeparts.partitions`: $\approx 44.6\text{ B}$ rows; mean $k \approx 1.88$.
- `primeparts.primes_k0`: $k=0$ primes; flat/unpartitioned, Iceberg format-version
  2, merge-on-read.
- `primeparts.mdiff_k{K}` row = `(p int64, hit_mask int64)`: `hit_mask` = OR of
  `1<<m` over the prime's hit positions (popcount==K), the **sole stored truth**.
  The d-vector, the translation-invariant `shape = hit_mask>>ctz`, the anchor phase
  (`m_min mod period`, the coset), and `prime_rank` are all derived on read, never
  stored. Unpartitioned, physically ordered by `p`; derived groupings (gap `d`,
  coset) belong in views, not columns.
- `primeparts.mersenne_factors` = `(d, prime, exponent, ord2, is_primitive)`,
  `is_primitive = (ord2==d)`.

## Catalog (operating notes)

- Catalog of record is the native LMDB-backed IRC (`pp-catalogd` serves it over
  `/v1`). Tools obtain a catalog through `OpenCatalog` (`pp_iceberg_rest.{h,cc}`):
  it always resolves a URI (`--rest-uri` / `PRIMEPARTS_REST_URI` / compiled
  `kDefaultRestUri` = `http://127.0.0.1:8181`), uses REST when that server answers
  `GET /v1/config`, else transparently falls back to the in-process LMDB catalog.
- Data files enter the warehouse only through `CommitFiles` (single table) or
  `CommitFilesAtomic` (multiple tables, one transaction): clients write parquet to
  `StagingDataDir` (outside the table tree), the commit moves it in on success. A
  build killed before commit can only leave `.pp-staging` debris (safe to `rm`).
- `primes_k0_sieve` is a metadata-only shallow clone that **shares `primes_k0`'s
  data files** — do not REBUILD or orphan-clean `primes_k0` during a sieve
  campaign; reset with `pp-catalog --clone-sieve`.
- Design detail: `markdown/data_eng/irc_catalog_design.md`,
  `markdown/data_eng/delete_primitive_spike.md`.

## Vendored iceberg-cpp patches (`native/vendor/PATCHES.md`)

Submodule pinned at `v0.3.0`; patches in `native/vendor/patches/`, applied
idempotently by `scripts/apply_vendor_patches.sh` (run by `native/configure`). On
a tag bump, a patch that fails to re-apply was upstreamed (retire) or needs a
forward-port.

1. **`CMakeLists.txt`** — honor `-DCMAKE_COMPILE_WARNING_AS_ERROR` (upstream
   hard-`set()`s it ON; GCC trips a `-Werror=free-nonheap-object` false positive
   in `json_serde.cc`).
2. **`table_metadata.cc`** — `FreshPartitionSpec` starts `last_partition_field_id`
   at `kLegacyPartitionDataIdStart - 1` (1000-convention) so partition IDs don't
   collide with reserved manifest_entry IDs.
3. **`arrow/arrow_io.cc`** — `ResolvePath` accepts `file:/` single-slash URIs;
   without it native iceberg-cpp cannot scan any Hive-created table (incl.
   `primes_k0`).

## Remaining work

- Derived read indexes for fast number-theoretic reads (approach open).
- Derived `mdiff` views (gap `d`, anchor coset) as Iceberg view objects; the
  `representations` list is user-defined (a `lua` type, not SQL-only),
  "materialized" = a rebuildable derived table. Spec:
  https://raw.githubusercontent.com/apache/iceberg/refs/heads/main/format/view-spec.md
- Server-side scan planning (`planTableScan`/`fetchScanTasks`).
- Optional: a janitor for killed-run `.pp-staging` debris.

## Terminology

- Call the `(m_k, n_k, q_k)` tuples **"partitions"** in prose; the iceberg table
  name `decompositions` is a code identifier only.
- **`prime_rank`** = the prime-counting function $\pi(p)$ (library-agnostic).
- **"Snap"** = physically re-sort on-disk data to match a declared `sort_order`;
  sort violations get fixed by re-snapping, not by relaxing the check.
