#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
ATLAS_AGENT_SRC = REPO_ROOT / "python" / "atlas_agent" / "src"
if str(ATLAS_AGENT_SRC) not in sys.path:
    sys.path.insert(0, str(ATLAS_AGENT_SRC))

from atlas_agent.scene_rpc import SceneClient

from atlas_volume_benchmark import (
    AXIS_SCOPE_ID,
    BACKGROUND_SCOPE_ID,
    CAMERA_JSON_KEY,
    NO_BOUND_BOX_VALUE,
    _loaded_ids,
    _set_canvas_size,
    _set_scope_bool_param,
    _wait_ready,
    SHOW_AXIS_JSON_KEY,
    SHOW_BACKGROUND_JSON_KEY,
)
from volume_benchmark_common import EventLogger, GenericCamera


DEFAULT_ROI_MANIFEST = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "high_res_20220219_roi_validation_v2"
    / "manifest.json"
)
DEFAULT_BASE_CAMERA = (
    REPO_ROOT / "large_test_image" / "high_res_scene_camera_exact_2000x1500.json"
)
DEFAULT_ADAPTIVE_DATASET = (
    REPO_ROOT
    / "large_test_image"
    / "high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim"
)
DEFAULT_OUTPUT_ROOT = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "high_res_20220219_fidelity_render_v2"
)
COMPOSITING_MODES = ("MIP Opaque", "Direct Volume Rendering")


@dataclass(frozen=True)
class ParamKeys:
    compositing: str
    sampling_rate: str
    bound_box: str | None
    full_resolution: str | None
    coord_transform: str | None
    x_cut: str | None
    y_cut: str | None
    z_cut: str | None
    display_range: str
    transfer_function: str


@dataclass(frozen=True)
class RenderCase:
    roi_label: str
    mode: str
    condition: str
    dataset_path: Path
    camera: GenericCamera
    screenshot_path: Path
    raw_mip_path: Path | None
    metadata_path: Path
    sampling_rate: float
    enable_full_resolution: bool
    roi_metadata: dict[str, Any]
    coord_transform_payload: dict[str, Any] | None
    local_cut_spans: dict[str, list[float]] | None


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Render the fidelity-validation ROI suite in Atlas. For each ROI and mode, "
            "this driver captures a resident native-resolution reference, the adaptive "
            "large-dataset output, and coarse L1/L2 controls."
        )
    )
    parser.add_argument("--roi-manifest", type=Path, default=DEFAULT_ROI_MANIFEST)
    parser.add_argument(
        "--adaptive-dataset", type=Path, default=DEFAULT_ADAPTIVE_DATASET
    )
    parser.add_argument("--base-camera-spec", type=Path, default=DEFAULT_BASE_CAMERA)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--address", default="localhost:50051")
    parser.add_argument("--atlas-dir", default="")
    parser.add_argument(
        "--atlas-log-path",
        default="/Users/feng/Library/Logs/Atlas",
        help=(
            "Deprecated compatibility flag. Fidelity renders now synchronize on the "
            "fixed-size screenshot/raw-MIP export calls instead of benchmark log markers."
        ),
    )
    parser.add_argument(
        "--mode",
        action="append",
        choices=COMPOSITING_MODES,
        default=None,
        help="Render mode to include. Repeat to render multiple modes. Defaults to MIP Opaque and Direct Volume Rendering.",
    )
    parser.add_argument(
        "--roi-label",
        action="append",
        default=None,
        help="Optional ROI label filter. Repeat to select multiple ROIs.",
    )
    parser.add_argument("--viewport-width", type=int, default=2000)
    parser.add_argument("--viewport-height", type=int, default=1500)
    parser.add_argument("--canvas-logical-width", type=int, default=1000)
    parser.add_argument("--canvas-logical-height", type=int, default=750)
    parser.add_argument(
        "--reference-sampling-rate",
        type=float,
        default=8.0,
        help="Sampling rate for resident reference renders and coarse controls.",
    )
    parser.add_argument(
        "--adaptive-sampling-rate",
        type=float,
        default=2.0,
        help="Sampling rate for the adaptive large-dataset renders.",
    )
    parser.add_argument(
        "--coarse-sampling-rate",
        type=float,
        default=None,
        help=(
            "Optional sampling rate for the standalone coarse L1/L2 controls. "
            "Defaults to the adaptive sampling rate so the coarse controls isolate "
            "spatial resolution instead of also changing ray-integration density."
        ),
    )
    parser.add_argument(
        "--display-range-min",
        type=float,
        default=0.0,
        help="Pinned display-range minimum.",
    )
    parser.add_argument(
        "--display-range-max",
        type=float,
        default=255.0,
        help="Pinned display-range maximum.",
    )
    parser.add_argument(
        "--camera-fit-margin",
        type=float,
        default=1.08,
        help="Extra margin multiplier applied when fitting the ROI box into the camera frustum.",
    )
    parser.add_argument(
        "--camera-distance-scale",
        type=float,
        default=1.0,
        help=(
            "Multiplier applied to the fitted camera distance. Values below 1.0 zoom in "
            "beyond the ROI-fit view while keeping the same view direction."
        ),
    )
    parser.add_argument(
        "--task-timeout-seconds",
        type=float,
        default=300.0,
        help="Timeout for Atlas StartLoadTask/WaitTask.",
    )
    parser.add_argument(
        "--ready-timeout-seconds",
        type=float,
        default=120.0,
        help="Timeout while waiting for Atlas objects to become ready.",
    )
    parser.add_argument(
        "--preview-timeout-seconds",
        type=float,
        default=120.0,
        help="Deprecated compatibility flag; no longer used by the export-based fidelity driver.",
    )
    parser.add_argument(
        "--final-timeout-seconds",
        type=float,
        default=600.0,
        help="Deprecated compatibility flag; no longer used by the export-based fidelity driver.",
    )
    parser.add_argument(
        "--hide-background",
        action="store_true",
        default=True,
        help="Hide the Atlas background pseudo-object.",
    )
    parser.add_argument(
        "--hide-axis",
        action="store_true",
        default=True,
        help="Hide the Atlas axis pseudo-object.",
    )
    parser.add_argument(
        "--hide-bound-box",
        action="store_true",
        default=True,
        help="Set image objects to No Bound Box.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite an existing output root.",
    )
    parser.add_argument(
        "--transfer-function-overrides",
        type=Path,
        default=None,
        help=(
            "Optional JSON file mapping ROI labels to mode names to Atlas transfer-function "
            "payloads. Overrides the bootstrapped mode transfer function when provided."
        ),
    )
    return parser.parse_args()


