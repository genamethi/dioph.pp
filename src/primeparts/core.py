"""
Core implementation of prime power partition algorithm: p = 2^m + q^n

For each prime p:
  - Compute max_m = floor(log2(p))
  - For each m in [1, max_m], check if r = p - 2^m is a prime power
  - Record every p in the primes output with k = number of decompositions
  - Record only actual decompositions in the decompositions output

Workers receive (start_idx, count) as 1-indexed prime ranks. Each worker
resolves its range via nth_prime and generates primes locally. The feeder
does no prime generation — just one prime_pi call then arithmetic.

Above ``PRIME_RANGE_THRESHOLD`` (where sage's prime_range internally
falls back to ``list(primes(start, stop))``), the worker uses the
``primes`` iterator directly to skip the list materialization.

Concurrency:
  - ``pool.imap_unordered`` provides the sliding window of at-most-``cores``
    tasks in flight.
  - Each result is shaped into prime/decomposition polars DataFrames and
    handed to ``append_data`` (typically ``IcebergWriter.flush_shaped``)
    inline on the main thread.
  - Catalog commits are deferred: ``append_data`` writes parquet only;
    the caller runs ``IcebergWriter.commit_pending`` once after
    ``run_gen`` returns.

Interrupt handling:
  - Workers ignore SIGINT and read shared multiprocessing Events.
  - First Ctrl-C: stop dispatching new batches and ask workers to return
    after the current prime, preserving partial batch output.
  - Second Ctrl-C: ask workers to abandon the current prime and return
    partial batch output sooner.
  - Third Ctrl-C: abandon in-flight batches via pool.terminate().
"""

from sage.all import Integer, prime_range, primes, prime_pi, proof, nth_prime
from dataclasses import dataclass
import multiprocessing as mp
import numpy as np
import signal
import time as _time
from .utils import PARTITION_DISTRIBUTION

try:
    mp.set_start_method('spawn')
except RuntimeError:
    pass  # already set (e.g. by test runner or prior import)

proof.arithmetic(False)

# Above this stop value sage's prime_range internally builds
# `list(primes(start, stop))` (pari_isprime path); we use the iterator
# directly to skip the list materialization and save memory. Below it,
# prime_range uses pari_primes (sieve), which is faster.
PRIME_RANGE_THRESHOLD = 436_273_009

# ---------------------------------------------------------------------------
# Signal handling helpers (module-level for pickling)
# ---------------------------------------------------------------------------

_interrupt_count = 0
_manager_stop_event = None
_manager_abort_event = None
_worker_stop_event = None
_worker_abort_event = None


def _event_is_set(event) -> bool:
    return bool(event is not None and event.is_set())


def _sigint_handler(signum, frame):
    """Count interrupts without raising KeyboardInterrupt."""
    global _interrupt_count, _manager_stop_event, _manager_abort_event
    _interrupt_count += 1
    if _interrupt_count == 1:
        if _manager_stop_event is not None:
            _manager_stop_event.set()
        print("\nInterrupt received - returning partial batches after the current prime. "
              "Press Ctrl-C again to stop current primes sooner.", flush=True)
    elif _interrupt_count == 2:
        if _manager_stop_event is not None:
            _manager_stop_event.set()
        if _manager_abort_event is not None:
            _manager_abort_event.set()
        print("\nSecond interrupt - asking workers to abandon current primes. "
              "Press Ctrl-C again to terminate in-flight work.", flush=True)
    else:
        print("\nThird interrupt - terminating in-flight batches.", flush=True)


def _worker_init(stop_event=None, abort_event=None):
    """Pool initializer: make workers immune to SIGINT and share stop events."""
    global _worker_stop_event, _worker_abort_event
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    _worker_stop_event = stop_event
    _worker_abort_event = abort_event


def _worker_ignore_sigint():
    """Compatibility initializer: make workers immune to SIGINT."""
    _worker_init()


@dataclass(slots=True)
class PPBatchResult:
    """Pickle-friendly worker result with schema-width arrays."""

    prime_p: np.ndarray
    prime_k: np.ndarray
    decomp_p: np.ndarray
    decomp_m: np.ndarray
    decomp_n: np.ndarray
    decomp_q: np.ndarray
    processed_count: int
    start_idx: int
    requested_count: int
    interrupted: bool = False

    @classmethod
    def empty(cls, start_idx: int, requested_count: int, *, interrupted: bool = False):
        return cls(
            np.empty(0, dtype=np.int64),
            np.empty(0, dtype=np.int32),
            np.empty(0, dtype=np.int64),
            np.empty(0, dtype=np.int32),
            np.empty(0, dtype=np.int32),
            np.empty(0, dtype=np.int64),
            0,
            start_idx,
            requested_count,
            interrupted,
        )

    @property
    def decomp_count(self) -> int:
        return int(self.decomp_p.size)

    @property
    def max_p(self) -> int | None:
        if self.processed_count == 0:
            return None
        return int(self.prime_p[-1])


