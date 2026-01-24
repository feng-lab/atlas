import json
from typing import Any, Dict, List

from ...tool_registry import Tool, tool_from_schema
from .context import ToolDispatchContext
from .preconditions import require_engine_ready
from .schemas_camera_value import CAMERA_TYPED_VALUE_SCHEMA

VEC3_NUMBER_SCHEMA: Dict[str, Any] = {
    "type": "array",
    "items": {"type": "number"},
    "minItems": 3,
    "maxItems": 3,
}

VEC3_NUMBER_OR_NULL_SCHEMA: Dict[str, Any] = {
    **VEC3_NUMBER_SCHEMA,
    "type": ["array", "null"],
}

CAMERA_PATH_WAYPOINT_EYE_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": "Waypoint eye position. Exactly one of world or bbox_fraction should be non-null.",
    "properties": {
        "world": {
            **VEC3_NUMBER_OR_NULL_SCHEMA,
            "description": "Absolute world-space eye [x,y,z].",
        },
        "bbox_fraction": {
            **VEC3_NUMBER_OR_NULL_SCHEMA,
            "description": "Fractions [fx,fy,fz] in [0..1] inside the target bbox of ids (or all visual objects).",
        },
    },
}

CAMERA_PATH_WAYPOINT_LOOK_AT_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": "Waypoint look-at target. Exactly one of world, bbox_center, or bbox_fraction should be set.",
    "properties": {
        "world": {
            **VEC3_NUMBER_OR_NULL_SCHEMA,
            "description": "Absolute world-space look_at target [x,y,z].",
        },
        "bbox_center": {
            "type": ["boolean", "null"],
            "description": "When true, aim at the bbox center of ids (or all visual objects).",
        },
        "bbox_fraction": {
            **VEC3_NUMBER_OR_NULL_SCHEMA,
            "description": "Fractions [fx,fy,fz] in [0..1] inside the target bbox of ids (or all visual objects).",
        },
    },
}

CAMERA_PATH_WAYPOINT_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": "Waypoint: {time, eye?, look_at?}. eye/look_at may be omitted (null).",
    "properties": {
        "time": {"type": "number", "description": "Waypoint time in seconds."},
        "eye": {
            "type": ["object", "null"],
            "description": "Optional: eye position (world or bbox_fraction).",
            "properties": CAMERA_PATH_WAYPOINT_EYE_SCHEMA["properties"],
        },
        "look_at": {
            "type": ["object", "null"],
            "description": "Optional: look-at target (world / bbox_center / bbox_fraction).",
            "properties": CAMERA_PATH_WAYPOINT_LOOK_AT_SCHEMA["properties"],
        },
    },
}


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


