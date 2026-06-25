# native/vendor — vendored native dependencies

Three git submodules (see `../../.gitmodules`), plus the local patch series and
its docs. Provisioning and builds are driven by `native/configure`; nothing here
needs a system package manager or `sudo`.

| Path | Upstream | Ref policy | Populated |
|---|---|---|---|
| `lmdb/` | `LMDB/lmdb` (`mdb.master3`) | pinned branch | **yes**, on `git submodule update --init` — its `libraries/liblmdb/{mdb.c,midl.c}` are compiled directly into `build/liblmdb.a` by `native/Makefile`. |
| `iceberg-cpp/` | `apache/iceberg-cpp` | **latest tag ≥ 0.3.x** (now `v0.3.0`); patches merged forward | on demand — `update = none`. `native/configure` checks it out and applies `patches/*.patch`. |
| `arrow/` | `apache/arrow` | **`main`** (the unreleased 25.x dev line — Arrow has no ≥25 release tag yet) | on demand — `update = none`, blobless + sparse (`cpp/`). |

`patches/` and `PATCHES.md` hold the iceberg-cpp local patches; see PATCHES.md.

## Version policy

Minimums, prefer latest, almost no caps:

- **arrow** ≥ 25 → track `main` (`ARROW_REF`, default `main`).
- **iceberg-cpp** ≥ 0.3.x → latest matching tag; patches merged forward / retired.
- **lua** ≥ 5.5, < 5.6 (only capped dep — ABI).
- **gmp / flint / pari / primesieve / primecount** — recent minor or later, no cap.

## Bumping a fresh-tracked dep

```sh
# iceberg-cpp → newest tag
git -C native/vendor/iceberg-cpp fetch --tags --depth 1
git -C native/vendor/iceberg-cpp checkout <new-tag>
scripts/apply_vendor_patches.sh          # re-applies; flags any patch to retire
git add native/vendor/iceberg-cpp        # record the new gitlink

# arrow → latest main
git -C native/vendor/arrow fetch --depth 1 origin main
git -C native/vendor/arrow checkout FETCH_HEAD
```

`ignore = all` on the on-demand submodules keeps these moving checkouts (and the
applied patches / build trees) out of `git status`; use `git add --force` to
record a new gitlink when you intend to.
