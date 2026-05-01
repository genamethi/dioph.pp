"""Bridge from the C/C++ native pipeline to the existing Iceberg catalog.

Owns the orchestration of the native fast path:
    primeparts-generate (C core + iceberg-cpp Parquet writer)
        -> native_files.jsonl
        -> commit_native_manifest (PyIceberg add_files)
"""

from __future__ import annotations

import atexit
import ctypes as ct
import json
import os
import shutil
import subprocess
import time as _time
from datetime import datetime
from pathlib import Path
from typing import Any


_DEFAULT_CHUNKS_PER_FILE = 4
_DEFAULT_CHECKPOINT_PRIMES = 250_000_000


class _CBatchResult(ct.Structure):
    _fields_ = [
        ("prime_p", ct.POINTER(ct.c_int64)),
        ("prime_k", ct.POINTER(ct.c_int32)),
        ("decomp_p", ct.POINTER(ct.c_int64)),
        ("decomp_m", ct.POINTER(ct.c_int32)),
        ("decomp_n", ct.POINTER(ct.c_int32)),
        ("decomp_q", ct.POINTER(ct.c_int64)),
        ("prime_count", ct.c_size_t),
        ("decomp_count", ct.c_size_t),
        ("prime_capacity", ct.c_size_t),
        ("decomp_capacity", ct.c_size_t),
        ("processed_count", ct.c_int64),
        ("start_idx", ct.c_int64),
        ("requested_count", ct.c_int64),
        ("interrupted", ct.c_int),
        ("first_p", ct.c_int64),
        ("last_p", ct.c_int64),
    ]


class NativeCore:
    """Thin ctypes binding for libprimeparts_core. Used here only for
    prime_pi / previous_prime; the data plane runs in the
    primeparts-generate binary."""

    PP_OK = 0

    def __init__(self, library_path: Path | None = None):
        repo_root = Path(__file__).resolve().parents[2]
        self.library_path = library_path or repo_root / "native" / "build" / "libprimeparts_core.so"
        if not self.library_path.exists():
            raise RuntimeError(
                f"native core library not found at {self.library_path}; run `pixi run native-build`"
            )

        self.lib = ct.CDLL(str(self.library_path))
        self.lib.pp_init.argtypes = []
        self.lib.pp_init.restype = ct.c_int
        self.lib.pp_shutdown.argtypes = []
        self.lib.pp_shutdown.restype = None
        self.lib.pp_status_message.argtypes = [ct.c_int]
        self.lib.pp_status_message.restype = ct.c_char_p
        self.lib.pp_prime_pi.argtypes = [ct.c_int64]
        self.lib.pp_prime_pi.restype = ct.c_int64
        self.lib.pp_previous_prime.argtypes = [ct.c_int64]
        self.lib.pp_previous_prime.restype = ct.c_int64

        status = self.lib.pp_init()
        if status != self.PP_OK:
            raise RuntimeError(self._status_message(status))
        atexit.register(self.lib.pp_shutdown)

    def _status_message(self, status: int) -> str:
        msg = self.lib.pp_status_message(status)
        return msg.decode() if msg else f"native error {status}"

    def prime_pi(self, n: int) -> int:
        value = int(self.lib.pp_prime_pi(int(n)))
        if value < 0:
            raise ValueError(f"prime_pi failed for {n}")
        return value

    def previous_prime(self, n: int) -> int:
        value = int(self.lib.pp_previous_prime(int(n)))
        if value < 0:
            raise ValueError("no previous prime")
        return value


def _find_generate_binary() -> Path:
    name = "primeparts-generate"
    found = shutil.which(name)
    if found:
        return Path(found)
    repo_root = Path(__file__).resolve().parents[2]
    candidate = repo_root / "native" / "build" / name
    if candidate.exists():
        return candidate
    raise RuntimeError(
        f"could not locate {name} on PATH or at {candidate}; run `pixi run native-build`"
    )