TOOLS: List[Tool] = [
    tool_from_schema(
        name="fit_candidates",
        description="Return ids of visual objects suitable for camera fit/orbit (excludes Animation3D).",
        parameters_schema={"type": "object", "properties": {}},
        handler=_tool_handler("fit_candidates"),
        preconditions=(require_engine_ready,),
    ),
    tool_from_schema(
        name="camera_get",
        description="Return the current engine camera as a typed camera value (no key writes). Useful as a deterministic base_value for camera_move_local/camera_look_at.",
        parameters_schema={"type": "object", "properties": {}},
        handler=_tool_handler("camera_get"),
        preconditions=(require_engine_ready,),
    ),
    tool_from_schema(
        name="camera_focus",
        description="Compute a camera that focuses on the given ids using the current scene bbox. Returns a typed camera value.",
        parameters_schema={
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
            "required": ["ids"],
        },
        handler=_tool_handler("camera_focus"),
        preconditions=(require_engine_ready,),
    ),
    tool_from_schema(
        name="camera_point_to",
        description="Compute a camera that points to the targets (center moves, eye unchanged). Returns a typed camera value.",
        parameters_schema={
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
            "required": ["ids"],
        },
        handler=_tool_handler("camera_point_to"),
        preconditions=(require_engine_ready,),
    ),
    tool_from_schema(
        name="camera_rotate",
        description="Apply a camera operator to a typed camera value: AZIMUTH/ELEVATION/ROLL/YAW/PITCH/FLIP. Returns a typed camera value. base_value is required for determinism (pass a typed camera from camera_get/camera_focus/camera_reset_view). Angles >120° are segmented internally into ≤90° steps for stability.",
        parameters_schema={
            "type": "object",
            "properties": {
                "op": {
                    "type": "string",
                    "enum": ["AZIMUTH", "ELEVATION", "ROLL", "YAW", "PITCH", "FLIP"],
                },
                "degrees": {"type": "number", "default": 90.0},
                "base_value": {
                    **CAMERA_TYPED_VALUE_SCHEMA,
                    "description": "Required typed camera value to apply the operator to.",
                },
            },
            "required": ["op", "base_value"],
        },
        handler=_tool_handler("camera_rotate"),
        preconditions=(require_engine_ready,),
    ),
    tool_from_schema(
        name="camera_reset_view",
        description="Reset camera to XY/XZ/YZ/RESET view using scene bbox (Animation3D excluded). Returns a typed camera value.",
        parameters_schema={
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
            "required": [],
        },
        handler=_tool_handler("camera_reset_view"),
        preconditions=(require_engine_ready,),
    ),
    tool_from_schema(
        name="camera_move_local",
        description="Move the camera in its local basis (first-person fly/dolly building block). Returns a typed camera value. base_value is required for determinism (pass a typed camera from camera_get/camera_focus/etc). Use distance_is_fraction_of_bbox_radius=true to avoid guessing world units (distance is scaled by the target bbox enclosing-sphere radius).",
        parameters_schema={
            "type": "object",
            "properties": {
                "op": {
                    "type": "string",
                    "enum": ["FORWARD", "BACK", "RIGHT", "LEFT", "UP", "DOWN"],
                },
                "distance": {
                    "type": "number",
                    "description": "World-units, or a fraction of bbox radius when distance_is_fraction_of_bbox_radius=true.",
                },
                "distance_is_fraction_of_bbox_radius": {
                    "type": "boolean",
                    "default": True,
                    "description": "When true, interpret distance as a fraction of target bbox enclosing-sphere radius.",
                },
                "ids": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Optional target ids for bbox scaling. Empty → all visual objects.",
                },
                "after_clipping": {
                    "type": "boolean",
                    "default": True,
                    "description": "When computing bbox for scaling, use clipped bbox (true) or full bbox (false).",
                },
                "move_center": {
                    "type": "boolean",
                    "default": True,
                    "description": "When true, translate eye+center together (fly). When false, translate eye only (dolly/boom).",
                },
                "base_value": {
                    **CAMERA_TYPED_VALUE_SCHEMA,
                    "description": "Required typed camera value to move from.",
                },
            },
            "required": ["op", "distance", "base_value"],
        },
        handler=_tool_handler("camera_move_local"),
        preconditions=(require_engine_ready,),
    ),
    tool_from_schema(
        name="camera_look_at",
        description="Aim the camera at a point (sets camera center; keeps eye). Returns a typed camera value. Exactly one target mode is required: world_point, target_bbox_center, or bbox_fraction_point. base_value is required for determinism (pass a typed camera from camera_get/camera_focus/etc).",
        parameters_schema={
            "type": "object",
            "properties": {
                "world_point": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                    "description": "World-space target [x,y,z] to aim at (camera center).",
                },
                "target_bbox_center": {
                    "type": "boolean",
                    "description": "When true, aim at the bbox center of ids (or all visual objects).",
                },
                "bbox_fraction_point": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                    "description": "Fractions [fx,fy,fz] in [0..1] inside the target bbox of ids (or all visual objects).",
                },
                "ids": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Optional target ids for bbox computations. Empty → all visual objects.",
                },
                "after_clipping": {
                    "type": "boolean",
                    "default": True,
                    "description": "Use clipped bbox (true) or full bbox (false) for bbox-derived targets.",
                },
                "base_value": {
                    **CAMERA_TYPED_VALUE_SCHEMA,
                    "description": "Required typed camera value to aim from.",
                },
            },
            "required": ["base_value"],
        },
        handler=_tool_handler("camera_look_at"),
        preconditions=(require_engine_ready,),
    ),
    tool_from_schema(
        name="camera_path_solve",
        description=(
            "Solve typed camera keys from waypoints (does NOT write keys). Waypoints may specify eye and/or look_at in world coords or bbox fractions. "
            "If look_at is omitted for a waypoint, the solver preserves the previous view direction + center distance.\n\n"
            "For smoother motion from sparse waypoints, provide additional waypoints or sample intermediate keys before writing."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "ids": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Optional target ids for bbox computations (fractions/center). Empty → all visual objects.",
                },
                "after_clipping": {
                    "type": "boolean",
                    "default": True,
                    "description": "Use clipped bbox (true) or full bbox (false) for bbox-derived points.",
                },
                "base_value": {
                    **CAMERA_TYPED_VALUE_SCHEMA,
                    "description": "Required typed camera value used as defaults for projection/fov/up and for the initial direction when look_at is omitted.",
                },
                "waypoints": {
                    "type": "array",
                    "items": CAMERA_PATH_WAYPOINT_SCHEMA,
                    "description": "Waypoints: [{time, eye?, look_at?}]. Prefer bbox_fraction coordinates for dataset-scale invariant paths.",
                },
            },
            "required": ["base_value", "waypoints"],
        },
        handler=_tool_handler("camera_path_solve"),
        preconditions=(require_engine_ready,),
    ),
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

    if name == "camera_get":
        try:
            val = client.camera_get()
            return json.dumps({"ok": True, "value": val})
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

    if name == "camera_move_local":
        try:
            op = str(args.get("op"))
            distance = float(args.get("distance", 0.0))
            frac = bool(args.get("distance_is_fraction_of_bbox_radius", True))
            ids = args.get("ids") or []
            ac = bool(args.get("after_clipping", True))
            move_center = bool(args.get("move_center", True))
            base_value = args.get("base_value")
            if not isinstance(base_value, dict) or not base_value:
                return json.dumps(
                    {"ok": False, "error": "camera_move_local: base_value is required"}
                )
            val = client.camera_move_local(
                op=op,
                distance=distance,
                distance_is_fraction_of_bbox_radius=frac,
                ids=ids,
                after_clipping=ac,
                move_center=move_center,
                base_value=base_value,
            )
            return json.dumps({"ok": True, "value": val})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "camera_look_at":
        try:
            ids = args.get("ids") or []
            ac = bool(args.get("after_clipping", True))
            base_value = args.get("base_value")
            if not isinstance(base_value, dict) or not base_value:
                return json.dumps(
                    {"ok": False, "error": "camera_look_at: base_value is required"}
                )

            world_point = args.get("world_point")
            bbox_fraction_point = args.get("bbox_fraction_point")
            bbox_center = bool(args.get("target_bbox_center", False))

            wp = None
            if isinstance(world_point, list) and len(world_point) == 3:
                wp = (
                    float(world_point[0]),
                    float(world_point[1]),
                    float(world_point[2]),
                )
            bfp = None
            if isinstance(bbox_fraction_point, list) and len(bbox_fraction_point) == 3:
                bfp = (
                    float(bbox_fraction_point[0]),
                    float(bbox_fraction_point[1]),
                    float(bbox_fraction_point[2]),
                )

            val = client.camera_look_at(
                world_point=wp,
                target_bbox_center=bbox_center,
                bbox_fraction_point=bfp,
                ids=ids,
                after_clipping=ac,
                base_value=base_value,
            )
            return json.dumps({"ok": True, "value": val})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "camera_path_solve":
        try:
            ids = args.get("ids") or []
            ac = bool(args.get("after_clipping", True))
            base_value = args.get("base_value")
            if not isinstance(base_value, dict) or not base_value:
                return json.dumps(
                    {"ok": False, "error": "camera_path_solve: base_value is required"}
                )
            waypoints = args.get("waypoints") or []
            keys = client.camera_path_solve(
                ids=ids,
                after_clipping=ac,
                base_value=base_value,
                waypoints=waypoints if isinstance(waypoints, list) else [],
            )
            return json.dumps({"ok": True, "keys": keys})
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
            if not isinstance(base_value, dict) or not base_value:
                return json.dumps(
                    {"ok": False, "error": "camera_rotate: base_value is required"}
                )

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
                return json.dumps({"ok": True, "value": last_val})

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
