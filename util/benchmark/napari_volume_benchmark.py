from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT / "python" / "atlas_agent" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "python" / "atlas_agent" / "src"))

import napari
import numpy as np
from napari.qt import get_qapp
from napari.utils import perf
from qtpy import QtCore
from qtpy.QtCore import QCoreApplication
from qtpy.QtWidgets import QApplication

from process_memory_sampler import ProcessMemorySampler
from volume_benchmark_common import (
    EventLogger,
    GenericCamera,
    interpolate_action_cameras,
    load_benchmark_spec,
    sleep_until,
)


RENDERING_CHOICES = (
    "mip",
    "translucent",
    "attenuated_mip",
    "average",
    "additive",
    "iso",
)

DATASET_LOADER_CHOICES = (
    "zimg-array",
    "tifffile-array",
    "napari-open",
)

NAPARI_BENCHMARK_ORIENTATION = ("towards", "down", "left")


def _parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description=(
            "Run one deterministic napari volume benchmark session and persist "
            "timing events, screenshots, and optional RSS samples."
        )
    )
    parser.add_argument(
        "--dataset",
        required=True,
        help=(
            "Path to the source dataset. Use .nim with --dataset-loader zimg-array, "
            ".tif/.tiff with --dataset-loader tifffile-array, or any "
            "napari-reader-compatible on-disk file with --dataset-loader napari-open."
        ),
    )
    parser.add_argument(
        "--dataset-loader",
        choices=DATASET_LOADER_CHOICES,
        default="zimg-array",
        help=(
            "Dataset ingestion path. zimg-array eagerly reads a .nim payload through zimg; "
            "tifffile-array eagerly reads a TIFF stack from disk through tifffile; "
            "napari-open measures napari's own disk-backed viewer.open(...) reader path."
        ),
    )
    parser.add_argument(
        "--open-plugin",
        default=None,
        help=(
            "Optional explicit napari reader plugin name used with --dataset-loader napari-open. "
            "Use this to pin the built-in reader, e.g. --open-plugin napari."
        ),
    )
    parser.add_argument(
        "--scene-scale-zyx",
        type=float,
        nargs=3,
        metavar=("Z", "Y", "X"),
        default=None,
        help=(
            "Optional scene scale override applied to the opened napari layer. "
            "Required when the file format does not preserve the Atlas benchmark scale."
        ),
    )
    parser.add_argument(
        "--camera-spec",
        required=True,
        help="Shared benchmark camera spec JSON",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory for screenshots, event logs, and summaries",
    )
    parser.add_argument(
        "--rendering",
        choices=RENDERING_CHOICES,
        default="mip",
        help="napari 3D image rendering mode",
    )
    parser.add_argument(
        "--channel-index",
        type=int,
        default=0,
        help="Channel index inside the .nim payload",
    )
    parser.add_argument(
        "--time-index",
        type=int,
        default=0,
        help="Time index inside the .nim payload",
    )
    parser.add_argument(
        "--contrast-limits-min",
        type=float,
        default=0.0,
        help="Lower contrast limit forwarded to napari",
    )
    parser.add_argument(
        "--contrast-limits-max",
        type=float,
        default=255.0,
        help="Upper contrast limit forwarded to napari",
    )
    parser.add_argument(
        "--sample-rss",
        action="store_true",
        help="Sample the benchmark process RSS while the session runs",
    )
    parser.add_argument(
        "--rss-interval-seconds",
        type=float,
        default=0.1,
        help="RSS sampling interval when --sample-rss is enabled",
    )
    parser.add_argument(
        "--process-events-count",
        type=int,
        default=6,
        help="Extra Qt processEvents passes before each forced screenshot flush",
    )
    parser.add_argument(
        "--draw-sync-timeout-seconds",
        type=float,
        default=5.0,
        help=(
            "Maximum time to wait for napari frame presentation to settle after a camera "
            "update before failing the benchmark step"
        ),
    )
    parser.add_argument(
        "--frame-settle-quiet-seconds",
        type=float,
        default=0.05,
        help=(
            "How long frameSwapped must stay quiet after the last observed swap "
            "before the step counts as render-settled"
        ),
    )
    parser.add_argument(
        "--screenshot-reference-every-step",
        action="store_true",
        help=(
            "Force a canvas-only napari screenshot on every step and record the "
            "installed napari screenshot timing breakdown for each one. "
            "Intermediate steps still avoid file save by keeping path=None. "
            "This mode is intrusive and intended only as a screenshot-synchronized "
            "reference metric."
        ),
    )
    parser.add_argument(
        "--save-screenshot-reference-images",
        action="store_true",
        help=(
            "When --screenshot-reference-every-step is enabled, also save the "
            "intermediate step screenshots to disk for visual inspection."
        ),
    )
    parser.add_argument(
        "--window-title",
        default=None,
        help=(
            "Optional top-level napari window title. Useful when another tool "
            "needs to match the deterministic benchmark window for capture."
        ),
    )
    parser.add_argument(
        "--window-x",
        type=int,
        default=None,
        help="Optional top-level napari window X position in points.",
    )
    parser.add_argument(
        "--window-y",
        type=int,
        default=None,
        help="Optional top-level napari window Y position in points.",
    )
    parser.add_argument(
        "--window-width",
        type=int,
        default=None,
        help="Optional top-level napari window width in points.",
    )
    parser.add_argument(
        "--window-height",
        type=int,
        default=None,
        help="Optional top-level napari window height in points.",
    )
    return parser.parse_args()


def _atlas_xyz_to_napari_zyx(
    vec: tuple[float, float, float],
) -> tuple[float, float, float]:
    return (float(vec[2]), float(vec[1]), float(vec[0]))


def _atlas_xyz_direction_to_napari_zyx_camera(
    vec: tuple[float, float, float],
) -> tuple[float, float, float]:
    converted = _atlas_xyz_to_napari_zyx(vec)
    # Napari's 3D camera basis under the retained benchmark orientation
    # mirrors the horizontal screen axis relative to the shared Atlas/ParaView
    # camera convention. Keep the scene center in the same world basis, but
    # flip only the horizontal component of the direction vectors we feed into
    # set_view_direction(...) so the oblique rotate/zoom endpoints line up.
    return (converted[0], converted[1], -converted[2])


def _normalize(vec: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2])
    if length <= 1e-12:
        raise ValueError("camera direction vector must be non-zero")
    return (vec[0] / length, vec[1] / length, vec[2] / length)


def _camera_distance(camera: GenericCamera) -> float:
    dx = float(camera.eye[0] - camera.center[0])
    dy = float(camera.eye[1] - camera.center[1])
    dz = float(camera.eye[2] - camera.center[2])
    return math.sqrt(dx * dx + dy * dy + dz * dz)


def _napari_zoom_for_camera(
    camera: GenericCamera,
    *,
    logical_viewport_height: float,
) -> float:
    if camera.projection != "Perspective":
        raise ValueError(
            f"unsupported napari benchmark projection {camera.projection!r}"
        )
    distance = max(_camera_distance(camera), 1e-6)
    fov_radians = math.radians(float(camera.field_of_view_degrees))
    fy = 2.0 * distance * math.tan(fov_radians / 2.0)
    return float(logical_viewport_height) / max(fy, 1e-6)


