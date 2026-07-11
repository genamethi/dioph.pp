# native/vendor — vendored native dependencies

Six git submodules (see `../../.gitmodules`). Provisioning and builds are
driven by `native/configure`.

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
| `snappy/` | `google/snappy` | `main` | cmake static+PIC, tests/benchmarks off → `$PREFIX`, gated on `libsnappy.a` |
| `arrow/` | `apache/arrow` | `main` | `--preset ninja-release -DARROW_BUILD_STATIC=ON` → `$PREFIX`, gated on `libarrow.a` |
| `iceberg-cpp/` | `apache/iceberg-cpp` | `main` | cmake `ICEBERG_BUILD_BUNDLE + ICEBERG_SQL_SQLITE + ICEBERG_BUILD_SQL_CATALOG` → `$PREFIX`, gated on `libiceberg_sql_catalog.a` |

The only version-capped dep is **lua ≥ 5.5, < 5.6** (ABI), which is not a
submodule — configure downloads and builds the release tarball.

Truly-system deps (gmp/mpfr, pari, primesieve/primecount, sqlite3, openssl,
curl, boost headers, zlib/zstd/bz2/brotli, readline) come from the distro;
arrow bundles whatever else it needs (`libarrow_bundled_dependencies.a`) and
iceberg-cpp FetchContents its pinned extras (nlohmann-json, cpr, avro,
croaring, sqlpp23).
