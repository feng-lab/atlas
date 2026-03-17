from __future__ import annotations

import json
import math
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_FIELD_OF_VIEW_DEGREES = 45.0


def _vec3(value: Any, *, field_name: str) -> tuple[float, float, float]:
    if not isinstance(value, (list, tuple)) or len(value) != 3:
        raise ValueError(f"{field_name} must be a 3-element array")
    return (float(value[0]), float(value[1]), float(value[2]))


def _normalize(vec: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2])
    if length <= 1e-12:
        return (0.0, 0.0, 1.0)
    return (vec[0] / length, vec[1] / length, vec[2] / length)


def _lerp_vec3(
    a: tuple[float, float, float], b: tuple[float, float, float], t: float
) -> tuple[float, float, float]:
    return (
        a[0] + (b[0] - a[0]) * t,
        a[1] + (b[1] - a[1]) * t,
        a[2] + (b[2] - a[2]) * t,
    )


@dataclass(frozen=True)
class GenericCamera:
    projection: str
    eye: tuple[float, float, float]
    center: tuple[float, float, float]
    up: tuple[float, float, float]
    field_of_view_degrees: float
    eye_separation_angle_degrees: float = 5.0

    @classmethod
    def from_json(cls, payload: dict[str, Any]) -> "GenericCamera":
        if not isinstance(payload, dict):
            raise ValueError("camera payload must be an object")
        projection = str(
            payload.get("projection")
            or payload.get("Projection Type StringIntOption")
            or "Perspective"
        )
        if projection not in {"Perspective", "Orthographic"}:
            raise ValueError(f"unsupported projection {projection!r}")

        eye = payload.get("eye")
        if eye is None:
            eye = payload.get("position")
        if eye is None:
            eye = payload.get("Eye Position Vec3")

        center = payload.get("center")
        if center is None:
            center = payload.get("focal_point")
        if center is None:
            center = payload.get("Center Position Vec3")

        up = payload.get("up")
        if up is None:
            up = payload.get("view_up")
        if up is None:
            up = payload.get("Up Vector Vec3")

        field_of_view_degrees = payload.get("field_of_view_degrees")
        if field_of_view_degrees is None:
            field_of_view_degrees = payload.get("view_angle")
        if field_of_view_degrees is None:
            field_of_view_degrees = payload.get("Field of View Float")
        if field_of_view_degrees is None:
            field_of_view_degrees = DEFAULT_FIELD_OF_VIEW_DEGREES

        eye_separation_angle_degrees = payload.get("eye_separation_angle_degrees")
        if eye_separation_angle_degrees is None:
            eye_separation_angle_degrees = payload.get("Eye Separation Angle Float")
        if eye_separation_angle_degrees is None:
            eye_separation_angle_degrees = 5.0

        return cls(
            projection=projection,
            eye=_vec3(eye, field_name="eye"),
            center=_vec3(center, field_name="center"),
            up=_normalize(_vec3(up, field_name="up")),
            field_of_view_degrees=float(field_of_view_degrees),
            eye_separation_angle_degrees=float(eye_separation_angle_degrees),
        )

    def to_json(self) -> dict[str, Any]:
        return {
            "projection": self.projection,
            "eye": list(self.eye),
            "center": list(self.center),
            "up": list(self.up),
            "field_of_view_degrees": self.field_of_view_degrees,
            "eye_separation_angle_degrees": self.eye_separation_angle_degrees,
        }

    def to_atlas_typed_value(self) -> dict[str, Any]:
        return {
            "Projection Type StringIntOption": self.projection,
            "Eye Position Vec3": list(self.eye),
            "Center Position Vec3": list(self.center),
            "Up Vector Vec3": list(self.up),
            "Eye Separation Angle Float": self.eye_separation_angle_degrees,
            "Field of View Float": self.field_of_view_degrees,
        }

    def to_paraview_view_state(self) -> dict[str, Any]:
        return {
            "CameraPosition": list(self.eye),
            "CameraFocalPoint": list(self.center),
            "CameraViewUp": list(self.up),
            "CameraViewAngle": self.field_of_view_degrees,
        }


def lerp_camera(a: GenericCamera, b: GenericCamera, t: float) -> GenericCamera:
    if a.projection != b.projection:
        raise ValueError("camera interpolation requires matching projection modes")
    return GenericCamera(
        projection=a.projection,
        eye=_lerp_vec3(a.eye, b.eye, t),
        center=_lerp_vec3(a.center, b.center, t),
        up=_normalize(_lerp_vec3(a.up, b.up, t)),
        field_of_view_degrees=a.field_of_view_degrees
        + (b.field_of_view_degrees - a.field_of_view_degrees) * t,
        eye_separation_angle_degrees=a.eye_separation_angle_degrees
        + (b.eye_separation_angle_degrees - a.eye_separation_angle_degrees) * t,
    )


