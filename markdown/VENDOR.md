# vendored dependencies

Five native submodules under `native/vendor/`, plus the Iceberg spec source
under `docs/vendor/` (see `.gitmodules`). Provisioning and builds are driven
by `native/configure`.

## Policy: no pins, track upstream

Every submodule tracks its upstream branch head; nothing is pinned to a tag.
`native/configure` populates an empty submodule at the branch head
(`git submodule update --init --remote`) and builds/installs it into the
prefix (`/usr/local`) **only when the prefix doesn't already provide it** —
to pick up a newer build, remove the installed artifact from the prefix and
rerun configure. Bumping the recorded gitlink is a manual
`git -C native/vendor/<dep> pull` + `git add` when you choose to.

| Path | Upstream | Branch | Built as |
|---|---|---|---|
| `lmdb/` | `LMDB/lmdb` | `mdb.master3` | compiled directly by `native/Makefile` into `build/liblmdb.a` (plus the `mdb_*` tools) |
| `flint/` | `flintlib/flint` | `main` | autotools → `$PREFIX` (shared), gated on `$PREFIX/lib/pkgconfig/flint.pc` |
| `notcurses/` | `dankamongmen/notcurses` | `master` | cmake full-option build (multimedia/pandoc/doctest at upstream defaults) → `$PREFIX`, gated on `notcurses-core.pc` |
| `arrow/` | `apache/arrow` | `main` | `--preset ninja-release -DARROW_BUILD_STATIC=ON -DARROW_TESTING=ON` → `$PREFIX`, gated on `libarrow.a` (and on `include/arrow-gtest/`, which the suite links) |
| `iceberg-cpp/` | `apache/iceberg-cpp` | `main` | cmake `ICEBERG_BUILD_BUNDLE + ICEBERG_BUILD_SHARED + ICEBERG_SQL_SQLITE + ICEBERG_BUILD_SQL_CATALOG` → `$PREFIX`, gated on `libiceberg_sql_catalog.a` |
| `docs/vendor/iceberg/` | `apache/iceberg` | `main` | not built — sparse-checked-out to `open-api/` for `rest-catalog-open-api.yaml`, the spec the surfaces are aligned to |

The only version-capped dep is **lua ≥ 5.5, < 5.6** (ABI), which is not a
submodule — configure downloads and builds the release tarball.

Truly-system deps (gmp/mpfr, pari, primesieve/primecount, sqlite3, openssl,
curl, boost headers, zlib/zstd/bz2/brotli, readline) come from the distro;
arrow bundles whatever else it needs (`libarrow_bundled_dependencies.a`) and
iceberg-cpp FetchContents its pinned extras (nlohmann-json, cpr, avro,
croaring, sqlpp23).

## The spec source

`docs/vendor/iceberg` is `apache/iceberg` itself, shallow and sparse-checked-out
to `open-api/` — roughly 640K on disk against a 12M git dir, versus the whole
Java tree. It is never built. `rest-catalog-open-api.yaml` lives there rather
than as a cached copy, so `git submodule update --remote` is the whole refresh
story and the gitlink records which revision a claim was checked against.

Sparse config lives in `.git` and does not survive a fresh clone, so
`native/configure` reapplies it and fails loudly if the yaml is absent.

## Sanitizers

`./configure --sanitize` instruments only objects we compile. `--sanitize-vendor`
also builds arrow (`ARROW_USE_ASAN/UBSAN`) and iceberg-cpp
(`ICEBERG_ENABLE_ASAN/UBSAN`) instrumented, and refuses to run against the
default `/usr/local` — instrumented libs need their own prefix.
## UPDATE 8/4/26

 Added ginac submodule as well.
