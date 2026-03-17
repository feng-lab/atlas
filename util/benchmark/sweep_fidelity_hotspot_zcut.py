#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[2]
ATLAS_AGENT_SRC = REPO_ROOT / "python" / "atlas_agent" / "src"
if str(ATLAS_AGENT_SRC) not in sys.path:
    sys.path.insert(0, str(ATLAS_AGENT_SRC))

from atlas_agent.scene_rpc import SceneClient

from atlas_fidelity_render import _discover_param_keys
from atlas_volume_benchmark import _apply_camera, _set_canvas_size
from volume_benchmark_common import EventLogger, GenericCamera


DEFAULT_INSPECTION_SUMMARY = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "inspection"
    / "roi03_mip_adaptive_maxdiff_v1"
    / "inspection_summary.json"
)
DEFAULT_OUTPUT_DIR = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "inspection"
    / "roi03_mip_adaptive_maxdiff_v1"
    / "zcut_sweep_reference_v1"
)
WINDOW_SIZES = (1, 3, 5)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Sweep the lower Z Cut bound for one loaded Atlas image object and record "
            "the hotspot pixel/window intensities from fixed-size screenshots."
        )
    )
    parser.add_argument("--address", default="localhost:50051")
    parser.add_argument(
        "--inspection-summary", type=Path, default=DEFAULT_INSPECTION_SUMMARY
    )
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument(
        "--object-condition",
        default="reference",
        help="Loaded condition in inspection_summary.json to sweep. Defaults to reference.",
    )
    parser.add_argument(
        "--canvas-logical-width",
        type=int,
        default=1000,
        help="Logical canvas width used in the retained benchmark convention.",
    )
    parser.add_argument(
        "--canvas-logical-height",
        type=int,
        default=750,
        help="Logical canvas height used in the retained benchmark convention.",
    )
    parser.add_argument(
        "--screenshot-width",
        type=int,
        default=2000,
        help="Export screenshot width. Must match the retained benchmark coordinate space.",
    )
    parser.add_argument(
        "--screenshot-height",
        type=int,
        default=1500,
        help="Export screenshot height. Must match the retained benchmark coordinate space.",
    )
    parser.add_argument(
        "--z-start",
        type=int,
        default=0,
        help="Inclusive lower Z Cut bound to start sweeping from.",
    )
    parser.add_argument(
        "--z-end",
        type=int,
        default=168,
        help="Inclusive lower Z Cut bound to end sweeping at.",
    )
    parser.add_argument(
        "--coarse-step",
        type=int,
        default=4,
        help="Coarse sweep step before local single-slice refinement.",
    )
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return payload


def _find_loaded_object_id(summary: dict[str, Any], condition: str) -> int:
    loaded = summary.get("loaded_objects")
    if not isinstance(loaded, list):
        raise ValueError("inspection summary missing loaded_objects")
    for item in loaded:
        if isinstance(item, dict) and item.get("condition") == condition:
            return int(item["object_id"])
    raise ValueError(f"condition {condition!r} not found in loaded_objects")


def _all_loaded_object_ids(summary: dict[str, Any]) -> list[int]:
    loaded = summary.get("loaded_objects")
    if not isinstance(loaded, list):
        raise ValueError("inspection summary missing loaded_objects")
    return [int(item["object_id"]) for item in loaded if isinstance(item, dict)]


def _load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def _pixel_and_window_metrics(
    image: np.ndarray, x: int, y: int, window_sizes: tuple[int, ...]
) -> dict[str, float]:
    if not (0 <= x < image.shape[1] and 0 <= y < image.shape[0]):
        raise ValueError(f"hotspot {(x, y)} is outside screenshot bounds {image.shape}")
    gray = image.max(axis=2).astype(np.float32)
    metrics: dict[str, float] = {}
    for size in window_sizes:
        half = size // 2
        x0 = max(0, x - half)
        y0 = max(0, y - half)
        x1 = min(gray.shape[1], x + half + 1)
        y1 = min(gray.shape[0], y + half + 1)
        patch = gray[y0:y1, x0:x1]
        prefix = f"w{size}"
        metrics[f"{prefix}_max"] = float(patch.max())
        metrics[f"{prefix}_mean"] = float(patch.mean())
    return metrics


