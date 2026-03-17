#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
ATLAS_AGENT_SRC = REPO_ROOT / "python" / "atlas_agent" / "src"
if str(ATLAS_AGENT_SRC) not in sys.path:
    sys.path.insert(0, str(ATLAS_AGENT_SRC))

from atlas_agent.scene_rpc import SceneClient

from atlas_fidelity_render import (
    _bootstrap_mode_preset,
    _build_fit_camera,
    _camera_basis,
    _clear_existing_scene_objects,
    _configure_loaded_dataset,
    _discover_param_keys,
    _load_dataset,
    _prepare_scene_once,
    _remove_objects_if_needed,
    _screenshot,
    _voxel_aspect_scale,
)
from volume_benchmark_common import EventLogger, GenericCamera


DEFAULT_MANIFEST = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "mip_phantom_phase_validation_v2"
    / "datasets"
    / "manifest.json"
)
DEFAULT_OUTPUT_ROOT = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "mip_phantom_phase_validation_v2"
    / "renders"
)
DEFAULT_ATLAS_LOG_ROOT = Path.home() / "Library" / "Logs" / "Atlas"
LOG_TIMESTAMP_RE = re.compile(
    r"^[IWEF](?:(?P<year>\d{4})(?P<month>\d{2})(?P<day>\d{2})|(?P<month2>\d{2})(?P<day2>\d{2})) "
    r"(?P<hour>\d{2}):(?P<minute>\d{2}):(?P<second>\d{2})\.(?P<fraction>\d+)"
)
FINAL_RE = re.compile(
    r"ATLAS_BENCHMARK_RENDER_FINISHED(?:\s+elapsed_ms=(?P<elapsed>[-+0-9.eE]+))?"
    r"(?:\s+progress=(?P<progress>[-+0-9.eE]+))?"
    r"(?:\s+source=(?P<source>\S+))?"
)


@dataclass(frozen=True)
class ConditionSpec:
    name: str
    dataset_path: Path
    dataset_shape_xyz: tuple[int, int, int]
    enable_full_resolution: bool
    scale_xyz: tuple[float, float, float] | None
    base_translation_xyz: tuple[float, float, float]
    local_cut_spans: dict[str, list[float]] | None


def _resolve_atlas_log_path(path: str | Path) -> Path:
    candidate = Path(path).expanduser().resolve()
    if candidate.is_file():
        return candidate
    if not candidate.is_dir():
        raise FileNotFoundError(f"Atlas log path does not exist: {candidate}")
    log_files = sorted(candidate.glob("**/atlas_info_*_log.txt"))
    if not log_files:
        raise FileNotFoundError(
            f"Could not find any atlas_info_*_log.txt under {candidate}"
        )
    return log_files[-1]


def _glog_line_time_ns(line: str, fallback_year: int) -> int | None:
    match = LOG_TIMESTAMP_RE.match(line)
    if not match:
        return None
    if match.group("year") is not None:
        year = int(match.group("year"))
        month = int(match.group("month"))
        day = int(match.group("day"))
    else:
        year = int(fallback_year)
        month = int(match.group("month2"))
        day = int(match.group("day2"))
    hour = int(match.group("hour"))
    minute = int(match.group("minute"))
    second = int(match.group("second"))
    fraction = match.group("fraction")
    fraction_ns = int((fraction + "000000000")[:9])
    base = datetime(year, month, day, hour, minute, second, tzinfo=timezone.utc)
    return int(base.timestamp()) * 1_000_000_000 + fraction_ns


def _parse_final_render_entries(text: str, fallback_year: int) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for line in text.splitlines():
        match = FINAL_RE.search(line)
        if match is None:
            continue
        wall_time_ns = _glog_line_time_ns(line, fallback_year)
        if wall_time_ns is None:
            continue
        elapsed_ms = match.groupdict().get("elapsed")
        progress = match.groupdict().get("progress")
        source = match.groupdict().get("source")
        entries.append(
            {
                "kind": "final",
                "wall_time_ns": wall_time_ns,
                "elapsed_ms": float(elapsed_ms) if elapsed_ms is not None else None,
                "progress": float(progress) if progress is not None else None,
                "source": source,
                "line": line,
            }
        )
    entries.sort(key=lambda entry: int(entry["wall_time_ns"]))
    return entries


