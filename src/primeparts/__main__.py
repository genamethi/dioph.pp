#!/usr/bin/env python
# PYTHON_ARGCOMPLETE_OK
"""
Main entry point for prime power partition analysis.


"""

import argparse
import sys
import time
import psutil
from .utils import (setup_logging, get_config, setup_analysis_mode,
                    JournalWriter)


def main():
    parser = argparse.ArgumentParser(
        description='Prime power partition analysis: p = 2^m + q^n.\n'
                    'Resumes from max(p) upper_bound in the iceberg primes\n'
                    'table manifest metadata (no data-file scan).',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    # --- Generation ---
    gen = parser.add_argument_group('generation', 'Compute new prime partitions')
    gen.add_argument('-n', '--num-primes', type=int, required=False,
                     help='Number of primes to process')
    gen.add_argument('-b', '--batch-size', '--chunk-primes', type=int, default=None,
                     dest='batch_size',
                     help='Number of primes per worker batch '
                          '(default: 500000 for native pipeline, 10000 for --sage)')
    gen.add_argument('-p', '--processes', '--threads', type=int, default=None,
                     dest='processes',
                     help='Number of worker processes/threads (default: physical cores)')
    gen.add_argument('--logical', '--logical-cores', action='store_true',
                     dest='logical_cores',
                     help='Use logical CPU count for the default worker count '
                          '(default uses physical cores; ignored when -p is set)')
    gen.add_argument('--ckpt', '--checkpoint-primes', type=int, default=None, metavar='N',
                     dest='native_checkpoint_primes',
                     help='Native production checkpoint size in primes '
                          '(default: 250000000; env: PRIMEPARTS_NATIVE_CHECKPOINT_PRIMES)')
    gen.add_argument('-t', '--temp', action='store_true',
                     help='Run analysis in temporary file (for experiments)')
    gen.add_argument('--data-file', type=str, default=None,
                     help='Specify non-default data location')
    gen.add_argument('-i', '--init', type=int, default=None, metavar='P',
                     help='Override resume prime (start generation from prime P)')
    gen.add_argument('--sage', action='store_true',
                     help='Use the legacy Sage multiprocessing path '
                          '(default: native pipeline via primeparts-generate + PyIceberg)')
#   gen.add_argument('-g', '--genpp', type=int, metavar='N',
#                 help='Prepare prime powers data for first N primes (p^1 through p^100)')

    # --- Verbosity ---
    #TODO: Implement debug mode and keep this as level 1 verbosity (level 0 is default)
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Enable verbose output for debugging and profiling')
    #TODO: Implement this as level 2 verbosity
    parser.add_argument('-d', '-vv', '--debug', action='store_true',
                        help='More verbose with profiling of memory usage and timing data.')

    try:
        import argcomplete
    except ImportError:
        argcomplete = None
    if argcomplete is not None:
        argcomplete.autocomplete(parser)

    args = parser.parse_args()
    if args.processes is not None and args.processes <= 0:
        parser.error("-p/--processes must be positive")
    if args.native_checkpoint_primes is not None and args.native_checkpoint_primes <= 0:
        parser.error("--checkpoint-primes must be positive")

    # Default mode: prime generation (requires -n)
    if args.num_primes is None:
        parser.print_help()
        return

    use_native = not args.sage
    if args.batch_size is None:
        args.batch_size = 500_000 if use_native else 10_000

    # Determine number of workers
    if use_native:
        if args.processes is not None:
            cores = args.processes
            core_kind = "user-specified"
        else:
            cores = psutil.cpu_count(logical=args.logical_cores) or psutil.cpu_count(logical=True) or 1
            core_kind = "logical cores" if args.logical_cores else "physical cores"
        print(
            f"Using native pipeline: {cores} threads ({core_kind}, primeparts-generate)"
        )
    elif args.processes is not None:
        cores = args.processes
        print(f"Using {cores} workers (user-specified)")
    else:
        cores = psutil.cpu_count(logical=args.logical_cores) or psutil.cpu_count(logical=True) or 1
        core_kind = "logical cores" if args.logical_cores else "physical cores"
        print(f"Using {cores} workers ({core_kind})")

    setup_logging()

    config = get_config()

    # Journal for generation runs
    journal = JournalWriter(name="funbuns")
    t0 = time.monotonic()
    journal.log("main", "run_start",
                num_primes=args.num_primes, batch_size=args.batch_size,
                cores=cores)

    if use_native:
        from .native_core import setup_native_mode, run_native_pipeline
        native, init_p, commit_seq, warehouse, manifest, temp_root = setup_native_mode(args, config)
    else:
        init_p, writer, temp_root = setup_analysis_mode(args, config)
    if temp_root is not None:
        print(f"Running in temporary mode: {temp_root}")
    if args.init is not None:
        print(f"Starting from prime {args.init} (-i override, writing to iceberg)")

    if use_native:
        # primeparts-generate handles materialization + Parquet write,
        # then commit_native_manifest registers the files via PyIceberg.
        gen_status = run_native_pipeline(
            native,
            init_p=init_p,
            commit_seq=commit_seq,
            num_primes=args.num_primes,
            batch_size=args.batch_size,
            checkpoint_primes=args.native_checkpoint_primes,
            threads=cores,
            warehouse=warehouse,
            manifest=manifest,
            temp=args.temp,
            verbose=args.verbose,
        )
    else:
        from .core import PPManager
        manager = PPManager(init_p, args.num_primes, args.batch_size, cores,
                            append_data=writer.flush_shaped,
                            verbose=args.verbose)
        gen_status = manager.run_gen() or {}

        # Register every parquet file flushed during the run in a single
        # catalog commit. Runs even if interrupted — parquet on disk is
        # durable and worth registering. The anchor enables contiguous-prefix
        # filtering at the catalog boundary so an interrupted run never
        # leaves a gap visible to readers; out-of-order tail files past the
        # first gap are deleted from disk before add_files.
        try:
            writer.commit_pending(anchor_start_idx=gen_status.get('start_idx'))
        except Exception as exc:
            journal.log("main", "commit_pending_failed", error=repr(exc))
            raise

    elapsed = round(time.monotonic() - t0, 2)
    primes_processed = gen_status.get('primes_processed', 0)
    throughput = primes_processed / elapsed if elapsed > 0 else 0.0
    print(f"Timing: {elapsed:,.2f}s total | {throughput:,.0f} prime/s")

    # Emit shutdown/end journal event
    if gen_status.get('interrupted'):
        journal.log("main", "shutdown",
                    elapsed_s=elapsed,
                    abandoned=gen_status.get('abandoned', False),
                    primes_processed=gen_status.get('primes_processed', 0),
                    primes_not_processed=gen_status.get('primes_not_processed', 0))
    else:
        journal.log("main", "run_end",
                    elapsed_s=elapsed,
                    primes_processed=primes_processed)

    # Print resume command if interrupted
    remaining = gen_status.get('primes_not_processed', 0)
    if isinstance(remaining, int) and remaining > 0:
        print(f"\n{remaining:,} primes not processed.")
        resume = [
            "primeparts",
            "-n", str(remaining),
            "--chunk-primes", str(args.batch_size),
        ]
        if use_native:
            resume += ["--threads", str(cores)]
            if args.native_checkpoint_primes is not None:
                resume += ["--ckpt", str(args.native_checkpoint_primes)]
        else:
            resume += ["-p", str(cores), "--sage"]
        print(f"Resume: {' '.join(resume)}")


if __name__ == "__main__":
    main()