def _apply_camera(
    viewer: napari.Viewer,
    camera: GenericCamera,
    *,
    logical_viewport_height: float,
) -> dict[str, Any]:
    view_direction_xyz = _normalize(
        (
            float(camera.center[0] - camera.eye[0]),
            float(camera.center[1] - camera.eye[1]),
            float(camera.center[2] - camera.eye[2]),
        )
    )
    up_xyz = _normalize(camera.up)
    center_zyx = _atlas_xyz_to_napari_zyx(camera.center)
    view_direction_zyx = _atlas_xyz_direction_to_napari_zyx_camera(view_direction_xyz)
    up_direction_zyx = _atlas_xyz_direction_to_napari_zyx_camera(up_xyz)
    zoom = _napari_zoom_for_camera(
        camera,
        logical_viewport_height=logical_viewport_height,
    )

    # Match the Atlas/ParaView handedness. Napari's default 3D horizontal
    # orientation mirrors the shared benchmark cameras left-to-right.
    viewer.camera.orientation = NAPARI_BENCHMARK_ORIENTATION
    viewer.camera.center = center_zyx
    viewer.camera.perspective = float(camera.field_of_view_degrees)
    viewer.camera.set_view_direction(
        view_direction=view_direction_zyx,
        up_direction=up_direction_zyx,
    )
    viewer.camera.zoom = zoom
    return {
        "center_zyx": list(center_zyx),
        "view_direction_zyx": list(view_direction_zyx),
        "up_direction_zyx": list(up_direction_zyx),
        "zoom": zoom,
        "perspective": float(camera.field_of_view_degrees),
        "eye_center_distance": _camera_distance(camera),
        "logical_viewport_height": float(logical_viewport_height),
        "orientation": list(NAPARI_BENCHMARK_ORIENTATION),
    }


def _load_nim_volume(
    path: str | Path,
    *,
    channel_index: int,
    time_index: int,
) -> tuple[np.ndarray, dict[str, Any]]:
    import zimg

    dataset_path = Path(path)
    infos = zimg.ZImg.readImgInfos(str(dataset_path))
    if not infos:
        raise RuntimeError(f"zimg returned no image infos for {dataset_path}")
    info = infos[0]

    img = zimg.ZImg(str(dataset_path))
    arrays = img.to_arrays()
    if time_index < 0 or time_index >= len(arrays):
        raise IndexError(
            f"time_index {time_index} is out of range for {dataset_path} "
            f"(time_count={len(arrays)})"
        )
    array_tczyx = np.asarray(arrays[time_index])
    if array_tczyx.ndim != 4:
        raise RuntimeError(
            f"expected zimg array shape [C,Z,Y,X], got {array_tczyx.shape} for {dataset_path}"
        )
    if channel_index < 0 or channel_index >= int(array_tczyx.shape[0]):
        raise IndexError(
            f"channel_index {channel_index} is out of range for {dataset_path} "
            f"(channel_count={array_tczyx.shape[0]})"
        )
    volume_zyx = np.asarray(array_tczyx[channel_index])
    if volume_zyx.ndim != 3:
        raise RuntimeError(
            f"expected selected volume to be 3D, got {volume_zyx.shape} for {dataset_path}"
        )

    voxel_size_x = float(info.voxelSizeX)
    voxel_size_y = float(info.voxelSizeY)
    voxel_size_z = float(info.voxelSizeZ)
    if voxel_size_x <= 0.0 or voxel_size_y <= 0.0 or voxel_size_z <= 0.0:
        raise RuntimeError(
            f"dataset {dataset_path} has invalid voxel sizes "
            f"({voxel_size_x}, {voxel_size_y}, {voxel_size_z})"
        )

    # Atlas benchmark cameras are authored in Atlas scene space, where X/Y are
    # normalized by voxelSizeX and Z is scaled by the anisotropy ratio.
    scene_scale_zyx = (
        voxel_size_z / voxel_size_x,
        voxel_size_y / voxel_size_x,
        1.0,
    )
    metadata = {
        "dataset_path": str(dataset_path),
        "dataset_loader": "zimg-array",
        "shape_zyx": [
            int(volume_zyx.shape[0]),
            int(volume_zyx.shape[1]),
            int(volume_zyx.shape[2]),
        ],
        "dtype": str(volume_zyx.dtype),
        "channel_count": int(array_tczyx.shape[0]),
        "time_count": len(arrays),
        "channel_index": int(channel_index),
        "time_index": int(time_index),
        "voxel_size_xyz_um": [voxel_size_x, voxel_size_y, voxel_size_z],
        "scene_scale_zyx": list(scene_scale_zyx),
    }
    return volume_zyx, metadata


def _load_tiff_volume(
    path: str | Path,
    *,
    scene_scale_zyx: tuple[float, float, float] | None,
) -> tuple[np.ndarray, dict[str, Any]]:
    import tifffile

    dataset_path = Path(path)
    volume_zyx = np.asarray(tifffile.imread(str(dataset_path)))
    if volume_zyx.ndim != 3:
        raise RuntimeError(
            f"expected TIFF volume to be 3D ZYX, got {volume_zyx.shape} for {dataset_path}"
        )
    resolved_scene_scale_zyx = (
        tuple(float(x) for x in scene_scale_zyx)
        if scene_scale_zyx is not None
        else (1.0, 1.0, 1.0)
    )
    metadata = {
        "dataset_path": str(dataset_path),
        "dataset_loader": "tifffile-array",
        "shape_zyx": [
            int(volume_zyx.shape[0]),
            int(volume_zyx.shape[1]),
            int(volume_zyx.shape[2]),
        ],
        "dtype": str(volume_zyx.dtype),
        "channel_count": 1,
        "time_count": 1,
        "channel_index": 0,
        "time_index": 0,
        "voxel_size_xyz_um": None,
        "scene_scale_zyx": list(resolved_scene_scale_zyx),
    }
    return volume_zyx, metadata


def _resolve_layer_shape_zyx(layer: Any) -> list[int]:
    data = getattr(layer, "data", None)
    shape = getattr(data, "shape", None)
    if shape is None:
        raise RuntimeError(
            f"napari-open layer {getattr(layer, 'name', '<unnamed>')} does not expose a shape"
        )
    if len(shape) != 3:
        raise RuntimeError(f"expected a 3D layer from napari-open, got shape {shape}")
    return [int(shape[0]), int(shape[1]), int(shape[2])]


def _resolve_layer_dtype(layer: Any) -> str:
    data = getattr(layer, "data", None)
    dtype = getattr(data, "dtype", None)
    return str(dtype) if dtype is not None else type(data).__name__


