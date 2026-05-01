# Ingest Performance Regression Hunt

**Date**: 2026-04-21
**Command**: `funbuns -n $((10**8)) -b $((10**6)) -t`
**Reported regression**: 52s (2026-04-14) → 4:23 / 263s (2026-04-20)
**Machine**: single-node promix (12 physical cores)

## Environment delta (fresh resolve, 2026-04-21)

| Package | 5bf1fd4 (Apr 14) | liminal HEAD | Notes |
|---------|------------------|--------------|-------|
| polars | 1.40.0 | 1.39.3 | main is older |
| pyarrow | 24.0.0 (PyPI) | 23.0.1 (conda) | 5ef63cd moved pyarrow conda↔pypi |
| sage | 10.8 | 10.7 | main is older |
| thrift | — | 0.22.0 | added for Hive stack |
| pyiceberg | 0.11.1 | 0.11.1 | unchanged |
| zstandard | 0.25.0 | 0.25.0 | unchanged |
| numpy | 2.4.3 | 2.4.3 | unchanged |

The Apr 19 `pixi.toml` edit (5ef63cd) shuffled several packages between
conda-forge and PyPI channels. `pixi.lock` predates this edit (last
touched 2026-03-10 in an earlier branch; missing entirely at 5bf1fd4).
Fresh resolves at each commit pull today's latest matching each
pixi.toml's channel constraints, so the deltas above are what the
benchmark actually runs under.

## Commit bracket

| Commit | Date | Summary | Perf impact hypothesis |
|--------|------|---------|-----------------------|
| 5bf1fd4 | 2026-04-14 | Ingest cutover steps 4-5 | baseline anchor |
| c00b131 | 2026-04-14 | Rename batch_id→commit_seq | rename only, should be zero |
| 5ef63cd | 2026-04-19 | pixi.toml shuffle | env drift (pyarrow, polars, sage) |
| 8454dc9 | 2026-04-20 | Hive/HMS snapshot | briefly HiveCatalog default |
| 88faeed | 2026-04-20 | HMS Thrift shim + _patches.py | import-time monkey-patch |
| 7dbd0ae | 2026-04-20 | Dual-catalog option B | SqlCatalog restored |
| 35f111d | 2026-04-20 | kube-down scripts | shell only |

## Results

TBD — pending benchmark runs at each bracket.

## Hypothesis ranking (pre-data)

1. **Env drift from 5ef63cd** — pyarrow 24→23 is the biggest version jump
   and sits on every write path (polars → arrow → parquet). Strong
   candidate for a throughput regression on the flush hot path.
2. **_patches.py import-time overhead from 88faeed** — unlikely to be
   5×; the patches run once at module import and wrap two functions.
   Worth isolating via `FUNBUNS_SKIP_PATCHES=1` gate.
3. **HiveCatalog fallout from 8454dc9** — rolled back in 7dbd0ae; should
   not affect the temp (SqlCatalog) benchmark path.

## Method

Each bracket commit is benchmarked in `/tmp/funbuns-cutover` with a
fresh unlocked `pixi install` to establish a clean environment. Wall
clock captured via zsh `time` builtin (`/usr/bin/time -v` not available
on this host).
