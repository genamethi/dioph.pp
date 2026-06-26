# Building the native engine (rootless, portable)

The C/C++ engine under `native/` depends on a stack of libraries (Apache Arrow,
Apache iceberg-cpp, FLINT, PARI, primesieve, primecount, GMP/MPFR, notcurses,
Lua, nlohmann-json, LMDB). They are provisioned **from source into a profile
prefix** (`$HOME/.local` by default) by `native/configure`, using the **system
gcc toolchain** — **no `sudo`, no conda/pixi, no system package manager** for the
dependencies themselves. Targets **Arch** and **Debian/Ubuntu** with gcc.

See `native/vendor/README.md` for the submodule layout and version policy, and
`native/vendor/PATCHES.md` for the iceberg-cpp local patches.

## TL;DR

```sh
git clone <repo-url> dioph.pp && cd dioph.pp
native/configure --prefix "$HOME/.local" --with-arrow --build-missing
make -C native all
make -C native smoke      # acceptance test (LMDB + iceberg-cpp catalog seam)
```

First run builds the whole dependency stack and takes a while (Arrow alone is
~30–60 min and the prefix grows to a few GB). Re-runs are incremental:
`configure` detects what's already present and only builds what's missing.

## 1. One-time bootstrap toolchain

`configure` builds the *libraries*, but a base toolchain must already exist (it
can't build its own compiler). This is the only step that needs your package
manager / `sudo`:

**Arch**
```sh
sudo pacman -S --needed base-devel cmake git curl m4 perl openssl sqlite
```

**Debian / Ubuntu**
```sh
sudo apt install build-essential cmake git pkg-config curl m4 perl \
                 libcurl4-openssl-dev libssl-dev libsqlite3-dev
```

Required: `gcc`/`g++`, `make`, `cmake`, `pkg-config`, `git`, plus `m4` (GMP),
`perl` (PARI), and `curl` or `wget` (fetching sources). `libcurl`, `openssl`,
and `sqlite3` are **probed, not built** — `configure` errors with a hint if a
dev package is missing. Everything else is built from source.

## 2. Provision dependencies

```sh
native/configure --prefix "$HOME/.local" --with-arrow --build-missing
```

What it does:

- Initializes the small pinned `lmdb` submodule automatically.
- **Stage A** — detects each prerequisite (pkg-config min-version); with
  `--build-missing`, builds any that are absent/too-old into the prefix. Without
  it, prints a per-distro install hint and continues.
- **Stage B** (`--with-arrow`) — materializes the on-demand `arrow` (tracks
  `main`, i.e. the 25.x dev line — Arrow has no ≥25 release yet) and
  `iceberg-cpp` (`v0.3.0`) submodules, applies the local patches, and builds
  both **static** into the prefix.
- Writes `native/config.mk`, which `native/Makefile` includes (sets `PREFIX`,
  `PKG_CONFIG_PATH`, `CC`/`CXX`, Lua/notcurses paths).

Useful flags: `--prefix DIR` (default `$HOME/.local`), `--jobs N`, `--force`
(rebuild Arrow/iceberg even if present), `--help`. Source versions are
overridable via env (e.g. `FLINT_VERSION=…`, `ARROW_REF=…`); minimums are the
policy (Arrow ≥25, iceberg-cpp ≥0.3.x, Lua ≥5.5,<5.6, the rest recent-minor).

**System prefixes & sudo.** The default `$HOME/.local` needs no root. If you
point `--prefix` at a location you can't write (e.g. `/usr/local`), `configure`
detects this, **builds everything as your user**, and runs only the *install*
step under `sudo` — prompting once up front and keeping the credential warm
through the build. Compilation never runs as root. (Already root, or a writable
prefix → no sudo at all.) Scratch build trees always live in
`${XDG_CACHE_HOME:-$HOME/.cache}/primeparts-native`, never under the prefix.

Drop `--with-arrow` if you only need the number-theory binaries
(`primeparts-bench-core`, `test_core`, …) — they don't touch Arrow and build
in seconds.

## 3. Build & test

```sh
make -C native all        # all binaries (or: pixi run native-build)
make -C native smoke      # primeparts-lmdb-smoke — expect "PASS (0 failures)"
make -C native test       # unit tests
```

Binaries land in `native/build/`. The library rpath is baked to
`$PREFIX/lib`, so they run without setting `LD_LIBRARY_PATH`. Add
`$HOME/.local/bin` to `PATH` if you want the installed dep tools (e.g. `gp`).

> Known: `make test`'s `primeparts-test-primitive-factors` currently fails one
> stale assertion (expects 13 as a primitive factor of 211−2⁴, but 13 is in the
> backbone set) — unrelated to the build; tracked separately.

## 4. Moving to another machine

1. Install the bootstrap toolchain (§1) for that distro.
2. `git clone` the repo (submodules are on-demand; `configure` fetches what it
   needs — you do **not** need `--recurse-submodules`).
3. Run §2 + §3.

Requirements on the new box: the bootstrap toolchain, network access (to fetch
source tarballs + the arrow/iceberg submodules), and ~a few GB of disk under the
prefix. Nothing is hard-coded to this machine.

**Runtime data paths are separate from the build.** Warehouse/data locations
come from `pixi.toml` `[tool.funbuns.directories]`, the
`PRIMEPARTS_WAREHOUSE_ROOT` env var, or `~/.config/primeparts/tui.conf` (see
`native/README.md`) — set those per machine; they are not part of provisioning.

## 5. Keeping Arrow / iceberg-cpp fresh

```sh
# iceberg-cpp -> newest release tag, re-apply/retire patches, record gitlink
git -C native/vendor/iceberg-cpp fetch --tags --depth 1
git -C native/vendor/iceberg-cpp checkout <new-tag>
scripts/apply_vendor_patches.sh
git add native/vendor/iceberg-cpp

# arrow -> latest main
git -C native/vendor/arrow fetch --depth 1 origin main
git -C native/vendor/arrow checkout FETCH_HEAD

native/configure --prefix "$HOME/.local" --with-arrow --force   # rebuild
```

A patch that fails to re-apply has been upstreamed (retire it) or needs a
forward-port — see `native/vendor/PATCHES.md`.

## 6. Troubleshooting

- **`<tool> not found`** — install the bootstrap toolchain (§1).
- **`missing prerequisites: …`** — re-run with `--build-missing`, or install the
  printed package.
- **Stale prefix / weird link errors** — `rm -rf "$HOME/.local"` (or your
  prefix) and re-run `configure`; it's fully regenerable.
- **iceberg/arrow won't rebuild after a bump** — pass `--force`.