class AtlasRenderLogFollower:
    def __init__(self, path: Path, *, fallback_year: int) -> None:
        self.path = Path(path)
        self.fallback_year = int(fallback_year)
        self._offset = self.path.stat().st_size if self.path.exists() else 0
        self._pending: list[dict[str, Any]] = []

    def _refresh(self) -> None:
        if not self.path.exists():
            return
        size = self.path.stat().st_size
        if size < self._offset:
            self._offset = 0
            self._pending.clear()
        if size == self._offset:
            return
        with self.path.open("rb") as stream:
            stream.seek(self._offset)
            chunk = stream.read(size - self._offset)
        self._offset = size
        text = chunk.decode("utf-8", errors="replace")
        self._pending.extend(_parse_final_render_entries(text, self.fallback_year))
        self._pending.sort(key=lambda entry: int(entry["wall_time_ns"]))

    def wait_for_final_marker(
        self, *, start_ns: int, timeout_seconds: float
    ) -> dict[str, Any]:
        deadline = time.monotonic() + max(0.0, float(timeout_seconds))
        while True:
            self._refresh()
            kept: list[dict[str, Any]] = []
            selected: dict[str, Any] | None = None
            for entry in self._pending:
                entry_time_ns = int(entry["wall_time_ns"])
                if entry_time_ns < start_ns:
                    continue
                if selected is None:
                    selected = entry
                    continue
                kept.append(entry)
            self._pending = kept
            if selected is not None:
                return selected
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"Timed out waiting for Atlas final marker after {timeout_seconds:.3f}s "
                    f"from wall_time_ns={start_ns} in {self.path}"
                )
            time.sleep(0.05)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Render the MIP phantom phase-sweep experiment in Atlas. The driver keeps "
            "camera, transfer function, and sampling fixed, and sweeps only an "
            "image-plane subpixel translation across the stored phantom ROI."
        )
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--address", default="localhost:50051")
    parser.add_argument("--atlas-dir", default="")
    parser.add_argument(
        "--atlas-log-path",
        default=str(DEFAULT_ATLAS_LOG_ROOT),
        help=(
            "Path to the active Atlas application log file, or the Atlas log directory. "
            "When provided, the driver waits for ATLAS_BENCHMARK_RENDER_FINISHED "
            "before taking each screenshot."
        ),
    )
    parser.add_argument("--viewport-width", type=int, default=2000)
    parser.add_argument("--viewport-height", type=int, default=1500)
    parser.add_argument("--canvas-logical-width", type=int, default=1000)
    parser.add_argument("--canvas-logical-height", type=int, default=750)
    parser.add_argument("--sampling-rate", type=float, default=2.0)
    parser.add_argument("--display-range-min", type=float, default=0.0)
    parser.add_argument("--display-range-max", type=float, default=255.0)
    parser.add_argument("--task-timeout-seconds", type=float, default=300.0)
    parser.add_argument("--ready-timeout-seconds", type=float, default=120.0)
    parser.add_argument(
        "--final-timeout-seconds",
        type=float,
        default=120.0,
        help="Timeout while waiting for the Atlas final render marker before each screenshot.",
    )
    parser.add_argument(
        "--phase-count",
        type=int,
        default=None,
        help="Override the manifest phase count.",
    )
    parser.add_argument(
        "--hide-background",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--hide-axis",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--hide-bound-box",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--clear-existing",
        action="store_true",
        help="Remove existing Atlas objects before running the render sweep.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite an existing non-empty output root.",
    )
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return payload


def _coord_transform_payload(
    *,
    shape_xyz: tuple[int, int, int],
    scale_xyz: tuple[float, float, float],
    translation_xyz: tuple[float, float, float],
) -> dict[str, Any]:
    width, height, depth = shape_xyz
    return {
        "Translation Vec3": [
            float(translation_xyz[0]),
            float(translation_xyz[1]),
            float(translation_xyz[2]),
        ],
        "Rotation Vec4": [0.0, 0.0, 1.0, 0.0],
        "Scale Vec3": [float(scale_xyz[0]), float(scale_xyz[1]), float(scale_xyz[2])],
        "Rotation Center Vec3": [width * 0.5, height * 0.5, depth * 0.5],
    }


