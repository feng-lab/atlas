#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

from volume_benchmark_common import GenericCamera


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Extract Atlas camera settings from a .scene file and build a shared "
            "benchmark camera spec with derived open/rotate/zoom states."
        )
    )
    parser.add_argument("--scene", required=True, help="Atlas .scene file")
    parser.add_argument("--output", required=True, help="Output benchmark JSON")
    parser.add_argument("--viewport-width", type=int, default=2000)
    parser.add_argument("--viewport-height", type=int, default=1500)
    parser.add_argument("--azimuth-degrees", type=float, default=30.0)
    parser.add_argument("--elevation-degrees", type=float, default=15.0)
    parser.add_argument("--zoom-factor", type=float, default=4.0)
    parser.add_argument("--duration-seconds", type=float, default=0.5)
    parser.add_argument("--steps", type=int, default=30)
    parser.add_argument("--settle-seconds", type=float, default=2.0)
    return parser.parse_args()


def _normalize(v: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    if length <= 1e-12:
        raise ValueError("cannot normalize a zero-length vector")
    return (v[0] / length, v[1] / length, v[2] / length)


def _cross(
    a: tuple[float, float, float], b: tuple[float, float, float]
) -> tuple[float, float, float]:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _rotate(
    vec: tuple[float, float, float], axis: tuple[float, float, float], degrees: float
) -> tuple[float, float, float]:
    ax = _normalize(axis)
    angle = math.radians(degrees)
    cos_theta = math.cos(angle)
    sin_theta = math.sin(angle)
    dot = vec[0] * ax[0] + vec[1] * ax[1] + vec[2] * ax[2]
    cross = _cross(ax, vec)
    return (
        vec[0] * cos_theta + cross[0] * sin_theta + ax[0] * dot * (1.0 - cos_theta),
        vec[1] * cos_theta + cross[1] * sin_theta + ax[1] * dot * (1.0 - cos_theta),
        vec[2] * cos_theta + cross[2] * sin_theta + ax[2] * dot * (1.0 - cos_theta),
    )


def _find_camera(payload: Any) -> dict[str, Any] | None:
    if isinstance(payload, dict):
        camera = payload.get("Camera 3DCamera")
        if isinstance(camera, dict):
            return camera
        for value in payload.values():
            found = _find_camera(value)
            if found is not None:
                return found
    elif isinstance(payload, list):
        for value in payload:
            found = _find_camera(value)
            if found is not None:
                return found
    return None


def _orbit(
    camera: GenericCamera, azimuth_degrees: float, elevation_degrees: float
) -> GenericCamera:
    center = camera.center
    view = (
        camera.eye[0] - center[0],
        camera.eye[1] - center[1],
        camera.eye[2] - center[2],
    )
    up = _normalize(camera.up)

    view = _rotate(view, up, azimuth_degrees)
    right = _normalize(_cross(view, up))
    view = _rotate(view, right, elevation_degrees)
    up = _normalize(_rotate(up, right, elevation_degrees))

    return GenericCamera(
        projection=camera.projection,
        eye=(center[0] + view[0], center[1] + view[1], center[2] + view[2]),
        center=center,
        up=up,
        field_of_view_degrees=camera.field_of_view_degrees,
        eye_separation_angle_degrees=camera.eye_separation_angle_degrees,
    )


def _zoom(camera: GenericCamera, zoom_factor: float) -> GenericCamera:
    if zoom_factor <= 0.0:
        raise ValueError("--zoom-factor must be positive")
    center = camera.center
    view = (
        camera.eye[0] - center[0],
        camera.eye[1] - center[1],
        camera.eye[2] - center[2],
    )
    scaled = (view[0] / zoom_factor, view[1] / zoom_factor, view[2] / zoom_factor)
    return GenericCamera(
        projection=camera.projection,
        eye=(center[0] + scaled[0], center[1] + scaled[1], center[2] + scaled[2]),
        center=center,
        up=camera.up,
        field_of_view_degrees=camera.field_of_view_degrees,
        eye_separation_angle_degrees=camera.eye_separation_angle_degrees,
    )


def main() -> int:
    args = _parse_args()
    scene_path = Path(args.scene)
    output_path = Path(args.output)
    raw = json.loads(scene_path.read_text(encoding="utf-8"))
    camera_payload = _find_camera(raw)
    if camera_payload is None:
        raise ValueError(f"Could not find 'Camera 3DCamera' in {scene_path}")

    open_camera = GenericCamera.from_json(camera_payload)
    rotate_end = _orbit(open_camera, args.azimuth_degrees, args.elevation_degrees)
    zoom_end = _zoom(rotate_end, args.zoom_factor)

    spec = {
        "viewport": {
            "width": int(args.viewport_width),
            "height": int(args.viewport_height),
        },
        "states": {
            "open": open_camera.to_json(),
            "rotate_end": rotate_end.to_json(),
            "zoom_end": zoom_end.to_json(),
        },
        "actions": [
            {
                "name": "open",
                "kind": "jump",
                "state": "open",
                "settle_seconds": float(args.settle_seconds),
            },
            {
                "name": "rotate",
                "kind": "interpolate",
                "from_state": "open",
                "to_state": "rotate_end",
                "duration_seconds": float(args.duration_seconds),
                "steps": int(args.steps),
                "settle_seconds": float(args.settle_seconds),
            },
            {
                "name": "zoom",
                "kind": "interpolate",
                "from_state": "rotate_end",
                "to_state": "zoom_end",
                "duration_seconds": float(args.duration_seconds),
                "steps": int(args.steps),
                "settle_seconds": float(args.settle_seconds),
            },
        ],
        "source_scene": str(scene_path.resolve()),
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
