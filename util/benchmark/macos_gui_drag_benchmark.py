#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import time
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from AppKit import NSApplicationActivateIgnoringOtherApps, NSWorkspace
from Quartz import (
    CGEventCreateMouseEvent,
    CGEventPost,
    CGEventSetIntegerValueField,
    CGWindowListCopyWindowInfo,
    CGPointMake,
    kCGEventLeftMouseDown,
    kCGEventLeftMouseDragged,
    kCGEventLeftMouseUp,
    kCGEventMouseMoved,
    kCGEventRightMouseDown,
    kCGEventRightMouseDragged,
    kCGEventRightMouseUp,
    kCGHIDEventTap,
    kCGMouseButtonLeft,
    kCGMouseButtonRight,
    kCGMouseEventClickState,
    kCGNullWindowID,
    kCGWindowListExcludeDesktopElements,
    kCGWindowListOptionOnScreenOnly,
)

from volume_benchmark_common import EventLogger, sleep_until


@dataclass(frozen=True)
class Region:
    x: float
    y: float
    width: float
    height: float

    @classmethod
    def from_json(cls, payload: dict[str, Any], *, field_name: str) -> "Region":
        if not isinstance(payload, Mapping):
            raise ValueError(f"{field_name} must be an object")
        return cls(
            x=float(payload["x"]),
            y=float(payload["y"]),
            width=float(payload["width"]),
            height=float(payload["height"]),
        )

    def to_json(self) -> dict[str, float]:
        return {
            "x": self.x,
            "y": self.y,
            "width": self.width,
            "height": self.height,
        }


@dataclass(frozen=True)
class DragAction:
    name: str
    button: str
    start_norm: tuple[float, float]
    end_norm: tuple[float, float]
    duration_seconds: float
    steps: int
    settle_seconds: float

    @classmethod
    def from_json(cls, payload: dict[str, Any]) -> "DragAction":
        if not isinstance(payload, dict):
            raise ValueError("action must be an object")
        start_norm = payload.get("start_norm")
        end_norm = payload.get("end_norm")
        if not isinstance(start_norm, list | tuple) or len(start_norm) != 2:
            raise ValueError("action.start_norm must be a 2-element array")
        if not isinstance(end_norm, list | tuple) or len(end_norm) != 2:
            raise ValueError("action.end_norm must be a 2-element array")
        button = str(payload.get("button", "left")).lower()
        if button not in {"left", "right"}:
            raise ValueError(f"unsupported button {button!r}")
        return cls(
            name=str(payload.get("name") or "rotate"),
            button=button,
            start_norm=(float(start_norm[0]), float(start_norm[1])),
            end_norm=(float(end_norm[0]), float(end_norm[1])),
            duration_seconds=max(0.0, float(payload.get("duration_seconds", 0.5))),
            steps=max(1, int(payload.get("steps", 30))),
            settle_seconds=max(0.0, float(payload.get("settle_seconds", 2.0))),
        )

    def to_json(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "button": self.button,
            "start_norm": list(self.start_norm),
            "end_norm": list(self.end_norm),
            "duration_seconds": self.duration_seconds,
            "steps": self.steps,
            "settle_seconds": self.settle_seconds,
        }


@dataclass(frozen=True)
class GuiCalibration:
    app: str
    bundle_identifier: str | None
    window_owner_name: str | None
    window_name_substring: str | None
    region_coordinate_space: str
    activate_app: bool
    capture_region: Region
    input_region: Region
    actions: tuple[DragAction, ...]

    @classmethod
    def from_json(cls, payload: dict[str, Any]) -> "GuiCalibration":
        if not isinstance(payload, Mapping):
            raise ValueError("calibration must be a JSON object")
        actions_raw = payload.get("actions")
        if not isinstance(actions_raw, list) or not actions_raw:
            raise ValueError("calibration.actions must be a non-empty array")
        region_coordinate_space = str(
            payload.get("region_coordinate_space") or "absolute"
        ).lower()
        if region_coordinate_space not in {"absolute", "window-relative"}:
            raise ValueError(
                "calibration.region_coordinate_space must be 'absolute' or "
                "'window-relative'"
            )
        return cls(
            app=str(payload.get("app") or "unknown"),
            bundle_identifier=(
                str(payload["bundle_identifier"])
                if payload.get("bundle_identifier") is not None
                else None
            ),
            window_owner_name=(
                str(payload["window_owner_name"])
                if payload.get("window_owner_name") is not None
                else None
            ),
            window_name_substring=(
                str(payload["window_name_substring"])
                if payload.get("window_name_substring") is not None
                else None
            ),
            region_coordinate_space=region_coordinate_space,
            activate_app=bool(payload.get("activate_app", True)),
            capture_region=Region.from_json(
                payload["capture_region"], field_name="capture_region"
            ),
            input_region=Region.from_json(
                payload["input_region"], field_name="input_region"
            ),
            actions=tuple(DragAction.from_json(action) for action in actions_raw),
        )