def _open_dataset_with_napari(
    viewer: napari.Viewer,
    path: str | Path,
    *,
    plugin: str | None,
    rendering: str,
    contrast_limits: tuple[float, float],
    scene_scale_zyx: tuple[float, float, float] | None,
) -> tuple[Any, dict[str, Any], float]:
    dataset_path = Path(path)
    load_start_ns = time.monotonic_ns()
    opened_layers = viewer.open(
        str(dataset_path),
        plugin=plugin,
    )
    load_elapsed_ms = (time.monotonic_ns() - load_start_ns) / 1_000_000.0
    if len(viewer.layers) != 1 or len(opened_layers) != 1:
        raise RuntimeError(
            f"napari-open expected exactly one layer for {dataset_path}, "
            f"got viewer.layers={len(viewer.layers)} opened_layers={len(opened_layers)}"
        )
    layer = viewer.layers[0]
    layer.rendering = rendering
    layer.depiction = "volume"
    layer.contrast_limits = contrast_limits
    if scene_scale_zyx is not None:
        layer.scale = scene_scale_zyx
    metadata = {
        "dataset_path": str(dataset_path),
        "dataset_loader": "napari-open",
        "shape_zyx": _resolve_layer_shape_zyx(layer),
        "dtype": _resolve_layer_dtype(layer),
        "channel_count": 1,
        "time_count": 1,
        "channel_index": 0,
        "time_index": 0,
        "voxel_size_xyz_um": None,
        "scene_scale_zyx": [float(x) for x in layer.scale],
        "open_plugin": plugin,
    }
    return layer, metadata, load_elapsed_ms


def _process_events(app: QApplication, count: int) -> None:
    for _ in range(max(0, int(count))):
        app.processEvents()


class _FrameSwapTracker:
    def __init__(self, viewer: napari.Viewer) -> None:
        self._native_canvas = viewer.window._qt_viewer.canvas.native
        self.frame_swap_count = 0
        self.last_frame_swap_monotonic_ns: int | None = None
        self.frame_swap_timestamps_ns: list[int] = []
        self._native_canvas.frameSwapped.connect(self._on_frame_swapped)

    def _on_frame_swapped(self) -> None:
        timestamp_ns = time.monotonic_ns()
        self.frame_swap_count += 1
        self.last_frame_swap_monotonic_ns = timestamp_ns
        self.frame_swap_timestamps_ns.append(timestamp_ns)

    def wait_for_frame_settle(
        self,
        app: QApplication,
        *,
        baseline_frame_swap_count: int,
        timeout_seconds: float,
        quiet_seconds: float,
    ) -> tuple[float, float, float, int]:
        deadline_ns = time.monotonic_ns() + int(float(timeout_seconds) * 1e9)
        quiet_ns = int(float(quiet_seconds) * 1e9)
        self._native_canvas.update()
        observed_swap = False
        while time.monotonic_ns() < deadline_ns:
            app.processEvents()
            QCoreApplication.sendPostedEvents(None, 0)
            if self.frame_swap_count > baseline_frame_swap_count:
                observed_swap = True
                if self.last_frame_swap_monotonic_ns is None:
                    raise RuntimeError(
                        "napari frame swap tracker observed a swap without a timestamp"
                    )
                if time.monotonic_ns() - self.last_frame_swap_monotonic_ns >= quiet_ns:
                    if len(self.frame_swap_timestamps_ns) < self.frame_swap_count:
                        raise RuntimeError(
                            "napari frame swap tracker count exceeded recorded timestamps"
                        )
                    first_index = int(baseline_frame_swap_count)
                    if first_index >= len(self.frame_swap_timestamps_ns):
                        raise RuntimeError(
                            "napari frame swap tracker settled without recording a first swap timestamp"
                        )
                    return (
                        float(self.frame_swap_timestamps_ns[first_index]),
                        float(self.last_frame_swap_monotonic_ns),
                        float(time.monotonic_ns()),
                        int(self.frame_swap_count - baseline_frame_swap_count),
                    )
            time.sleep(0.001)
        if observed_swap and self.last_frame_swap_monotonic_ns is not None:
            raise TimeoutError(
                "timed out waiting for napari frameSwapped activity to settle after camera update"
            )
        raise TimeoutError(
            "timed out waiting for napari to emit a frameSwapped signal after camera update"
        )

    def wait_for_quiet(
        self,
        app: QApplication,
        *,
        timeout_seconds: float,
        quiet_seconds: float,
    ) -> int:
        deadline_ns = time.monotonic_ns() + int(float(timeout_seconds) * 1e9)
        quiet_ns = int(float(quiet_seconds) * 1e9)
        last_observed_count = int(self.frame_swap_count)
        last_activity_ns = (
            int(self.last_frame_swap_monotonic_ns)
            if self.last_frame_swap_monotonic_ns is not None
            else time.monotonic_ns()
        )
        while time.monotonic_ns() < deadline_ns:
            app.processEvents()
            QCoreApplication.sendPostedEvents(None, 0)
            if self.frame_swap_count != last_observed_count:
                last_observed_count = int(self.frame_swap_count)
                last_activity_ns = (
                    int(self.last_frame_swap_monotonic_ns)
                    if self.last_frame_swap_monotonic_ns is not None
                    else time.monotonic_ns()
                )
            elif time.monotonic_ns() - last_activity_ns >= quiet_ns:
                return last_observed_count
            time.sleep(0.001)
        raise TimeoutError(
            "timed out waiting for existing napari frameSwapped activity to go quiet"
        )


def _capture_canvas_screenshot(
    viewer: napari.Viewer,
    *,
    screenshot_path: Path | None,
    width: int,
    height: int,
) -> dict[str, Any]:
    image = viewer.screenshot(
        path=str(screenshot_path) if screenshot_path is not None else None,
        size=None,
        scale=1.0,
        canvas_only=True,
        flash=False,
    )
    image_shape = [int(x) for x in image.shape]
    if len(image_shape) < 2:
        raise RuntimeError(
            f"napari screenshot returned an unexpected array shape {image_shape}"
        )
    actual_height = int(image_shape[0])
    actual_width = int(image_shape[1])
    if actual_height != int(height) or actual_width != int(width):
        raise RuntimeError(
            "napari screenshot size mismatch: "
            f"requested {int(width)}x{int(height)}, "
            f"got {actual_width}x{actual_height}"
        )
    canvas = viewer.window._qt_viewer.canvas
    native_canvas = canvas.native
    return {
        "screenshot_array_shape": image_shape,
        "screenshot_array_dtype": str(image.dtype),
        "screenshot_array_height": actual_height,
        "screenshot_array_width": actual_width,
        "capture_canvas_size": [int(x) for x in canvas.size],
        "capture_native_canvas_size": [
            int(native_canvas.size().width()),
            int(native_canvas.size().height()),
        ],
        "capture_native_canvas_device_pixel_ratio": float(
            native_canvas.devicePixelRatioF()
        ),
    }


def _perf_instant(name: str, **kwargs: float | int | str) -> None:
    perf.timers.add_instant_event(name, category="benchmark", **kwargs)


