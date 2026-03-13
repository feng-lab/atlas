from __future__ import annotations

import json
import os
import subprocess
import threading
import time
from pathlib import Path


def sample_rss_bytes(pid: int) -> int | None:
    if pid <= 0:
        return None

    if os.name != "posix":
        return None

    result = subprocess.run(
        ["ps", "-o", "rss=", "-p", str(pid)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None

    text = result.stdout.strip()
    if not text:
        return None

    try:
        rss_kib = int(text)
    except ValueError:
        return None

    return rss_kib * 1024


class ProcessMemorySampler:
    def __init__(
        self,
        *,
        pid: int,
        interval_seconds: float,
        output_path: str | Path,
    ) -> None:
        self.pid = int(pid)
        self.interval_seconds = max(0.01, float(interval_seconds))
        self.output_path = Path(output_path)
        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        self._stream = self.output_path.open("w", encoding="utf-8")
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._run, name="process-memory-sampler", daemon=True
        )
        self._sample_count = 0
        self._peak_rss_bytes = 0
        self._first_rss_bytes: int | None = None
        self._last_rss_bytes: int | None = None
        self._started = False

    def start(self) -> None:
        if self._started:
            return
        self._started = True
        self._thread.start()

    def stop(self) -> dict[str, int | float | None | str]:
        if self._started:
            self._stop.set()
            self._thread.join(timeout=max(1.0, self.interval_seconds * 4.0))
            self._started = False
        self._stream.flush()
        self._stream.close()
        return {
            "pid": self.pid,
            "interval_seconds": self.interval_seconds,
            "samples_path": str(self.output_path),
            "sample_count": self._sample_count,
            "first_rss_bytes": self._first_rss_bytes,
            "last_rss_bytes": self._last_rss_bytes,
            "peak_rss_bytes": self._peak_rss_bytes,
        }

    def _write_sample(self, rss_bytes: int | None) -> None:
        wall_ns = time.time_ns()
        monotonic_ns = time.monotonic_ns()
        record = {
            "wall_time_ns": wall_ns,
            "monotonic_ns": monotonic_ns,
            "pid": self.pid,
            "rss_bytes": rss_bytes,
        }
        self._stream.write(json.dumps(record, sort_keys=True) + "\n")
        self._stream.flush()

    def _run(self) -> None:
        while not self._stop.is_set():
            rss_bytes = sample_rss_bytes(self.pid)
            self._write_sample(rss_bytes)
            if rss_bytes is not None:
                self._sample_count += 1
                if self._first_rss_bytes is None:
                    self._first_rss_bytes = rss_bytes
                self._last_rss_bytes = rss_bytes
                if rss_bytes > self._peak_rss_bytes:
                    self._peak_rss_bytes = rss_bytes
            self._stop.wait(self.interval_seconds)
