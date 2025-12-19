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
            "description": "Apply a camera operator to the current camera value: AZIMUTH/ELEVATION/ROLL/YAW/PITCH/FLIP. Returns a typed camera value. Angles >120° are segmented internally into ≤90° steps and chained from the previous value; prefer ≤90° inputs for clarity.",
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
                        "description": "Optional typed camera value to apply the operator to (defaults to current engine camera).",
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
            val = client.camera_rotate(op=op, degrees=deg, base_value=base_value)
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
            return json.dumps({"ok": True, "value": val})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    return None