def _first_cut_below_fraction(
    rows: list[dict[str, Any]], key: str, fraction: float
) -> int | None:
    if not rows:
        return None
    baseline = float(rows[0][key])
    threshold = baseline * fraction
    for row in rows:
        if float(row[key]) <= threshold:
            return int(row["z_cut_lower"])
    return None


def _largest_drop(rows: list[dict[str, Any]], key: str) -> dict[str, Any] | None:
    if len(rows) < 2:
        return None
    best: dict[str, Any] | None = None
    for prev, cur in zip(rows[:-1], rows[1:]):
        drop = float(prev[key]) - float(cur[key])
        if best is None or drop > float(best["drop"]):
            best = {
                "key": key,
                "from_z": int(prev["z_cut_lower"]),
                "to_z": int(cur["z_cut_lower"]),
                "drop": drop,
                "prev_value": float(prev[key]),
                "next_value": float(cur[key]),
            }
    return best


def _estimate_desired_voxel_size(
    *,
    z_cut_lower: int,
    camera_eye_z: float,
    field_of_view_degrees: float,
    screenshot_height: int,
    effective_device_pixel_ratio: float,
    z_scale: float,
) -> float:
    ze = abs(float(camera_eye_z) - float(z_cut_lower) * float(z_scale))
    return (
        ze
        * (2.0 * math.tan(math.radians(float(field_of_view_degrees)) / 2.0))
        / float(screenshot_height)
        * float(effective_device_pixel_ratio)
    )


def _save_selected_screenshots(
    client: SceneClient,
    *,
    obj_id: int,
    param_key: str,
    z_max: float,
    selected_cuts: list[int],
    screenshot_width: int,
    screenshot_height: int,
    output_dir: Path,
    camera: GenericCamera,
    canvas_logical_width: int,
    canvas_logical_height: int,
    logger: EventLogger,
) -> list[str]:
    saved: list[str] = []
    screenshots_dir = output_dir / "selected_screenshots"
    screenshots_dir.mkdir(parents=True, exist_ok=True)
    _set_canvas_size(
        client,
        logger,
        int(canvas_logical_width),
        int(canvas_logical_height),
        stage="zcut_selected_capture_setup",
    )
    _apply_camera(client, camera.to_atlas_typed_value())
    for cut in sorted(set(selected_cuts)):
        ok = client.apply_params(
            [
                {
                    "id": int(obj_id),
                    "json_key": param_key,
                    "value": [float(cut), float(z_max)],
                }
            ]
        )
        if not ok:
            raise RuntimeError(
                f"Failed to apply Z Cut [{cut}, {z_max}] while saving screenshots"
            )
        out_path = screenshots_dir / f"zcut_{cut:03d}.png"
        result = client.screenshot_3d(
            width=int(screenshot_width),
            height=int(screenshot_height),
            path=out_path,
            overwrite=True,
        )
        if not result.get("ok", False):
            raise RuntimeError(
                f"TakeScreenshot3D failed for cut {cut}: {result.get('error', '')}"
            )
        saved.append(str(out_path))
    return saved


def _capture_metrics_for_cut(
    client: SceneClient,
    *,
    obj_id: int,
    z_cut_key: str,
    z_lower: int,
    z_max: float,
    screenshot_width: int,
    screenshot_height: int,
    temp_screenshot: Path,
    hotspot_x: int,
    hotspot_y: int,
) -> dict[str, Any]:
    ok = client.apply_params(
        [
            {
                "id": int(obj_id),
                "json_key": z_cut_key,
                "value": [float(z_lower), float(z_max)],
            }
        ]
    )
    if not ok:
        raise RuntimeError(f"ApplySceneParams failed for Z Cut [{z_lower}, {z_max}]")
    result = client.screenshot_3d(
        width=int(screenshot_width),
        height=int(screenshot_height),
        path=temp_screenshot,
        overwrite=True,
    )
    if not result.get("ok", False):
        raise RuntimeError(
            f"TakeScreenshot3D failed for Z Cut [{z_lower}, {z_max}]: {result.get('error', '')}"
        )
    image = _load_rgb(temp_screenshot)
    metrics = _pixel_and_window_metrics(image, hotspot_x, hotspot_y, WINDOW_SIZES)
    row = {"z_cut_lower": int(z_lower)}
    row.update(metrics)
    return row