@dataclass(frozen=True)
class WindowInfo:
    owner_name: str
    title: str
    pid: int
    layer: int
    window_number: int
    bounds: Region

    def to_json(self) -> dict[str, Any]:
        return {
            "owner_name": self.owner_name,
            "title": self.title,
            "pid": self.pid,
            "layer": self.layer,
            "window_number": self.window_number,
            "bounds": self.bounds.to_json(),
        }


def _load_calibration(path: Path) -> GuiCalibration:
    return GuiCalibration.from_json(json.loads(path.read_text(encoding="utf-8")))


def _list_windows() -> list[WindowInfo]:
    raw_windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID,
    )
    windows: list[WindowInfo] = []
    for entry in raw_windows or []:
        bounds = entry.get("kCGWindowBounds")
        if not isinstance(bounds, Mapping):
            continue
        width = float(bounds.get("Width", 0.0))
        height = float(bounds.get("Height", 0.0))
        if width <= 0.0 or height <= 0.0:
            continue
        windows.append(
            WindowInfo(
                owner_name=str(entry.get("kCGWindowOwnerName", "") or ""),
                title=str(entry.get("kCGWindowName", "") or ""),
                pid=int(entry.get("kCGWindowOwnerPID", 0) or 0),
                layer=int(entry.get("kCGWindowLayer", 0) or 0),
                window_number=int(entry.get("kCGWindowNumber", 0) or 0),
                bounds=Region(
                    x=float(bounds.get("X", 0.0)),
                    y=float(bounds.get("Y", 0.0)),
                    width=width,
                    height=height,
                ),
            )
        )
    return windows


def _find_window(calibration: GuiCalibration) -> WindowInfo | None:
    owner_match = (calibration.window_owner_name or "").strip().lower()
    title_match = (calibration.window_name_substring or "").strip().lower()
    candidates = _list_windows()
    for window in candidates:
        if owner_match and owner_match not in window.owner_name.lower():
            continue
        if title_match and title_match not in window.title.lower():
            continue
        return window
    return None


def _activate_matching_app(calibration: GuiCalibration) -> dict[str, Any] | None:
    workspace = NSWorkspace.sharedWorkspace()
    running_apps = list(workspace.runningApplications() or [])
    owner_match = (calibration.window_owner_name or "").strip().lower()
    bundle_match = (calibration.bundle_identifier or "").strip().lower()

    for app in running_apps:
        bundle_id = str(app.bundleIdentifier() or "")
        localized_name = str(app.localizedName() or "")
        if bundle_match and bundle_id.lower() != bundle_match:
            continue
        if owner_match and owner_match not in localized_name.lower():
            continue
        app.activateWithOptions_(NSApplicationActivateIgnoringOtherApps)
        return {
            "bundle_identifier": bundle_id,
            "localized_name": localized_name,
            "pid": int(app.processIdentifier()),
        }
    return None


def _point_from_norm(
    region: Region, norm_xy: tuple[float, float]
) -> tuple[float, float]:
    return (
        region.x + region.width * norm_xy[0],
        region.y + region.height * norm_xy[1],
    )


