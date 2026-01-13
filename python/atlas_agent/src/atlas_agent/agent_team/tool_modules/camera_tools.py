import json
from typing import Any, Dict, List

from .context import ToolDispatchContext

HANDLED_TOOLS = (
    "fit_candidates",
    "camera_focus",
    "camera_point_to",
    "camera_rotate",
    "camera_reset_view",
)

TOOL_SPECS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "fit_candidates",
            "description": "Return ids of visual objects suitable for camera fit/orbit (excludes Animation3D).",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "camera_focus",
            "description": "Compute a camera that focuses on the given ids using the current scene bbox. Returns a typed camera value.",
            "parameters": {
                "type": "object",
                "properties": {
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "List of target object ids to focus",
                    },
                    "after_clipping": {
                        "type": "boolean",
                        "default": True,
                        "description": "Use clipped bbox (true) or full bbox (false)",
                    },
                    "min_radius": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Minimum radius to avoid degenerate views",
                    },
                },
                "required": ["ids", "after_clipping", "min_radius"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "camera_point_to",
            "description": "Compute a camera that points to the targets (center moves, eye unchanged). Returns a typed camera value.",
            "parameters": {
                "type": "object",
                "properties": {
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "List of target object ids to point to",
                    },
                    "after_clipping": {
                        "type": "boolean",
                        "default": True,
                        "description": "Use clipped bbox (true) or full bbox (false)",
                    },
                },
                "required": ["ids", "after_clipping"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "camera_rotate",
            "description": "Apply a camera operator to a typed camera value: AZIMUTH/ELEVATION/ROLL/YAW/PITCH/FLIP. Returns a typed camera value. If base_value is omitted, this tool chains from the last produced camera value within the current turn when available; otherwise it uses the current engine camera. Angles >120° are segmented internally into ≤90° steps for stability.",
            "parameters": {
                "type": "object",
                "properties": {
                    "op": {
                        "type": "string",
                        "enum": [
                            "AZIMUTH",
                            "ELEVATION",
                            "ROLL",
                            "YAW",
                            "PITCH",
                            "FLIP",
                        ],
                    },
                    "degrees": {"type": "number", "default": 90.0},
                    "base_value": {
                        "type": "object",
                        "description": "Optional typed camera value to apply the operator to. When omitted, the tool uses the last produced camera value within the current turn when available, otherwise the current engine camera.",
                    },
                },
                "required": ["op", "degrees"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "camera_reset_view",
            "description": "Reset camera to XY/XZ/YZ/RESET view using scene bbox (Animation3D excluded). Returns a typed camera value.",
            "parameters": {
                "type": "object",
                "properties": {
                    "mode": {
                        "type": "string",
                        "enum": ["XY", "XZ", "YZ", "RESET"],
                        "default": "RESET",
                        "description": "Preset view",
                    },
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Candidate ids for sizing (optional for RESET)",
                    },
                    "after_clipping": {
                        "type": "boolean",
                        "default": True,
                        "description": "Use clipped bbox (true) or full bbox (false)",
                    },
                    "min_radius": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Minimum radius to avoid degenerate views",
                    },
                },
                "required": ["mode", "ids", "after_clipping", "min_radius"],
            },
        },
    },
]


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    client = ctx.client
    atlas_dir = ctx.atlas_dir
    dispatch = ctx.dispatch
    _param_to_dict = ctx.param_to_dict
    _resolve_json_key = ctx.resolve_json_key
    _json_key_exists = ctx.json_key_exists
    _schema_validator_cache = ctx.schema_validator_cache
    runtime_state = ctx.runtime_state if isinstance(ctx.runtime_state, dict) else {}

    def _get_last_camera_value() -> dict | None:
        v = runtime_state.get("last_camera_value")
        return v if isinstance(v, dict) else None

    def _set_last_camera_value(v: dict | None) -> None:
        if isinstance(v, dict):
            runtime_state["last_camera_value"] = v

    if name == "fit_candidates":
        try:
            ids = client.fit_candidates()
            return json.dumps({"ok": True, "ids": ids})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "camera_focus":
        try:
            ids = args.get("ids") or []
            ac = bool(args.get("after_clipping", True))
            mr = float(args.get("min_radius", 0.0))
            val = client.camera_focus(ids=ids, after_clipping=ac, min_radius=mr)
            _set_last_camera_value(val if isinstance(val, dict) else None)
            return json.dumps({"ok": True, "value": val})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "camera_point_to":
        try:
            ids = args.get("ids") or []
            ac = bool(args.get("after_clipping", True))
            val = client.camera_point_to(ids=ids, after_clipping=ac)
            _set_last_camera_value(val if isinstance(val, dict) else None)
            return json.dumps({"ok": True, "value": val})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "camera_rotate":
        try:
            op = str(args.get("op"))
            deg = float(args.get("degrees", 90.0))
            base_value = args.get("base_value")

            # If base_value is omitted, chain from the last produced camera value
            # within this turn when available (common when composing camera_* tools).
            if base_value in (None, {}):
                last = _get_last_camera_value()
                if last is not None:
                    base_value = last

            # Segment large rotations for stability/predictable interpolation.
            # This matches the tool contract and avoids forcing the model to
            # manually split large angles.
            if abs(deg) > 120.0:
                sign = 1.0 if deg >= 0.0 else -1.0
                remaining = abs(deg)
                segments: list[float] = []
                while remaining > 90.0 + 1e-6:
                    segments.append(90.0 * sign)
                    remaining -= 90.0
                if remaining > 1e-6:
                    segments.append(remaining * sign)

                cur = base_value
                last_val: dict | None = None
                for sdeg in segments:
                    step_val = client.camera_rotate(
                        op=op, degrees=float(sdeg), base_value=cur
                    )
                    if not isinstance(step_val, dict):
                        raise RuntimeError("camera_rotate returned non-object value")
                    last_val = step_val
                    cur = step_val
                _set_last_camera_value(last_val)
                return json.dumps({"ok": True, "value": last_val})

            val = client.camera_rotate(op=op, degrees=deg, base_value=base_value)
            _set_last_camera_value(val if isinstance(val, dict) else None)
            return json.dumps({"ok": True, "value": val})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "camera_reset_view":
        try:
            mode = str(args.get("mode", "RESET"))
            ids = args.get("ids") or []
            ac = bool(args.get("after_clipping", True))
            mr = float(args.get("min_radius", 0.0))
            val = client.camera_reset_view(
                mode=mode, ids=ids, after_clipping=ac, min_radius=mr
            )
            _set_last_camera_value(val if isinstance(val, dict) else None)
            return json.dumps({"ok": True, "value": val})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    return None