@dataclass(frozen=True)
class BenchmarkAction:
    name: str
    kind: str
    state: str | None
    from_state: str | None
    to_state: str | None
    duration_seconds: float
    steps: int
    settle_seconds: float


@dataclass(frozen=True)
class BenchmarkSpec:
    viewport_width: int
    viewport_height: int
    states: dict[str, GenericCamera]
    actions: list[BenchmarkAction]


def load_benchmark_spec(path: str | Path) -> BenchmarkSpec:
    spec_path = Path(path)
    raw = json.loads(spec_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("benchmark spec must be a JSON object")

    viewport = raw.get("viewport") or {}
    width = int(viewport.get("width", 2000))
    height = int(viewport.get("height", 1500))

    raw_states = raw.get("states")
    if not isinstance(raw_states, dict) or not raw_states:
        raise ValueError("benchmark spec must define a non-empty 'states' object")
    states = {
        name: GenericCamera.from_json(payload) for name, payload in raw_states.items()
    }

    raw_actions = raw.get("actions")
    if raw_actions is None:
        inferred = []
        if "open" in states:
            inferred.append(
                {
                    "name": "open",
                    "kind": "jump",
                    "state": "open",
                    "settle_seconds": 2.0,
                }
            )
        if "open" in states and "rotate_end" in states:
            inferred.append(
                {
                    "name": "rotate",
                    "kind": "interpolate",
                    "from_state": "open",
                    "to_state": "rotate_end",
                    "duration_seconds": 0.5,
                    "steps": 30,
                    "settle_seconds": 2.0,
                }
            )
        if "rotate_end" in states and "zoom_end" in states:
            inferred.append(
                {
                    "name": "zoom",
                    "kind": "interpolate",
                    "from_state": "rotate_end",
                    "to_state": "zoom_end",
                    "duration_seconds": 0.5,
                    "steps": 30,
                    "settle_seconds": 2.0,
                }
            )
        raw_actions = inferred

    if not isinstance(raw_actions, list) or not raw_actions:
        raise ValueError("benchmark spec must define a non-empty 'actions' array")

    actions: list[BenchmarkAction] = []
    for idx, action in enumerate(raw_actions):
        if not isinstance(action, dict):
            raise ValueError(f"action #{idx} must be an object")
        kind = str(action.get("kind", "jump"))
        name = str(action.get("name") or f"action_{idx:02d}")
        state = action.get("state")
        from_state = action.get("from_state")
        to_state = action.get("to_state")
        duration_seconds = float(action.get("duration_seconds", 0.5))
        steps = int(action.get("steps", 30))
        settle_seconds = float(action.get("settle_seconds", 2.0))
        if kind == "jump":
            if state is None:
                raise ValueError(f"jump action {name!r} requires 'state'")
            if state not in states:
                raise ValueError(f"unknown state {state!r} in action {name!r}")
        elif kind == "interpolate":
            if from_state is None or to_state is None:
                raise ValueError(
                    f"interpolate action {name!r} requires 'from_state' and 'to_state'"
                )
            if from_state not in states or to_state not in states:
                raise ValueError(
                    f"unknown interpolate endpoints in action {name!r}: "
                    f"{from_state!r} -> {to_state!r}"
                )
            steps = max(1, steps)
            duration_seconds = max(0.0, duration_seconds)
        else:
            raise ValueError(f"unsupported action kind {kind!r}")
        actions.append(
            BenchmarkAction(
                name=name,
                kind=kind,
                state=str(state) if state is not None else None,
                from_state=str(from_state) if from_state is not None else None,
                to_state=str(to_state) if to_state is not None else None,
                duration_seconds=duration_seconds,
                steps=steps,
                settle_seconds=max(0.0, settle_seconds),
            )
        )

    return BenchmarkSpec(
        viewport_width=width,
        viewport_height=height,
        states=states,
        actions=actions,
    )


class EventLogger:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._stream = self.path.open("w", encoding="utf-8")

    def close(self) -> None:
        self._stream.close()

    def log(self, event: str, **payload: Any) -> dict[str, Any]:
        record = {
            "event": event,
            "wall_time_ns": time.time_ns(),
            "monotonic_ns": time.monotonic_ns(),
        }
        record.update(payload)
        self._stream.write(json.dumps(record, sort_keys=True) + "\n")
        self._stream.flush()
        return record


def sleep_until(target_monotonic_ns: int) -> None:
    while True:
        remaining_ns = target_monotonic_ns - time.monotonic_ns()
        if remaining_ns <= 0:
            return
        time.sleep(min(remaining_ns / 1e9, 0.01))


def interpolate_action_cameras(
    spec: BenchmarkSpec, action: BenchmarkAction
) -> list[GenericCamera]:
    if action.kind == "jump":
        assert action.state is not None
        return [spec.states[action.state]]

    assert action.from_state is not None
    assert action.to_state is not None
    start = spec.states[action.from_state]
    end = spec.states[action.to_state]
    return [
        lerp_camera(start, end, step / action.steps)
        for step in range(1, action.steps + 1)
    ]