def main() -> int:
    args = _parse_args()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    inspection_summary = _load_json(args.inspection_summary.resolve())
    hotspot_xy = inspection_summary["diff_details"]["max_diff_xy"]
    hotspot_x = int(hotspot_xy[0])
    hotspot_y = int(hotspot_xy[1])
    object_id = _find_loaded_object_id(inspection_summary, args.object_condition)
    all_ids = _all_loaded_object_ids(inspection_summary)
    camera = GenericCamera.from_json(inspection_summary["camera"])
    z_max = 169.0

    client = SceneClient(args.address)
    resp = client.list_objects()
    live_ids = {
        int(getattr(obj, "id", 0)) for obj in getattr(resp, "objects", []) or []
    }
    if object_id not in live_ids:
        raise RuntimeError(
            f"Inspection object id {object_id} is not present in the current Atlas scene. "
            "Reload the inspection scene first."
        )

    logger = EventLogger(output_dir / "events.jsonl")
    _set_canvas_size(
        client,
        logger,
        int(args.canvas_logical_width),
        int(args.canvas_logical_height),
        stage="zcut_sweep_setup",
    )
    _apply_camera(client, camera.to_atlas_typed_value())
    client.set_visibility([int(i) for i in all_ids], False)
    client.set_visibility([int(object_id)], True)

    param_keys = _discover_param_keys(client, object_id)
    if param_keys.z_cut is None:
        raise RuntimeError(f"Object {object_id} does not expose a Z Cut parameter")

    temp_screenshot = output_dir / "_tmp_sweep.png"
    coarse_step = max(1, int(args.coarse_step))
    coarse_cuts = list(range(int(args.z_start), int(args.z_end) + 1, coarse_step))
    if coarse_cuts[-1] != int(args.z_end):
        coarse_cuts.append(int(args.z_end))
    rows_by_cut: dict[int, dict[str, Any]] = {}
    for z_lower in coarse_cuts:
        rows_by_cut[z_lower] = _capture_metrics_for_cut(
            client,
            obj_id=int(object_id),
            z_cut_key=param_keys.z_cut,
            z_lower=int(z_lower),
            z_max=z_max,
            screenshot_width=int(args.screenshot_width),
            screenshot_height=int(args.screenshot_height),
            temp_screenshot=temp_screenshot,
            hotspot_x=hotspot_x,
            hotspot_y=hotspot_y,
        )

    coarse_rows = [rows_by_cut[key] for key in sorted(rows_by_cut)]
    refine_keys = ("w1_max", "w3_max", "w5_max", "w3_mean", "w5_mean")
    refine_cuts: set[int] = set()
    for key in refine_keys:
        drop = _largest_drop(coarse_rows, key)
        if drop is None:
            continue
        start = max(int(args.z_start), int(drop["from_z"]) - coarse_step)
        end = min(int(args.z_end), int(drop["to_z"]) + coarse_step)
        refine_cuts.update(range(start, end + 1))

    for z_lower in sorted(refine_cuts):
        if z_lower in rows_by_cut:
            continue
        rows_by_cut[z_lower] = _capture_metrics_for_cut(
            client,
            obj_id=int(object_id),
            z_cut_key=param_keys.z_cut,
            z_lower=int(z_lower),
            z_max=z_max,
            screenshot_width=int(args.screenshot_width),
            screenshot_height=int(args.screenshot_height),
            temp_screenshot=temp_screenshot,
            hotspot_x=hotspot_x,
            hotspot_y=hotspot_y,
        )

    rows = [rows_by_cut[key] for key in sorted(rows_by_cut)]

    restore_ok = client.apply_params(
        [
            {
                "id": int(object_id),
                "json_key": param_keys.z_cut,
                "value": [0.0, float(z_max)],
            }
        ]
    )
    if not restore_ok:
        raise RuntimeError(
            "Failed to restore Z Cut to the full [0, 169] span after sweep"
        )

    largest_drops = {
        key: _largest_drop(rows, key)
        for key in ("w1_max", "w3_max", "w5_max", "w3_mean", "w5_mean")
    }
    half_intensity_cuts = {
        key: _first_cut_below_fraction(rows, key, 0.5)
        for key in ("w1_max", "w3_max", "w5_max")
    }
    quarter_intensity_cuts = {
        key: _first_cut_below_fraction(rows, key, 0.25)
        for key in ("w1_max", "w3_max", "w5_max")
    }

    effective_dpr = float(args.screenshot_width) / float(args.canvas_logical_width)
    desired_voxel_estimates = {}
    for key, cut in half_intensity_cuts.items():
        if cut is None:
            continue
        desired_voxel_estimates[key] = _estimate_desired_voxel_size(
            z_cut_lower=int(cut),
            camera_eye_z=float(camera.eye[2]),
            field_of_view_degrees=float(camera.field_of_view_degrees),
            screenshot_height=int(args.screenshot_height),
            effective_device_pixel_ratio=effective_dpr,
            z_scale=20.0,
        )

    selected_cuts = [0]
    for result in largest_drops.values():
        if result is not None:
            selected_cuts.extend([int(result["from_z"]), int(result["to_z"])])
    for cut in half_intensity_cuts.values():
        if cut is not None:
            selected_cuts.append(int(cut))
    saved_screens = _save_selected_screenshots(
        client,
        obj_id=int(object_id),
        param_key=param_keys.z_cut,
        z_max=z_max,
        selected_cuts=selected_cuts,
        screenshot_width=int(args.screenshot_width),
        screenshot_height=int(args.screenshot_height),
        output_dir=output_dir,
        camera=camera,
        canvas_logical_width=int(args.canvas_logical_width),
        canvas_logical_height=int(args.canvas_logical_height),
        logger=logger,
    )

    csv_path = output_dir / "zcut_sweep.csv"
    csv_header = [
        "z_cut_lower",
        "w1_max",
        "w1_mean",
        "w3_max",
        "w3_mean",
        "w5_max",
        "w5_mean",
    ]
    csv_lines = [",".join(csv_header)]
    for row in rows:
        csv_lines.append(",".join(str(row[key]) for key in csv_header))
    csv_path.write_text("\n".join(csv_lines) + "\n", encoding="utf-8")

    summary = {
        "inspection_summary": str(args.inspection_summary.resolve()),
        "object_condition": args.object_condition,
        "object_id": int(object_id),
        "hotspot_xy": [int(hotspot_x), int(hotspot_y)],
        "camera": camera.to_json(),
        "canvas_logical_size": [
            int(args.canvas_logical_width),
            int(args.canvas_logical_height),
        ],
        "screenshot_size": [int(args.screenshot_width), int(args.screenshot_height)],
        "effective_device_pixel_ratio": effective_dpr,
        "rows": rows,
        "largest_drops": largest_drops,
        "half_intensity_cuts": half_intensity_cuts,
        "quarter_intensity_cuts": quarter_intensity_cuts,
        "desired_voxel_size_estimates_at_half_intensity": desired_voxel_estimates,
        "selected_screenshots": saved_screens,
        "notes": [
            f"The sweep uses a coarse step of {coarse_step} followed by local single-slice refinement around the largest-drop bands.",
            "The sweep resets the benchmark camera and logical canvas size before the capture pass starts so the hotspot coordinate stays in the retained benchmark coordinate space.",
            "desired_voxel_size estimates assume the retained benchmark export convention (screenshot_width / canvas_logical_width) as the effective device pixel ratio.",
            "The hotspot depth estimate is approximate and uses the lower Z Cut bound as a proxy for the frontmost surviving source slice.",
        ],
    }
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