def _checkpoint_primes(
    num_primes: int,
    batch_size: int,
    chunks_per_file: int,
    temp: bool,
    requested: int | None = None,
) -> int:
    if num_primes <= 0:
        return 1
    if temp:
        return num_primes
    if requested is not None:
        value = requested
    else:
        raw = os.getenv("PRIMEPARTS_NATIVE_CHECKPOINT_PRIMES")
        if raw is not None and raw.strip():
            value = int(raw)
            if value <= 0:
                return num_primes
        else:
            value = _DEFAULT_CHECKPOINT_PRIMES

    group_primes = max(1, int(batch_size) * int(chunks_per_file))
    value = max(group_primes, value)
    return (value // group_primes) * group_primes


def _segment_manifest(manifest: Path, segment_index: int, total_segments: int) -> Path:
    if total_segments <= 1:
        return manifest
    return manifest.with_name(f"{manifest.stem}_part{segment_index:05d}{manifest.suffix}")


def setup_native_mode(args, config):
    """Resolve init_p, commit_seq, and paths for the native pipeline.

    Returns ``(native, init_p, commit_seq, warehouse, manifest, temp_root)``.
    ``commit_seq`` is the first commit_seq the writer should assign — read
    from committed manifests plus existing local partition directories so
    reruns don't collide with committed or orphaned Parquet files.
    ``temp_root`` is the parent directory created for ``--temp`` runs, or
    None for production runs.
    """
    from .utils import get_temp_dir
    from .iceberg_schema import catalog_resume_state, get_warehouse_dir

    native = NativeCore()

    if args.temp:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        temp_root = get_temp_dir() / f"iceberg_temp_native_{ts}"
        warehouse = temp_root / "warehouse"
        manifest = temp_root / "native_files.jsonl"
    else:
        warehouse = get_warehouse_dir()
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        manifest = warehouse.parent / f"native_files_{ts}.jsonl"
        temp_root = None

    if args.temp:
        init_p = 2 if args.init is None else native.previous_prime(args.init)
        commit_seq = 0
    else:
        commit_seq, resume_p = catalog_resume_state()
        if args.init is not None:
            init_p = native.previous_prime(args.init)
            if args.verbose:
                print(f"Next commit_seq: {commit_seq:,} (validated warehouse summary + 1)")
        else:
            init_p = resume_p if resume_p > 0 else 2
            if resume_p > 0:
                if args.verbose:
                    print(f"Resuming from prime {init_p:,} (iceberg max p)")
                    print(f"Next commit_seq: {commit_seq:,} (validated warehouse summary + 1)")
                else:
                    print(f"Resuming from prime {init_p}")
            else:
                print("No existing iceberg data found, starting from beginning")

    return native, init_p, commit_seq, warehouse, manifest, temp_root


def _run_native_segment(
    *,
    binary: Path,
    start_idx: int,
    commit_seq: int,
    num_primes: int,
    batch_size: int,
    chunks_per_file: int,
    threads: int | None,
    warehouse: Path,
    manifest: Path,
    verbose: bool,
) -> tuple[dict[str, Any], float]:
    cmd = [
        str(binary),
        "--start-idx", str(start_idx),
        "--count", str(num_primes),
        "--warehouse", str(warehouse),
        "--manifest", str(manifest),
        "--chunk-primes", str(batch_size),
        "--chunks-per-file", str(chunks_per_file),
    ]
    if threads is not None and threads > 0:
        cmd += ["--threads", str(threads)]

    if verbose:
        print(f"$ {' '.join(cmd)}")

    env = os.environ.copy()
    env["PRIMEPARTS_COMMIT_SEQ_START"] = str(commit_seq)
    t0 = _time.monotonic()
    proc = subprocess.run(cmd, env=env, text=True, stdout=subprocess.PIPE)
    elapsed = _time.monotonic() - t0
    if proc.stdout:
        if verbose:
            print(proc.stdout, end="" if proc.stdout.endswith("\n") else "\n")
    if proc.returncode != 0:
        raise RuntimeError(
            f"primeparts-generate exited with code {proc.returncode}; "
            f"manifest: {manifest}"
        )

    try:
        return json.loads(proc.stdout.strip().splitlines()[-1]), elapsed
    except Exception as exc:
        raise RuntimeError(f"failed to parse native generator JSON: {proc.stdout!r}") from exc


def run_native_pipeline(
    native: NativeCore,
    *,
    init_p: int,
    commit_seq: int,
    num_primes: int,
    batch_size: int,
    chunks_per_file: int,
    checkpoint_primes: int | None,
    threads: int | None,
    warehouse: Path,
    manifest: Path,
    temp: bool,
    verbose: bool = False,
) -> dict[str, Any]:
    """Spawn primeparts-generate and register the manifest via PyIceberg.

    ``batch_size`` maps to ``--chunk-primes`` (the materialization chunk).
    ``chunks_per_file`` maps to native file-group width; the generator can
    use at most ``min(threads, chunks_per_file)`` workers for one group.
    ``commit_seq`` is coordinator-owned internal state. It is passed through
    the environment rather than exposed as a native CLI flag, because direct
    defaulting to zero caused production label reuse.
    """
    from .native_iceberg import commit_native_manifest

    start_idx = native.prime_pi(init_p) + 1
    binary = _find_generate_binary()

    print(f"Processing {num_primes:,} primes starting from {init_p:,}")
    print(
        f"Threads: {threads or 'auto'}, chunk_primes: {batch_size:,}, "
        f"chunks_per_file: {chunks_per_file:,}"
    )

    checkpoint_primes = _checkpoint_primes(
        num_primes,
        batch_size,
        chunks_per_file,
        temp,
        checkpoint_primes,
    )
    total_segments = (num_primes + checkpoint_primes - 1) // checkpoint_primes
    if total_segments > 1:
        print(
            f"Native checkpointing: {total_segments:,} segments, "
            f"up to {checkpoint_primes:,} primes per commit"
        )

    total_generated_elapsed = 0.0
    total_prime_rows = 0
    total_decomp_rows = 0
    total_files = 0
    graceful_stop = False
    current_start_idx = start_idx
    current_commit_seq = commit_seq
    remaining = num_primes

    for segment_index in range(1, total_segments + 1):
        segment_count = min(remaining, checkpoint_primes)
        segment_manifest = _segment_manifest(manifest, segment_index, total_segments)
        if total_segments > 1:
            print(
                f"[{segment_index:,}/{total_segments:,}] "
                f"Generating {segment_count:,} primes at rank {current_start_idx:,}"
            )

        segment_json, gen_elapsed = _run_native_segment(
            binary=binary,
            start_idx=current_start_idx,
            commit_seq=current_commit_seq,
            num_primes=segment_count,
            batch_size=batch_size,
            chunks_per_file=chunks_per_file,
            threads=threads,
            warehouse=warehouse,
            manifest=segment_manifest,
            verbose=verbose,
        )
        total_generated_elapsed += gen_elapsed
        segment_prime_rows = int(segment_json.get("prime_rows", segment_count))
        stop_requested = bool(segment_json.get("stop_requested", False))
        rate = segment_prime_rows / gen_elapsed if gen_elapsed > 0 else 0.0
        print(
            f"Generation: {segment_prime_rows:,} primes in "
            f"{gen_elapsed:.2f}s ({rate:,.0f} prime/s)"
        )

        print("Registering native Parquet files via PyIceberg add_files...")
        summary = commit_native_manifest(
            segment_manifest,
            warehouse=warehouse,
            temp=temp,
        )
        file_count = int(summary.get("files", 0))
        prime_rows = int(summary.get("prime_rows", 0))
        decomp_rows = int(summary.get("decomposition_rows", 0))
        print(f"Registered {file_count} files "
              f"({prime_rows:,} prime rows, "
              f"{decomp_rows:,} decomposition rows)")

        total_files += file_count
        total_prime_rows += prime_rows
        total_decomp_rows += decomp_rows
        if prime_rows <= 0:
            raise RuntimeError("native segment produced no committed prime rows")
        if prime_rows != segment_prime_rows:
            raise RuntimeError(
                f"committed prime rows ({prime_rows}) do not match generator "
                f"prime rows ({segment_prime_rows})"
            )
        if prime_rows < segment_count and not stop_requested:
            raise RuntimeError(
                f"native segment stopped short without stop_requested=true: "
                f"{prime_rows} < {segment_count}"
            )
        if prime_rows > segment_count:
            raise RuntimeError(
                f"native segment produced more rows than requested: "
                f"{prime_rows} > {segment_count}"
            )

        current_start_idx += prime_rows
        current_commit_seq = int(summary["max_commit_seq"]) + 1
        remaining -= prime_rows
        if stop_requested:
            graceful_stop = True
            if remaining > 0:
                print(
                    f"Stop requested; committed checkpoint and leaving "
                    f"{remaining:,} primes for resume."
                )
            break

    total_rate = total_prime_rows / total_generated_elapsed if total_generated_elapsed > 0 else 0.0
    if total_segments > 1:
        print(
            f"Native generation total: {total_prime_rows:,} primes in "
            f"{total_generated_elapsed:.2f}s ({total_rate:,.0f} prime/s)"
        )

    return {
        "interrupted": graceful_stop and remaining > 0,
        "abandoned": False,
        "primes_processed": total_prime_rows,
        "primes_not_processed": remaining,
        "batches_processed": total_files,
        "total_batches": total_files,
        "init_p": init_p,
        "batch_size": batch_size,
        "chunks_per_file": chunks_per_file,
        "start_idx": start_idx,
    }
