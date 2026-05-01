# Native Port Plan

## Current Slice

Implemented:

- C batch processor for the `core.py` generation hot path.
- Local Apache `iceberg-cpp` build and first native Parquet writer binary,
  `primeparts-generate`.
- Native equivalents for the Sage calls used by generation:
  - `prime_pi` -> `primecount_pi`
  - `nth_prime` -> `primecount_nth_prime`
  - `prime_range` / `primes` -> `primesieve`
  - `Integer.is_prime_power(proof=False, get_data=True)` -> FLINT
    `n_is_prime` / `n_is_perfect_power` on `uint64_t`, with a GMP `mpz_t`
    fallback API for wider integers
  - `previous_prime` -> `primesieve_nth_prime(-1, n)`
- `primeparts -n N` orchestrates the native pipeline by default:
  `primeparts-generate` (C core + iceberg-cpp Parquet) followed by
  `commit_native_manifest` (PyIceberg `add_files`). Pass `--sage` for the
  legacy multiprocessing path.
- Package import no longer loads Sage eagerly; Sage-backed symbols are lazy.
- `primeparts-bench-core` for materialization-free hot-loop throughput testing.
- `primeparts-generate` for temp warehouse data-file writes:
  C core materialization, threaded in-process file groups, C++ Arrow wrapping,
  and iceberg-cpp Parquet output in the existing
  `funbuns/{primes,decompositions}/data/commit_seq=...` layout.
- `python -m primeparts.native_iceberg` / `primeparts-commit` bridge
  that validates `native_files.jsonl` and registers the native Parquet files
  through PyIceberg.

Verified:

- `pixi run native-test`
- Native C output matches Sage for the first 100 prime ranks.
- Core-only throughput on this machine:
  `native/build/primeparts-bench-core --start-idx 1 --count 100000000 --threads 24`
  processed `100M` prime ranks in `13.985495s`, or `7,150,265 primes/s`.
- Materialized throughput from the current warehouse tail:
  `native/build/primeparts-bench-materialize --start-idx 10219850259 --count 10000000 --threads 24 --chunk-primes 500000`
  processed `10M` prime ranks in `2.232335s`, or `4,479,615 primes/s`,
  materializing `18,613,680` decomposition rows into `20` chunks.
  Total used column bytes were `566,728,320`; total allocated bytes were
  `629,145,600`; max chunk allocation was `31,457,280` bytes.
- `FUNBUNS_DATA_DIR=/tmp/primeparts-native-data HOME=/tmp pixi run primeparts -n 10 -b 10 --temp`
  writes and commits temp Iceberg tables.
- `iceberg-cpp` builds from the Apache GitHub checkout with
  `-Wno-error=free-nonheap-object` for a GCC 15 false positive in
  `json_serde.cc`.
- Native iceberg-cpp writer smoke:
  `primeparts-generate --start-idx 1 --count 10000 --temp --chunk-primes 5000 --chunks-per-file 1`
  wrote compatible Parquet files. PyIceberg registered them into a temp
  SqlCatalog, and Polars read back `10,000` primes with `18,916`
  decompositions.
- Native iceberg-cpp writer at the current 64-bit warehouse tail:
  `primeparts-generate --start-idx 10219850259 --count 10000000 --temp --chunk-primes 500000 --chunks-per-file 24 --threads 24`
  wrote `10M` prime rows and `18,613,680` decomposition rows in `4.45927s`,
  or `2,242,520 primes/s`. It produced two Parquet files totaling about
  `109MB`; PyIceberg `add_files` registered them into a temp SqlCatalog, and
  Polars read back the expected counts.

## Phase 1: C Core Parity

Todo:

- Add benchmark parity against `PPBatchProcessor` for larger ranges.
- Add tests around interrupted/partial batches once native concurrency lands.
- Add an exported ABI version constant for the ctypes bridge.
- Keep the `uint64_t` hot loop for speed, but preserve GMP APIs for exact
  arbitrary-precision fallback and future schema widening.