def _extract_screenshot_breakdown(viewer: napari.Viewer) -> dict[str, Any] | None:
    timing = getattr(viewer.window, "_last_screenshot_timing", None)
    if timing is None:
        return None

    qt_viewer_timing = timing.get("qt_viewer") or {}
    canvas_timing = qt_viewer_timing.get("canvas") or {}
    qimage_stage_ms = timing.get("qimage_stage_ms")
    qimg2array_ms = timing.get("qimg2array_ms")
    return {
        "screenshot_call_start_monotonic_ns": timing.get("call_start_monotonic_ns"),
        "screenshot_call_end_monotonic_ns": timing.get("call_end_monotonic_ns"),
        "screenshot_qimage_start_monotonic_ns": timing.get("qimage_start_monotonic_ns"),
        "screenshot_qimage_end_monotonic_ns": timing.get("qimage_end_monotonic_ns"),
        "screenshot_timing_total_ms": timing.get("total_ms"),
        "screenshot_qimage_stage_ms": qimage_stage_ms,
        "screenshot_qimg2array_ms": qimg2array_ms,
        "screenshot_imsave_ms": timing.get("imsave_ms"),
        "screenshot_qt_viewer_total_ms": qt_viewer_timing.get("total_ms"),
        "screenshot_qt_viewer_layer_slicer_wait_ms": qt_viewer_timing.get(
            "layer_slicer_wait_ms"
        ),
        "screenshot_qt_viewer_canvas_context_ms": qt_viewer_timing.get(
            "canvas_context_ms"
        ),
        "screenshot_canvas_total_ms": canvas_timing.get("total_ms"),
        "screenshot_canvas_draw_ms": canvas_timing.get("draw_ms"),
        "screenshot_grab_framebuffer_ms": canvas_timing.get("grab_framebuffer_ms"),
        # Upper-bound capture metric: includes the forced screenshot draw and
        # the GPU readback barrier, but excludes Python array conversion and
        # disk save.
        "screenshot_capture_upper_bound_ms": qimage_stage_ms,
        # Slightly wider upper bound that keeps the QImage->ndarray conversion
        # the benchmark currently pays for in the driver.
        "screenshot_capture_upper_bound_plus_array_ms": (
            None
            if qimage_stage_ms is None or qimg2array_ms is None
            else float(qimage_stage_ms) + float(qimg2array_ms)
        ),
    }


def _configure_live_render_surface(
    viewer: napari.Viewer,
    *,
    physical_width: int,
    physical_height: int,
    process_events_count: int,
) -> dict[str, Any]:
    qt_viewer = viewer.window._qt_viewer
    canvas = qt_viewer.canvas
    scene_canvas = canvas._scene_canvas
    native_canvas = canvas.native
    device_pixel_ratio = float(native_canvas.devicePixelRatioF())
    logical_width = int(round(float(physical_width) / max(device_pixel_ratio, 1e-6)))
    logical_height = int(round(float(physical_height) / max(device_pixel_ratio, 1e-6)))

    qt_viewer.resize(logical_width, logical_height)
    _process_events(get_qapp(), max(1, int(process_events_count)))
    # napari's canvas.size expects the screenshot-style (height, width) order.
    canvas.size = (logical_height, logical_width)
    _process_events(get_qapp(), max(1, int(process_events_count)))

    scene_size = tuple(int(x) for x in scene_canvas.size)
    scene_physical_size = tuple(int(x) for x in scene_canvas.physical_size)
    if scene_physical_size != (int(physical_width), int(physical_height)):
        raise RuntimeError(
            "napari live render surface size mismatch: "
            f"requested physical {int(physical_width)}x{int(physical_height)}, "
            f"got {scene_physical_size[0]}x{scene_physical_size[1]}"
        )

    return {
        "target_physical_size": [int(physical_width), int(physical_height)],
        "target_logical_size": [int(logical_width), int(logical_height)],
        "scene_size": [scene_size[0], scene_size[1]],
        "scene_physical_size": [scene_physical_size[0], scene_physical_size[1]],
        "device_pixel_ratio": device_pixel_ratio,
        "qt_viewer_size": [
            int(qt_viewer.size().width()),
            int(qt_viewer.size().height()),
        ],
    }


def _configure_window_layout(
    viewer: napari.Viewer,
    *,
    x: int | None,
    y: int | None,
    width: int | None,
    height: int | None,
    title: str | None,
) -> None:
    qt_window = viewer.window._qt_window
    if title is not None:
        viewer.title = str(title)
        qt_window.setWindowTitle(str(title))
    geometry = qt_window.geometry()
    target_x = int(x) if x is not None else int(geometry.x())
    target_y = int(y) if y is not None else int(geometry.y())
    target_width = int(width) if width is not None else int(geometry.width())
    target_height = int(height) if height is not None else int(geometry.height())
    qt_window.setGeometry(target_x, target_y, target_width, target_height)


def _region_payload(
    *, x: float, y: float, width: float, height: float
) -> dict[str, float]:
    return {
        "x": float(x),
        "y": float(y),
        "width": float(width),
        "height": float(height),
    }


def _widget_geometry_payload(
    widget: Any,
    *,
    top_level_window: Any,
) -> dict[str, Any]:
    local_top_left = QtCore.QPoint(0, 0)
    global_top_left = widget.mapToGlobal(local_top_left)
    frame_geometry = top_level_window.frameGeometry()
    geometry = widget.geometry()
    absolute_points = _region_payload(
        x=float(global_top_left.x()),
        y=float(global_top_left.y()),
        width=float(geometry.width()),
        height=float(geometry.height()),
    )
    window_relative_points = _region_payload(
        x=float(global_top_left.x() - frame_geometry.x()),
        y=float(global_top_left.y() - frame_geometry.y()),
        width=float(geometry.width()),
        height=float(geometry.height()),
    )
    dpr = float(widget.devicePixelRatioF())
    absolute_pixels = _region_payload(
        x=float(round(absolute_points["x"] * dpr)),
        y=float(round(absolute_points["y"] * dpr)),
        width=float(round(absolute_points["width"] * dpr)),
        height=float(round(absolute_points["height"] * dpr)),
    )
    window_relative_pixels = _region_payload(
        x=float(round(window_relative_points["x"] * dpr)),
        y=float(round(window_relative_points["y"] * dpr)),
        width=float(round(window_relative_points["width"] * dpr)),
        height=float(round(window_relative_points["height"] * dpr)),
    )
    return {
        "class_name": type(widget).__name__,
        "device_pixel_ratio": dpr,
        "absolute_points": absolute_points,
        "window_relative_points": window_relative_points,
        "absolute_pixels": absolute_pixels,
        "window_relative_pixels": window_relative_pixels,
    }