class PPBatchProcessor:
    """Worker class for processing prime batches using pre-allocated arrays."""

    def _ensure_power_cache(self, max_m):
        """Extend cached Sage powers of two up to ``2^max_m``."""
        powers = self.powers_of_two
        if len(powers) >= max_m:
            return

        next_power = (powers[-1] << 1) if powers else Integer(2)
        for _ in range(len(powers), max_m):
            powers.append(next_power)
            next_power <<= 1

    def _grow_prime_arrays(self):
        """Double prime arrays when needed."""
        new_size = max(1, len(self.prime_p) * 2)
        new_p = np.empty(new_size, dtype=np.int64)
        new_k = np.empty(new_size, dtype=np.int32)
        new_p[:self.current_prime] = self.prime_p[:self.current_prime]
        new_k[:self.current_prime] = self.prime_k[:self.current_prime]
        self.prime_p = new_p
        self.prime_k = new_k

    def _grow_decomp_arrays(self):
        """Double decomposition arrays when needed."""
        new_size = max(1, len(self.decomp_p) * 2)
        new_p = np.empty(new_size, dtype=np.int64)
        new_m = np.empty(new_size, dtype=np.int32)
        new_n = np.empty(new_size, dtype=np.int32)
        new_q = np.empty(new_size, dtype=np.int64)
        new_p[:self.current_decomp] = self.decomp_p[:self.current_decomp]
        new_m[:self.current_decomp] = self.decomp_m[:self.current_decomp]
        new_n[:self.current_decomp] = self.decomp_n[:self.current_decomp]
        new_q[:self.current_decomp] = self.decomp_q[:self.current_decomp]
        self.decomp_p = new_p
        self.decomp_m = new_m
        self.decomp_n = new_n
        self.decomp_q = new_q

    def _write_decomp(self, p_int: int, m_i: int, n_i: int, q_i: int):
        if self.current_decomp >= len(self.decomp_p):
            self._grow_decomp_arrays()
        self.decomp_p[self.current_decomp] = p_int
        self.decomp_m[self.current_decomp] = m_i
        self.decomp_n[self.current_decomp] = n_i
        self.decomp_q[self.current_decomp] = q_i
        self.current_decomp += 1

    def _write_prime(self, p_int: int, k_i: int):
        if self.current_prime >= len(self.prime_p):
            self._grow_prime_arrays()
        self.prime_p[self.current_prime] = p_int
        self.prime_k[self.current_prime] = k_i
        self.current_prime += 1

    def _process_prime_to_arrays(self, p):
        """Process single prime, writing results directly to pre-allocated arrays.

        Sage Integers go through __index__/__int__ on every numpy
        store; cast (p, pbase, pexp) to Python int once at the
        boundary so the assignment lands as a plain int64 store.
        """
        max_m = p.exact_log(2)
        self._ensure_power_cache(max_m)
        p_int = int(p)
        decomp_start = self.current_decomp
        q_hits = {}  # q_base -> count; at most 2 per (p, q) pair
        exhausted = []

        for power_idx in range(max_m):
            m_i = power_idx + 1
            two_i = self.powers_of_two[power_idx]
            q_cand_i = p - two_i

            if exhausted and any(q_cand_i % q == 0 for q in exhausted):
                continue

            (pbase, pexp) = q_cand_i.is_prime_power(proof=False, get_data=True)

            if pexp != 0:
                pbase_int = int(pbase)
                pexp_int = int(pexp)
                self._write_decomp(p_int, m_i, pexp_int, pbase_int)
                q_hits[pbase_int] = q_hits.get(pbase_int, 0) + 1
                if q_hits[pbase_int] >= 2:
                    exhausted.append(pbase)

        self._write_prime(p_int, self.current_decomp - decomp_start)

    def process_batch(
        self,
        prime_iter,
        expected_count,
        *,
        start_idx: int = 0,
        requested_count: int | None = None,
        stop_event=None,
        abort_event=None,
    ) -> PPBatchResult:
        """Process primes from an iterable, return schema-width NumPy arrays.

        ``expected_count`` is used to pre-size the result arrays;
        grow helpers cover the rare overflow case. The iterable may be
        a list, ``prime_range`` result, or ``primes`` iterator — we
        never call ``len()`` on it so iterators work directly.
        """
        requested_count = expected_count if requested_count is None else requested_count
        expected_count = max(0, int(expected_count))
        decomp_per_prime = max(
            PARTITION_DISTRIBUTION['avg_rows_per_prime']
            - PARTITION_DISTRIBUTION['zero_probability'],
            0.1,
        )
        estimated_decomp_rows = max(1, int(expected_count * decomp_per_prime * 1.2))
        self.prime_p = np.empty(max(1, expected_count), dtype=np.int64)
        self.prime_k = np.empty(max(1, expected_count), dtype=np.int32)
        self.decomp_p = np.empty(estimated_decomp_rows, dtype=np.int64)
        self.decomp_m = np.empty(estimated_decomp_rows, dtype=np.int32)
        self.decomp_n = np.empty(estimated_decomp_rows, dtype=np.int32)
        self.decomp_q = np.empty(estimated_decomp_rows, dtype=np.int64)
        self.current_prime = 0
        self.current_decomp = 0
        self.powers_of_two = []
        interrupted = False

        # Poll the cooperative-stop events every N primes; mp.Event.is_set()
        # is ~266 ns/call (futex-backed), so per-prime polling is non-trivial
        # at 100k+ primes/s/worker. N=256 caps the per-batch poll cost at
        # well under 1% while keeping abort latency at <2 ms.
        poll_stride = 256
        next_poll_at = poll_stride
        for i, prime in enumerate(prime_iter):
            if i >= next_poll_at:
                next_poll_at += poll_stride
                if _event_is_set(stop_event) or _event_is_set(abort_event):
                    interrupted = True
                    break
            self._process_prime_to_arrays(prime)

        return PPBatchResult(
            self.prime_p[:self.current_prime].copy(),
            self.prime_k[:self.current_prime].copy(),
            self.decomp_p[:self.current_decomp].copy(),
            self.decomp_m[:self.current_decomp].copy(),
            self.decomp_n[:self.current_decomp].copy(),
            self.decomp_q[:self.current_decomp].copy(),
            self.current_prime,
            start_idx,
            requested_count,
            interrupted,
        )


