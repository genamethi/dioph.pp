Please just excise things once they're done. No need to have running commentary about progress.

Furthermore, don't add high level summaries or try to describe the task. Keep it grounded.
Don't exposit or narrate. Leave that to the user. A lot of false assertions keep being added,
in particular the objectives have been rewritten by agents ruining the original message.
We are NOT primarily concerned with k = 0, nor are we solely concerned with covering systems.
These are tools, and they will revolve in and out of the project without me necessarily
stating as much. The point is: Leave the high level stuff to the user.

---

## Dataset basics (verified 2026-05-27)

- `primeparts.primes`: $\max(p) = 564{,}575{,}405{,}239 \approx 5.6\times10^{11}$;
  $\mathrm{count} = 21{,}698{,}850{,}257 \approx 21.7\text{ B}$ rows;
  $\min(p) = 3$ (p=2 absent). No primes skipped between 3 and $\max(p)$.
- `primeparts.partitions`: ~40 B rows. ~99.9 % are $n=1$ edges. These are the
  solutions for all primes in `primeparts.primes` (non-solutions omitted).
  **Caveat:** "almost always 1" refers to $n_k$, **not** $q_k$ multiplicity — at
  large magnitudes $q$ values repeat (~1.8×); the distinct-$q_k$ count is its own
  harder problem.
- `primeparts.primes_k0`: 3.87 B obstructed ($k=0$) primes; flat/unpartitioned,
  Iceberg format-version 2, merge-on-read, 111 data files.
- `primeparts.mdiff_k{K}` row = `(p int64, hit_mask int64, shape int64)`:
  `hit_mask` = OR of `1<<m` over the prime's hit positions (popcount==K) and is
  the source of truth; `shape = hit_mask>>ctz(hit_mask)` is derived.
- `primeparts.mersenne_factors` = `(d, prime, exponent, ord2, is_primitive)`,
  `is_primitive = (ord2==d)`; 124 rows (48 primitive).

## Catalog (operating notes)

- Catalog of record is the native LMDB-backed IRC (`pp-catalogd` serves it over
  `/v1`). Tools obtain a catalog through `OpenCatalog` (`pp_iceberg_rest.{h,cc}`):
  REST when `--rest-uri` / `PRIMEPARTS_REST_URI` names a reachable server, else a
  transparent fallback to the in-process LMDB catalog. There is **no** implicit
  localhost default — set the env/flag to make REST live.
- `CommitFiles` is the only path that puts data files into the warehouse: clients
  write parquet to `StagingDataDir` (outside the table tree) and `CommitFiles`
  moves it in on a successful FastAppend. A build killed before commit can only
  leave `.pp-staging` debris (safe to `rm`).
- `primes_k0_sieve` is a metadata-only shallow clone that **shares
  `primes_k0`'s data files** — do not REBUILD or orphan-clean `primes_k0` during a
  sieve campaign; reset with `pp-catalog --clone-sieve`.
- Design detail: `markdown/data_eng/irc_catalog_design.md`,
  `markdown/data_eng/delete_primitive_spike.md`.

## Vendored iceberg-cpp patches (`native/vendor/PATCHES.md`)

Submodule pinned at `v0.3.0`; 3 active patches in `native/vendor/patches/`,
applied idempotently by `scripts/apply_vendor_patches.sh` (run by
`native/configure`). On a tag bump, a patch that fails to re-apply was upstreamed
(retire) or needs a forward-port.

1. **`CMakeLists.txt`** — honor `-DCMAKE_COMPILE_WARNING_AS_ERROR` (upstream
   hard-`set()`s it ON; GCC trips a `-Werror=free-nonheap-object` false positive
   in `json_serde.cc`).
2. **`table_metadata.cc`** — `FreshPartitionSpec` starts `last_partition_field_id`
   at `kLegacyPartitionDataIdStart - 1` (1000-convention) so partition IDs don't
   collide with reserved manifest_entry IDs.
3. **`arrow/arrow_io.cc`** — `ResolvePath` accepts `file:/` single-slash URIs;
   without it native iceberg-cpp cannot scan any Hive-created table (incl.
   `primes_k0`). Re-apply if re-vendored (`project_icebergcpp_hive_uri_patch`).

## Toolchain hazard

The `/usr/local` GCC 16 toolchain miscompiles a `std::expected<nlohmann::json,
iceberg::Error>` **returned by value across the iceberg static-archive boundary**
(the `has_value()` discriminant reads garbage). `pp-catalogd` therefore never
consumes a `Result<nlohmann::json>` from the archive — it serializes via
`iceberg::ToJsonString` (`Result<std::string>`, unaffected) and assembles
response JSON with its own nlohmann. Re-check if iceberg-cpp or the compiler is
bumped. (`project_catalogd_expected_json_abi`.)

## Open decision — the `shape` column (needs the user)

`shape` is derived (`hit_mask>>ctz`). Validation found 7 `mdiff_k2` rows where
stored `shape` ≠ recompute (k3 clean), and row-group pruning by shape is
ineffective at the 64 M row-group size (single-shape row groups 0%; would need
row groups ≈ chunk/Nshapes ≈ 5 M). Decide:
(a) **drop `shape`**, recompute from `hit_mask` on read (removes the inconsistency
class; recommended), or
(b) **keep `shape`** and fix row-group sizing so the per-family pruning pays.
This gates the mdiff write path.

## Remaining work

- Phase 4 — derived read indexes for fast number-theoretic reads (approach open;
  not started).
- Fold `sieve_triage_main.cc` into the `covering_sieve` stepper.
- Commit `funbuns.boundaries` through the catalog on `--bucket-is-new` (today
  `generate` only writes the JSONL sidecar boundary row).
- Add the multi-table `POST /v1/{prefix}/transactions/commit` route if true
  two-table atomicity is wanted over the current partitions-before-primes order.
- Optional: a janitor for killed-run `.pp-staging` debris.

## Terminology

- Call the `(m_k, n_k, q_k)` tuples **"partitions"** in prose; the iceberg table
  name `decompositions` is a code identifier only.
- **`prime_rank`** = the prime-counting function $\pi(p)$ (library-agnostic);
  `rank_min` in boundaries is a **row-count**, not a $\pi$-value.
- **"Snap"** = physically re-sort on-disk data to match a declared `sort_order`;
  sort violations get fixed by re-snapping, not by relaxing the check.