def _resolve_region(
    region: Region, calibration: GuiCalibration, matched_window: WindowInfo | None
) -> Region:
    if calibration.region_coordinate_space == "absolute":
        return region
    if matched_window is None:
        raise RuntimeError(
            "window-relative calibration requires a matched on-screen window"
        )
    return Region(
        x=matched_window.bounds.x + region.x,
        y=matched_window.bounds.y + region.y,
        width=region.width,
        height=region.height,
    )


def _lerp_point(
    start_xy: tuple[float, float], end_xy: tuple[float, float], t: float
) -> tuple[float, float]:
    return (
        start_xy[0] + (end_xy[0] - start_xy[0]) * t,
        start_xy[1] + (end_xy[1] - start_xy[1]) * t,
    )


def _mouse_constants(button: str) -> tuple[int, int, int, int]:
    if button == "left":
        return (
            kCGMouseButtonLeft,
            kCGEventLeftMouseDown,
            kCGEventLeftMouseDragged,
            kCGEventLeftMouseUp,
        )
    return (
        kCGMouseButtonRight,
        kCGEventRightMouseDown,
        kCGEventRightMouseDragged,
        kCGEventRightMouseUp,
    )


def _post_mouse_event(
    event_type: int,
    point_xy: tuple[float, float],
    button: str,
    *,
    click_state: int | None = None,
) -> None:
    button_const, _, _, _ = _mouse_constants(button)
    event = CGEventCreateMouseEvent(
        None, event_type, CGPointMake(point_xy[0], point_xy[1]), button_const
    )
    if click_state is not None:
        CGEventSetIntegerValueField(event, kCGMouseEventClickState, click_state)
    CGEventPost(kCGHIDEventTap, event)