def worker_batch(start_idx: int, count: int) -> PPBatchResult | None:
    """Module-level worker: generate primes locally, process, return shaped arrays.

    Receives (start_idx, count) — 1-indexed prime ranks (matches
    ``nth_prime``). Only two integers cross the process boundary
    instead of a pickled list.

    Returns schema-width NumPy arrays. Every processed prime is returned
    once in ``prime_p``/``prime_k``; only actual decompositions cross the
    boundary in the decomposition arrays.
    """
    if _event_is_set(_worker_stop_event) or _event_is_set(_worker_abort_event):
        return PPBatchResult.empty(start_idx, count, interrupted=True)

    first_prime = nth_prime(start_idx)
    end_prime = nth_prime(start_idx + count)  # exclusive bound
    if end_prime > PRIME_RANGE_THRESHOLD:
        prime_iter = primes(first_prime, end_prime)
    else:
        prime_iter = prime_range(first_prime, end_prime)

    processor = PPBatchProcessor()
    result = processor.process_batch(
        prime_iter,
        count,
        start_idx=start_idx,
        requested_count=count,
        stop_event=_worker_stop_event,
        abort_event=_worker_abort_event,
    )

    if result.processed_count > 0 or result.interrupted:
        return result
    return None


def _worker_star(args):
    return worker_batch(*args)


class PPBatchFeeder:
    """Compute batch boundaries as index slices — no prime generation."""

    def __init__(self, init_p: int, num_primes: int, batch_size: int, verbose: bool = False):
        if num_primes % batch_size != 0:
            raise ValueError(f"num_primes ({num_primes}) must be divisible by batch_size ({batch_size})")

        self.batch_size = batch_size
        self.num_batches = num_primes // batch_size
        # nth_prime is 1-indexed; start_idx is the 1-indexed rank of the
        # FIRST prime to process. For init_p the last *processed* prime
        # (or 2 on a fresh start, which we treat as "p=2 already done"),
        # that's prime_pi(init_p) + 1, so nth_prime(start_idx)
        # == next_prime(init_p).
        self.start_idx = prime_pi(init_p) + 1

        if verbose:
            print(f"Start index: {self.start_idx} (prime_pi({init_p}) + 1)")

    def generate_batches(self):
        """Yield (start_idx, count) tuples — pure arithmetic, no prime generation."""
        for i in range(self.num_batches):
            yield (self.start_idx + i * self.batch_size, self.batch_size)