def _write_gui_geometry_and_calibration(
    *,
    output_dir: Path,
    viewer: napari.Viewer,
    window_title: str | None,
) -> tuple[Path, Path]:
    qt_window = viewer.window._qt_window
    qt_viewer = viewer.window._qt_viewer
    canvas_native = qt_viewer.canvas.native
    frame_geometry = qt_window.frameGeometry()
    qt_window_geometry = qt_window.geometry()
    canvas_geometry = _widget_geometry_payload(
        canvas_native,
        top_level_window=qt_window,
    )
    qt_viewer_geometry = _widget_geometry_payload(
        qt_viewer,
        top_level_window=qt_window,
    )
    geometry_payload = {
        "window_title": str(window_title or qt_window.windowTitle()),
        "window_owner_name_guess": Path(sys.executable).name,
        "qt_window": {
            "geometry": _region_payload(
                x=float(qt_window_geometry.x()),
                y=float(qt_window_geometry.y()),
                width=float(qt_window_geometry.width()),
                height=float(qt_window_geometry.height()),
            ),
            "frame_geometry": _region_payload(
                x=float(frame_geometry.x()),
                y=float(frame_geometry.y()),
                width=float(frame_geometry.width()),
                height=float(frame_geometry.height()),
            ),
        },
        "qt_viewer": qt_viewer_geometry,
        "canvas_native": canvas_geometry,
    }
    calibration_payload = {
        "app": "napari",
        "bundle_identifier": "",
        "window_owner_name": Path(sys.executable).name,
        "window_name_substring": str(window_title or qt_window.windowTitle()),
        "region_coordinate_space": "window-relative",
        "activate_app": True,
        "capture_region": canvas_geometry["window_relative_points"],
        "analysis_region_norm": {
            "x": 0.0,
            "y": 0.0,
            "width": 1.0,
            "height": 1.0,
        },
        "input_region": canvas_geometry["window_relative_points"],
    }
    geometry_path = output_dir / "napari_gui_geometry.json"
    calibration_path = output_dir / "napari_gui_calibration.json"
    geometry_path.write_text(
        json.dumps(geometry_payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    calibration_path.write_text(
        json.dumps(calibration_payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return geometry_path, calibration_path


def main() -> int:
    args = _parse_args()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    screenshots_dir = output_dir / "screenshots"
    screenshots_dir.mkdir(parents=True, exist_ok=True)

    logger = EventLogger(output_dir / "napari_events.jsonl")
    rss_sampler: ProcessMemorySampler | None = None
    summary_path = output_dir / "napari_timer_summary.json"
    memory_summary_path = output_dir / "napari_memory_summary.json"

    summary: dict[str, Any] = {
        "dataset": str(Path(args.dataset).resolve()),
        "camera_spec": str(Path(args.camera_spec).resolve()),
        "dataset_loader": args.dataset_loader,
        "open_plugin": args.open_plugin,
        "scene_scale_zyx_override": (
            [float(x) for x in args.scene_scale_zyx]
            if args.scene_scale_zyx is not None
            else None
        ),
        "rendering": args.rendering,
        "screenshot_reference_every_step": bool(args.screenshot_reference_every_step),
        "contrast_limits": [
            float(args.contrast_limits_min),
            float(args.contrast_limits_max),
        ],
        "output_dir": str(output_dir),
        "open": {},
        "actions": [],
    }

    session_start = logger.log(
        "session_start",
        app="napari",
        dataset=str(Path(args.dataset).resolve()),
        camera_spec=str(Path(args.camera_spec).resolve()),
        rendering=args.rendering,
    )
    _perf_instant(
        "benchmark.session_start",
        dataset=str(Path(args.dataset).resolve()),
        rendering=str(args.rendering),
    )

    try:
        if args.sample_rss:
            rss_sampler = ProcessMemorySampler(
                pid=os.getpid(),
                interval_seconds=float(args.rss_interval_seconds),
                output_path=output_dir / "napari_rss_samples.jsonl",
            )
            rss_sampler.start()

        spec = load_benchmark_spec(args.camera_spec)
        frame_settle_quiet_ms = float(args.frame_settle_quiet_seconds) * 1_000.0
        summary["viewport"] = {
            "width": spec.viewport_width,
            "height": spec.viewport_height,
        }

        app = get_qapp()

        load_start = logger.log("dataset_load_start")
        _perf_instant("benchmark.dataset_load_start")
        # A hidden napari viewer keeps a stale small canvas on this machine,
        # which causes screenshot(size=...) to render only into the old
        # lower-left subregion. Keep the viewer shown so the Qt/VisPy canvas
        # adopts the requested size before capture.
        viewer = napari.Viewer(show=True)
        if any(
            value is not None
            for value in (
                args.window_title,
                args.window_x,
                args.window_y,
                args.window_width,
                args.window_height,
            )
        ):
            _configure_window_layout(
                viewer,
                x=args.window_x,
                y=args.window_y,
                width=args.window_width,
                height=args.window_height,
                title=args.window_title,
            )
        else:
            viewer.window.resize(int(spec.viewport_width), int(spec.viewport_height))
        _process_events(app, max(12, int(args.process_events_count)))
        frame_swap_tracker = _FrameSwapTracker(viewer)

        add_layer_start_ns = time.monotonic_ns()
        if args.dataset_loader == "zimg-array":
            load_monotonic_ns = time.monotonic_ns()
            volume_zyx, dataset_metadata = _load_nim_volume(
                args.dataset,
                channel_index=int(args.channel_index),
                time_index=int(args.time_index),
            )
            load_complete = logger.log(
                "dataset_load_complete",
                elapsed_ms=(time.monotonic_ns() - load_monotonic_ns) / 1_000_000.0,
                **dataset_metadata,
            )
            _perf_instant(
                "benchmark.dataset_load_complete",
                elapsed_ms=float(load_complete["elapsed_ms"]),
            )
            summary["dataset_metadata"] = dataset_metadata
            layer = viewer.add_image(
                volume_zyx,
                rendering=args.rendering,
                depiction="volume",
                scale=tuple(dataset_metadata["scene_scale_zyx"]),
                contrast_limits=(
                    float(args.contrast_limits_min),
                    float(args.contrast_limits_max),
                ),
                name=Path(args.dataset).stem,
            )
        elif args.dataset_loader == "tifffile-array":
            load_monotonic_ns = time.monotonic_ns()
            scene_scale_zyx = (
                tuple(float(x) for x in args.scene_scale_zyx)
                if args.scene_scale_zyx is not None
                else None
            )
            volume_zyx, dataset_metadata = _load_tiff_volume(
                args.dataset,
                scene_scale_zyx=scene_scale_zyx,
            )
            load_complete = logger.log(
                "dataset_load_complete",
                elapsed_ms=(time.monotonic_ns() - load_monotonic_ns) / 1_000_000.0,
                **dataset_metadata,
            )
            summary["dataset_metadata"] = dataset_metadata
            layer = viewer.add_image(
                volume_zyx,
                rendering=args.rendering,
                depiction="volume",
                scale=tuple(dataset_metadata["scene_scale_zyx"]),
                contrast_limits=(
                    float(args.contrast_limits_min),
                    float(args.contrast_limits_max),
                ),
                name=Path(args.dataset).stem,
            )
        else:
            scene_scale_zyx = (
                tuple(float(x) for x in args.scene_scale_zyx)
                if args.scene_scale_zyx is not None
                else None
            )
            layer, dataset_metadata, load_elapsed_ms = _open_dataset_with_napari(
                viewer,
                args.dataset,
                plugin=args.open_plugin,
                rendering=args.rendering,
                contrast_limits=(
                    float(args.contrast_limits_min),
                    float(args.contrast_limits_max),
                ),
                scene_scale_zyx=scene_scale_zyx,
            )
            load_complete = logger.log(
                "dataset_load_complete",
                elapsed_ms=load_elapsed_ms,
                **dataset_metadata,
            )
            _perf_instant(
                "benchmark.dataset_load_complete",
                elapsed_ms=float(load_complete["elapsed_ms"]),
            )
            summary["dataset_metadata"] = dataset_metadata
        viewer.dims.ndisplay = 3
        _process_events(app, max(12, int(args.process_events_count)))
        canvas = viewer.window._qt_viewer.canvas
        native_canvas = canvas.native
        native_canvas_device_pixel_ratio = float(native_canvas.devicePixelRatioF())
        live_render_surface = _configure_live_render_surface(
            viewer,
            physical_width=int(spec.viewport_width),
            physical_height=int(spec.viewport_height),
            process_events_count=max(12, int(args.process_events_count)),
        )
        canvas = viewer.window._qt_viewer.canvas
        native_canvas = canvas.native
        native_canvas_device_pixel_ratio = float(native_canvas.devicePixelRatioF())
        capture_logical_height = float(spec.viewport_height) / max(
            native_canvas_device_pixel_ratio, 1e-6
        )
        add_layer_complete = logger.log(
            "layer_add_complete",
            elapsed_ms=(time.monotonic_ns() - add_layer_start_ns) / 1_000_000.0,
            layer_shape=list(layer.data.shape),
            tile2data_scale=[float(x) for x in layer._transforms["tile2data"].scale],
            canvas_size=list(canvas.size),
            native_canvas_size=[
                int(native_canvas.size().width()),
                int(native_canvas.size().height()),
            ],
            native_canvas_device_pixel_ratio=native_canvas_device_pixel_ratio,
            capture_logical_height=capture_logical_height,
            live_render_surface=live_render_surface,
        )
        _perf_instant(
            "benchmark.layer_add_complete",
            elapsed_ms=float(add_layer_complete["elapsed_ms"]),
        )
        summary["capture_geometry"] = {
            "native_canvas_device_pixel_ratio": native_canvas_device_pixel_ratio,
            "capture_logical_height": capture_logical_height,
            "live_render_surface": live_render_surface,
        }
        geometry_path, calibration_path = _write_gui_geometry_and_calibration(
            output_dir=output_dir,
            viewer=viewer,
            window_title=args.window_title,
        )
        summary["gui_capture"] = {
            "geometry_path": str(geometry_path),
            "calibration_path": str(calibration_path),
        }
        frame_swap_tracker.wait_for_quiet(
            app,
            timeout_seconds=float(args.draw_sync_timeout_seconds),
            quiet_seconds=float(args.frame_settle_quiet_seconds),
        )
        logger.log("dataset_render_settled")
        _perf_instant("benchmark.dataset_render_settled")

        action_records: list[dict[str, Any]] = []
        open_first_frame_swap_monotonic_ns: int | None = None
        open_last_frame_swap_monotonic_ns: int | None = None
        open_frame_settle_return_monotonic_ns: int | None = None
        open_step_start_wall_time_ns: int | None = None
        open_step_render_wall_ms: float | None = None
        for action in spec.actions:
            if action.kind == "jump":
                cameras = [spec.states[action.state]]  # type: ignore[index]
            else:
                cameras = interpolate_action_cameras(spec, action)

            action_start = logger.log(
                "action_start",
                action=action.name,
                kind=action.kind,
                steps=len(cameras),
                duration_seconds=action.duration_seconds,
                settle_seconds=action.settle_seconds,
            )
            _perf_instant(
                "benchmark.action_start",
                action=str(action.name),
                kind=str(action.kind),
                steps=int(len(cameras)),
            )
            action_start_monotonic_ns = time.monotonic_ns()
            per_step: list[dict[str, Any]] = []
            screenshot_path: Path | None = None
            use_screenshot_reference_sync = bool(args.screenshot_reference_every_step)
            for step_index, camera in enumerate(cameras, start=1):
                if action.kind == "interpolate" and action.duration_seconds > 0.0:
                    target = action_start_monotonic_ns + int(
                        action.duration_seconds * (step_index / len(cameras)) * 1e9
                    )
                    sleep_until(target)
                step_start = logger.log(
                    "step_start",
                    action=action.name,
                    step=step_index,
                    steps=len(cameras),
                )
                _perf_instant(
                    "benchmark.step_start",
                    action=str(action.name),
                    step=int(step_index),
                    steps=int(len(cameras)),
                )
                render_start_ns = time.monotonic_ns()
                apply_info = _apply_camera(
                    viewer,
                    camera,
                    logical_viewport_height=capture_logical_height,
                )
                first_frame_swap_sync_ms: float | None = None
                last_frame_swap_sync_ms: float | None = None
                frame_settle_sync_ms: float | None = None
                frame_swap_count: int | None = None
                if use_screenshot_reference_sync:
                    first_frame_swap_monotonic_ns: int | None = None
                    last_frame_swap_monotonic_ns: int | None = None
                    frame_settle_return_monotonic_ns: int | None = None
                else:
                    baseline_frame_swap_count = frame_swap_tracker.frame_swap_count
                    (
                        first_frame_swap_monotonic_ns,
                        last_frame_swap_monotonic_ns,
                        frame_settle_return_monotonic_ns,
                        frame_swap_count,
                    ) = frame_swap_tracker.wait_for_frame_settle(
                        app,
                        baseline_frame_swap_count=baseline_frame_swap_count,
                        timeout_seconds=float(args.draw_sync_timeout_seconds),
                        quiet_seconds=float(args.frame_settle_quiet_seconds),
                    )
                    first_frame_swap_sync_ms = (
                        first_frame_swap_monotonic_ns - render_start_ns
                    ) / 1_000_000.0
                    last_frame_swap_sync_ms = (
                        last_frame_swap_monotonic_ns - render_start_ns
                    ) / 1_000_000.0
                    frame_settle_sync_ms = (
                        frame_settle_return_monotonic_ns - render_start_ns
                    ) / 1_000_000.0
                    _process_events(app, int(args.process_events_count))
                    if action.name == "open" and step_index == 1:
                        open_first_frame_swap_monotonic_ns = int(
                            first_frame_swap_monotonic_ns
                        )
                        open_last_frame_swap_monotonic_ns = int(
                            last_frame_swap_monotonic_ns
                        )
                        open_frame_settle_return_monotonic_ns = int(
                            frame_settle_return_monotonic_ns
                        )
                is_last = step_index == len(cameras)
                capture_screenshot_reference = bool(
                    args.screenshot_reference_every_step
                )
                should_capture_screenshot = is_last or capture_screenshot_reference
                if is_last:
                    screenshot_path = screenshots_dir / f"{action.name}.png"
                elif capture_screenshot_reference and bool(
                    args.save_screenshot_reference_images
                ):
                    screenshot_path = screenshots_dir / (
                        f"{action.name}_step{step_index:02d}.png"
                    )
                else:
                    screenshot_path = None
                screenshot_sync_ms: float | None = None
                screenshot_post_quiet_sync_ms: float | None = None
                screenshot_breakdown: dict[str, Any] | None = None
                camera_apply_to_screenshot_call_start_ms: float | None = None
                camera_apply_to_screenshot_capture_upper_bound_ms: float | None = None
                camera_apply_to_screenshot_total_ms: float | None = None
                screenshot_capture_info: dict[str, Any] | None = None
                if should_capture_screenshot:
                    step_start_ns = time.monotonic_ns()
                    screenshot_capture_info = _capture_canvas_screenshot(
                        viewer,
                        screenshot_path=screenshot_path,
                        width=int(spec.viewport_width),
                        height=int(spec.viewport_height),
                    )
                    screenshot_sync_ms = (
                        time.monotonic_ns() - step_start_ns
                    ) / 1_000_000.0
                    screenshot_breakdown = _extract_screenshot_breakdown(viewer)
                    screenshot_call_start_monotonic_ns = (
                        screenshot_breakdown or {}
                    ).get("screenshot_qimage_start_monotonic_ns")
                    screenshot_capture_end_monotonic_ns = (
                        screenshot_breakdown or {}
                    ).get("screenshot_qimage_end_monotonic_ns")
                    screenshot_call_end_monotonic_ns = (screenshot_breakdown or {}).get(
                        "screenshot_call_end_monotonic_ns"
                    )
                    if screenshot_call_start_monotonic_ns is not None:
                        camera_apply_to_screenshot_call_start_ms = (
                            int(screenshot_call_start_monotonic_ns) - render_start_ns
                        ) / 1_000_000.0
                    if screenshot_capture_end_monotonic_ns is not None:
                        camera_apply_to_screenshot_capture_upper_bound_ms = (
                            int(screenshot_capture_end_monotonic_ns) - render_start_ns
                        ) / 1_000_000.0
                    if screenshot_call_end_monotonic_ns is not None:
                        camera_apply_to_screenshot_total_ms = (
                            int(screenshot_call_end_monotonic_ns) - render_start_ns
                        ) / 1_000_000.0
                    if not use_screenshot_reference_sync:
                        # viewer.screenshot(...) itself can trigger one or more
                        # presented frames after the capture returns. Drain that
                        # activity here so the next timed camera step starts
                        # from a quiet canvas rather than inheriting
                        # screenshot-induced swaps.
                        screenshot_post_quiet_start_ns = time.monotonic_ns()
                        frame_swap_tracker.wait_for_quiet(
                            app,
                            timeout_seconds=float(args.draw_sync_timeout_seconds),
                            quiet_seconds=float(args.frame_settle_quiet_seconds),
                        )
                        screenshot_post_quiet_sync_ms = (
                            time.monotonic_ns() - screenshot_post_quiet_start_ns
                        ) / 1_000_000.0
                step_complete = logger.log(
                    "step_complete",
                    action=action.name,
                    step=step_index,
                    steps=len(cameras),
                    first_frame_swap_sync_ms=first_frame_swap_sync_ms,
                    last_frame_swap_sync_ms=last_frame_swap_sync_ms,
                    frame_settle_sync_ms=frame_settle_sync_ms,
                    frame_swap_count=frame_swap_count,
                    screenshot_sync_ms=screenshot_sync_ms,
                    screenshot_post_quiet_sync_ms=screenshot_post_quiet_sync_ms,
                    camera_apply_to_screenshot_call_start_ms=(
                        camera_apply_to_screenshot_call_start_ms
                    ),
                    camera_apply_to_screenshot_capture_upper_bound_ms=(
                        camera_apply_to_screenshot_capture_upper_bound_ms
                    ),
                    camera_apply_to_screenshot_total_ms=(
                        camera_apply_to_screenshot_total_ms
                    ),
                    screenshot_path=str(screenshot_path) if screenshot_path else None,
                    **(screenshot_capture_info or {}),
                    **(screenshot_breakdown or {}),
                    **apply_info,
                )
                client_wall_ms_from_step_start = (
                    int(step_complete["wall_time_ns"]) - int(step_start["wall_time_ns"])
                ) / 1_000_000.0
                render_wall_ms_excluding_quiet_and_capture: float | None
                if use_screenshot_reference_sync:
                    # In screenshot-reference mode, viewer.screenshot(...) is
                    # the synchronizer. There is no pre-step settle wait to
                    # subtract, and the forced screenshot draw/readback is the
                    # reference workload rather than removable overhead.
                    render_wall_ms_excluding_quiet_and_capture = None
                else:
                    render_wall_ms_excluding_quiet_and_capture = (
                        client_wall_ms_from_step_start
                        - frame_settle_quiet_ms
                        - float(screenshot_sync_ms or 0.0)
                        - float(screenshot_post_quiet_sync_ms or 0.0)
                    )
                    if render_wall_ms_excluding_quiet_and_capture < 0.0:
                        raise RuntimeError(
                            "napari render wall metric became negative after removing "
                            "the configured quiet window, screenshot capture cost, "
                            "and post-capture quiet drain"
                        )
                if action.name == "open" and step_index == 1:
                    open_step_start_wall_time_ns = int(step_start["wall_time_ns"])
                    open_step_render_wall_ms = (
                        float(render_wall_ms_excluding_quiet_and_capture)
                        if render_wall_ms_excluding_quiet_and_capture is not None
                        else None
                    )
                step_complete_perf = {
                    "action": str(action.name),
                    "step": int(step_index),
                    "client_wall_ms_from_step_start": float(
                        client_wall_ms_from_step_start
                    ),
                    "screenshot_post_quiet_sync_ms": float(
                        screenshot_post_quiet_sync_ms or 0.0
                    ),
                    "camera_apply_to_screenshot_capture_upper_bound_ms": float(
                        camera_apply_to_screenshot_capture_upper_bound_ms or 0.0
                    ),
                    "screenshot_capture_upper_bound_ms": float(
                        (screenshot_breakdown or {}).get(
                            "screenshot_capture_upper_bound_ms"
                        )
                        or 0.0
                    ),
                }
                if first_frame_swap_sync_ms is not None:
                    step_complete_perf["first_frame_swap_sync_ms"] = float(
                        first_frame_swap_sync_ms
                    )
                if last_frame_swap_sync_ms is not None:
                    step_complete_perf["last_frame_swap_sync_ms"] = float(
                        last_frame_swap_sync_ms
                    )
                if frame_settle_sync_ms is not None:
                    step_complete_perf["frame_settle_sync_ms"] = float(
                        frame_settle_sync_ms
                    )
                if render_wall_ms_excluding_quiet_and_capture is not None:
                    step_complete_perf["render_wall_ms_excluding_quiet_and_capture"] = (
                        float(render_wall_ms_excluding_quiet_and_capture)
                    )
                _perf_instant("benchmark.step_complete", **step_complete_perf)
                per_step.append(
                    {
                        "step": step_index,
                        "first_frame_swap_sync_ms": first_frame_swap_sync_ms,
                        "last_frame_swap_sync_ms": last_frame_swap_sync_ms,
                        "frame_settle_sync_ms": frame_settle_sync_ms,
                        "frame_swap_count": frame_swap_count,
                        "screenshot_sync_ms": screenshot_sync_ms,
                        "screenshot_post_quiet_sync_ms": screenshot_post_quiet_sync_ms,
                        "camera_apply_to_screenshot_call_start_ms": (
                            camera_apply_to_screenshot_call_start_ms
                        ),
                        "camera_apply_to_screenshot_capture_upper_bound_ms": (
                            camera_apply_to_screenshot_capture_upper_bound_ms
                        ),
                        "camera_apply_to_screenshot_total_ms": (
                            camera_apply_to_screenshot_total_ms
                        ),
                        "client_wall_ms_from_step_start": client_wall_ms_from_step_start,
                        "render_wall_ms_excluding_quiet_and_capture": (
                            render_wall_ms_excluding_quiet_and_capture
                        ),
                        **(screenshot_capture_info or {}),
                        **(screenshot_breakdown or {}),
                    }
                )

            if action.settle_seconds > 0.0:
                time.sleep(float(action.settle_seconds))
            action_complete = logger.log(
                "action_complete",
                action=action.name,
                steps=len(cameras),
                action_total_ms=(
                    int(time.time_ns()) - int(action_start["wall_time_ns"])
                )
                / 1_000_000.0,
                screenshot_path=str(screenshot_path) if screenshot_path else None,
            )
            _perf_instant(
                "benchmark.action_complete",
                action=str(action.name),
                action_total_ms=float(action_complete["action_total_ms"]),
            )
            action_record = {
                "name": action.name,
                "kind": action.kind,
                "steps": len(cameras),
                "duration_seconds": action.duration_seconds,
                "settle_seconds": action.settle_seconds,
                "action_total_ms": action_complete["action_total_ms"],
                "action_render_wall_ms_excluding_quiet_and_capture": (
                    float(
                        sum(
                            float(step["render_wall_ms_excluding_quiet_and_capture"])
                            for step in per_step
                            if step.get("render_wall_ms_excluding_quiet_and_capture")
                            is not None
                        )
                    )
                    if any(
                        step.get("render_wall_ms_excluding_quiet_and_capture")
                        is not None
                        for step in per_step
                    )
                    else None
                ),
                "action_start_wall_time_ns": int(action_start["wall_time_ns"]),
                "action_complete_wall_time_ns": int(action_complete["wall_time_ns"]),
                "screenshot_path": str(screenshot_path) if screenshot_path else None,
                "step_metrics": per_step,
            }
            action_records.append(action_record)

        session_end = logger.log(
            "session_end",
            ok=True,
        )
        _perf_instant("benchmark.session_end", ok=1)
        summary["actions"] = action_records
        open_action = next(
            (record for record in action_records if str(record["name"]) == "open"),
            None,
        )
        open_step_camera_apply_to_screenshot_capture_upper_bound_ms = (
            float(
                open_action["step_metrics"][0][
                    "camera_apply_to_screenshot_capture_upper_bound_ms"
                ]
            )
            if open_action is not None
            and open_action["step_metrics"]
            and open_action["step_metrics"][0].get(
                "camera_apply_to_screenshot_capture_upper_bound_ms"
            )
            is not None
            else None
        )
        summary["open"] = {
            "dataset_load_ms": (
                int(load_complete["wall_time_ns"]) - int(load_start["wall_time_ns"])
            )
            / 1_000_000.0,
            "layer_add_ms": (
                int(add_layer_complete["wall_time_ns"])
                - int(load_complete["wall_time_ns"])
            )
            / 1_000_000.0,
            "open_action_total_ms": (
                float(open_action["action_total_ms"])
                if open_action is not None
                else None
            ),
            "open_action_render_wall_ms_excluding_quiet_and_capture": (
                float(open_action["action_render_wall_ms_excluding_quiet_and_capture"])
                if open_action is not None
                and open_action.get("action_render_wall_ms_excluding_quiet_and_capture")
                is not None
                else None
            ),
            "session_to_open_complete_ms": (
                (
                    int(open_action["action_complete_wall_time_ns"])
                    - int(session_start["wall_time_ns"])
                )
                / 1_000_000.0
                if open_action is not None
                else None
            ),
            "session_to_open_render_complete_ms": (
                (
                    (
                        int(open_step_start_wall_time_ns)
                        - int(session_start["wall_time_ns"])
                    )
                    / 1_000_000.0
                    + float(open_step_render_wall_ms)
                )
                if open_step_start_wall_time_ns is not None
                and open_step_render_wall_ms is not None
                else None
            ),
            "dataset_load_start_to_open_render_complete_ms": (
                (
                    (
                        int(open_step_start_wall_time_ns)
                        - int(load_start["wall_time_ns"])
                    )
                    / 1_000_000.0
                    + float(open_step_render_wall_ms)
                )
                if open_step_start_wall_time_ns is not None
                and open_step_render_wall_ms is not None
                else None
            ),
            "session_to_open_first_frame_swap_ms": (
                (
                    int(open_first_frame_swap_monotonic_ns)
                    - int(session_start["monotonic_ns"])
                )
                / 1_000_000.0
                if open_first_frame_swap_monotonic_ns is not None
                else None
            ),
            "dataset_load_start_to_open_first_frame_swap_ms": (
                (
                    int(open_first_frame_swap_monotonic_ns)
                    - int(load_start["monotonic_ns"])
                )
                / 1_000_000.0
                if open_first_frame_swap_monotonic_ns is not None
                else None
            ),
            "session_to_open_last_frame_swap_ms": (
                (
                    int(open_last_frame_swap_monotonic_ns)
                    - int(session_start["monotonic_ns"])
                )
                / 1_000_000.0
                if open_last_frame_swap_monotonic_ns is not None
                else None
            ),
            "session_to_open_frame_settle_ms": (
                (
                    int(open_frame_settle_return_monotonic_ns)
                    - int(session_start["monotonic_ns"])
                )
                / 1_000_000.0
                if open_frame_settle_return_monotonic_ns is not None
                else None
            ),
            "session_to_open_screenshot_capture_upper_bound_ms": (
                (
                    (
                        int(open_step_start_wall_time_ns)
                        - int(session_start["wall_time_ns"])
                    )
                    / 1_000_000.0
                    + float(open_step_camera_apply_to_screenshot_capture_upper_bound_ms)
                )
                if open_step_camera_apply_to_screenshot_capture_upper_bound_ms
                is not None
                and open_step_start_wall_time_ns is not None
                else None
            ),
            "dataset_load_start_to_open_screenshot_capture_upper_bound_ms": (
                (
                    (
                        int(open_step_start_wall_time_ns)
                        - int(load_start["wall_time_ns"])
                    )
                    / 1_000_000.0
                    + float(open_step_camera_apply_to_screenshot_capture_upper_bound_ms)
                )
                if open_step_camera_apply_to_screenshot_capture_upper_bound_ms
                is not None
                and open_step_start_wall_time_ns is not None
                else None
            ),
            "total_wall_ms": (
                int(session_end["wall_time_ns"]) - int(session_start["wall_time_ns"])
            )
            / 1_000_000.0,
        }
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        viewer.close()
        app.quit()
        return 0
    except Exception as exc:
        failure = {
            "error_type": type(exc).__name__,
            "error": str(exc),
        }
        logger.log("session_end", ok=False, **failure)
        _perf_instant("benchmark.session_end", ok=0, error_type=str(type(exc).__name__))
        summary["error"] = failure
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        raise
    finally:
        perf.timers.stop_trace_file()
        if rss_sampler is not None:
            memory_summary = rss_sampler.stop()
            memory_summary_path.write_text(
                json.dumps(memory_summary, indent=2) + "\n",
                encoding="utf-8",
            )
        logger.close()


if __name__ == "__main__":
    raise SystemExit(main())
