# Primeparts Native Port

This directory is the native C/C++ implementation of the ingest hot path
(the earlier Sage/Python path has been removed). The number-theory core for
Int64 prime batches:

- `primecount` replaces Sage `prime_pi`.
- `primecount_nth_prime` replaces Sage `nth_prime`.
- `primesieve` replaces Sage `prime_range` / `primes` for native generation.
  Sage itself does not use primesieve here; Sage 10.8 `prime_range` uses PARI
  `primes` below `436273009` and a PARI `isprime` iterator path above it.
- FLINT `n_is_prime` / `n_is_perfect_power` handle the hot `uint64_t`
  prime-power test.
- GMP backs the arbitrary-precision `mpz_t` fallback API and is also a direct
  dependency of FLINT/PARI.
- PARI is still linked for parity/fallback work, but it is not in the current
  hot loop because the FLINT path is the speed target.

## Binaries

| Binary | Purpose |
|---|---|
| `primeparts-generate` | Generate primes + materialize columns + write zstd Parquet via iceberg-cpp, then **commit** `primes`+`partitions` through the local catalog (`CommitFiles`; `--rest-uri` routes through `pp-catalogd`). Threaded. |
| `primeparts-covering-sieve` | Covering-system FILTER / interactive stepper over `primes_k0` (MOR position deletes). |
| `primeparts-sieve-triage` | Bitmask-distribution triage (folding into the covering-sieve stepper). |
| `primeparts-catalogd` | Native IRC HTTP server over `SqlCatalog(LmdbStore)` (cpp-httplib). |
| `pp-catalog` | Catalog admin: `--register` (on-disk metadata → local catalog), `--clone-sieve`. |
| `primeparts-bench-core` | Hot-loop count-only benchmark (no row materialization). |
| `primeparts-bench-materialize` | Columnar materialization benchmark (no Parquet write). |
| `primeparts-tui` | Native notcurses workbench (status/snapshots/ops-query views) on `QueryService`. |

The catalog of record is a native local `iceberg::sql::SqlCatalog` backed by an
LMDB `CatalogStore` (`MakeLocalCatalog`), optionally fronted by `pp-catalogd`.
**There is no Python in the write/commit path.** Full design:
`../markdown/data_eng/irc_catalog_design.md`, `../HANDOFF.md` §5–6.

## Provisioning the native dependencies (rootless, no sudo)

Dependencies are git submodules + a `configure` script that builds the
fast-moving pieces from source into a profile prefix (`$HOME/.local` by
default). No system package manager is required; it targets Arch and Debian with
gcc. See `vendor/README.md` for the version policy.

```sh
# 1. populate the small pinned submodule (LMDB); arrow/iceberg are on-demand.
git submodule update --init native/vendor/lmdb

# 2. detect deps, build any missing prereqs, and build Arrow(main)+iceberg-cpp.
#    Drop --with-arrow for the number-theory-only binaries (no Arrow build).
native/configure --prefix "$HOME/.local" --with-arrow --build-missing

# 3. build (the Makefile picks up native/config.mk written by configure).
make -C native all          # or: pixi run native-build
make -C native test         # or: pixi run native-test
```

`configure` is detect-or-build: present, new-enough deps are reused; only what's
missing is built. Slow movers (gmp/flint/pari/primesieve/primecount/notcurses/
lua-5.5.x) can also come from your distro — `configure` prints the
`pacman`/`apt` hint when one is absent and `--build-missing` is off.

Quick check after provisioning:

```sh
native/build/primeparts-bench-core --start-idx 1 --count 1000000
native/build/primeparts-tui
```

`primeparts-tui` warehouse root resolution order:

1. CLI argument: `native/build/primeparts-tui /path/to/iceberg`
2. Env var: `PRIMEPARTS_WAREHOUSE_ROOT=/path/to/iceberg`
3. Config file: `~/.config/primeparts/tui.conf`

Config file format:

```ini
# ~/.config/primeparts/tui.conf
warehouse_root=/media/extssd/research/dioph.pp/data/iceberg
```

`primeparts-tui` controls:

- Arrow keys (or `h/j/k/l`): switch selected table (`primes` / `partitions`)
- `1`: status view (`max_p`, row count, snapshot count)
- `2`: snapshot view (current snapshot id + recent snapshot rows)
- `3`: ops/query view (cross-table checks + operational command hints)
- `r` or `Enter`: refresh warehouse reads
- `q`: quit

## Production pipeline

`primeparts-generate` is self-contained: it generates primes via the C core,
writes the bucketed `primes`/`partitions` Parquet via iceberg-cpp, and at
end-of-run commits both tables through the local catalog (`CommitFiles`,
partitions-before-primes for hole-free resume). `--rest-uri URL` routes the
commit through a running `pp-catalogd`; omit it to commit in-process via
`MakeLocalCatalog`. `--temp` is the ephemeral mode (files only, no commit).