def _slug(text: str) -> str:
    return text.strip().lower().replace(" ", "_").replace("-", "_").replace("/", "_")


def _is_mip_mode(mode: str) -> bool:
    return "MIP" in mode


def _load_json(path: Path) -> dict[str, Any]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return raw


def _transfer_function_override(
    overrides: dict[str, Any] | None,
    *,
    roi_label: str,
    mode: str,
    default_payload: Any,
) -> Any:
    if overrides is None:
        return default_payload
    roi_payload = overrides.get(roi_label)
    if not isinstance(roi_payload, dict):
        return default_payload
    candidate = roi_payload.get(mode)
    if not isinstance(candidate, (dict, list)):
        return default_payload
    return candidate


def _select_modes(args: argparse.Namespace) -> list[str]:
    if args.mode:
        return list(args.mode)
    return list(COMPOSITING_MODES)


def _select_rois(
    manifest: dict[str, Any], requested_labels: list[str] | None
) -> list[dict[str, Any]]:
    rois = manifest.get("rois")
    if not isinstance(rois, list) or not rois:
        raise ValueError("ROI manifest must define a non-empty 'rois' list")
    if not requested_labels:
        return rois
    allowed = set(requested_labels)
    selected = [roi for roi in rois if str(roi.get("label", "")) in allowed]
    missing = sorted(allowed - {str(roi.get("label", "")) for roi in selected})
    if missing:
        raise ValueError(f"Unknown ROI labels requested: {missing}")
    return selected


def _load_base_camera(path: Path) -> GenericCamera:
    raw = _load_json(path)
    states = raw.get("states")
    if not isinstance(states, dict) or "open" not in states:
        raise ValueError(f"{path} must define states.open")
    return GenericCamera.from_json(states["open"])


