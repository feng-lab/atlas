from __future__ import annotations

import json
import os
import sys
import traceback
from pathlib import Path

from paraview.simple import (
    GetActiveViewOrCreate,
    HideUnusedScalarBars,
    OpenDataFile,
    Show,
)
from vtkmodules.vtkCommonCore import vtkCommand

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

from paraview_volume_benchmark import _apply_camera, _configure_display
from volume_benchmark_common import load_benchmark_spec

try:
    import PythonQt  # type: ignore
    from PythonQt import QtCore, QtGui  # type: ignore
except Exception:
    PythonQt = None
    QtCore = None
    QtGui = None


def _env_required(name: str) -> str:
    value = os.environ.get(name)
    if value is None or value.strip() == "":
        raise RuntimeError(f"missing required environment variable {name}")
    return value


def _env_float(name: str, default: float) -> float:
    value = os.environ.get(name)
    return float(value) if value not in (None, "") else default


def _env_int(name: str, default: int) -> int:
    value = os.environ.get(name)
    return int(value) if value not in (None, "") else default


def _env_bool(name: str, default: bool) -> bool:
    value = os.environ.get(name)
    if value in (None, ""):
        return default
    value_lower = value.strip().lower()
    if value_lower in {"1", "true", "yes", "on"}:
        return True
    if value_lower in {"0", "false", "no", "off"}:
        return False
    raise RuntimeError(f"invalid boolean value for {name}: {value!r}")


def _get_main_window():
    if PythonQt is None or QtGui is None:
        return None
    for widget in QtGui.QApplication.topLevelWidgets():
        if isinstance(widget, PythonQt.private.ParaViewMainWindow):
            return widget
    return None


def _get_pq_view(view):
    if PythonQt is None:
        return None
    model = PythonQt.paraview.pqPVApplicationCore.instance().getServerManagerModel()
    return PythonQt.paraview.pqPythonQtMethodHelpers.findProxyItem(model, view.SMProxy)


def _geometry_payload(widget) -> dict[str, float]:
    top_left = widget.mapToGlobal(QtCore.QPoint(0, 0))
    return {
        "x": float(top_left.x()),
        "y": float(top_left.y()),
        "width": float(widget.width()),
        "height": float(widget.height()),
    }


def _write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def _finite_bounds_center(bounds) -> list[float]:
    if len(bounds) != 6:
        raise RuntimeError(f"unexpected bounds length: {bounds!r}")
    center: list[float] = []
    for low, high in zip(bounds[0::2], bounds[1::2]):
        low_value = float(low)
        high_value = float(high)
        if not (abs(low_value) < float("inf") and abs(high_value) < float("inf")):
            raise RuntimeError(f"non-finite data bounds: {bounds!r}")
        center.append((low_value + high_value) * 0.5)
    return center


def _recenter_view_to_data(view, source) -> dict[str, object]:
    bounds = source.GetDataInformation().GetBounds()
    data_center = _finite_bounds_center(bounds)
    current_position = [float(value) for value in view.CameraPosition]
    current_focal_point = [float(value) for value in view.CameraFocalPoint]
    delta = [data_center[idx] - current_focal_point[idx] for idx in range(3)]
    view.CameraPosition = [current_position[idx] + delta[idx] for idx in range(3)]
    view.CameraFocalPoint = data_center
    if hasattr(view, "CenterOfRotation"):
        view.CenterOfRotation = data_center
    return {
        "bounds": [float(value) for value in bounds],
        "data_center": data_center,
        "delta": delta,
    }


