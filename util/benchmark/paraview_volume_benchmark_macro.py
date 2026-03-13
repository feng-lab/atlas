from __future__ import annotations

import os
import sys
from pathlib import Path


def _write_debug_line(message: str) -> None:
    log_path = Path(
        os.environ.get(
            "PARAVIEW_BENCHMARK_MACRO_DEBUG_LOG", "/tmp/paraview_macro_wrapper.log"
        )
    )
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as stream:
        stream.write(message + "\n")


def _ensure_util_dir_on_path() -> None:
    script_dir_env = os.environ.get("PARAVIEW_BENCHMARK_SCRIPT_DIR")
    if script_dir_env:
        util_dir = Path(script_dir_env).resolve()
    else:
        util_dir = Path(__file__).resolve().parent
    _write_debug_line(f"util_dir={util_dir}")
    if str(util_dir) not in sys.path:
        sys.path.insert(0, str(util_dir))


def run() -> int:
    _write_debug_line("macro run() entered")
    os.environ["PARAVIEW_BENCHMARK_RUNTIME_MODE"] = "gui-macro"
    _ensure_util_dir_on_path()
    import paraview_volume_benchmark

    _write_debug_line("imported paraview_volume_benchmark")
    return paraview_volume_benchmark.main()


if __name__ == "__main__":
    raise SystemExit(run())
else:
    run()
