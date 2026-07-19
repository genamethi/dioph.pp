# HANDOFF for agents

## Guidance for agents

Please just excise things once they're done. No need to have running commentary
about progress. No narrating.

Furthermore, don't add high level summaries or try to describe the task. Keep it
grounded.
Don't exposit or narrate. Leave that to the user. A lot of false assertions keep
being added,
in particular the objectives have been rewritten by agents ruining the original
message.
We are NOT primarily concerned with k = 0, nor are we solely concerned with
covering systems.
I don't want any comments in the code. You can't be trusted to know what's relevant.
So zero comments. All the time.
These are tools, and they will revolve in and out of the project without me necessarily
stating as much. The point is: Leave the high level stuff to the user.

---

## Tables

- `primeparts.primes`: one row per prime, `p`-ordered. $\min(p)=3$ (p=2 absent);
  no primes skipped between 3 and $\max(p)$.
- `primeparts.partitions`: the `(m_k, n_k, q_k)` tuples; most edges are `n=1`.
- `primeparts.primes_k0`: `k=0` primes; flat/unpartitioned, Iceberg format-version
  2, merge-on-read.

## Catalog (operating notes)

- Catalog of record is the native LMDB-backed IRC (`pp-catalogd` serves it over
  `/v1`). Tools obtain a catalog through `OpenCatalog` (`pp_iceberg_rest.{h,cc}`):
  it resolves a URI (`--rest-uri` / `PRIMEPARTS_REST_URI` / compiled
  `kDefaultRestUri` = `http://127.0.0.1:8181`) and requires that server to answer
  `GET /v1/config` — no in-process fallback; nothing touches the LMDB state
  except catalogd.
- Data files enter the warehouse only through `CommitFiles` (single table) or
  `CommitFilesAtomic` (multiple tables, one transaction): clients write parquet
  to `StagingDataDir` (outside the table tree), the commit moves it in on
  success. A build killed before commit can only leave `.pp-staging` debris
  (safe to `rm`).
- Design detail: `markdown/data_eng/irc_catalog_design.md`.

## Remaining work

- Client access and the read seam: `../data_eng/clients_rest_gap.md`,
  `../data_eng/catalogd_rest_gap.md`. Row-group/zone-map pruning
  (`scan::RefineSplits`) and server-side scan planning both landed 2026-07-18.
- Derived read indexes for fast number-theoretic reads (approach open).
- Implement views
- `https://raw.githubusercontent.com/apache/iceberg/refs/heads/main/format/view-spec.md`
- `LoadAlignedResume` still derives bucket fill + per-table file seq by fs-glob;
  replace with a read of the current snapshot's manifests.
- Optional: a janitor for killed-run `.pp-staging` debris.

## Terminology

- Call the `(m_k, n_k, q_k)` tuples **"partitions"** in prose.
- **`prime_rank`** = the prime-counting function $\pi(p)$ (library-agnostic).
- **"Snap"** = physically re-sort on-disk data to match a declared `sort_order`;
  sort violations get fixed by re-snapping, not by relaxing the check.
