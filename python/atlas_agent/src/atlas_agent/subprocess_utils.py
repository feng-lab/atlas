from __future__ import annotations

import logging
import os
import sys
import subprocess
import tempfile
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from .defaults import (
    DEFAULT_SUBPROCESS_LOG_HEAD_LINES,
    DEFAULT_SUBPROCESS_LOG_TAIL_LINES,
)

_GLOBAL_LIVE_SUBPROCESS_TAIL: bool = False


def set_live_subprocess_tail(enabled: bool) -> None:
    """Enable/disable live-updating subprocess output tails (console UI only)."""

    global _GLOBAL_LIVE_SUBPROCESS_TAIL
    _GLOBAL_LIVE_SUBPROCESS_TAIL = bool(enabled)


def _should_live_tail(policy_live: bool | None) -> bool:
    if policy_live is None:
        return bool(_GLOBAL_LIVE_SUBPROCESS_TAIL)
    return bool(policy_live)


@dataclass(frozen=True)
class SubprocessCapturePolicy:
    head_lines: int = DEFAULT_SUBPROCESS_LOG_HEAD_LINES
    tail_lines: int = DEFAULT_SUBPROCESS_LOG_TAIL_LINES
    # None means "use global default set by the CLI".
    live_tail: bool | None = None
    live_refresh_hz: float = 8.0
    show_full_output_on_error: bool = True
    show_tail_on_success: bool = True
    delete_temp_log_on_success: bool = True


@dataclass(frozen=True)
class SubprocessCaptureResult:
    returncode: int
    log_path: Path
    total_lines: int
    tail: list[str]


def run_subprocess_with_captured_output(
    args: Sequence[str],
    *,
    env: dict[str, str] | None = None,
    cwd: str | None = None,
    log_path: Path | None = None,
    logger: logging.Logger | None = None,
    log_prefix: str = "subprocess",
    policy: SubprocessCapturePolicy | None = None,
) -> SubprocessCaptureResult:
    """Run a subprocess while capturing stdout/stderr to a log file.

    This keeps the CLI readable by default (tail-only on success), while still
    preserving full output for debugging when the process fails.
    """

    pol = policy or SubprocessCapturePolicy()
    head_n = max(0, int(pol.head_lines))
    tail_n = max(0, int(pol.tail_lines))
    live_tail = _should_live_tail(pol.live_tail)
    live_refresh_hz = float(pol.live_refresh_hz) if pol.live_refresh_hz else 0.0
    if not (live_refresh_hz > 0.0):
        live_refresh_hz = 8.0

    lg = logger or logging.getLogger("atlas_agent.subprocess_utils")
    env_real = dict(os.environ)
    if env:
        env_real.update({str(k): str(v) for k, v in env.items()})

    # If the caller didn't provide a log path, write to a temp file.
    temp_log = False
    if log_path is None:
        temp_log = True
        fd, tmp_name = tempfile.mkstemp(prefix="atlas_subprocess_", suffix=".log")
        os.close(fd)
        log_path = Path(tmp_name)
    else:
        try:
            log_path.parent.mkdir(parents=True, exist_ok=True)
        except Exception:
            pass

    head: list[str] = []
    tail: deque[str] = deque(maxlen=tail_n if tail_n > 0 else 1)
    total_lines = 0
    live = None
    live_update = None

    def _format_head_omitted_tail(*, total: int) -> str:
        if total <= 0:
            return ""

        head_count = min(head_n, total)
        # Avoid overlap: show at most the remainder after head.
        tail_count = min(tail_n, max(0, total - head_count))

        head_lines = head[:head_count]
        tail_lines = list(tail)[-tail_count:] if tail_count > 0 else []
        omitted = max(0, total - len(head_lines) - len(tail_lines))

        out_lines: list[str] = []
        out_lines.extend(head_lines)
        if omitted > 0:
            out_lines.append(f"…[{omitted} lines omitted]…")
        out_lines.extend(tail_lines)
        return "\n".join(out_lines)

    def _live_render_text(*, total: int) -> str:
        body = _format_head_omitted_tail(total=total)
        if not body:
            return f"{log_prefix} (no output yet)"
        return body

    if live_tail and sys.stderr.isatty():
        try:
            from rich.console import Console  # type: ignore
            from rich.live import Live  # type: ignore
            from rich.panel import Panel  # type: ignore
            from rich.text import Text  # type: ignore

            console = Console(stderr=True)

            def _render_panel() -> Panel:
                txt = _live_render_text(total=total_lines)
                return Panel(
                    Text(txt),
                    title=f"{log_prefix} (running)",
                    subtitle=f"lines: {total_lines}",
                    border_style="dim",
                )

            live = Live(
                _render_panel(),
                console=console,
                refresh_per_second=max(1, int(round(live_refresh_hz))),
                transient=True,
            )

            def _maybe_live_update(force: bool = False) -> None:
                nonlocal live_update
                if live is None:
                    return
                now = time.monotonic()
                interval = 1.0 / float(max(1.0, live_refresh_hz))
                if force or live_update is None or (now - live_update) >= interval:
                    live.update(_render_panel(), refresh=True)
                    live_update = now

            live.__enter__()
        except Exception:
            live = None

    try:
        proc = subprocess.Popen(
            list(args),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env_real,
            cwd=cwd,
        )
    except FileNotFoundError:
        lg.error("Could not execute: %s", args[0] if args else "<empty args>")
        return SubprocessCaptureResult(
            returncode=127, log_path=log_path, total_lines=0, tail=[]
        )

    try:
        with open(log_path, "w", encoding="utf-8", errors="replace") as f:
            assert proc.stdout is not None
            for line in proc.stdout:
                total_lines += 1
                stripped = line.rstrip("\n")
                if head_n > 0 and len(head) < head_n:
                    head.append(stripped)
                try:
                    f.write(line)
                except Exception:
                    # Best-effort: console/log hygiene must not break execution.
                    pass
                if tail_n > 0:
                    tail.append(stripped)
                if live is not None:
                    _maybe_live_update()
        rc = int(proc.wait())
    finally:
        try:
            if proc.stdout is not None:
                proc.stdout.close()
        except Exception:
            pass
        try:
            if live is not None:
                _maybe_live_update(force=True)
                live.__exit__(None, None, None)
        except Exception:
            pass

    tail_list = list(tail) if tail_n > 0 else []
    if rc == 0:
        if pol.show_tail_on_success and (head or tail_list):
            body = _format_head_omitted_tail(total=total_lines)
            shown_head = min(len(head), head_n)
            shown_tail = min(len(tail_list), tail_n)
            lg.info(
                "%s output (total=%d, head=%d, tail=%d):\n%s",
                log_prefix,
                total_lines,
                shown_head,
                shown_tail,
                body,
            )
        if temp_log and pol.delete_temp_log_on_success:
            try:
                log_path.unlink(missing_ok=True)
            except Exception:
                pass
    else:
        if pol.show_full_output_on_error:
            try:
                full = log_path.read_text(encoding="utf-8", errors="replace")
            except Exception as e:
                full = f"(failed to read subprocess log at {log_path}: {e})"
            lg.error("%s failed (exit_code=%d). Full output:\n%s", log_prefix, rc, full)
        else:
            lg.error("%s failed (exit_code=%d).", log_prefix, rc)
            if tail_list:
                lg.error("%s output tail:\n%s", log_prefix, "\n".join(tail_list))

        lg.error("Full %s log saved at: %s", log_prefix, str(log_path))

    return SubprocessCaptureResult(
        returncode=rc,
        log_path=log_path,
        total_lines=int(total_lines),
        tail=tail_list,
    )
