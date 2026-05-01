"""
Utility functions for file handling, OS operations, and I/O.
"""

import json
import logging
import os
from datetime import datetime, timezone
from pathlib import Path
import polars as pl
import tomllib
import time
import shutil
from typing import Dict, List, Optional
def get_log_dir() -> Path:
    """Get the log directory path."""
    if log_dir := os.getenv('FUNBUNS_LOG_DIR'):
        p = Path(log_dir)
    elif data_dir := os.getenv('FUNBUNS_DATA_DIR'):
        # When data dir is overridden (e.g. tests), put logs under it
        p = Path(data_dir) / "logs"
    else:
        try:
            config = get_config()
            if log_dir := config.get('log_dir'):
                p = Path(log_dir)
            else:
                p = Path('logs')
        except Exception:
            p = Path('logs')
    p.mkdir(parents=True, exist_ok=True)
    return p


class JournalWriter:
    """Append-only JSONL event logger. One line per event, central journal.

    Every event automatically includes PID and memory stats (RSS/VMS in MB)
    via psutil, so crashes and leaks can be diagnosed after the fact.
    """

    def __init__(self, path: Optional[Path] = None, name: Optional[str] = None):
        if path is not None:
            self.path = path
        elif name is not None:
            self.path = get_log_dir() / f"{name}.jsonl"
        else:
            self.path = get_log_dir() / "journal.jsonl"
        self._pid = os.getpid()

    @staticmethod
    def _mem_stats() -> dict:
        """Return RSS and VMS in MB, or empty dict if psutil unavailable."""
        try:
            import psutil
            mem = psutil.Process().memory_info()
            return {
                "rss_mb": round(mem.rss / 1_048_576, 1),
                "vms_mb": round(mem.vms / 1_048_576, 1),
            }
        except Exception:
            return {}

    def log(self, module: str, event: str, **payload):
        entry = {
            "ts": datetime.now(timezone.utc).isoformat(),
            "pid": self._pid,
            "module": module,
            "event": event,
            **self._mem_stats(),
            **payload,
        }
        with open(self.path, "a") as f:
            f.write(json.dumps(entry, default=str) + "\n")


def setup_logging():
    """Set up logging configuration."""
    log_dir = get_log_dir()
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s',
        handlers=[
            logging.FileHandler(log_dir / 'decomp_analysis.log'),
            logging.StreamHandler()
        ]
    )


# Partition schema and constants for worker optimization
PARTITION_SCHEMA = {'p': pl.Int64, 'm_k': pl.Int32, 'n_k': pl.Int32, 'q_k': pl.Int64}

# Empirical partition distribution from 459M primes analysis
# Used for accurate batch size estimation in workers
PARTITION_DISTRIBUTION = {
    'avg_rows_per_prime': 1.7,  # Including zero rows
    'avg_partitions_per_prime': 2.05,  # Excluding zero rows  
    'zero_probability': 0.173,  # ~17.3% have no partitions
    'max_observed_partitions': 14  # Theoretical max observed
}


def get_config():
    """Get application configuration from pixi.toml following hierarchy."""
    try:
        config_file = get_config_file()
        with open(config_file, "rb") as f:
            config = tomllib.load(f)
        
        # Get main config. Older checkouts used [tool.primeparts]; this
        # package now stores the runtime paths under [tool.funbuns].
        tool_config = config.get("tool", {})
        funbuns_config = tool_config.get("funbuns", tool_config.get("primeparts", {}))
        
        # Merge directory configuration
        directories = funbuns_config.get("directories", {})
        funbuns_config.update(directories)
        
        return funbuns_config
    except (FileNotFoundError, tomllib.TOMLDecodeError):
        logging.warning(f"Could not load {config_file}, using defaults")
        return {}


def get_data_dir():
    """Get the application data directory path following configuration hierarchy."""
    # 1. Try environment variables (highest priority)
    if data_dir := os.getenv('FUNBUNS_DATA_DIR'):
        return Path(data_dir)
    
    # 2. Try pyproject.toml configuration
    try:
        config = get_config()
        if data_dir := config.get('data_dir'):
            return Path(data_dir)
    except Exception:
        pass
    
    # 3. Fallback to default
    data_dir = Path('data')
    data_dir.mkdir(exist_ok=True)
    return data_dir


def get_config_file():
    """Get the application configuration file path."""
    # For development: use local pixi.toml
    # For distribution: could use package resources or user config directory
    return Path('pixi.toml')


def get_temp_dir():
    """Get the temporary directory (under data_dir)."""
    d = get_data_dir() / "tmp"
    d.mkdir(parents=True, exist_ok=True)
    return d


def _build_temp_iceberg_writer():
    """Create an IcebergWriter backed by a fresh timestamped catalog in
    get_temp_dir(). Each --temp run gets its own isolated warehouse."""
    from .iceberg_schema import IcebergWriter, open_catalog

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    temp_root = get_temp_dir() / f"iceberg_temp_{ts}"
    cat = open_catalog(warehouse_root=temp_root / "warehouse")
    return IcebergWriter(cat=cat), temp_root


def setup_analysis_mode(args, config):
    """
    Resolve (init_p, writer, temp_root) from args. Handles all four
    combinations of --temp and --init. --init skips the resume scan and
    starts at the prime before args.init (PPBatchFeeder semantics:
    init_p is the last *processed* prime, so we pass previous_prime to
    include args.init itself). --temp builds an isolated per-run
    catalog; absent --temp the canonical catalog is used.
    """
    if args.temp:
        writer, temp_root = _build_temp_iceberg_writer()
    else:
        writer = None
        temp_root = None

    if args.init is not None:
        from sage.all import Integer, previous_prime
        init_p = previous_prime(Integer(args.init))
        if writer is None:
            from .iceberg_schema import IcebergWriter
            writer = IcebergWriter()
    elif args.temp:
        init_p = 2
    else:
        init_p, writer = setup_resume_mode(args.verbose)

    return init_p, writer, temp_root


def setup_resume_mode(verbose):
    """
    Open the canonical iceberg catalog and derive init_p from its current
    manifest state. Returns (init_p, writer).
    """
    from .iceberg_schema import IcebergWriter

    writer = IcebergWriter()
    if writer.resume_p > 0:
        init_p = writer.resume_p
        if verbose:
            print(f"Resuming from prime {init_p:,} (iceberg max p)")
        else:
            print(f"Resuming from prime {init_p}")
    else:
        init_p = 2
        print("No existing iceberg data found, starting from beginning")
    return init_p, writer


def resume_p(verbose: bool = False) -> int | None:
    """Return the last committed prime from the iceberg primes table, or None
    if the catalog is empty. Thin wrapper over IcebergWriter's manifest scan."""
    from .iceberg_schema import IcebergWriter

    writer = IcebergWriter()
    if writer.resume_p <= 0:
        return None
    if verbose:
        print(f"Resume: max prime {writer.resume_p:,} from iceberg manifest")
    return writer.resume_p
