# Primeparts Native Port

This directory is the C-first migration path for the Sage-dependent ingest
hot path. The first target is parity with `src/primeparts/core.py` for Int64
prime batches:

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
| `primeparts-generate` | Stage 1 — generate primes + materialize columns + write zstd Parquet via iceberg-cpp + emit `native_files.jsonl`. Threaded. |
| `primeparts-bench-core` | Hot-loop count-only benchmark (no row materialization). |
| `primeparts-bench-materialize` | Columnar materialization benchmark (no Parquet write). |

The Stage 2 catalog/metadata commit is Python — `primeparts-commit` (a.k.a.
`python -m primeparts.native_iceberg`). It reads the manifest, validates
Parquet footers, and registers files via PyIceberg `add_files`.

Production writes require the warehouse to be in good standing before the
native writer touches it: snapshot summary properties, manifest metrics, and
local data files must agree. Check that state with:

```sh
PYTHONPATH=src python -m primeparts.native_iceberg --check-warehouse
```

If this reports orphaned local files, register them explicitly from their
`native_files_*.jsonl` manifest or remove them only after verifying they are
not needed. The generation path will refuse to skip past or overwrite them.

Build and smoke test:

```sh
make -C native
make -C native test
native/build/primeparts-bench-core --start-idx 1 --count 1000000
```

## Production pipeline

`primeparts -n N` orchestrates Stage 1 + Stage 2 in process. The native
pipeline is the default; pass `--sage` to fall back to the legacy
multiprocessing path.

```text
primeparts -n N
    └── primeparts-generate         # C core + iceberg-cpp Parquet
        └── data/.../funbuns/{primes,decompositions}/data/<partition>/*.parquet
        └── native_files.jsonl
    └── commit_native_manifest      # PyIceberg add_files
```

As of the May 2026 compaction, production table partition directories are
`p_trunc=...`. Older examples and the native generator's pre-compaction append
layout used `commit_seq=...`; do not infer resume state from directory names.

Production native runs are checkpointed by the Python coordinator. Each
checkpoint starts a fresh `primeparts-generate` process, writes a bounded
manifest, commits it with PyIceberg, then advances resume state. This bounds
RSS growth in Arrow / iceberg-cpp writer code and means a killed process only
leaves at most one checkpoint worth of files to recover. The default checkpoint
size is `250,000,000` primes; override it with:

```sh
PRIMEPARTS_NATIVE_CHECKPOINT_PRIMES=100000000 primeparts -n 1000000000
```

The production generator derives resume state from validated Iceberg snapshot
summary + manifest metrics. Direct `primeparts-generate` writes to an existing
warehouse are intentionally refused unless the coordinator supplies internal
state.

Interactive native runs also support a graceful stop request. Press `q` or `c`
while `primeparts-generate` is running to finish the current native file group,
flush its manifest, exit the native process with `stop_requested=true`, and let
the Python coordinator commit that completed checkpoint. The coordinator then
stops launching new segments and prints a resume command for the remaining
prime count. This is intentionally not a mid-batch cancel: the stop is honored
only after a complete primes/decompositions file pair is durable.

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

One caveat matters: `primeparts-generate` can use at most
`min(threads, chunks_per_file)` workers for one file group. The coordinator
therefore defaults `chunks_per_file=max(4, threads)`. Override it when tuning
file size or memory:

```sh
primeparts -n 1000000000 -b 100000 -p 12 --cpf 12
primeparts -n 1000000000 --chunk-primes 500000 --threads 24 --cpf 24
```

Larger `chunks_per_file` improves CPU utilization and produces larger Parquet
files. Smaller values reduce per-group memory and make checkpoint progress more
granular. For production, keep `chunks_per_file >= threads` unless deliberately
throttling memory.

Useful native CLI aliases:

| Alias | Full option | Meaning |
|---|---|---|
| `--threads` | `-p`, `--processes` | Native worker thread count. |
| `--chunk-primes` | `-b`, `--batch-size` | Materialized prime chunk size. |
| `--cpf` | `--native-chunks-per-file` | Chunks per native file group. |
| `--ckpt` | `--checkpoint-primes` | Production checkpoint size in primes. |
| `--logical` | `--logical-cores` | Use logical core count when `-p` is omitted. |

Example bounded run:

```sh
primeparts -n 1000000000 --chunk-primes 500000 --threads 12 --cpf 12 --ckpt 100000000
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

For a manual run against a temp warehouse:

```sh
native/build/primeparts-generate \
  --start-idx 10219850259 \
  --count 10000000 \
  --temp \
  --chunk-primes 500000 \
  --chunks-per-file 24 \
  --threads 24

PYTHONPATH=src python -m primeparts.native_iceberg \
  --manifest data/tmp/iceberg_temp_native_YYYYMMDD_HHMMSS/native_files.jsonl \
  --temp
```

`--chunks-per-file` is the main memory/file-size control: `24` materialized
`500k` chunks at the current 64-bit tail produced two Parquet files totaling
about `109MB` and ran at `2.24M primes/s` on this machine.

## Recovery and warehouse health

The normal health check is:

```sh
PYTHONPATH=src python -m primeparts.native_iceberg --check-warehouse
```

If a native process is killed after writing Parquet but before committing the
manifest, the warehouse will contain local files not referenced by Iceberg. A
complete gap-free prefix can be reconstructed from Parquet footer metadata:

```sh
PYTHONPATH=src python scripts/recover_native_orphan_prefix.py
PYTHONPATH=src python scripts/recover_native_orphan_prefix.py --apply
```

Use `scripts/repair_native_commit_seq.py` only for the historical April 2026
`commit_seq` collision repair; it is not part of normal operation.

When stderr is a TTY, `primeparts-generate` prints a single-line progress
bar (`groups N/M | rate | ETA`); when piped or redirected it stays silent so
JSON summaries on stdout remain clean.

The commit step uses PyIceberg because upstream `iceberg-cpp` currently
ships memory and REST catalogs only, not the SQLite SqlCatalog this
warehouse uses. The interim catalog path is:

```text
native C/C++ -> iceberg-cpp Parquet files -> native_files.jsonl -> PyIceberg add_files
```

That bridge has been verified on a temp 10M-prime write with Polars reading
back `10,000,000` prime rows and `18,613,680` decomposition rows from the
registered Iceberg tables.

## Iceberg Writer Direction

The previous production writer in `src/primeparts/iceberg_schema.py` used Polars only as a frame-shaping bridge and
PyArrow/PyIceberg for durable writes:

```text
shaped rows -> Arrow table -> zstd Parquet -> PyIceberg add_files
```

Apache `iceberg-cpp` now has an Arrow-native Parquet writer, so the native
writer uses it directly instead of hand-writing Parquet or treating
PyIceberg as the long-term catalog layer:

1. Keep batch generation in C.
2. Use a narrow C++ boundary for `iceberg-cpp` and Arrow C Data Interface.
3. Use `iceberg::parquet::ParquetWriter` for data files.
4. Use the Iceberg C++ table/catalog APIs for append commits once SqlCatalog or
   an equivalent local catalog path is validated against the existing layout.
5. Keep PyIceberg only as the catalog-commit layer while the C++ writer is
   being validated.

The performance bar for this path is a temp ingest of the first `100M` prime
ranks with observed generation throughput above `1M primes/s`.