def _run_drag_action(
    action: DragAction,
    calibration: GuiCalibration,
    input_region: Region,
    logger: EventLogger,
    injection_logger: EventLogger,
) -> None:
    down_type: int
    drag_type: int
    up_type: int
    _, down_type, drag_type, up_type = _mouse_constants(action.button)

    start_xy = _point_from_norm(input_region, action.start_norm)
    end_xy = _point_from_norm(input_region, action.end_norm)

    logger.log(
        "action_start",
        app=calibration.app,
        action=action.name,
        kind="drag",
        button=action.button,
        duration_seconds=action.duration_seconds,
        steps=action.steps,
        settle_seconds=action.settle_seconds,
        start_point=start_xy,
        end_point=end_xy,
    )

    _post_mouse_event(kCGEventMouseMoved, start_xy, action.button)
    injection_logger.log(
        "mouse_move",
        action=action.name,
        button=action.button,
        x=start_xy[0],
        y=start_xy[1],
    )

    _post_mouse_event(down_type, start_xy, action.button, click_state=1)
    injection_logger.log(
        "mouse_down",
        action=action.name,
        button=action.button,
        x=start_xy[0],
        y=start_xy[1],
    )

    logger.log(
        "drag_start",
        app=calibration.app,
        action=action.name,
        button=action.button,
        steps=action.steps,
    )
    drag_start_ns = time.monotonic_ns()
    duration_ns = int(action.duration_seconds * 1e9)
    for step in range(1, action.steps + 1):
        t = step / action.steps
        point_xy = _lerp_point(start_xy, end_xy, t)
        target_ns = drag_start_ns + (duration_ns * step) // action.steps
        _post_mouse_event(drag_type, point_xy, action.button, click_state=1)
        injection_logger.log(
            "mouse_dragged",
            action=action.name,
            button=action.button,
            step=step,
            steps=action.steps,
            t=t,
            x=point_xy[0],
            y=point_xy[1],
        )
        sleep_until(target_ns)

    _post_mouse_event(up_type, end_xy, action.button, click_state=1)
    injection_logger.log(
        "mouse_up",
        action=action.name,
        button=action.button,
        x=end_xy[0],
        y=end_xy[1],
    )
    logger.log(
        "drag_end", app=calibration.app, action=action.name, button=action.button
    )
    logger.log("action_end", app=calibration.app, action=action.name)
    if action.settle_seconds > 0.0:
        time.sleep(action.settle_seconds)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Inject macOS Quartz mouse drags for a real-GUI benchmark and write the "
            "same JSONL action markers used by the screen-capture observer."
        )
    )
    parser.add_argument(
        "--calibration",
        required=False,
        help="GUI drag calibration JSON path",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Output directory for gui_events.jsonl and injected_mouse_events.jsonl",
    )
    parser.add_argument(
        "--action",
        action="append",
        default=None,
        help="Action name to run. Repeat to run a subset in order. Defaults to all actions.",
    )
    parser.add_argument(
        "--initial-delay-seconds",
        type=float,
        default=1.0,
        help="Delay after session_start before injecting the first action.",
    )
    parser.add_argument(
        "--activate-app",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="Override calibration.activate_app.",
    )
    parser.add_argument(
        "--activate-delay-seconds",
        type=float,
        default=0.5,
        help="Delay after app activation before injecting events.",
    )
    parser.add_argument(
        "--list-windows",
        action="store_true",
        help="List on-screen windows and exit.",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    if args.list_windows:
        for window in _list_windows():
            print(json.dumps(window.to_json(), sort_keys=True))
        return 0

    if not args.calibration:
        raise SystemExit("--calibration is required unless --list-windows is used")

    calibration_path = Path(args.calibration).resolve()
    calibration = _load_calibration(calibration_path)
    output_dir = (
        Path(args.output_dir).resolve()
        if args.output_dir is not None
        else calibration_path.parent / f"{calibration.app}_gui_benchmark"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    logger = EventLogger(output_dir / "gui_events.jsonl")
    injection_logger = EventLogger(output_dir / "injected_mouse_events.jsonl")

    selected_names = set(args.action or [])
    actions = (
        [action for action in calibration.actions if action.name in selected_names]
        if selected_names
        else list(calibration.actions)
    )
    if not actions:
        raise RuntimeError("No actions selected for GUI benchmark injection")

    try:
        activate_app = (
            bool(args.activate_app)
            if args.activate_app is not None
            else calibration.activate_app
        )
        logger.log(
            "session_start",
            app=calibration.app,
            calibration_path=str(calibration_path),
            region_coordinate_space=calibration.region_coordinate_space,
            capture_region=calibration.capture_region.to_json(),
            input_region=calibration.input_region.to_json(),
            action_names=[action.name for action in actions],
            activate_app=activate_app,
        )

        if activate_app:
            app_info = _activate_matching_app(calibration)
            if app_info is None:
                raise RuntimeError(
                    "Could not find a running app matching the calibration for activation"
                )
            logger.log("app_activated", app=calibration.app, running_app=app_info)
            time.sleep(max(0.0, args.activate_delay_seconds))

        matched_window = _find_window(calibration)
        if matched_window is not None:
            resolved_capture_region = _resolve_region(
                calibration.capture_region, calibration, matched_window
            )
            resolved_input_region = _resolve_region(
                calibration.input_region, calibration, matched_window
            )
            logger.log(
                "matched_window", app=calibration.app, window=matched_window.to_json()
            )
            logger.log(
                "resolved_regions",
                app=calibration.app,
                capture_region=resolved_capture_region.to_json(),
                input_region=resolved_input_region.to_json(),
            )
        else:
            resolved_capture_region = calibration.capture_region
            resolved_input_region = calibration.input_region
            logger.log(
                "matched_window_missing",
                app=calibration.app,
                window_owner_name=calibration.window_owner_name,
                window_name_substring=calibration.window_name_substring,
            )

        if args.initial_delay_seconds > 0.0:
            time.sleep(max(0.0, args.initial_delay_seconds))

        for action in actions:
            _run_drag_action(
                action,
                calibration,
                resolved_input_region,
                logger,
                injection_logger,
            )
            logger.log(
                "action_settle_complete", app=calibration.app, action=action.name
            )

        logger.log("session_end", app=calibration.app, ok=True)
        return 0
    except Exception as exc:
        logger.log("session_end", app=calibration.app, ok=False, error=str(exc))
        raise
    finally:
        logger.close()
        injection_logger.close()


if __name__ == "__main__":
    raise SystemExit(main())