def _require_manifest_list(payload: dict[str, Any], key: str, size: int) -> list[float]:
    value = payload.get(key)
    if not isinstance(value, list) or len(value) != size:
        raise ValueError(f"manifest field {key!r} must be a {size}-element array")
    return [float(item) for item in value]


def _build_camera(
    manifest: dict[str, Any], viewport_width: int, viewport_height: int
) -> GenericCamera:
    camera_template = GenericCamera.from_json(manifest["camera_template"])
    voxel_size_values = manifest.get("large_voxel_size_xyz")
    if not isinstance(voxel_size_values, list) or len(voxel_size_values) != 3:
        raise ValueError("manifest must define large_voxel_size_xyz")
    scene_scale_xyz = _voxel_aspect_scale(
        (
            float(voxel_size_values[0]),
            float(voxel_size_values[1]),
            float(voxel_size_values[2]),
        )
    )
    fit_bounds = manifest.get("camera_fit_bounds_xyz")
    if not isinstance(fit_bounds, dict):
        fit_bounds = manifest["roi"]["bounds_xyz"]
    bounds = fit_bounds
    center = (
        (float(bounds["x"][0]) + float(bounds["x"][1])) * 0.5,
        (float(bounds["y"][0]) + float(bounds["y"][1])) * 0.5,
        (float(bounds["z"][0]) + float(bounds["z"][1]))
        * 0.5
        * float(scene_scale_xyz[2]),
    )
    half_extents = (
        (float(bounds["x"][1]) - float(bounds["x"][0]))
        * 0.5
        * float(scene_scale_xyz[0]),
        (float(bounds["y"][1]) - float(bounds["y"][0]))
        * 0.5
        * float(scene_scale_xyz[1]),
        (float(bounds["z"][1]) - float(bounds["z"][0]))
        * 0.5
        * float(scene_scale_xyz[2]),
    )
    return _build_fit_camera(
        center=center,
        half_extents=half_extents,
        base_camera=camera_template,
        viewport_width=int(viewport_width),
        viewport_height=int(viewport_height),
        margin=float(manifest.get("camera_fit_margin", 1.0)),
        distance_scale=float(manifest.get("camera_distance_scale", 1.0)),
    )


def _validate_axis_aligned_camera(camera: GenericCamera) -> None:
    dx = abs(float(camera.eye[0]) - float(camera.center[0]))
    dy = abs(float(camera.eye[1]) - float(camera.center[1]))
    if dx > 1e-6 or dy > 1e-6:
        raise ValueError(
            "The phantom phase sweep assumes an axis-aligned camera looking along +Z/-Z. "
            f"Got eye-center offsets dx={dx}, dy={dy}."
        )


def _phase_translation_world(
    *,
    camera: GenericCamera,
    reference_depth_scene_z: float,
    viewport_width: int,
    viewport_height: int,
    phase_fraction: float,
    axis: str,
) -> tuple[float, float, float]:
    _validate_axis_aligned_camera(camera)
    forward, right, up = _camera_basis(camera)
    aspect = float(viewport_width) / float(viewport_height)
    ze = abs(float(camera.eye[2]) - float(reference_depth_scene_z))
    pixel_world_y = (
        2.0 * ze * math.tan(math.radians(float(camera.field_of_view_degrees)) / 2.0)
    ) / float(viewport_height)
    pixel_world_x = pixel_world_y * aspect
    if axis == "x":
        return (
            right[0] * pixel_world_x * phase_fraction,
            right[1] * pixel_world_x * phase_fraction,
            right[2] * pixel_world_x * phase_fraction,
        )
    if axis == "y":
        return (
            up[0] * pixel_world_y * phase_fraction,
            up[1] * pixel_world_y * phase_fraction,
            up[2] * pixel_world_y * phase_fraction,
        )
    raise ValueError(f"unsupported phase axis {axis!r}")


