from __future__ import annotations

import logging
import sys
from pathlib import Path

import pytest

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.subprocess_utils import (  # type: ignore  # noqa: E402
    SubprocessCapturePolicy,
    run_subprocess_with_captured_output,
)


def test_subprocess_capture_logs_tail_on_success_and_deletes_temp_log(caplog) -> None:
    script = "import sys\nfor i in range(50):\n    print(f'LINE_{i}')\nsys.exit(0)\n"
    args = [sys.executable, "-c", script]

    logger = logging.getLogger("atlas_agent.tests.subprocess_tail_success")
    with caplog.at_level(logging.INFO):
        res = run_subprocess_with_captured_output(
            args,
            logger=logger,
            log_prefix="test",
            policy=SubprocessCapturePolicy(tail_lines=20),
        )

    assert res.returncode == 0
    # Default behavior: temp log is deleted on success.
    assert not res.log_path.exists()

    joined = "\n".join(r.message for r in caplog.records)
    # Head + tail formatting: keep the first few lines, then omit the middle, then
    # keep the last N lines.
    assert "LINE_0" in joined
    assert "LINE_4" in joined
    assert "…[25 lines omitted]…" in joined
    assert "LINE_5" not in joined
    assert "LINE_49" in joined


def test_subprocess_capture_logs_full_output_on_error_and_keeps_temp_log(
    caplog,
) -> None:
    script = "import sys\nfor i in range(50):\n    print(f'LINE_{i}')\nsys.exit(2)\n"
    args = [sys.executable, "-c", script]

    logger = logging.getLogger("atlas_agent.tests.subprocess_tail_error")
    with caplog.at_level(logging.ERROR):
        res = run_subprocess_with_captured_output(
            args,
            logger=logger,
            log_prefix="test",
            policy=SubprocessCapturePolicy(tail_lines=20),
        )

    assert res.returncode == 2
    assert res.log_path.exists()
    try:
        content = res.log_path.read_text(encoding="utf-8", errors="replace")
    finally:
        # Cleanup temp log so tests don't leak files.
        res.log_path.unlink(missing_ok=True)

    assert "LINE_0" in content
    assert "LINE_49" in content

    joined = "\n".join(r.message for r in caplog.records)
    # Full output is included on error.
    assert "LINE_0" in joined
    assert "LINE_49" in joined