- Keep the core benchmark above `1M primes/s` while adding materialization and
  writer integration.

Notes:

- The current Iceberg schema is `Long`/`Int64`, so the writer surface is limited
  to positive primes `<= INT64_MAX`.
- Sage 10.8 `prime_range` does not use primesieve. It uses PARI `primes` below
  `436273009` and a PARI `isprime` iterator path above that threshold. Using
  primesieve is a native replacement decision for speed.

## Phase 2: Native Generation Manager

Done:

- `primeparts-generate` is the native generation manager: pthread pool
  pulls chunks via a mutex-guarded counter inside `materialize_group`,
  threaded materialization runs concurrently with sequential Parquet
  writes per file group.

Todo:

- Preserve existing batch identity: `(start_idx, processed_count)`.
- Preserve contiguous-prefix commit behavior for interrupted runs.
- Add signal handling equivalent to the Python first/second/third Ctrl-C model.

## Phase 3: Iceberg C++ / Parquet Writer

Done:

- Keep row production and validation in C.
- Vendor or add Apache `iceberg-cpp` as the native table writer dependency.
- Use the Arrow C Data Interface as the C/C++ boundary.
- Use `iceberg::parquet::ParquetWriter` rather than hand-writing Parquet:
  - accepts shaped C arrays,
  - writes zstd Parquet,
  - writes the existing `funbuns.*` footer key-value metadata,
  - normalizes `funbuns.n_rows` and `funbuns.n_primes` footer keys,
  - preserves schema field IDs in the Parquet schema,
  - emits one JSONL manifest row per data file for bridge commits.
- Keep C as the generation layer and C++ as the Iceberg/Arrow integration
  layer.

Todo:

- Add a bounded producer/writer pipeline. Current native writer materializes a
  whole file group before writing it; this is simple and fast for temp runs, but
  a production append loop should keep a fixed number of materialized groups in
  flight and apply backpressure from the writer.
- Tune row group boundaries:
  - current implementation writes one Arrow record batch per materialized chunk;
  - target is still file/row-group sizing around committed `128MB` blocks;
  - `--chunks-per-file=24` is close at the present 64-bit tail, while
    `--chunks-per-file=4` caps memory more tightly but underuses CPU.
- Chunk decompositions at `p` boundaries if future file splitting happens
  inside a materialized chunk.
- Decide whether to add a C API around the C++ writer so the CLI can stay mostly
  C.
- Make the iceberg-cpp build reproducible:
  - either vendor a pinned commit as a submodule/subtree,
  - or add a `native/iceberg-cpp-build` task/script with the GCC 15 warning
    override.

Deferred:

- Returning Iceberg `DataFile` metrics from iceberg-cpp is not ready for this
  workload. Its current `ParquetWriter::metrics()` returns an empty metrics
  object, so native catalog append should not rely on that path yet.

Reason:

- Apache `iceberg-cpp` provides a native Parquet writer and Arrow-native data
  boundary. That is the correct data-file writer target.

## Phase 4: Iceberg Catalog Commit

Current bridge:

- The native writer emits compatible Parquet files and `native_files.jsonl`.
- `python -m primeparts.native_iceberg --manifest ...` validates file paths,
  row counts, byte counts, and normalized `funbuns.*` footer metadata.
- PyIceberg `SqlCatalog` creates/loads the existing local catalog and calls
  `add_files` for those paths, one append per table.
- Production `primeparts -n ...` runs checkpoint the native writer: a bounded
  `primeparts-generate` process writes one manifest, PyIceberg commits it, and
  the coordinator starts the next segment. This bounds RSS growth from the
  Arrow / iceberg-cpp writer stack and limits recovery after a SIGKILL.
- Interactive native generation accepts `q` or `c` on stdin as a graceful stop:
  the monitor thread sets an atomic flag, the writer finishes the current file
  group, flushes the manifest, exits `0` with `stop_requested=true`, and the
  coordinator commits the completed checkpoint before stopping.