def _condition_specs(manifest: dict[str, Any]) -> list[ConditionSpec]:
    roi = manifest["roi"]
    bounds = roi["bounds_xyz"]
    roi_shape = roi["shape_xyz"]
    voxel_size_values = manifest.get("large_voxel_size_xyz")
    if not isinstance(voxel_size_values, list) or len(voxel_size_values) != 3:
        raise ValueError("manifest must define large_voxel_size_xyz")
    scene_scale_xyz = _voxel_aspect_scale(
        (
            float(voxel_size_values[0]),
            float(voxel_size_values[1]),
            float(voxel_size_values[2]),
        )
    )
    local_cut_spans = {
        "x": [float(bounds["x"][0]), float(bounds["x"][1])],
        "y": [float(bounds["y"][0]), float(bounds["y"][1])],
        "z": [float(bounds["z"][0]), float(bounds["z"][1])],
    }
    roi_origin_xyz = (
        float(bounds["x"][0]),
        float(bounds["y"][0]),
        float(bounds["z"][0]) * float(scene_scale_xyz[2]),
    )
    large_shape = manifest["large_shape_xyz"]
    level1_shape = roi["level1"]["shape_czyx"]
    level2_shape = roi["level2"]["shape_czyx"]
    datasets = manifest["datasets"]
    return [
        ConditionSpec(
            name="adaptive",
            dataset_path=Path(str(datasets["adaptive"]["path"])),
            dataset_shape_xyz=(
                int(large_shape[0]),
                int(large_shape[1]),
                int(large_shape[2]),
            ),
            enable_full_resolution=True,
            scale_xyz=None,
            base_translation_xyz=(0.0, 0.0, 0.0),
            local_cut_spans=local_cut_spans,
        ),
        ConditionSpec(
            name="forced_level0",
            dataset_path=Path(str(datasets["forced_level0"]["path"])),
            dataset_shape_xyz=(int(roi_shape[0]), int(roi_shape[1]), int(roi_shape[2])),
            enable_full_resolution=False,
            scale_xyz=(1.0, 1.0, float(scene_scale_xyz[2])),
            base_translation_xyz=roi_origin_xyz,
            local_cut_spans=None,
        ),
        ConditionSpec(
            name="forced_level1",
            dataset_path=Path(str(datasets["forced_level1"]["path"])),
            dataset_shape_xyz=(
                int(level1_shape[3]),
                int(level1_shape[2]),
                int(level1_shape[1]),
            ),
            enable_full_resolution=False,
            scale_xyz=(2.0, 2.0, float(scene_scale_xyz[2])),
            base_translation_xyz=roi_origin_xyz,
            local_cut_spans=None,
        ),
        ConditionSpec(
            name="forced_level2",
            dataset_path=Path(str(datasets["forced_level2"]["path"])),
            dataset_shape_xyz=(
                int(level2_shape[3]),
                int(level2_shape[2]),
                int(level2_shape[1]),
            ),
            enable_full_resolution=False,
            scale_xyz=(4.0, 4.0, float(scene_scale_xyz[2])),
            base_translation_xyz=roi_origin_xyz,
            local_cut_spans=None,
        ),
    ]


