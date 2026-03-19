from __future__ import annotations

import json
import os
import sys
from pathlib import Path

script_dir_env = os.environ.get("PARAVIEW_BENCHMARK_SCRIPT_DIR")
if script_dir_env:
    SCRIPT_DIR = Path(script_dir_env).resolve()
elif "__file__" in globals():
    SCRIPT_DIR = Path(__file__).resolve().parent
elif sys.argv and sys.argv[0]:
    SCRIPT_DIR = Path(sys.argv[0]).resolve().parent
else:
    SCRIPT_DIR = Path.cwd()
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from paraview.simple import (  # noqa: E402
    GetActiveViewOrCreate,
    HideUnusedScalarBars,
    OpenDataFile,
    Show,
)

import paraview_volume_benchmark as pvb  # noqa: E402
from volume_benchmark_common import load_benchmark_spec  # noqa: E402

HOME = Path.home()


def _parse_rgb_env(
    name: str, default: tuple[float, float, float]
) -> tuple[float, float, float]:
    value = os.environ.get(name)
    if value is None:
        return default
    parts = [part for part in value.replace(",", " ").split() if part]
    if len(parts) != 3:
        raise ValueError(f"{name} must contain three numbers, got {value!r}")
    return (float(parts[0]), float(parts[1]), float(parts[2]))


def main() -> int:
    dataset = os.environ.get(
        "PARAVIEW_SETUP_DATASET",
        str(
            HOME
            / "Dropbox"
            / "atlas_test"
            / "slice15_paraview"
            / "slice15_ch2_grid_atlasscenespace.vtpd"
        ),
    )
    camera_spec = os.environ.get(
        "PARAVIEW_SETUP_CAMERA_SPEC",
        str(
            HOME
            / "Dropbox"
            / "atlas_test"
            / "slice15_paraview"
            / "slice15_scene_camera_exact_2000x1500.json"
        ),
    )
    array_name = os.environ.get("PARAVIEW_SETUP_ARRAY_NAME", "channels")
    channel_mode = os.environ.get("PARAVIEW_SETUP_CHANNEL_MODE", "component")
    component = int(os.environ.get("PARAVIEW_SETUP_COMPONENT", "0"))
    blend_mode = os.environ.get("PARAVIEW_SETUP_BLEND_MODE", "maximum-intensity")
    data_range_min = float(os.environ.get("PARAVIEW_SETUP_DATA_RANGE_MIN", "0"))
    data_range_max = float(os.environ.get("PARAVIEW_SETUP_DATA_RANGE_MAX", "255"))
    color_min_rgb = _parse_rgb_env("PARAVIEW_SETUP_COLOR_MIN_RGB", (0.0, 0.0, 0.0))
    color_max_rgb = _parse_rgb_env(
        "PARAVIEW_SETUP_COLOR_MAX_RGB", (0.99215686, 0.0, 0.0)
    )
    output_info = Path(
        os.environ.get(
            "PARAVIEW_SETUP_INFO_PATH", "/tmp/paraview_load_slice15_scene.json"
        )
    )

    spec = load_benchmark_spec(camera_spec)
    view = GetActiveViewOrCreate("RenderView")
    view.ViewSize = [spec.viewport_width, spec.viewport_height]
    view.OrientationAxesVisibility = 0

    source = OpenDataFile(str(Path(dataset).resolve()))
    source.UpdatePipeline()
    resolved_array_name = pvb._resolve_array_name(source, array_name)
    display = Show(source, view)
    pvb._configure_display(
        display,
        resolved_array_name,
        channel_mode,
        component,
        blend_mode,
        data_range_min,
        data_range_max,
        color_min_rgb,
        color_max_rgb,
    )
    HideUnusedScalarBars(view=view)
    pvb._apply_camera(view, spec.states["open"])
    view.StillRender()

    render_window = view.GetRenderWindow()
    info = {
        "dataset": str(Path(dataset).resolve()),
        "camera_spec": str(Path(camera_spec).resolve()),
        "runtime_mode": "gui-macro-setup",
        "requested_view_size": [int(spec.viewport_width), int(spec.viewport_height)],
        "render_window_size": [int(v) for v in render_window.GetSize()],
        "blend_mode": blend_mode,
        "channel_mode": channel_mode,
        "component": component,
    }
    output_info.write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