- Production generation refuses to write unless snapshot summary properties,
  manifest metrics, and local Parquet files are in agreement. Use
  `python -m primeparts.native_iceberg --check-warehouse` to inspect this.
- This preserves the temp `--temp` behavior and existing production warehouse
  layout while avoiding catalog IPC in the generation hot path.

Todo:

- Implement a native equivalent of PyIceberg `SqlCatalog` for the local
  `catalog.db`, or validate an upstream-compatible C++ catalog path if it lands.
- Keep append commits run-level, not file-level: one append for primes and one
  append for decompositions per generation run/checkpoint.
- Preserve snapshot properties:
  - `funbuns.max_p`
  - `funbuns.max_commit_seq`
- Validate manifest metrics fidelity against PyIceberg-added files.
- Preserve contiguous-prefix commit semantics for interrupted threaded runs.
- Expose checkpoint and native file-group settings in the eventual C CLI:
  `checkpoint_primes` / `--ckpt`, `chunk_primes` / `--chunk-primes`,
  `chunks_per_file` / `--cpf`, and `threads` / `--threads`.

Reason:

- Upstream `iceberg-cpp` currently provides in-memory and REST catalogs, not the
  SQLite SQL catalog used by this warehouse. The native data-file writer is
  ready; the remaining catalog work is a separate local-catalog implementation
  problem.

## Phase 5: Utils and CLI

Todo:

- Move config resolution into native code:
  - env vars first,
  - `pixi.toml` directory settings,
  - explicit CLI overrides.
- Replace `JournalWriter` with C JSONL logging.
- Port only the generation CLI first; analysis modes can stay Python until
  their Sage dependencies are mapped.
- Keep `sync_hms.py` as a Python tool until the catalog layer is native.

## Phase 6: Terminal Workbench

Direction:

- Build a full-screen terminal UI as an operations and exploration layer, not
  as part of the generation hot path.
- Prefer a modern ncurses-style framework such as Textual/Rich for the first
  version: it gives tables, async workers, keyboard navigation, progress panes,
  and structured layout while keeping query/generation code in ordinary Python
  modules. A pure `curses` frontend remains possible later if dependency
  minimization matters.

Initial screens:

- Warehouse status: catalog path, current snapshot IDs, `max_p`,
  `max_commit_seq`, orphan/reuse checks, recent manifests, and recovery actions.
- Generate: configure `num_primes`, `chunk_primes`, `chunks_per_file`,
  checkpoint size, physical/logical/explicit worker count, temp/production
  target, and run/pause/stop with live progress.
- Browse: inspect `funbuns.primes` and `funbuns.decompositions` by `p` range,
  `commit_seq`, `k`, and file group.
- Filters: saved predicates for `k`, `q_k`, `m_k`, `n_k`, near misses, and
  local obstruction views.
- Queries: run Polars/Iceberg SQL-style queries, show plans where available,
  export results, and later target materialized views.
- Jobs: background generation/query history, logs, manifests, and recovery
  scripts with dry-run/apply separation.

Architecture:

- Keep UI state separate from data operations. The UI should call stable service
  functions for health checks, generation launch, recovery dry-run/apply,
  scans, and query execution.
- All destructive or warehouse-mutating operations should present the same
  invariant checks as the CLI: refuse writes unless the warehouse is in good
  standing or a specific recovery path has validated the pending files.
- Long-running generation should stay in subprocesses so the UI can survive a
  worker crash and offer recovery.

## Open Questions

- The interim PyIceberg commit bridge is accepted for now while C owns
  generation and Parquet writing.
- Should the first fully native CLI keep the current divisibility rule
  `num_primes % batch_size == 0`, or should it support a short final batch?
- Is `INT64_MAX` a hard table boundary for the foreseeable dataset, or should
  the native ABI be widened to GMP before more code depends on it?
- For catalog compatibility, is SQLite `SqlCatalog` the only required write
  target, or should native code also commit directly to HMS?