def _normalize(vec: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(sum(component * component for component in vec))
    if length <= 1e-12:
        raise ValueError("cannot normalize a zero-length vector")
    return tuple(component / length for component in vec)  # type: ignore[return-value]


def _cross(
    a: tuple[float, float, float], b: tuple[float, float, float]
) -> tuple[float, float, float]:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _dot(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _camera_basis(
    base_camera: GenericCamera,
) -> tuple[
    tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]
]:
    forward = _normalize(
        (
            base_camera.center[0] - base_camera.eye[0],
            base_camera.center[1] - base_camera.eye[1],
            base_camera.center[2] - base_camera.eye[2],
        )
    )
    right = _normalize(_cross(forward, base_camera.up))
    up = _normalize(_cross(right, forward))
    return forward, right, up


def _fit_distance_for_box(
    *,
    half_extents: tuple[float, float, float],
    forward: tuple[float, float, float],
    right: tuple[float, float, float],
    up: tuple[float, float, float],
    vertical_fov_degrees: float,
    aspect: float,
    margin: float,
) -> float:
    half_width, half_height, half_depth = half_extents
    corners: list[tuple[float, float, float]] = []
    for sx in (-half_width, half_width):
        for sy in (-half_height, half_height):
            for sz in (-half_depth, half_depth):
                corners.append((sx, sy, sz))

    tan_half_v = math.tan(math.radians(vertical_fov_degrees) / 2.0)
    tan_half_h = tan_half_v * aspect
    required_distance = 0.0
    for corner in corners:
        x = _dot(corner, right)
        y = _dot(corner, up)
        z = _dot(corner, forward)
        required_distance = max(required_distance, abs(x) / tan_half_h - z)
        required_distance = max(required_distance, abs(y) / tan_half_v - z)
    required_distance = max(required_distance, half_depth * 2.0)
    return required_distance * max(1.0, float(margin))


def _voxel_aspect_scale(
    voxel_size_xyz: tuple[float, float, float],
) -> tuple[float, float, float]:
    vx, vy, vz = voxel_size_xyz
    xy = max(vx, vy)
    if not math.isfinite(xy) or xy <= 0.0 or not math.isfinite(vz) or vz <= 0.0:
        raise ValueError(
            f"Invalid source voxel size for Atlas scene-space scaling: {voxel_size_xyz}"
        )
    return (1.0, 1.0, vz / xy)


def _roi_center_and_half_extents_scene(
    roi_metadata: dict[str, Any],
    *,
    scale_xyz: tuple[float, float, float],
) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    bounds = roi_metadata.get("bounds_xyz")
    if not isinstance(bounds, dict):
        raise ValueError(f"ROI metadata missing bounds_xyz: {roi_metadata}")
    x_bounds = bounds.get("x")
    y_bounds = bounds.get("y")
    z_bounds = bounds.get("z")
    if not (
        isinstance(x_bounds, list)
        and len(x_bounds) == 2
        and isinstance(y_bounds, list)
        and len(y_bounds) == 2
        and isinstance(z_bounds, list)
        and len(z_bounds) == 2
    ):
        raise ValueError(f"ROI bounds are malformed: {bounds}")
    x0, x1 = int(x_bounds[0]), int(x_bounds[1])
    y0, y1 = int(y_bounds[0]), int(y_bounds[1])
    z0, z1 = int(z_bounds[0]), int(z_bounds[1])
    sx, sy, sz = scale_xyz
    center = (
        (x0 + x1) * 0.5 * sx,
        (y0 + y1) * 0.5 * sy,
        (z0 + z1) * 0.5 * sz,
    )
    half_extents = (
        (x1 - x0) * 0.5 * sx,
        (y1 - y0) * 0.5 * sy,
        (z1 - z0) * 0.5 * sz,
    )
    return center, half_extents


def _build_fit_camera(
    *,
    center: tuple[float, float, float],
    half_extents: tuple[float, float, float],
    base_camera: GenericCamera,
    viewport_width: int,
    viewport_height: int,
    margin: float,
    distance_scale: float,
) -> GenericCamera:
    forward, right, up = _camera_basis(base_camera)
    distance = _fit_distance_for_box(
        half_extents=half_extents,
        forward=forward,
        right=right,
        up=up,
        vertical_fov_degrees=base_camera.field_of_view_degrees,
        aspect=float(viewport_width) / float(viewport_height),
        margin=margin,
    )
    distance *= float(distance_scale)
    eye = (
        center[0] - forward[0] * distance,
        center[1] - forward[1] * distance,
        center[2] - forward[2] * distance,
    )
    return GenericCamera(
        projection=base_camera.projection,
        eye=eye,
        center=center,
        up=up,
        field_of_view_degrees=base_camera.field_of_view_degrees,
        eye_separation_angle_degrees=base_camera.eye_separation_angle_degrees,
    )


def _local_case_center_and_half_extents(
    roi_metadata: dict[str, Any], *, scene_scale_xyz: tuple[float, float, float]
) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    requested_shape = roi_metadata.get("requested_shape_xyz")
    if not isinstance(requested_shape, list) or len(requested_shape) != 3:
        raise ValueError(f"ROI metadata missing requested_shape_xyz: {roi_metadata}")
    width = int(requested_shape[0])
    height = int(requested_shape[1])
    depth = int(requested_shape[2])
    sx, sy, sz = scene_scale_xyz
    center = (width * 0.5 * sx, height * 0.5 * sy, depth * 0.5 * sz)
    half_extents = (width * 0.5 * sx, height * 0.5 * sy, depth * 0.5 * sz)
    return center, half_extents


def _coord_transform_payload(
    *,
    shape_xyz: tuple[int, int, int],
    scale_xyz: tuple[float, float, float],
) -> dict[str, Any]:
    width, height, depth = shape_xyz
    return {
        "Translation Vec3": [0.0, 0.0, 0.0],
        "Rotation Vec4": [0.0, 0.0, 1.0, 0.0],
        "Scale Vec3": [float(scale_xyz[0]), float(scale_xyz[1]), float(scale_xyz[2])],
        "Rotation Center Vec3": [width * 0.5, height * 0.5, depth * 0.5],
    }


def _roi_local_cut_spans(roi_metadata: dict[str, Any]) -> dict[str, list[float]]:
    bounds = roi_metadata.get("bounds_xyz")
    if not isinstance(bounds, dict):
        raise ValueError(f"ROI metadata missing bounds_xyz: {roi_metadata}")
    out: dict[str, list[float]] = {}
    for axis in ("x", "y", "z"):
        axis_bounds = bounds.get(axis)
        if not isinstance(axis_bounds, list) or len(axis_bounds) != 2:
            raise ValueError(
                f"ROI metadata bounds for axis {axis!r} are malformed: {bounds}"
            )
        out[axis] = [float(axis_bounds[0]), float(axis_bounds[1])]
    return out


def _discover_param_keys(client: SceneClient, obj_id: int) -> ParamKeys:
    params = client.list_params(id=int(obj_id))
    compositing = None
    sampling_rate = None
    bound_box = None
    full_resolution = None
    coord_transform = None
    x_cut = None
    y_cut = None
    z_cut = None
    display_range = None
    transfer_function = None
    for param in getattr(params, "params", []) or []:
        name = str(getattr(param, "name", "") or "")
        json_key = str(getattr(param, "json_key", "") or "")
        if name == "Compositing":
            compositing = json_key
        elif name == "Sampling Rate":
            sampling_rate = json_key
        elif name == "Bound Box":
            bound_box = json_key
        elif name == "Full Resolution Rendering":
            full_resolution = json_key
        elif name == "Coord Transform":
            coord_transform = json_key
        elif name == "X Cut":
            x_cut = json_key
        elif name == "Y Cut":
            y_cut = json_key
        elif name == "Z Cut":
            z_cut = json_key
        elif (
            display_range is None
            and name.startswith("Channel ")
            and name.endswith(" Display Range")
        ):
            display_range = json_key
        elif transfer_function is None and name.startswith("Transfer Function "):
            transfer_function = json_key
    if (
        not compositing
        or not sampling_rate
        or not display_range
        or not transfer_function
    ):
        raise RuntimeError(
            "Failed to discover required Atlas image parameter keys for fidelity rendering: "
            f"compositing={compositing!r}, sampling_rate={sampling_rate!r}, "
            f"display_range={display_range!r}, transfer_function={transfer_function!r}"
        )
    return ParamKeys(
        compositing=compositing,
        sampling_rate=sampling_rate,
        bound_box=bound_box,
        full_resolution=full_resolution,
        coord_transform=coord_transform,
        x_cut=x_cut,
        y_cut=y_cut,
        z_cut=z_cut,
        display_range=display_range,
        transfer_function=transfer_function,
    )


def _load_dataset(
    client: SceneClient,
    dataset_path: Path,
    *,
    task_timeout_seconds: float,
    ready_timeout_seconds: float,
) -> list[int]:
    task_id = client.start_load_task([str(dataset_path)], set_visible=True)
    task_status = client.wait_task(
        task_id,
        timeout_sec=float(task_timeout_seconds),
        poll_interval_sec=0.2,
    )
    ids = _loaded_ids(task_status)
    if not ids:
        raise RuntimeError(
            f"Atlas load task produced no objects for {dataset_path}: {json.dumps(task_status, sort_keys=True)}"
        )
    ready = _wait_ready(client, ids, timeout_sec=float(ready_timeout_seconds))
    if not ready.get("ok", False):
        raise RuntimeError(
            f"Atlas objects never became ready for {dataset_path}: {json.dumps(ready, sort_keys=True)}"
        )
    client.delete_task(task_id)
    return ids


def _remove_objects_if_needed(client: SceneClient, ids: list[int]) -> None:
    if not ids:
        return
    ok = client.remove_objects([int(obj_id) for obj_id in ids], allow_unsaved=True)
    if not ok:
        raise RuntimeError(f"Failed to remove previous Atlas objects: {ids}")


def _clear_existing_scene_objects(client: SceneClient) -> None:
    resp = client.list_objects()
    objects = getattr(resp, "objects", []) or []
    ids = [
        int(getattr(obj, "id", 0)) for obj in objects if int(getattr(obj, "id", 0)) > 0
    ]
    if not ids:
        return
    ok = client.remove_objects(ids, allow_unsaved=True)
    if not ok:
        raise RuntimeError(f"Failed to clear existing Atlas scene objects: {ids}")


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def _prepare_scene_once(
    client: SceneClient,
    logger: EventLogger,
    *,
    canvas_logical_width: int,
    canvas_logical_height: int,
    hide_background: bool,
    hide_axis: bool,
) -> None:
    client.ensure_view()
    logger.log("engine_ready")
    _set_canvas_size(
        client,
        logger,
        canvas_logical_width,
        canvas_logical_height,
        stage="pre_suite",
    )
    if hide_background:
        _set_scope_bool_param(
            client,
            BACKGROUND_SCOPE_ID,
            SHOW_BACKGROUND_JSON_KEY,
            False,
            logger,
            event_name="background_hidden",
            unsupported_event_name="background_hide_not_supported",
        )
    if hide_axis:
        _set_scope_bool_param(
            client,
            AXIS_SCOPE_ID,
            SHOW_AXIS_JSON_KEY,
            False,
            logger,
            event_name="axis_hidden",
            unsupported_event_name="axis_hide_not_supported",
        )


def _mode_preset_output(mode: str) -> dict[str, Any]:
    return {"mode": mode}


def _render_anchor(logger: EventLogger, *, case: RenderCase) -> dict[str, Any]:
    return logger.log(
        "render_request",
        roi=case.roi_label,
        mode=case.mode,
        condition=case.condition,
        dataset=str(case.dataset_path),
        sampling_rate=case.sampling_rate,
        enable_full_resolution=case.enable_full_resolution,
        camera=case.camera.to_json(),
        screenshot=str(case.screenshot_path),
        raw_mip=(str(case.raw_mip_path) if case.raw_mip_path is not None else None),
    )


def _current_wall_ns() -> int:
    return time.time_ns()


def _configure_loaded_dataset(
    client: SceneClient,
    *,
    obj_id: int,
    param_keys: ParamKeys,
    compositing_mode: str,
    sampling_rate: float,
    display_range_min: float,
    display_range_max: float,
    camera: GenericCamera,
    transfer_function_payload: Any,
    hide_bound_box: bool,
    enable_full_resolution: bool,
    coord_transform_payload: dict[str, Any] | None,
    local_cut_spans: dict[str, list[float]] | None,
) -> None:
    set_params: list[dict[str, Any]] = [
        {
            "id": 0,
            "json_key": CAMERA_JSON_KEY,
            "value": camera.to_atlas_typed_value(),
        },
        {
            "id": int(obj_id),
            "json_key": param_keys.compositing,
            "value": compositing_mode,
        },
        {
            "id": int(obj_id),
            "json_key": param_keys.sampling_rate,
            "value": float(sampling_rate),
        },
        {
            "id": int(obj_id),
            "json_key": param_keys.display_range,
            "value": [float(display_range_min), float(display_range_max)],
        },
        {
            "id": int(obj_id),
            "json_key": param_keys.transfer_function,
            "value": transfer_function_payload,
        },
    ]
    if hide_bound_box and param_keys.bound_box:
        set_params.append(
            {
                "id": int(obj_id),
                "json_key": param_keys.bound_box,
                "value": NO_BOUND_BOX_VALUE,
            }
        )
    if param_keys.full_resolution is not None:
        set_params.append(
            {
                "id": int(obj_id),
                "json_key": param_keys.full_resolution,
                "value": bool(enable_full_resolution),
            }
        )
    if coord_transform_payload is not None and param_keys.coord_transform is not None:
        set_params.append(
            {
                "id": int(obj_id),
                "json_key": param_keys.coord_transform,
                "value": coord_transform_payload,
            }
        )
    if local_cut_spans is not None:
        for axis_name, json_key in (
            ("x", param_keys.x_cut),
            ("y", param_keys.y_cut),
            ("z", param_keys.z_cut),
        ):
            if json_key is None:
                continue
            set_params.append(
                {
                    "id": int(obj_id),
                    "json_key": json_key,
                    "value": list(local_cut_spans[axis_name]),
                }
            )
    ok = client.apply_params(set_params)
    if not ok:
        raise RuntimeError(
            f"ApplySceneParams failed for object {obj_id} mode={compositing_mode} "
            f"condition camera/sampling configuration"
        )


def _bootstrap_mode_preset(
    *,
    client: SceneClient,
    logger: EventLogger,
    bootstrap_dataset: Path,
    bootstrap_camera: GenericCamera,
    mode: str,
    display_range_min: float,
    display_range_max: float,
    task_timeout_seconds: float,
    ready_timeout_seconds: float,
    canvas_logical_width: int,
    canvas_logical_height: int,
    hide_bound_box: bool,
) -> tuple[ParamKeys, Any]:
    ids = _load_dataset(
        client,
        bootstrap_dataset,
        task_timeout_seconds=task_timeout_seconds,
        ready_timeout_seconds=ready_timeout_seconds,
    )
    if len(ids) != 1:
        raise RuntimeError(
            f"Expected exactly one image object for bootstrap dataset {bootstrap_dataset}, got {ids}"
        )
    obj_id = int(ids[0])
    _set_canvas_size(
        client,
        logger,
        canvas_logical_width,
        canvas_logical_height,
        stage=f"bootstrap_{_slug(mode)}",
    )
    param_keys = _discover_param_keys(client, obj_id)
    bootstrap_params: list[dict[str, Any]] = [
        {
            "id": 0,
            "json_key": CAMERA_JSON_KEY,
            "value": bootstrap_camera.to_atlas_typed_value(),
        },
        {
            "id": obj_id,
            "json_key": param_keys.compositing,
            "value": mode,
        },
        {
            "id": obj_id,
            "json_key": param_keys.display_range,
            "value": [float(display_range_min), float(display_range_max)],
        },
    ]
    if hide_bound_box and param_keys.bound_box:
        bootstrap_params.append(
            {
                "id": obj_id,
                "json_key": param_keys.bound_box,
                "value": NO_BOUND_BOX_VALUE,
            }
        )
    anchor_record = logger.log(
        "mode_preset_bootstrap_request",
        mode=mode,
        dataset=str(bootstrap_dataset),
        camera=bootstrap_camera.to_json(),
    )
    ok = client.apply_params(bootstrap_params)
    if not ok:
        raise RuntimeError(f"ApplySceneParams failed during mode bootstrap for {mode}")
    logger.log(
        "mode_preset_bootstrap_complete",
        mode=mode,
        applied_wall_time_ns=_current_wall_ns(),
        request_wall_time_ns=int(anchor_record["wall_time_ns"]),
    )
    values = client.get_param_values(
        id=obj_id, json_keys=[param_keys.transfer_function]
    )
    transfer_payload = values.get(param_keys.transfer_function)
    if not isinstance(transfer_payload, (dict, list)):
        raise RuntimeError(
            f"Failed to read transfer-function payload for mode {mode}: {transfer_payload!r}"
        )
    _remove_objects_if_needed(client, ids)
    return param_keys, transfer_payload


def _screenshot(
    client: SceneClient,
    *,
    width: int,
    height: int,
    screenshot_path: Path,
) -> None:
    result = client.screenshot_3d(
        width=int(width),
        height=int(height),
        path=screenshot_path,
        overwrite=True,
    )
    if not result.get("ok", False):
        raise RuntimeError(
            f"TakeScreenshot3D failed for {screenshot_path}: {result.get('error', '')}"
        )


def _export_raw_mip(
    client: SceneClient,
    *,
    obj_id: int,
    raw_mip_path: Path,
) -> None:
    result = client.export_raw_mip_3d(
        id=int(obj_id),
        path=raw_mip_path,
        overwrite=True,
    )
    if not result.get("ok", False):
        raise RuntimeError(
            f"ExportRawMIP3D failed for object {obj_id} -> {raw_mip_path}: {result.get('error', '')}"
        )


def _export_screen_space_sufficiency_audit(
    client: SceneClient, *, obj_id: int
) -> dict[str, Any]:
    result = client.export_screen_space_sufficiency_audit_3d(id=int(obj_id))
    if not result.get("ok", False):
        raise RuntimeError(
            "ExportScreenSpaceSufficiencyAudit3D failed "
            f"for object {obj_id}: {result.get('error', '')}"
        )
    audit = result.get("audit")
    if not isinstance(audit, dict):
        raise RuntimeError(
            f"ExportScreenSpaceSufficiencyAudit3D returned no audit payload for object {obj_id}"
        )
    return json.loads(json.dumps(audit))


def _build_cases(
    *,
    rois: list[dict[str, Any]],
    modes: list[str],
    base_camera: GenericCamera,
    viewport_width: int,
    viewport_height: int,
    voxel_size_xyz: tuple[float, float, float],
    margin: float,
    adaptive_dataset: Path,
    output_root: Path,
    reference_sampling_rate: float,
    adaptive_sampling_rate: float,
    coarse_sampling_rate: float,
    camera_distance_scale: float,
) -> tuple[list[RenderCase], dict[str, dict[str, Any]]]:
    scene_scale_xyz = _voxel_aspect_scale(voxel_size_xyz)
    roi_cameras: dict[str, dict[str, Any]] = {}
    cases: list[RenderCase] = []
    for roi in rois:
        roi_label = str(roi["label"])
        adaptive_center, adaptive_half_extents = _roi_center_and_half_extents_scene(
            roi, scale_xyz=scene_scale_xyz
        )
        adaptive_camera = _build_fit_camera(
            center=adaptive_center,
            half_extents=adaptive_half_extents,
            base_camera=base_camera,
            viewport_width=viewport_width,
            viewport_height=viewport_height,
            margin=margin,
            distance_scale=camera_distance_scale,
        )
        local_center, local_half_extents = _local_case_center_and_half_extents(
            roi, scene_scale_xyz=scene_scale_xyz
        )
        local_camera = _build_fit_camera(
            center=local_center,
            half_extents=local_half_extents,
            base_camera=base_camera,
            viewport_width=viewport_width,
            viewport_height=viewport_height,
            margin=margin,
            distance_scale=camera_distance_scale,
        )
        roi_cameras[roi_label] = {
            "adaptive": adaptive_camera.to_json(),
            "local": local_camera.to_json(),
        }
        outputs = roi.get("outputs")
        if not isinstance(outputs, list):
            raise ValueError(f"ROI outputs are malformed for {roi_label}")
        output_by_level = {int(output["coarse_level"]): output for output in outputs}
        required_levels = {0, 1, 2}
        missing = sorted(required_levels - set(output_by_level))
        if missing:
            raise ValueError(
                f"ROI {roi_label} is missing required coarse levels: {missing}"
            )
        for mode in modes:
            mode_slug = _slug(mode)
            base_dir = output_root / "renders" / roi_label / mode_slug
            cases.append(
                RenderCase(
                    roi_label=roi_label,
                    mode=mode,
                    condition="reference",
                    dataset_path=Path(str(output_by_level[0]["path"])),
                    camera=local_camera,
                    screenshot_path=base_dir / "reference" / "screenshot.png",
                    raw_mip_path=(
                        base_dir / "reference" / "raw_mip.tif"
                        if _is_mip_mode(mode)
                        else None
                    ),
                    metadata_path=base_dir / "reference" / "render.json",
                    sampling_rate=float(reference_sampling_rate),
                    enable_full_resolution=False,
                    roi_metadata=roi,
                    coord_transform_payload=_coord_transform_payload(
                        shape_xyz=(
                            int(output_by_level[0]["shape_czyx"][3]),
                            int(output_by_level[0]["shape_czyx"][2]),
                            int(output_by_level[0]["shape_czyx"][1]),
                        ),
                        scale_xyz=scene_scale_xyz,
                    ),
                    local_cut_spans=None,
                )
            )
            cases.append(
                RenderCase(
                    roi_label=roi_label,
                    mode=mode,
                    condition="adaptive",
                    dataset_path=adaptive_dataset,
                    camera=adaptive_camera,
                    screenshot_path=base_dir / "adaptive" / "screenshot.png",
                    raw_mip_path=(
                        base_dir / "adaptive" / "raw_mip.tif"
                        if _is_mip_mode(mode)
                        else None
                    ),
                    metadata_path=base_dir / "adaptive" / "render.json",
                    sampling_rate=float(adaptive_sampling_rate),
                    enable_full_resolution=True,
                    roi_metadata=roi,
                    coord_transform_payload=None,
                    local_cut_spans=_roi_local_cut_spans(roi),
                )
            )
            for coarse_level in (1, 2):
                coarse_output = output_by_level[coarse_level]
                coarse_shape = coarse_output["shape_czyx"]
                coarse_factor = int(coarse_output["xy_downsample_factor"])
                cases.append(
                    RenderCase(
                        roi_label=roi_label,
                        mode=mode,
                        condition=f"coarse_l{coarse_level}",
                        dataset_path=Path(str(coarse_output["path"])),
                        camera=local_camera,
                        screenshot_path=base_dir
                        / f"coarse_l{coarse_level}"
                        / "screenshot.png",
                        raw_mip_path=(
                            base_dir / f"coarse_l{coarse_level}" / "raw_mip.tif"
                            if _is_mip_mode(mode)
                            else None
                        ),
                        metadata_path=base_dir
                        / f"coarse_l{coarse_level}"
                        / "render.json",
                        sampling_rate=float(coarse_sampling_rate),
                        enable_full_resolution=False,
                        roi_metadata=roi,
                        coord_transform_payload=_coord_transform_payload(
                            shape_xyz=(
                                int(coarse_shape[3]),
                                int(coarse_shape[2]),
                                int(coarse_shape[1]),
                            ),
                            scale_xyz=(
                                float(coarse_factor),
                                float(coarse_factor),
                                float(scene_scale_xyz[2]),
                            ),
                        ),
                        local_cut_spans=None,
                    )
                )
    return cases, roi_cameras


def main() -> int:
    args = _parse_args()
    output_root = args.output_root.resolve()
    if output_root.exists() and any(output_root.iterdir()) and not args.overwrite:
        raise FileExistsError(
            f"{output_root} already exists and is not empty. Pass --overwrite to reuse it."
        )
    output_root.mkdir(parents=True, exist_ok=True)

    roi_manifest = _load_json(args.roi_manifest.resolve())
    transfer_function_overrides = (
        _load_json(args.transfer_function_overrides.resolve())
        if args.transfer_function_overrides is not None
        else None
    )
    rois = _select_rois(roi_manifest, args.roi_label)
    modes = _select_modes(args)
    adaptive_dataset = args.adaptive_dataset.resolve()
    base_camera = _load_base_camera(args.base_camera_spec.resolve())
    voxel_size_values = roi_manifest.get("source_voxel_size_xyz")
    if not isinstance(voxel_size_values, list) or len(voxel_size_values) != 3:
        raise ValueError("ROI manifest must define source_voxel_size_xyz")
    voxel_size_xyz = (
        float(voxel_size_values[0]),
        float(voxel_size_values[1]),
        float(voxel_size_values[2]),
    )
    cases, roi_cameras = _build_cases(
        rois=rois,
        modes=modes,
        base_camera=base_camera,
        viewport_width=int(args.viewport_width),
        viewport_height=int(args.viewport_height),
        voxel_size_xyz=voxel_size_xyz,
        margin=float(args.camera_fit_margin),
        adaptive_dataset=adaptive_dataset,
        output_root=output_root,
        reference_sampling_rate=float(args.reference_sampling_rate),
        adaptive_sampling_rate=float(args.adaptive_sampling_rate),
        coarse_sampling_rate=float(
            args.coarse_sampling_rate
            if args.coarse_sampling_rate is not None
            else args.adaptive_sampling_rate
        ),
        camera_distance_scale=float(args.camera_distance_scale),
    )

    logger = EventLogger(output_root / "render_events.jsonl")
    _write_json(
        output_root / "config.json",
        {
            "roi_manifest": str(args.roi_manifest.resolve()),
            "adaptive_dataset": str(adaptive_dataset),
            "base_camera_spec": str(args.base_camera_spec.resolve()),
            "modes": modes,
            "viewport": {
                "width": int(args.viewport_width),
                "height": int(args.viewport_height),
            },
            "canvas_logical_size": {
                "width": int(args.canvas_logical_width),
                "height": int(args.canvas_logical_height),
            },
            "reference_sampling_rate": float(args.reference_sampling_rate),
            "adaptive_sampling_rate": float(args.adaptive_sampling_rate),
            "coarse_sampling_rate": float(
                args.coarse_sampling_rate
                if args.coarse_sampling_rate is not None
                else args.adaptive_sampling_rate
            ),
            "display_range": [
                float(args.display_range_min),
                float(args.display_range_max),
            ],
            "camera_fit_margin": float(args.camera_fit_margin),
            "camera_distance_scale": float(args.camera_distance_scale),
            "transfer_function_overrides": (
                str(args.transfer_function_overrides.resolve())
                if args.transfer_function_overrides is not None
                else None
            ),
            "hide_background": bool(args.hide_background),
            "hide_axis": bool(args.hide_axis),
            "hide_bound_box": bool(args.hide_bound_box),
        },
    )
    camera_dir = output_root / "roi_cameras"
    for roi_label, camera_payload in roi_cameras.items():
        _write_json(camera_dir / f"{roi_label}.json", camera_payload)

    client = SceneClient(
        address=args.address,
        atlas_dir=(args.atlas_dir.strip() or None),
    )
    active_ids: list[int] = []
    render_records: list[dict[str, Any]] = []
    mode_presets: dict[str, dict[str, Any]] = {}

    try:
        logger.log(
            "session_start",
            roi_manifest=str(args.roi_manifest.resolve()),
            adaptive_dataset=str(adaptive_dataset),
            base_camera_spec=str(args.base_camera_spec.resolve()),
            output_root=str(output_root),
            modes=modes,
            roi_labels=[str(roi["label"]) for roi in rois],
        )
        _clear_existing_scene_objects(client)
        _prepare_scene_once(
            client,
            logger,
            canvas_logical_width=int(args.canvas_logical_width),
            canvas_logical_height=int(args.canvas_logical_height),
            hide_background=bool(args.hide_background),
            hide_axis=bool(args.hide_axis),
        )

        bootstrap_case = next(case for case in cases if case.condition == "reference")
        for mode in modes:
            param_keys, transfer_payload = _bootstrap_mode_preset(
                client=client,
                logger=logger,
                bootstrap_dataset=bootstrap_case.dataset_path,
                bootstrap_camera=bootstrap_case.camera,
                mode=mode,
                display_range_min=float(args.display_range_min),
                display_range_max=float(args.display_range_max),
                task_timeout_seconds=float(args.task_timeout_seconds),
                ready_timeout_seconds=float(args.ready_timeout_seconds),
                canvas_logical_width=int(args.canvas_logical_width),
                canvas_logical_height=int(args.canvas_logical_height),
                hide_bound_box=bool(args.hide_bound_box),
            )
            preset_record = _mode_preset_output(mode)
            preset_record.update(
                {
                    "param_keys": {
                        "compositing": param_keys.compositing,
                        "sampling_rate": param_keys.sampling_rate,
                        "bound_box": param_keys.bound_box,
                        "full_resolution": param_keys.full_resolution,
                        "coord_transform": param_keys.coord_transform,
                        "x_cut": param_keys.x_cut,
                        "y_cut": param_keys.y_cut,
                        "z_cut": param_keys.z_cut,
                        "display_range": param_keys.display_range,
                        "transfer_function": param_keys.transfer_function,
                    },
                    "display_range": [
                        float(args.display_range_min),
                        float(args.display_range_max),
                    ],
                    "transfer_function": transfer_payload,
                }
            )
            mode_presets[mode] = preset_record
            _write_json(
                output_root / "mode_presets" / f"{_slug(mode)}.json", preset_record
            )

        for case in cases:
            _remove_objects_if_needed(client, active_ids)
            active_ids = _load_dataset(
                client,
                case.dataset_path,
                task_timeout_seconds=float(args.task_timeout_seconds),
                ready_timeout_seconds=float(args.ready_timeout_seconds),
            )
            if len(active_ids) != 1:
                raise RuntimeError(
                    f"Expected exactly one image object for {case.dataset_path}, got {active_ids}"
                )
            obj_id = int(active_ids[0])
            _set_canvas_size(
                client,
                logger,
                int(args.canvas_logical_width),
                int(args.canvas_logical_height),
                stage=f"render_{case.roi_label}_{_slug(case.mode)}_{case.condition}",
            )
            param_keys = ParamKeys(
                compositing=mode_presets[case.mode]["param_keys"]["compositing"],
                sampling_rate=mode_presets[case.mode]["param_keys"]["sampling_rate"],
                bound_box=mode_presets[case.mode]["param_keys"]["bound_box"],
                full_resolution=mode_presets[case.mode]["param_keys"][
                    "full_resolution"
                ],
                coord_transform=mode_presets[case.mode]["param_keys"][
                    "coord_transform"
                ],
                x_cut=mode_presets[case.mode]["param_keys"]["x_cut"],
                y_cut=mode_presets[case.mode]["param_keys"]["y_cut"],
                z_cut=mode_presets[case.mode]["param_keys"]["z_cut"],
                display_range=mode_presets[case.mode]["param_keys"]["display_range"],
                transfer_function=mode_presets[case.mode]["param_keys"][
                    "transfer_function"
                ],
            )
            transfer_function_payload = _transfer_function_override(
                transfer_function_overrides,
                roi_label=case.roi_label,
                mode=case.mode,
                default_payload=mode_presets[case.mode]["transfer_function"],
            )
            anchor_record = _render_anchor(logger, case=case)
            _configure_loaded_dataset(
                client,
                obj_id=obj_id,
                param_keys=param_keys,
                compositing_mode=case.mode,
                sampling_rate=case.sampling_rate,
                display_range_min=float(args.display_range_min),
                display_range_max=float(args.display_range_max),
                camera=case.camera,
                transfer_function_payload=json.loads(
                    json.dumps(transfer_function_payload)
                ),
                hide_bound_box=bool(args.hide_bound_box),
                enable_full_resolution=case.enable_full_resolution,
                coord_transform_payload=case.coord_transform_payload,
                local_cut_spans=case.local_cut_spans,
            )
            export_start_wall_time_ns = _current_wall_ns()
            case.screenshot_path.parent.mkdir(parents=True, exist_ok=True)
            _screenshot(
                client,
                width=int(args.viewport_width),
                height=int(args.viewport_height),
                screenshot_path=case.screenshot_path,
            )
            if case.raw_mip_path is not None:
                case.raw_mip_path.parent.mkdir(parents=True, exist_ok=True)
                _export_raw_mip(
                    client,
                    obj_id=obj_id,
                    raw_mip_path=case.raw_mip_path,
                )
            screen_space_sufficiency_audit = _export_screen_space_sufficiency_audit(
                client, obj_id=obj_id
            )
            export_finish_wall_time_ns = _current_wall_ns()
            record = {
                "roi_label": case.roi_label,
                "mode": case.mode,
                "condition": case.condition,
                "dataset_path": str(case.dataset_path),
                "screenshot_path": str(case.screenshot_path),
                "raw_mip_path": (
                    str(case.raw_mip_path) if case.raw_mip_path is not None else None
                ),
                "analysis_domain": (
                    "raw_mip_scalar"
                    if case.raw_mip_path is not None
                    else "screenshot_rgb"
                ),
                "sampling_rate": float(case.sampling_rate),
                "enable_full_resolution": bool(case.enable_full_resolution),
                "display_range": [
                    float(args.display_range_min),
                    float(args.display_range_max),
                ],
                "transfer_function": transfer_function_payload,
                "camera": case.camera.to_json(),
                "coord_transform": case.coord_transform_payload,
                "local_cut_spans": case.local_cut_spans,
                "screen_space_sufficiency_audit": screen_space_sufficiency_audit,
                "render_request_wall_time_ns": int(anchor_record["wall_time_ns"]),
                "export_start_wall_time_ns": int(export_start_wall_time_ns),
                "export_finish_wall_time_ns": int(export_finish_wall_time_ns),
                "export_client_ms": max(
                    0.0,
                    (int(export_finish_wall_time_ns) - int(export_start_wall_time_ns))
                    / 1_000_000.0,
                ),
                "roi_metadata": case.roi_metadata,
                "rendered_wall_time_ns": _current_wall_ns(),
            }
            _write_json(case.metadata_path, record)
            render_records.append(record)
            logger.log(
                "render_complete",
                roi=case.roi_label,
                mode=case.mode,
                condition=case.condition,
                screenshot=str(case.screenshot_path),
                raw_mip=(
                    str(case.raw_mip_path) if case.raw_mip_path is not None else None
                ),
                screen_space_sufficient_samples=screen_space_sufficiency_audit.get(
                    "sufficient_samples"
                ),
                screen_space_sufficient_pixels=screen_space_sufficiency_audit.get(
                    "sufficient_pixels"
                ),
                export_client_ms=record["export_client_ms"],
            )

        _write_json(
            output_root / "manifest.json",
            {
                "roi_manifest": str(args.roi_manifest.resolve()),
                "adaptive_dataset": str(adaptive_dataset),
                "base_camera_spec": str(args.base_camera_spec.resolve()),
                "output_root": str(output_root),
                "modes": modes,
                "roi_labels": [str(roi["label"]) for roi in rois],
                "viewport": {
                    "width": int(args.viewport_width),
                    "height": int(args.viewport_height),
                },
                "canvas_logical_size": {
                    "width": int(args.canvas_logical_width),
                    "height": int(args.canvas_logical_height),
                },
                "mode_presets": mode_presets,
                "render_records": render_records,
            },
        )
        logger.log("session_end", render_count=len(render_records))
    finally:
        try:
            _remove_objects_if_needed(client, active_ids)
        except Exception:
            pass
        logger.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
