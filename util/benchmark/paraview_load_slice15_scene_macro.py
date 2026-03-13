from __future__ import annotations

import os
import sys
from pathlib import Path


def _ensure_util_dir_on_path() -> None:
    script_dir_env = os.environ.get("PARAVIEW_BENCHMARK_SCRIPT_DIR")
    if script_dir_env:
        util_dir = Path(script_dir_env).resolve()
    else:
        util_dir = Path(__file__).resolve().parent
    if str(util_dir) not in sys.path:
        sys.path.insert(0, str(util_dir))


def run() -> int:
    os.environ["PARAVIEW_BENCHMARK_RUNTIME_MODE"] = "gui-macro-setup"
    _ensure_util_dir_on_path()
    import paraview_load_slice15_scene

    return paraview_load_slice15_scene.main()


if __name__ == "__main__":
    raise SystemExit(run())
else:
    run()
