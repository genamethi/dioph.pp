"""
Funbuns: Prime power partition analysis package.

Studying primes p that can be expressed as p = 2^m + q^n where q is prime.
"""

import os

# Cap thread-pool fan-out before sage/numpy/openblas/polars init. Workers
# inherit these via spawn re-import. Without these, OpenBLAS-pthreads
# spawns nproc threads per worker (24 unnamed threads observed in
# /proc/<pid>/task/*/comm) and glibc malloc allocates 8*nproc per-thread
# arenas — both blow up RSS at high -p.
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("MALLOC_ARENA_MAX", "1")


def _read_version() -> str:
    """ pixi run and pixi shell both add project version to the environment. """
    return os.getenv("PIXI_PROJECT_VERSION") or "0.0.0"


__version__ = _read_version()
## I suspect this isn't needed in the __init__ and can be moved to the test that actually uses it.
    ##TODO confirm and move to test file, delete here.
VERSION = tuple(int(x) for x in __version__.split("."))

__all__ = [
    'PPBatchProcessor',
    'worker_batch',
    'PPBatchFeeder',
    'PPManager',
    'setup_logging',
    'resume_p',
    'get_config',
]


def __getattr__(name):
    """Lazy-load Sage-backed symbols only when callers ask for them."""
    if name in {'PPBatchProcessor', 'worker_batch', 'PPBatchFeeder', 'PPManager'}:
        from importlib import import_module
        core = import_module(".core", __name__)
        return getattr(core, name)
    if name in {'setup_logging', 'resume_p', 'get_config'}:
        from importlib import import_module
        utils = import_module(".utils", __name__)
        return getattr(utils, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
