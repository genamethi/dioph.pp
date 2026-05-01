"""
Throwaway instrumentation — DO NOT LAND.

Gated on FUNBUNS_PROBE_FILE. When unset, every probe is a no-op (one env
lookup + a None check). When set, events are appended as JSONL to the
named file. Isolated from the regular journal stream on purpose.
"""

from __future__ import annotations

import json
import os
import threading
import time
from datetime import datetime, timezone
from typing import Optional

_PROBE_FILE: Optional[str] = os.getenv("FUNBUNS_PROBE_FILE")
_LOCK = threading.Lock()
_PID = os.getpid()


def probe_enabled() -> bool:
    return _PROBE_FILE is not None


def _rss_mb() -> Optional[float]:
    try:
        import psutil
        return round(psutil.Process().memory_info().rss / 1_048_576, 1)
    except Exception:
        return None


def probe_write(event: str, **kv) -> None:
    if _PROBE_FILE is None:
        return
    entry = {
        "ts": time.monotonic(),
        "wall": datetime.now(timezone.utc).isoformat(),
        "pid": os.getpid(),
        "event": event,
        **kv,
    }
    line = json.dumps(entry, default=str) + "\n"
    with _LOCK:
        with open(_PROBE_FILE, "a") as f:
            f.write(line)


class RssSampler(threading.Thread):
    """Background thread: sample main-process RSS at `interval` seconds."""

    def __init__(self, interval: float = 5.0, label: str = "main"):
        super().__init__(daemon=True)
        self.interval = interval
        self.label = label
        self._stop = threading.Event()

    def run(self) -> None:
        if not probe_enabled():
            return
        while not self._stop.wait(self.interval):
            probe_write("rss_sample", label=self.label, rss_mb=_rss_mb())

    def stop(self) -> None:
        self._stop.set()


# Exposed for callers that want a cheap one-off RSS reading.
rss_mb = _rss_mb