def _prepare() -> None:
    dataset = _env_required("PARAVIEW_GUI_PREP_DATASET")
    camera_spec_path = _env_required("PARAVIEW_GUI_PREP_CAMERA_SPEC")
    output_dir = Path(_env_required("PARAVIEW_GUI_PREP_OUTPUT_DIR"))
    array_name = os.environ.get("PARAVIEW_GUI_PREP_ARRAY_NAME", "channels")
    blend_mode = os.environ.get("PARAVIEW_GUI_PREP_BLEND_MODE", "maximum-intensity")
    volume_rendering_mode = os.environ.get(
        "PARAVIEW_GUI_PREP_VOLUME_RENDERING_MODE", "gpu-based"
    )
    component = _env_int("PARAVIEW_GUI_PREP_COMPONENT", 0)
    initial_state_name = os.environ.get("PARAVIEW_GUI_PREP_INITIAL_STATE", "open")
    main_x = _env_int("PARAVIEW_GUI_PREP_MAINWINDOW_X", 40)
    main_y = _env_int("PARAVIEW_GUI_PREP_MAINWINDOW_Y", 60)
    main_width = _env_int("PARAVIEW_GUI_PREP_MAINWINDOW_WIDTH", 1600)
    main_height = _env_int("PARAVIEW_GUI_PREP_MAINWINDOW_HEIGHT", 1100)
    settle_ms = _env_int("PARAVIEW_GUI_PREP_SETTLE_MS", 750)
    recenter_to_data_center = _env_bool(
        "PARAVIEW_GUI_PREP_RECENTER_TO_DATA_CENTER", True
    )
    rotate_start_x = _env_float("PARAVIEW_GUI_PREP_ROTATE_START_X", 0.45)
    rotate_start_y = _env_float("PARAVIEW_GUI_PREP_ROTATE_START_Y", 0.55)
    rotate_end_x = _env_float("PARAVIEW_GUI_PREP_ROTATE_END_X", 0.75)
    rotate_end_y = _env_float("PARAVIEW_GUI_PREP_ROTATE_END_Y", 0.55)

    spec = load_benchmark_spec(camera_spec_path)
    if initial_state_name not in spec.states:
        raise RuntimeError(
            f"initial state {initial_state_name!r} not present in {camera_spec_path}"
        )

    source = OpenDataFile(dataset)
    display = Show(source)
    _configure_display(
        display,
        array_name=array_name,
        channel_mode="component",
        component=component,
        blend_mode=blend_mode,
        volume_rendering_mode=volume_rendering_mode,
        data_range_min=0.0,
        data_range_max=255.0,
        color_min_rgb=(0.0, 0.0, 0.0),
        color_max_rgb=(0.99215686, 0.0, 0.0),
    )

    view = GetActiveViewOrCreate("RenderView")
    view.ViewSize = [spec.viewport_width, spec.viewport_height]
    _apply_camera(view, spec.states[initial_state_name])
    recenter_payload = None
    if recenter_to_data_center:
        recenter_payload = _recenter_view_to_data(view, source)
    elif hasattr(view, "CenterOfRotation"):
        view.CenterOfRotation = [float(value) for value in view.CameraFocalPoint]
    HideUnusedScalarBars(view=view)
    view.OrientationAxesVisibility = 0
    view.Background = [0.0, 0.0, 0.0]
    if hasattr(view, "UseColorPaletteForBackground"):
        view.UseColorPaletteForBackground = 0
    if hasattr(view, "Background2"):
        view.Background2 = [0.0, 0.0, 0.0]
    view.StillRender()

    if QtCore is None:
        status_payload = {
            "camera_focal_point": [float(value) for value in view.CameraFocalPoint],
            "camera_position": [float(value) for value in view.CameraPosition],
            "center_of_rotation": (
                [float(value) for value in view.CenterOfRotation]
                if hasattr(view, "CenterOfRotation")
                else None
            ),
            "dataset": dataset,
            "camera_spec": camera_spec_path,
            "initial_state": initial_state_name,
            "pythonqt_available": False,
            "recenter_payload": recenter_payload,
            "recenter_to_data_center": recenter_to_data_center,
            "reason": "installed ParaView build does not expose PythonQt in startup scripts",
        }
        _write_json(output_dir / "paraview_gui_prepare_status.json", status_payload)
        print(
            json.dumps(
                {
                    "event": "paraview_gui_prepare_ready",
                    "pythonqt_available": False,
                    "status_path": str(output_dir / "paraview_gui_prepare_status.json"),
                },
                sort_keys=True,
            )
        )
        return

    main_window = _get_main_window()
    if main_window is None:
        raise RuntimeError("could not find ParaView main window")

    main_window.move(main_x, main_y)
    main_window.resize(main_width, main_height)

    def _write_geometry() -> None:
        pq_view = _get_pq_view(view)
        if pq_view is None:
            raise RuntimeError("could not resolve active pqRenderView")
        qvtk_widget = pq_view.widget()
        render_widget = (
            qvtk_widget.renderWidget()
            if hasattr(qvtk_widget, "renderWidget")
            else qvtk_widget
        )
        dpr = (
            float(qvtk_widget.effectiveDevicePixelRatio())
            if hasattr(qvtk_widget, "effectiveDevicePixelRatio")
            else float(render_widget.devicePixelRatioF())
        )
        input_region = _geometry_payload(render_widget)
        capture_region = {
            "x": input_region["x"] * dpr,
            "y": input_region["y"] * dpr,
            "width": input_region["width"] * dpr,
            "height": input_region["height"] * dpr,
        }
        main_window_geometry = _geometry_payload(main_window)

        geometry_payload = {
            "camera_focal_point": [float(value) for value in view.CameraFocalPoint],
            "camera_position": [float(value) for value in view.CameraPosition],
            "center_of_rotation": (
                [float(value) for value in view.CenterOfRotation]
                if hasattr(view, "CenterOfRotation")
                else None
            ),
            "dataset": dataset,
            "camera_spec": camera_spec_path,
            "initial_state": initial_state_name,
            "main_window": {
                "title": str(main_window.windowTitle),
                "geometry": main_window_geometry,
            },
            "recenter_payload": recenter_payload,
            "recenter_to_data_center": recenter_to_data_center,
            "render_widget": {
                "device_pixel_ratio": dpr,
                "input_region": input_region,
                "capture_region": capture_region,
                "view_size": [int(view.ViewSize[0]), int(view.ViewSize[1])],
            },
        }
        calibration_payload = {
            "app": "paraview",
            "bundle_identifier": "",
            "window_owner_name": "ParaView",
            "window_name_substring": "",
            "activate_app": True,
            "capture_region": capture_region,
            "input_region": input_region,
            "actions": [
                {
                    "name": "rotate",
                    "button": "left",
                    "start_norm": [rotate_start_x, rotate_start_y],
                    "end_norm": [rotate_end_x, rotate_end_y],
                    "duration_seconds": 0.5,
                    "steps": 30,
                    "settle_seconds": 2.0,
                }
            ],
        }

        _write_json(output_dir / "paraview_gui_geometry.json", geometry_payload)
        _write_json(output_dir / "paraview_gui_calibration.json", calibration_payload)
        print(
            json.dumps(
                {
                    "event": "paraview_gui_calibration_ready",
                    "geometry_path": str(output_dir / "paraview_gui_geometry.json"),
                    "calibration_path": str(
                        output_dir / "paraview_gui_calibration.json"
                    ),
                },
                sort_keys=True,
            )
        )

    QtCore.QTimer.singleShot(settle_ms, _write_geometry)


def _run_prepare() -> None:
    output_dir_env = os.environ.get("PARAVIEW_GUI_PREP_OUTPUT_DIR")
    output_dir = Path(output_dir_env) if output_dir_env else None
    try:
        _prepare()
    except Exception as exc:
        error_payload = {
            "error": str(exc),
            "pythonqt_available": PythonQt is not None,
            "traceback": traceback.format_exc(),
        }
        if output_dir is not None:
            _write_json(output_dir / "paraview_gui_prepare_error.json", error_payload)
        print(
            json.dumps(
                {
                    "error_path": str(output_dir / "paraview_gui_prepare_error.json")
                    if output_dir is not None
                    else "",
                    "event": "paraview_gui_prepare_failed",
                    "message": str(exc),
                },
                sort_keys=True,
            )
        )
        raise


if QtCore is not None:
    QtCore.QTimer.singleShot(0, _run_prepare)
else:
    _run_prepare()