class PPManager:
    """Coordinates parallel prime power partition computation.

    Returns a status dict from run_gen() with ``interrupted`` (bool)
    and ``primes_not_processed`` so the caller can print a resume command.
    """

    def __init__(self, init_p, num_primes, batch_size, cores, append_data, verbose=False):
        self.init_p = init_p or 2
        self.num_primes = num_primes
        self.batch_size = batch_size
        self.cores = cores
        self.append_data = append_data
        self.verbose = verbose
        self.batches_processed = 0
        self.primes_processed = 0

    def run_gen(self):
        global _interrupt_count, _manager_stop_event, _manager_abort_event
        from tqdm import tqdm
        import polars as pl

        feeder = PPBatchFeeder(self.init_p, self.num_primes, self.batch_size, self.verbose)

        print(f"Processing {self.num_primes} primes starting from {self.init_p}")
        print(f"Batch size: {self.batch_size}, workers: {self.cores}")

        batches = feeder.generate_batches
        total_batches = self.num_primes // self.batch_size

        _interrupt_count = 0
        stop_event = mp.Event()
        abort_event = mp.Event()
        _manager_stop_event = stop_event
        _manager_abort_event = abort_event
        old_handler = signal.signal(signal.SIGINT, _sigint_handler)

        pool = mp.Pool(
            self.cores,
            initializer=_worker_init,
            initargs=(stop_event, abort_event),
            maxtasksperchild=8,
        )
        interrupted = False
        abandoned = False

        primes_schema = {"p": pl.Int64, "k": pl.Int32}
        decomp_schema = {
            "p": pl.Int64,
            "m_k": pl.Int32,
            "n_k": pl.Int32,
            "q_k": pl.Int64,
        }
        pbar = tqdm(total=self.num_primes, desc="Prime partition",
                    unit="prime", smoothing=0)
        rate_window_start = _time.monotonic()
        rate_window_primes = 0

        def _result_to_frames(result: PPBatchResult):
            primes_df = pl.DataFrame(
                {"p": result.prime_p, "k": result.prime_k},
                schema=primes_schema,
            )
            decomp_df = pl.DataFrame(
                {
                    "p": result.decomp_p,
                    "m_k": result.decomp_m,
                    "n_k": result.decomp_n,
                    "q_k": result.decomp_q,
                },
                schema=decomp_schema,
            )
            return primes_df, decomp_df

        def _batch_iter():
            # Stop dispatching after first Ctrl-C; in-flight results still
            # drain through imap_unordered.
            for b in batches():
                if _interrupt_count >= 1 or stop_event.is_set():
                    return
                yield b

        try:
            result_iter = pool.imap_unordered(_worker_star, _batch_iter(), chunksize=1)
            while True:
                if _interrupt_count >= 3:
                    abandoned = True
                    interrupted = True
                    break

                try:
                    result = result_iter.next(timeout=0.5)
                except StopIteration:
                    break
                except mp.TimeoutError:
                    if _interrupt_count >= 1 or stop_event.is_set() or abort_event.is_set():
                        interrupted = True
                    continue

                if result is None:
                    continue

                if result.interrupted:
                    interrupted = True

                processed = result.processed_count
                if processed > 0:
                    primes_df, decomp_df = _result_to_frames(result)
                    self.append_data(
                        primes_df,
                        decomp_df,
                        start_idx=result.start_idx,
                        processed_count=processed,
                    )

                    self.primes_processed += processed
                    self.batches_processed += 1
                    rate_window_primes += processed
                    pbar.update(processed)

                    now = _time.monotonic()
                    window_elapsed = now - rate_window_start
                    if window_elapsed >= 60.0:
                        rate_window_start = now
                        rate_window_primes = 0
                        window_elapsed = 0.001
                    pbar.set_postfix_str(
                        f"{self.batches_processed}/{total_batches} batches | "
                        f"{rate_window_primes / max(window_elapsed, 0.001):.0f} prime/s (60s)"
                    )

                if _interrupt_count >= 1 or stop_event.is_set() or abort_event.is_set():
                    interrupted = True

        finally:
            pbar.close()
            if abandoned:
                pool.terminate()
            else:
                pool.close()
            pool.join()
            signal.signal(signal.SIGINT, old_handler)
            _manager_stop_event = None
            _manager_abort_event = None

        remaining = self.num_primes - self.primes_processed

        if interrupted:
            print(f"\nShutdown: processed {self.primes_processed:,} of "
                  f"{self.num_primes:,} primes "
                  f"({self.batches_processed}/{total_batches} batches)"
                  + (" [abandoned in-flight]" if abandoned else " [drained in-flight]"))
        else:
            print(f"\nCompleted {self.primes_processed:,} primes "
                  f"in {self.batches_processed} batches")

        return {
            'interrupted': interrupted,
            'abandoned': abandoned,
            'primes_processed': self.primes_processed,
            'primes_not_processed': remaining,
            'batches_processed': self.batches_processed,
            'total_batches': total_batches,
            'init_p': self.init_p,
            'batch_size': self.batch_size,
            'start_idx': feeder.start_idx,
        }

    def get_status(self):
        return {
            'init_p': self.init_p,
            'num_primes': self.num_primes,
            'batch_size': self.batch_size,
            'cores': self.cores,
            'batches_processed': self.batches_processed,
            'primes_processed': self.primes_processed,
            'progress_pct': (self.primes_processed / self.num_primes * 100) if self.num_primes > 0 else 0,
        }

    def reset(self):
        self.batches_processed = 0
        self.primes_processed = 0