def main() -> int:
    args = _parse_args()
    output_root = args.output_root.resolve()
    if output_root.exists() and any(output_root.iterdir()) and not args.overwrite:
        raise FileExistsError(
            f"{output_root} already exists and is not empty. Pass --overwrite to reuse it."
        )
    output_root.mkdir(parents=True, exist_ok=True)

    manifest = _load_json(args.manifest.resolve())
    camera = _build_camera(
        manifest, int(args.viewport_width), int(args.viewport_height)
    )
    phase_sweep = manifest["phase_sweep"]
    phase_axis = str(phase_sweep["axis"])
    phase_count = int(
        args.phase_count if args.phase_count is not None else phase_sweep["phase_count"]
    )
    if phase_count < 1:
        raise ValueError(f"phase_count must be >= 1, got {phase_count}")
    fraction_step = float(phase_sweep["fraction_step"])
    reference_depth_scene_z = float(phase_sweep["reference_depth_scene_z"])
    logger = EventLogger(output_root / "render_events.jsonl")
    client = SceneClient(
        address=args.address, atlas_dir=(args.atlas_dir.strip() or None)
    )
    render_records: list[dict[str, Any]] = []
    phase_records: list[dict[str, Any]] = []
    log_follower: AtlasRenderLogFollower | None = None

    try:
        logger.log(
            "session_start",
            manifest=str(args.manifest.resolve()),
            output_root=str(output_root),
            viewport={
                "width": int(args.viewport_width),
                "height": int(args.viewport_height),
            },
            canvas_logical_size={
                "width": int(args.canvas_logical_width),
                "height": int(args.canvas_logical_height),
            },
            sampling_rate=float(args.sampling_rate),
            camera=camera.to_json(),
            phase_axis=phase_axis,
            phase_count=int(phase_count),
            fraction_step=float(fraction_step),
        )
        if args.clear_existing:
            _clear_existing_scene_objects(client)
        _prepare_scene_once(
            client,
            logger,
            canvas_logical_width=int(args.canvas_logical_width),
            canvas_logical_height=int(args.canvas_logical_height),
            hide_background=bool(args.hide_background),
            hide_axis=bool(args.hide_axis),
        )
        atlas_log_path = _resolve_atlas_log_path(args.atlas_log_path)
        log_follower = AtlasRenderLogFollower(
            atlas_log_path,
            fallback_year=datetime.now(timezone.utc).year,
        )
        logger.log("atlas_log_following", atlas_log_path=str(atlas_log_path))
        mode = "MIP Opaque"
        bootstrap_dataset = Path(str(manifest["datasets"]["forced_level0"]["path"]))
        param_keys, transfer_function_payload = _bootstrap_mode_preset(
            client=client,
            logger=logger,
            bootstrap_dataset=bootstrap_dataset,
            bootstrap_camera=camera,
            mode=mode,
            display_range_min=float(args.display_range_min),
            display_range_max=float(args.display_range_max),
            task_timeout_seconds=float(args.task_timeout_seconds),
            ready_timeout_seconds=float(args.ready_timeout_seconds),
            canvas_logical_width=int(args.canvas_logical_width),
            canvas_logical_height=int(args.canvas_logical_height),
            hide_bound_box=bool(args.hide_bound_box),
        )

        for condition in _condition_specs(manifest):
            logger.log(
                "condition_start",
                condition=condition.name,
                dataset=str(condition.dataset_path),
            )
            ids = _load_dataset(
                client,
                condition.dataset_path,
                task_timeout_seconds=float(args.task_timeout_seconds),
                ready_timeout_seconds=float(args.ready_timeout_seconds),
            )
            if len(ids) != 1:
                raise RuntimeError(
                    f"Expected exactly one image object for {condition.dataset_path}, got {ids}"
                )
            obj_id = int(ids[0])
            param_keys = _discover_param_keys(client, obj_id)

            for phase_index in range(phase_count):
                phase_fraction = float(phase_index) * float(fraction_step)
                phase_start = logger.log(
                    "phase_request",
                    condition=condition.name,
                    phase_index=int(phase_index),
                    phase_fraction=float(phase_fraction),
                )
                phase_translation_xyz = _phase_translation_world(
                    camera=camera,
                    reference_depth_scene_z=reference_depth_scene_z,
                    viewport_width=int(args.viewport_width),
                    viewport_height=int(args.viewport_height),
                    phase_fraction=phase_fraction,
                    axis=phase_axis,
                )
                translation_xyz = (
                    float(condition.base_translation_xyz[0])
                    + float(phase_translation_xyz[0]),
                    float(condition.base_translation_xyz[1])
                    + float(phase_translation_xyz[1]),
                    float(condition.base_translation_xyz[2])
                    + float(phase_translation_xyz[2]),
                )
                coord_transform_payload = None
                if condition.scale_xyz is not None:
                    coord_transform_payload = _coord_transform_payload(
                        shape_xyz=condition.dataset_shape_xyz,
                        scale_xyz=condition.scale_xyz,
                        translation_xyz=translation_xyz,
                    )
                _configure_loaded_dataset(
                    client,
                    obj_id=obj_id,
                    param_keys=param_keys,
                    compositing_mode=mode,
                    sampling_rate=float(args.sampling_rate),
                    display_range_min=float(args.display_range_min),
                    display_range_max=float(args.display_range_max),
                    camera=camera,
                    transfer_function_payload=transfer_function_payload,
                    hide_bound_box=bool(args.hide_bound_box),
                    enable_full_resolution=bool(condition.enable_full_resolution),
                    coord_transform_payload=coord_transform_payload,
                    local_cut_spans=condition.local_cut_spans,
                )
                final_marker = log_follower.wait_for_final_marker(
                    start_ns=int(phase_start["wall_time_ns"]),
                    timeout_seconds=float(args.final_timeout_seconds),
                )
                logger.log(
                    "phase_final_marker",
                    condition=condition.name,
                    phase_index=int(phase_index),
                    phase_fraction=float(phase_fraction),
                    marker_wall_time_ns=int(final_marker["wall_time_ns"]),
                    client_ms=max(
                        0.0,
                        (
                            int(final_marker["wall_time_ns"])
                            - int(phase_start["wall_time_ns"])
                        )
                        / 1_000_000.0,
                    ),
                    engine_elapsed_ms=final_marker.get("elapsed_ms"),
                    progress=final_marker.get("progress"),
                    source=final_marker.get("source"),
                    line=final_marker.get("line"),
                )
                screenshot_path = (
                    output_root
                    / "renders"
                    / condition.name
                    / f"phase_{phase_index:02d}.png"
                )
                _screenshot(
                    client,
                    width=int(args.viewport_width),
                    height=int(args.viewport_height),
                    screenshot_path=screenshot_path,
                )
                record = {
                    "condition": condition.name,
                    "object_id": obj_id,
                    "dataset_path": str(condition.dataset_path),
                    "phase_index": int(phase_index),
                    "phase_fraction": float(phase_fraction),
                    "phase_axis": phase_axis,
                    "phase_translation_xyz": [
                        float(phase_translation_xyz[0]),
                        float(phase_translation_xyz[1]),
                        float(phase_translation_xyz[2]),
                    ],
                    "base_translation_xyz": [
                        float(condition.base_translation_xyz[0]),
                        float(condition.base_translation_xyz[1]),
                        float(condition.base_translation_xyz[2]),
                    ],
                    "translation_xyz": [
                        float(translation_xyz[0]),
                        float(translation_xyz[1]),
                        float(translation_xyz[2]),
                    ],
                    "screenshot_path": str(screenshot_path),
                    "enable_full_resolution": bool(condition.enable_full_resolution),
                    "scale_xyz": (
                        None
                        if condition.scale_xyz is None
                        else [
                            float(condition.scale_xyz[0]),
                            float(condition.scale_xyz[1]),
                            float(condition.scale_xyz[2]),
                        ]
                    ),
                    "camera": camera.to_json(),
                }
                phase_records.append(record)
                logger.log("phase_render_complete", **record)

            render_records.append(
                {
                    "condition": condition.name,
                    "dataset_path": str(condition.dataset_path),
                    "object_id": obj_id,
                }
            )
            _remove_objects_if_needed(client, ids)
            logger.log("condition_complete", condition=condition.name)

        manifest_out = {
            "source_manifest": str(args.manifest.resolve()),
            "camera": camera.to_json(),
            "mode": mode,
            "sampling_rate": float(args.sampling_rate),
            "display_range": [
                float(args.display_range_min),
                float(args.display_range_max),
            ],
            "phase_axis": phase_axis,
            "phase_count": int(phase_count),
            "fraction_step": float(fraction_step),
            "reference_depth_scene_z": float(reference_depth_scene_z),
            "conditions": render_records,
            "phase_records": phase_records,
            "features": manifest.get("features", []),
            "roi": manifest["roi"],
        }
        (output_root / "manifest.json").write_text(
            json.dumps(manifest_out, indent=2) + "\n", encoding="utf-8"
        )
        logger.log("session_complete", manifest=str(output_root / "manifest.json"))
        return 0
    finally:
        logger.close()


if __name__ == "__main__":
    raise SystemExit(main())
