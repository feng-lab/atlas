#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import napari
from napari.qt import get_qapp
from qtpy import QtCore

from napari_volume_benchmark import (
    _apply_camera,
    _configure_live_render_surface,
    _load_nim_volume,
    _load_tiff_volume,
    _open_dataset_with_napari,
    _process_events,
)
from volume_benchmark_common import load_benchmark_spec


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Launch a prepared napari GUI session for real on-screen interaction "
            "benchmarking with ScreenCaptureKit capture and Quartz drag injection."
        )
    )
    parser.add_argument("--dataset", required=True)
    parser.add_argument(
        "--dataset-loader",
        choices=("zimg-array", "tifffile-array", "napari-open"),
        default="tifffile-array",
    )
    parser.add_argument("--open-plugin", default=None)
    parser.add_argument(
        "--scene-scale-zyx",
        type=float,
        nargs=3,
        metavar=("Z", "Y", "X"),
        default=None,
    )
    parser.add_argument("--camera-spec", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--window-x", type=int, default=100)
    parser.add_argument("--window-y", type=int, default=95)
    parser.add_argument("--window-width", type=int, default=1400)
    parser.add_argument("--window-height", type=int, default=960)
    parser.add_argument("--rendering", default="mip")
    parser.add_argument("--contrast-limits-min", type=float, default=0.0)
    parser.add_argument("--contrast-limits-max", type=float, default=255.0)
    parser.add_argument("--process-events-count", type=int, default=12)
    parser.add_argument(
        "--window-title",
        default="napari GUI Benchmark",
        help="Top-level napari window title for capture calibration and lookup.",
    )
    parser.add_argument(
        "--show-fps-overlay",
        action="store_true",
        help=(
            "Show napari's internal FPS overlay for manual inspection. Leave this "
            "off for exact-pixel GUI capture benchmarks because the changing text "
            "will count as visible updates."
        ),
    )
    parser.add_argument(
        "--show-layer-docks",
        action="store_true",
        help="Keep the layer list and layer controls docks visible.",
    )
    parser.add_argument(
        "--show-status-bar",
        action="store_true",
        help="Keep the napari status bar visible.",
    )
    return parser.parse_args()


def _configure_window_layout(
    viewer: napari.Viewer,
    *,
    x: int,
    y: int,
    width: int,
    height: int,
    title: str,
    show_layer_docks: bool,
    show_status_bar: bool,
) -> None:
    viewer.title = title
    qt_window = viewer.window._qt_window
    qt_window.setWindowTitle(title)
    qt_window.setGeometry(int(x), int(y), int(width), int(height))
    if not show_status_bar:
        qt_window.statusBar().hide()
    qt_viewer = viewer.window._qt_viewer
    if not show_layer_docks:
        qt_viewer.dockLayerList.hide()
        qt_viewer.dockLayerControls.hide()


def _enable_fps_overlay(viewer: napari.Viewer) -> None:
    viewer.text_overlay.visible = True

    def update_fps(fps: float) -> None:
        viewer.text_overlay.text = f"{fps:1.1f} FPS"

    # This is the same private hook napari documents in its example.
    viewer.window._qt_viewer.canvas._scene_canvas.measure_fps(callback=update_fps)
    update_fps(0.0)


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


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _write_gui_geometry_and_calibration(
    *,
    output_dir: Path,
    viewer: napari.Viewer,
    window_title: str,
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
        "window_title": str(window_title),
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
        "window_name_substring": str(window_title),
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
        "actions": [
            {
                "name": "rotate",
                "button": "left",
                "start_norm": [0.45, 0.55],
                "end_norm": [0.75, 0.55],
                "pre_drag_still_seconds": 0.25,
                "duration_seconds": 0.5,
                "steps": 30,
                "settle_seconds": 2.0,
            }
        ],
    }
    geometry_path = output_dir / "napari_gui_geometry.json"
    calibration_path = output_dir / "napari_gui_calibration.json"
    _write_json(geometry_path, geometry_payload)
    _write_json(calibration_path, calibration_payload)
    return geometry_path, calibration_path


def main() -> int:
    args = _parse_args()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    status_path = output_dir / "napari_gui_prepare_status.json"
    error_path = output_dir / "napari_gui_prepare_error.json"

    try:
        spec = load_benchmark_spec(args.camera_spec)
        app = get_qapp()
        viewer = napari.Viewer(show=True)
        _configure_window_layout(
            viewer,
            x=int(args.window_x),
            y=int(args.window_y),
            width=int(args.window_width),
            height=int(args.window_height),
            title=str(args.window_title),
            show_layer_docks=bool(args.show_layer_docks),
            show_status_bar=bool(args.show_status_bar),
        )
        _process_events(app, max(12, int(args.process_events_count)))

        dataset_metadata: dict[str, Any]
        if args.dataset_loader == "zimg-array":
            volume_zyx, dataset_metadata = _load_nim_volume(
                args.dataset,
                channel_index=0,
                time_index=0,
            )
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
            scene_scale_zyx = (
                tuple(float(x) for x in args.scene_scale_zyx)
                if args.scene_scale_zyx is not None
                else None
            )
            volume_zyx, dataset_metadata = _load_tiff_volume(
                args.dataset,
                scene_scale_zyx=scene_scale_zyx,
            )
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
            layer, dataset_metadata, _load_elapsed_ms = _open_dataset_with_napari(
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

        viewer.dims.ndisplay = 3
        _process_events(app, max(12, int(args.process_events_count)))

        live_render_surface = _configure_live_render_surface(
            viewer,
            physical_width=int(spec.viewport_width),
            physical_height=int(spec.viewport_height),
            process_events_count=max(12, int(args.process_events_count)),
        )
        _process_events(app, max(12, int(args.process_events_count)))

        capture_logical_height = float(spec.viewport_height) / float(
            viewer.window._qt_viewer.canvas.native.devicePixelRatioF()
        )
        camera_info = _apply_camera(
            viewer,
            spec.states["open"],
            logical_viewport_height=capture_logical_height,
        )
        _process_events(app, max(12, int(args.process_events_count)))

        if args.show_fps_overlay:
            _enable_fps_overlay(viewer)

        geometry_path, calibration_path = _write_gui_geometry_and_calibration(
            output_dir=output_dir,
            viewer=viewer,
            window_title=str(args.window_title),
        )

        status_payload = {
            "dataset": str(Path(args.dataset).resolve()),
            "dataset_loader": args.dataset_loader,
            "camera_spec": str(Path(args.camera_spec).resolve()),
            "window_title": str(args.window_title),
            "window_geometry": {
                "x": int(args.window_x),
                "y": int(args.window_y),
                "width": int(args.window_width),
                "height": int(args.window_height),
            },
            "dataset_metadata": dataset_metadata,
            "live_render_surface": live_render_surface,
            "capture_logical_height": capture_logical_height,
            "prepared_camera_state": camera_info,
            "qt_window_geometry": {
                "x": int(viewer.window._qt_window.geometry().x()),
                "y": int(viewer.window._qt_window.geometry().y()),
                "width": int(viewer.window._qt_window.geometry().width()),
                "height": int(viewer.window._qt_window.geometry().height()),
            },
            "layer_scale": [float(x) for x in layer.scale],
            "layer_translate": [float(x) for x in layer.translate],
            "show_fps_overlay": bool(args.show_fps_overlay),
            "geometry_path": str(geometry_path),
            "calibration_path": str(calibration_path),
        }
        _write_json(status_path, status_payload)
        napari.run()
        return 0
    except Exception as exc:
        error_path.write_text(
            json.dumps(
                {
                    "error": str(exc),
                    "dataset": str(Path(args.dataset).resolve()),
                    "camera_spec": str(Path(args.camera_spec).resolve()),
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        raise


if __name__ == "__main__":
    raise SystemExit(main())