```text
primeparts-generate --start-idx S --count N --warehouse WH [--rest-uri URL]
    └── C core → iceberg-cpp Parquet under WH/primeparts/{primes,partitions}/data/...
    └── CommitFiles → FastAppend snapshot per table (resume reads the primes frontier)
```

Generation/commit paths share the project data root
`/media/extssd/research/dioph.pp/data`; keep temp runs and intermediate files
under that tree (e.g. `.../data/tmp/...`), not inside the repo checkout.

Interactive runs support a graceful stop: press `q` or `c` while
`primeparts-generate` is running to finish the current file group, then commit
the completed work and exit with `stop_requested=true`. This is not a mid-batch
cancel — the stop is honored only after a complete primes/partitions file pair
is durable, so resume stays hole-free.

## Threading and file-group width

The top-level CLI defaults to physical cores:

```sh
primeparts -n 1000000000              # physical core count
primeparts -n 1000000000 -p 24         # explicit thread count
primeparts -n 1000000000 --threads 24  # explicit thread count, native spelling
primeparts -n 1000000000 --logical     # logical core count
```

Physical cores are the safer default for this workload. The hot loop mixes
branch-heavy prime-power tests, primesieve iteration, FLINT/GMP calls, and
Parquet writes; SMT threads can help hide latency, but they also increase
pressure on cache, memory bandwidth, allocator arenas, and writer buffers.
Benchmark `-p 12` versus `-p 24` on the target machine before making logical
cores the normal production setting.

`primeparts-generate` now chooses file-group width internally from thread and
chunk settings. The user-facing controls are thread count and chunk size:

Useful native CLI aliases:

| Alias | Full option | Meaning |
|---|---|---|
| `--threads` | `-p`, `--processes` | Native worker thread count. |
| `--chunk-primes` | `-b`, `--batch-size` | Materialized prime chunk size. |
| `--ckpt` | `--checkpoint-primes` | Production checkpoint size in primes. |
| `--logical` | `--logical-cores` | Use logical core count when `-p` is omitted. |

Example bounded run:

```sh
primeparts -n 1000000000 --chunk-primes 500000 --threads 12 --ckpt 100000000
```

## Shell completion

The pixi environment includes `argcomplete`. Completion can be enabled in the
current shell with:

```sh
eval "$(pixi run completion-zsh)"
```

For bash:

```sh
eval "$(pixi run completion-bash)"
```

Or, after `pixi shell`, source the checked-in helper:

```sh
source scripts/primeparts-completion.zsh
```

For a manual committing run against a warehouse (commit happens at end-of-run
via `CommitFiles`; drop `--rest-uri` to commit in-process):

```sh
native/build/primeparts-generate \
  --start-idx 10219850259 \
  --count 10000000 \
  --warehouse /media/extssd/research/dioph.pp/data/tmp/wh \
  --chunk-primes 500000 \
  --threads 24
  # --rest-uri http://127.0.0.1:8181   # to commit through a running pp-catalogd

# Or --temp instead of --warehouse for an ephemeral, files-only run (no commit).
```

On this machine, a temp run with `--chunk-primes 500000 --threads 24` produced
about `109MB` of Parquet and ran around `2.24M primes/s`.

## Catalog on-disk format

The catalog store (`<warehouse>/catalog.lmdb`) is written by the vendored LMDB
(`native/vendor/lmdb`, on-disk `MDB_DATA_VERSION=3`). To inspect, dump, or
restore a catalog, build the CLI tools from the vendored tree so they match the
library the `pp` binaries link:

```
make lmdb-tools                                        # -> build/mdb_{stat,dump,load,copy,drop}
build/mdb_stat -ea <warehouse>/catalog.lmdb           # inspect env + all sub-DBs (tables, nsprops)
build/mdb_dump -a  <warehouse>/catalog.lmdb > cat.dump # portable backup of all sub-DBs
build/mdb_load -f  cat.dump <fresh-dir>/catalog.lmdb   # restore into a fresh env
```

`mdb_dump -a` / `mdb_load -f` round-trip the named sub-DBs together and are the
supported backup/restore path.

## Recovery and warehouse health

A native process killed after writing Parquet but before committing leaves
local files not referenced by any Iceberg snapshot. The commit is end-of-run,
so an interrupted run simply re-runs from the last committed `primes` frontier
(the uncommitted files are inert and overwritten/ignored on the next run). The
catalog snapshot — not directory names — is the source of truth for resume
state.

When stderr is a TTY, `primeparts-generate` prints a single-line progress
bar (`groups N/M | rate | ETA`); when piped or redirected it stays silent so
JSON summaries on stdout remain clean.

The whole write path is native C++ + iceberg-cpp: batch generation in C, a
narrow C++ boundary to iceberg-cpp/Arrow, `iceberg::parquet::ParquetWriter` for
data files, and the iceberg-cpp table/catalog APIs (via `CommitFiles`) for the
FastAppend commit against the local `SqlCatalog`/`pp-catalogd`.
