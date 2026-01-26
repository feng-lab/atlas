import glob
import json
import math
import os
import tempfile
from pathlib import Path
from typing import Any, Dict, List

from ...camera_director_templates import expand_walkthrough_segments
from ...describe import load_animation, load_capabilities, summarize_animation
from ...discovery import (
    compute_paths_from_atlas_dir,
    default_install_dirs,
    discover_schema_dir,
)
from ...easing import normalize_easing_name
from ...tool_registry import Tool, tool_from_schema

# Fail-fast for internal exporter helpers
from ...exporter import export_video, preview_frames
from .context import ToolDispatchContext
from .preconditions import (
    require_animation_id,
    require_engine_ready,
    require_screenshot_consent,
)
from .schemas_camera_value import (
    CAMERA_CONSTRAINTS_SCHEMA,
    CAMERA_POLICIES_SCHEMA,
    CAMERA_TYPED_VALUE_SCHEMA,
)

JSON_VALUE_SCHEMA: Dict[str, Any] = {
    "description": "Native JSON value (supports object/array/scalars; nested structures allowed).",
    # Use anyOf rather than a "type": [...] union. Some providers validate a more
    # restrictive subset of JSON Schema and require explicit items for any
    # schema that can be an array.
    "anyOf": [
        {"type": "object"},
        {
            "type": "array",
            "items": {"type": ["string", "number", "boolean", "null", "object"]},
        },
        {"type": "number"},
        {"type": "string"},
        {"type": "boolean"},
        {"type": "null"},
    ],
}

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

CAMERA_SOLVE_PARAMS_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "description": (
        "Mode-specific camera solve parameters.\n"
        "ORBIT: axis ('x'|'y'|'z').\n"
        "DOLLY: start_dist and end_dist (absolute eye-to-center distance). At least one must be > 0."
    ),
    "properties": {
        "axis": {
            "type": "string",
            "enum": ["x", "y", "z"],
            "description": "ORBIT: orbit axis.",
        },
        "start_dist": {
            "type": "number",
            "description": (
                "DOLLY: start distance from camera eye to center. Must be finite; >0 sets an explicit start distance. "
                "<=0 means keep the base camera distance."
            ),
        },
        "end_dist": {
            "type": "number",
            "description": (
                "DOLLY: end distance from camera eye to center. Must be finite; >0 sets an explicit end distance. "
                "<=0 means keep the base camera distance."
            ),
        },
    },
}

# When a camera key uses easing="Switch", the animation becomes a step function at
# that key time (a jump cut). That is occasionally desirable, but in practice it is
# easy for an agent to accidentally overwrite a boundary key (e.g., the end of an
# ORBIT segment) and create a jarring camera “reset”.
#
# This threshold is a *tool-level guardrail* (not an engine framing constraint):
# if a Switch key would move the camera too far from the currently evaluated
# timeline camera at that time, we require an explicit allow_jump_cut=true.
#
# The unit is a fraction of the reference eye↔center distance (scale-invariant).
DEFAULT_MAX_SWITCH_CAMERA_JUMP_FRACTION = 0.10

CAMERA_WAYPOINT_EYE_SCHEMA: Dict[str, Any] = {
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

CAMERA_WAYPOINT_LOOK_AT_SCHEMA: Dict[str, Any] = {
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

CAMERA_SPLINE_WAYPOINT_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": (
        "Spline waypoint for camera solving.\n"
        "Timing: provide either time (seconds) or u (0..1) and set the other to null.\n"
        "Position: eye and look_at may be omitted (null). When look_at is omitted, look_at_policy controls filling."
    ),
    "properties": {
        "time": {
            "type": ["number", "null"],
            "description": "Absolute time (seconds) for this waypoint. Use null when using u.",
        },
        "u": {
            "type": ["number", "null"],
            "description": "Normalized time in [0,1] within [t0,t1]. Use null when using time.",
        },
        "eye": {
            "type": ["object", "null"],
            "description": "Optional: eye position (world or bbox_fraction).",
            "properties": CAMERA_WAYPOINT_EYE_SCHEMA["properties"],
        },
        "look_at": {
            "type": ["object", "null"],
            "description": "Optional: look-at target (world / bbox_center / bbox_fraction).",
            "properties": CAMERA_WAYPOINT_LOOK_AT_SCHEMA["properties"],
        },
    },
}

WALKTHROUGH_MOVE_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": (
        "Local move directive in bbox-radius fractions. Keys may be omitted or null. "
        "Supported keys: forward, back, right, left, up, down."
    ),
    "properties": {
        "forward": {"type": ["number", "null"]},
        "back": {"type": ["number", "null"]},
        "right": {"type": ["number", "null"]},
        "left": {"type": ["number", "null"]},
        "up": {"type": ["number", "null"]},
        "down": {"type": ["number", "null"]},
    },
}

WALKTHROUGH_ROTATE_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": "Local rotation directive in degrees. Keys may be omitted or null. Supported keys: yaw, pitch, roll.",
    "properties": {
        "yaw": {"type": ["number", "null"]},
        "pitch": {"type": ["number", "null"]},
        "roll": {"type": ["number", "null"]},
    },
}

WALKTHROUGH_SEGMENT_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": (
        "Walkthrough segment.\n"
        "Timing: use exactly one of (u0/u1) OR (duration) OR neither (equal split).\n"
        "Motion: use move and/or rotate, or pause=true.\n"
        "Templates: optional internal templates ('enter', 'turn_right', 'pause') expand into concrete move/rotate."
    ),
    "properties": {
        "u0": {
            "type": ["number", "null"],
            "description": "Normalized start time in [0,1] (requires u1).",
        },
        "u1": {
            "type": ["number", "null"],
            "description": "Normalized end time in [0,1] (requires u0).",
        },
        "duration": {
            "type": ["number", "null"],
            "description": "Segment duration in seconds (relative; normalized across segments).",
        },
        "pause": {
            "type": ["boolean", "null"],
            "description": "When true, hold pose for the segment duration.",
        },
        "move": {
            "type": ["object", "null"],
            "properties": WALKTHROUGH_MOVE_SCHEMA["properties"],
        },
        "rotate": {
            "type": ["object", "null"],
            "properties": WALKTHROUGH_ROTATE_SCHEMA["properties"],
        },
        "template": {
            "type": ["string", "null"],
            "description": "Optional internal template: enter|turn_right|pause (plus aliases). Null disables templating.",
        },
        "amount": {
            "type": ["string", "number", "null"],
            "description": "Optional template amount (e.g., 'small'|'medium' or a numeric override).",
        },
        "distance": {
            "type": ["number", "null"],
            "description": "Optional template distance override (bbox-radius fraction).",
        },
        "degrees": {
            "type": ["number", "null"],
            "description": "Optional template degrees override.",
        },
        "label": {
            "type": ["string", "null"],
            "description": "Optional label for debugging/inspection.",
        },
    },
}

ANIMATION_ID_PARAM_SCHEMA: Dict[str, Any] = {
    "type": "integer",
    "description": "Animation3D object id. Use animation_ensure_animation to create/select an animation and obtain this id.",
}

ANIMATION_BATCH_SET_KEY_ENTRY_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": (
        "SetKey entry. Writes one timeline key for a non-camera parameter.\n"
        "Fields:\n"
        "- id: target id (1=background, 2=axis, 3=global, ≥4=object ids)\n"
        "- json_key: parameter json_key (resolve via scene_list_params)\n"
        "- time: seconds\n"
        "- value: typed JSON value for the parameter\n"
        "- easing: easing type (Qt/QEasingCurve name, e.g. Linear/InOutQuad/Switch)"
    ),
    "properties": {
        "id": {
            "type": "integer",
            "description": "Target id (non-camera): 1=background, 2=axis, 3=global, ≥4=object ids",
        },
        "json_key": {
            "type": "string",
            "description": "Parameter json_key for the target id",
        },
        "time": {"type": "number", "description": "Key time (seconds)"},
        "value": dict(JSON_VALUE_SCHEMA),
        "easing": {
            "type": "string",
            "default": "Linear",
            "description": "Easing type (Qt/QEasingCurve name, e.g. Linear/InOutQuad/Switch).",
        },
    },
    "required": ["id", "json_key", "time", "value"],
}

ANIMATION_BATCH_REMOVE_KEY_ENTRY_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "description": (
        "RemoveKey entry. Removes one timeline key for a non-camera parameter.\n"
        "Fields:\n"
        "- id: target id (1=background, 2=axis, 3=global, ≥4=object ids)\n"
        "- json_key: parameter json_key\n"
        "- time: seconds"
    ),
    "properties": {
        "id": {
            "type": "integer",
            "description": "Target id (non-camera): 1=background, 2=axis, 3=global, ≥4=object ids",
        },
        "json_key": {
            "type": "string",
            "description": "Parameter json_key for the target id",
        },
        "time": {"type": "number", "description": "Key time (seconds)"},
    },
    "required": ["id", "json_key", "time"],
}


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


def _normalize_easing_name(raw: Any) -> str:
    return normalize_easing_name(raw)


TOOLS: List[Tool] = [
    tool_from_schema(
        name="animation_describe_file",
        description="Parse an .animation3d file and return a concise natural-language summary (uses capabilities.json for names).",
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to a .animation3d file.",
                },
                "schema_dir": {
                    "type": ["string", "null"],
                    "description": "Optional schema directory override for capabilities.json lookups.",
                },
                "style": {
                    "type": "string",
                    "enum": ["short", "long"],
                    "default": "short",
                    "description": "Summary style.",
                },
            },
            "required": ["path"],
        },
        preconditions=(),
        handler=_tool_handler("animation_describe_file"),
    ),
    tool_from_schema(
        name="animation_camera_solve_and_apply",
        description=(
            "Timeline camera solver (writes keys): generate validated camera keys using FIT|ORBIT|DOLLY|STATIC.\n\n"
            "This tool first sets the engine timeline time to t0 before solving so chained segments stay continuous.\n\n"
            "Use when:\n"
            "- FIT: establish a good starting frame for ids (presentation framing).\n"
            "- ORBIT: rotate around the subject (exterior orbit / turntable shots).\n"
            "- DOLLY: zoom/dolly in or out while keeping the subject framed.\n"
            "- STATIC: hold the current pose.\n\n"
            "Avoid when:\n"
            "- You have explicit spatial beats (A→B→C coordinates) → use animation_camera_waypoint_spline_apply.\n"
            "- You want first-person motion verbs (fly/turn/pause) → use animation_camera_walkthrough_apply.\n\n"
            "Primary knobs:\n"
            "- Smoothness: ORBIT uses max_step_degrees (smaller → more keys → smoother). DOLLY uses more keys/windows.\n"
            "- Framing vs exploration: constraints.keep_visible=true for exterior presentation; false for interior flythroughs.\n"
            "- Timing feel: easing changes per-key timing curves only (Qt/QEasingCurve names like Linear/InOutQuad/Switch).\n\n"
            "If the result looks wrong:\n"
            "- ORBIT: usually wrong ids/target selection, or keys too sparse → lower max_step_degrees.\n"
            "- Subject looks too small / too much empty space: you're likely solving against too many ids (whole scene) "
            "and/or using a large margin. For close-ups, solve/validate against only the highlighted ids and use "
            "constraints.min_frame_coverage (tight framing) while keeping margin small.\n"
            "- DOLLY: requires params.start_dist/end_dist (>0). If you don't know absolute distances, prefer walkthrough "
            "with look_at_policy='bbox_center' and bbox-scaled move.forward/back. If you wanted an arc (move+rotate), "
            "use waypoints/walkthrough instead of DOLLY.\n\n"
            "Camera interpolation is always evaluated using a stable look-at + distance convention; easing is separate."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "mode": {
                    "type": "string",
                    "enum": ["FIT", "ORBIT", "DOLLY", "STATIC"],
                    "description": "Solve mode for generating camera keys.",
                },
                "ids": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": (
                        "Target ids to frame. Empty uses fit_candidates() (typically 'all visual objects'), which is best for establishing shots. "
                        "For close-ups/highlight beats, pass only the relevant object ids so the camera can frame tightly."
                    ),
                },
                "t0": {
                    "type": "number",
                    "description": "Start time (seconds) of the write window.",
                },
                "t1": {
                    "type": "number",
                    "description": "End time (seconds) of the write window.",
                },
                "constraints": {
                    **CAMERA_CONSTRAINTS_SCHEMA,
                    "description": (
                        "Framing constraints. Typical defaults: keep_visible=true for exterior presentation (no cropping). "
                        "For close-ups, set min_frame_coverage>0 (tighter framing) and keep margin small. For interior flythroughs, "
                        "set keep_visible=false."
                    ),
                },
                "params": {
                    **CAMERA_SOLVE_PARAMS_SCHEMA,
                    "description": "Mode-specific parameters (ORBIT: axis; DOLLY: start_dist/end_dist).",
                },
                "degrees": {
                    "type": "number",
                    "description": "ORBIT: total rotation in degrees (default 360).",
                },
                "max_step_degrees": {
                    "type": "number",
                    "description": "ORBIT: maximum degrees per solver step (controls key density). Smaller values produce more keys and smoother motion. Default 90.",
                },
                "tolerance": {
                    "type": "number",
                    "default": 1e-3,
                    "description": "Time tolerance used when clearing/replacing keys.",
                },
                "easing": {
                    "type": "string",
                    "default": "Linear",
                    "description": "Key easing type (Qt/QEasingCurve name, e.g., Linear/InOutQuad/Switch). This affects per-key timing curves and is separate from camera interpolation.",
                },
                "clear_range": {
                    "type": "boolean",
                    "default": True,
                    "description": "Remove existing camera keys inside [t0,t1] (within tolerance) before applying new keys.",
                },
            },
            "required": ["animation_id", "mode", "ids", "t0", "t1"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_camera_solve_and_apply"),
    ),
    # Camera interpolation method tools are intentionally disabled for now.
    # Camera interpolation is evaluated using a stable look-at + distance convention.
    tool_from_schema(
        name="animation_camera_waypoint_spline_apply",
        description=(
            "Guided waypoint camera path (timeline; writes keys): solve camera keys from explicit bbox/world waypoints.\n\n"
            "Required input:\n"
            "- base_value: typed camera used as defaults for projection/fov/up and for the initial direction when look_at is omitted.\n"
            "  Tip: use animation_camera_sample(animation_id,[t0]) to sample from the timeline.\n\n"
            "Use when:\n"
            "- The user provides explicit points/waypoints (world coords or bbox fractions) or clear spatial beats (A→B→C).\n\n"
            "Avoid when:\n"
            "- The user describes motion verbs (fly forward, strafe, yaw/pitch, pause) → use animation_camera_walkthrough_apply.\n"
            "- The goal is a clean exterior orbit around a subject → use animation_camera_solve_and_apply(mode='ORBIT').\n\n"
            "Primary knobs:\n"
            "- Aim behavior: look_at_policy controls how missing look_at is filled.\n"
            "  - preserve_direction (default): keeps the current view direction + distance when look_at is omitted.\n"
            "  - bbox_center: fills missing look_at to keep tracking the bbox center (third-person track/orbit-like).\n"
            '- Smoothness: add intermediate waypoints (spatial key density). For continuous "drone" motion, use walkthrough (step_seconds).\n'
            "- Timing feel: easing changes per-key timing curves only.\n\n"
            "If the result looks wrong:\n"
            "- Path drifts instead of tracking the subject → set look_at_policy='bbox_center' or provide explicit waypoint look_at.\n"
            "- Motion is too sharp between points → add intermediate waypoints.\n\n"
            "Camera interpolation is always evaluated using a stable look-at + distance convention; easing is separate."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "ids": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Target ids for bbox computations and validation. Empty → fit_candidates() for validation; bbox computations use all visual objects.",
                },
                "after_clipping": {
                    "type": "boolean",
                    "default": True,
                    "description": "Use clipped bbox for bbox-fraction waypoints.",
                },
                "t0": {"type": "number", "description": "Start time (seconds)."},
                "t1": {"type": "number", "description": "End time (seconds)."},
                "base_value": {
                    **CAMERA_TYPED_VALUE_SCHEMA,
                    "description": "Required base camera value used as defaults for projection/fov/up and for the initial direction when look_at is omitted. Tip: use animation_camera_sample(animation_id,[t0]) to sample from the timeline.",
                },
                "waypoints": {
                    "type": "array",
                    "items": CAMERA_SPLINE_WAYPOINT_SCHEMA,
                    "description": "Waypoints (time or u + optional eye/look_at). Prefer bbox_fraction coordinates for dataset-scale invariant paths.",
                },
                "look_at_policy": {
                    "type": "string",
                    "enum": ["preserve_direction", "bbox_center"],
                    "default": "preserve_direction",
                    "description": "How to handle waypoints that omit look_at. preserve_direction keeps the current view direction; bbox_center fills missing look_at with bbox_center:true.",
                },
                "easing": {
                    "type": "string",
                    "default": "Linear",
                    "description": "Key easing type (Qt/QEasingCurve name, e.g., Linear/InOutQuad/Switch). This does not change the waypoint geometry; it only affects per-key transition timing.",
                },
                "clear_range": {
                    "type": "boolean",
                    "default": True,
                    "description": "Remove existing camera keys inside [t0,t1] (tolerance-aware) before applying new keys.",
                },
                "tolerance": {
                    "type": "number",
                    "default": 1e-3,
                    "description": "Time tolerance used when clearing/replacing keys.",
                },
                "constraints": {
                    **CAMERA_CONSTRAINTS_SCHEMA,
                    "description": "Camera validation constraints. For interior walkthroughs, set keep_visible=false (disables framing validation).",
                },
            },
            "required": ["animation_id", "t0", "t1", "waypoints", "base_value"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_camera_waypoint_spline_apply"),
    ),
    tool_from_schema(
        name="animation_camera_walkthrough_apply",
        description=(
            "First-person walkthrough authoring (timeline; writes keys): build a smooth camera path from motion segments "
            "(local moves + yaw/pitch/roll), optionally clear existing camera keys in [t0,t1], then write validated camera keys.\n\n"
            "Required input:\n"
            "- base_value: typed camera used as the initial camera pose (projection/fov/up defaults).\n"
            "  Tip: use animation_camera_sample(animation_id,[t0]) to sample from the timeline.\n\n"
            "Use when:\n"
            "- The user describes motion verbs rather than explicit points: fly forward, strafe, turn, look around, pause.\n"
            '- Interior exploration (moving inside a volume/mesh) where "keep the whole bbox visible" is not desired.\n\n'
            "Avoid when:\n"
            "- The user gives explicit waypoints/coordinates → use animation_camera_waypoint_spline_apply.\n"
            "- The goal is a clean exterior turntable orbit → use animation_camera_solve_and_apply(mode='ORBIT').\n\n"
            "Primary knobs:\n"
            "- Smoothness: step_seconds (smaller → more sampled keys → smoother curved motion).\n"
            "- Framing vs exploration: constraints.keep_visible=false for interior flythroughs; true only when explicitly requested.\n"
            "- Aim behavior: look_at_policy controls first-person vs third-person tracking semantics.\n\n"
            "If the result looks wrong:\n"
            '- Interior path keeps "popping out" due to framing constraints → set constraints.keep_visible=false.\n'
            "- You wanted the camera to keep aiming at the subject → set look_at_policy='bbox_center'.\n\n"
            "Camera interpolation is always evaluated using a stable look-at + distance convention; easing is separate."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "ids": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Target ids for bbox computations and validation. Empty → fit_candidates() for validation; bbox computations use all visual objects.",
                },
                "after_clipping": {
                    "type": "boolean",
                    "default": True,
                    "description": "Use clipped bbox for bbox-scaled movement.",
                },
                "t0": {"type": "number", "description": "Start time (seconds)."},
                "t1": {"type": "number", "description": "End time (seconds)."},
                "base_value": {
                    **CAMERA_TYPED_VALUE_SCHEMA,
                    "description": "Required base camera value used as the initial camera pose (projection/fov/up defaults). Tip: use animation_camera_sample(animation_id,[t0]) to sample from the timeline.",
                },
                "segments": {
                    "type": "array",
                    "items": WALKTHROUGH_SEGMENT_SCHEMA,
                    "description": "Walkthrough segments (timed by u0/u1, duration, or equal split). Move distances are bbox-radius fractions; rotations are degrees.",
                },
                "step_seconds": {
                    "type": "number",
                    "default": 1.0,
                    "description": "Sampling step used to approximate motion inside each segment. Smaller → more keys (smoother curved motion).",
                },
                "look_at_policy": {
                    "type": "string",
                    "enum": ["preserve_direction", "bbox_center"],
                    "default": "preserve_direction",
                    "description": "preserve_direction keeps first-person yaw/pitch look control; bbox_center keeps aiming at the target bbox center (third-person track), interpreting yaw/pitch as azimuth/elevation around the center.",
                },
                "easing": {
                    "type": "string",
                    "default": "Linear",
                    "description": "Key easing type (Qt/QEasingCurve name, e.g., Linear/InOutQuad/Switch). This affects per-key timing, not the sampled path geometry.",
                },
                "clear_range": {
                    "type": "boolean",
                    "default": True,
                    "description": "Remove existing camera keys inside [t0,t1] (tolerance-aware) before applying new keys.",
                },
                "tolerance": {
                    "type": "number",
                    "default": 1e-3,
                    "description": "Time tolerance used when clearing/replacing keys.",
                },
                "constraints": {
                    **CAMERA_CONSTRAINTS_SCHEMA,
                    "description": "Camera validation constraints. For interior walkthroughs, set keep_visible=false (disables framing validation).",
                },
            },
            "required": ["animation_id", "t0", "t1", "segments", "base_value"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_camera_walkthrough_apply"),
    ),
    tool_from_schema(
        name="animation_camera_validate",
        description=(
            "Animation timeline: validate camera values against framing constraints (keep_visible, margin, min_frame_coverage).\n\n"
            "Notes:\n"
            "- Validate against the ids you actually intend to frame. Validating against the whole scene produces wide shots.\n"
            "- min_frame_coverage is a screen-space framing metric (dominant-dimension fill). Higher values push toward tighter framing.\n\n"
            "Values are optional; when omitted, the server samples the current animation camera at the given times."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "ids": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": (
                        "Target ids to validate against. For establishing shots, this can be fit_candidates() (all visual objects). "
                        "For close-ups/highlight beats, validate against only the ids you intend to frame; validating the whole scene forces wide shots."
                    ),
                },
                "times": {
                    "type": "array",
                    "items": {"type": "number"},
                    "description": "Times (seconds) to validate.",
                },
                "values": {
                    "type": "array",
                    "items": CAMERA_TYPED_VALUE_SCHEMA,
                    "description": "Optional: typed camera values aligned with times. If omitted or shorter than times, the server fills by sampling.",
                },
                "constraints": {
                    **CAMERA_CONSTRAINTS_SCHEMA,
                    "description": "Framing constraints (keep_visible, margin, min_frame_coverage).",
                },
                "policies": {
                    **CAMERA_POLICIES_SCHEMA,
                    "description": "Adjustment policies (adjust_distance).",
                },
            },
            "required": ["animation_id", "ids", "times"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_camera_validate"),
    ),
    tool_from_schema(
        name="animation_camera_sample",
        description=(
            "Animation timeline: sample the evaluated camera value from the animation at specific time(s). "
            "Returns typed camera values (no key writes, no validation, does not change engine time). "
            "Use this to get a deterministic base_value for camera_rotate/camera_move_local/camera_look_at while authoring."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "times": {
                    "type": "array",
                    "items": {"type": "number"},
                    "description": "One or more times (seconds) to sample.",
                },
            },
            "required": ["animation_id", "times"],
        },
        preconditions=(require_animation_id,),
        handler=_tool_handler("animation_camera_sample"),
    ),
    tool_from_schema(
        name="animation_set_param_by_name",
        description="Set a parameter by display name (case-insensitive) by id. Resolves json_key via scene_list_params, then calls animation_set_key_param. Id map: 1=background, 2=axis, 3=global, ≥4=objects.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "name": {"type": "string"},
                "type_hint": {"type": "string"},
                "time": {"type": "number"},
                "easing": {"type": "string", "default": "Linear"},
                "value": {
                    "description": 'Native JSON value. For composite params like 3DTransform, pass an object with canonical subfields (e.g., {"Translation Vec3":[x,y,z],"Rotation Vec4":[ang,x,y,z],"Scale Vec3":[sx,sy,sz],"Rotation Center Vec3":[cx,cy,cz]}).',
                    "type": ["object", "array", "number", "string", "boolean", "null"],
                    "items": {"type": ["string", "number", "boolean", "null"]},
                },
            },
            "required": ["animation_id", "id", "name", "time", "value"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_set_param_by_name"),
    ),
    tool_from_schema(
        name="animation_remove_key_param_at_time",
        description="Remove one or more keys near a time for a parameter by json_key and id. Uses a tolerance window to match existing keys.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {"type": "string"},
                "time": {"type": "number"},
                "tolerance": {"type": "number", "default": 1e-3},
            },
            "required": ["animation_id", "id", "json_key", "time"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_remove_key_param_at_time"),
    ),
    tool_from_schema(
        name="animation_replace_key_param",
        description="Replace (or set) a parameter key by json_key at time by id: remove any key within tolerance then set a new typed value.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {"type": "string"},
                "time": {"type": "number"},
                "easing": {"type": "string", "default": "Linear"},
                "value": JSON_VALUE_SCHEMA,
                "tolerance": {"type": "number", "default": 1e-3},
            },
            "required": ["animation_id", "id", "json_key", "time", "value"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_replace_key_param"),
    ),
    tool_from_schema(
        name="animation_replace_key_camera",
        description="Replace (or set) a camera key at time: remove any camera key within tolerance then set a new camera value. Use for explicit single-time edits (small tweaks or intentional jump-cuts). For holds, prefer animation_camera_solve_and_apply(mode='STATIC') so the hold continues the timeline pose at t0. Using easing='Switch' creates an instantaneous cut; avoid unless intentional.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "time": {"type": "number"},
                "easing": {
                    "type": "string",
                    "default": "Linear",
                    "description": "Key easing type (Qt/QEasingCurve name, e.g., Linear/InOutQuad/Switch). This affects per-key timing curves and is separate from camera interpolation.",
                },
                "allow_jump_cut": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "When easing='Switch' (a jump cut), require explicit opt-in to write a key that differs "
                        "substantially from the currently evaluated camera at that time. Set true only when an "
                        "instantaneous cut is intentional."
                    ),
                },
                "max_switch_jump_fraction": {
                    "type": "number",
                    "default": DEFAULT_MAX_SWITCH_CAMERA_JUMP_FRACTION,
                    "description": (
                        "Continuity guardrail for easing='Switch' when allow_jump_cut=false. If the new camera key "
                        "would change the camera by more than this fraction of the eye↔center distance (scale-invariant), "
                        "the tool rejects the write. Set to 0 to require near-identical values for Switch keys."
                    ),
                },
                "value": {
                    "description": ("Typed camera value."),
                    **CAMERA_TYPED_VALUE_SCHEMA,
                },
                "ids": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Optional ids for camera validation. When omitted/empty, uses fit_candidates().",
                },
                "constraints": {
                    **CAMERA_CONSTRAINTS_SCHEMA,
                    "description": "Optional camera validation constraints. When omitted, defaults to keep_visible=true and min_frame_coverage=0.0.",
                },
                "tolerance": {"type": "number", "default": 1e-3},
            },
            "required": ["animation_id", "time", "value"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_replace_key_camera"),
    ),
    tool_from_schema(
        name="animation_list_keys",
        description="Timeline only: list animation keys by id. Requires an existing Animation3D. Id map: 0=camera, 1=background, 2=axis, 3=global, ≥4=objects. For camera (id=0) json_key is ignored. Do NOT use for scene-only verification — read current camera via scene_get_values(id=0, 'Camera 3DCamera').",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {
                    "type": "string",
                    "description": "Parameter json_key (ignored for camera); use canonical names from scene_list_params(id)",
                },
                "include_values": {
                    "type": "boolean",
                    "description": "True to include value_json samples for each key",
                },
            },
            "required": ["animation_id", "id", "json_key", "include_values"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_list_keys"),
    ),
    tool_from_schema(
        name="animation_clear_keys_range",
        description="Remove keys within [t0,t1] (inclusive, tolerance-aware) for a specific track. Camera uses id=0 and ignores json_key. Non-camera uses (id,json_key) and requires an existing Animation3D.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {
                    "type": "string",
                    "description": "Canonical json_key (ignored for camera).",
                },
                "name": {
                    "type": "string",
                    "description": "Optional display name to resolve to json_key when json_key is not provided (ignored for camera).",
                },
                "t0": {"type": "number", "description": "Range start time (seconds)."},
                "t1": {"type": "number", "description": "Range end time (seconds)."},
                "tolerance": {
                    "type": "number",
                    "default": 1e-3,
                    "description": "Time tolerance used for range boundary inclusion and conflict matching.",
                },
                "include_times": {
                    "type": "boolean",
                    "default": False,
                    "description": "When true, include the full list of removed key times (no truncation).",
                },
            },
            "required": ["animation_id", "id", "t0", "t1"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_clear_keys_range"),
    ),
    tool_from_schema(
        name="animation_shift_keys_range",
        description="Shift keys within [t0,t1] by delta seconds (preserves value and easing). Uses a saved .animation3d snapshot to preserve easing. Conflict policy: error|overwrite|skip.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {
                    "type": "string",
                    "description": "Canonical json_key (ignored for camera).",
                },
                "name": {
                    "type": "string",
                    "description": "Optional display name to resolve to json_key when json_key is not provided (ignored for camera).",
                },
                "t0": {"type": "number", "description": "Range start time (seconds)."},
                "t1": {"type": "number", "description": "Range end time (seconds)."},
                "delta": {
                    "type": "number",
                    "description": "Time shift in seconds (can be negative).",
                },
                "tolerance": {
                    "type": "number",
                    "default": 1e-3,
                    "description": "Time tolerance used for range boundary inclusion and conflict matching.",
                },
                "on_conflict": {
                    "type": "string",
                    "enum": ["error", "overwrite", "skip"],
                    "default": "error",
                    "description": "If shifted keys land on existing key times: error (abort), overwrite (remove existing keys), or skip (leave conflicting keys unmoved).",
                },
                "include_times": {
                    "type": "boolean",
                    "default": False,
                    "description": "When true, include the full list of moved/skipped mappings (no truncation).",
                },
            },
            "required": ["animation_id", "id", "t0", "t1", "delta"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_shift_keys_range"),
    ),
    tool_from_schema(
        name="animation_scale_keys_range",
        description="Scale key times within [t0,t1] around an anchor (t0|center). Preserves value and easing via a saved .animation3d snapshot. Conflict policy: error|overwrite|skip.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {
                    "type": "string",
                    "description": "Canonical json_key (ignored for camera).",
                },
                "name": {
                    "type": "string",
                    "description": "Optional display name to resolve to json_key when json_key is not provided (ignored for camera).",
                },
                "t0": {"type": "number", "description": "Range start time (seconds)."},
                "t1": {"type": "number", "description": "Range end time (seconds)."},
                "scale": {"type": "number", "description": "Scale factor (>0)."},
                "anchor": {
                    "type": "string",
                    "enum": ["t0", "center"],
                    "default": "t0",
                    "description": "Anchor mode for scaling: t0 uses the range start; center uses (t0+t1)/2.",
                },
                "tolerance": {
                    "type": "number",
                    "default": 1e-3,
                    "description": "Time tolerance used for range boundary inclusion and conflict matching.",
                },
                "on_conflict": {
                    "type": "string",
                    "enum": ["error", "overwrite", "skip"],
                    "default": "error",
                    "description": "If scaled keys land on existing key times: error (abort), overwrite (remove existing keys), or skip (leave conflicting keys unmoved).",
                },
                "include_times": {
                    "type": "boolean",
                    "default": False,
                    "description": "When true, include the full list of moved/skipped mappings (no truncation).",
                },
            },
            "required": ["animation_id", "id", "t0", "t1", "scale"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_scale_keys_range"),
    ),
    tool_from_schema(
        name="animation_duplicate_keys_range",
        description="Duplicate/copy keys within [t0,t1] so they reappear starting at dest_t0 (preserves relative offsets, value, and easing). Uses a saved .animation3d snapshot to preserve easing. Conflict policy: error|overwrite|skip.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {
                    "type": "string",
                    "description": "Canonical json_key (ignored for camera).",
                },
                "name": {
                    "type": "string",
                    "description": "Optional display name to resolve to json_key when json_key is not provided (ignored for camera).",
                },
                "t0": {
                    "type": "number",
                    "description": "Source range start time (seconds).",
                },
                "t1": {
                    "type": "number",
                    "description": "Source range end time (seconds).",
                },
                "dest_t0": {
                    "type": "number",
                    "description": "Destination start time (seconds). Keys keep their relative offsets from the source range start.",
                },
                "tolerance": {
                    "type": "number",
                    "default": 1e-3,
                    "description": "Time tolerance used for range boundary inclusion and conflict matching.",
                },
                "on_conflict": {
                    "type": "string",
                    "enum": ["error", "overwrite", "skip"],
                    "default": "error",
                    "description": "If duplicated keys land on existing key times: error (abort), overwrite (remove existing keys), or skip (do not create conflicting duplicates).",
                },
                "include_times": {
                    "type": "boolean",
                    "default": False,
                    "description": "When true, include the full list of duplicated/skipped mappings (no truncation).",
                },
            },
            "required": ["animation_id", "id", "t0", "t1", "dest_t0"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_duplicate_keys_range"),
    ),
    tool_from_schema(
        name="animation_get_time",
        description="Animation (timeline): get current playback seconds and duration. Fails (ok=false) when no Animation3D exists yet; call animation_ensure_animation first.",
        parameters_schema={
            "type": "object",
            "properties": {"animation_id": dict(ANIMATION_ID_PARAM_SCHEMA)},
            "required": ["animation_id"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_get_time"),
    ),
    tool_from_schema(
        name="animation_ensure_animation",
        description=(
            "Ensure a 3D animation exists and is bound/selected for editing. Returns {ok, animation_id, created}.\n"
            "If this call created a new animation (response `created=true`; typically when `create_new=true` or when no Animation3D exists yet),\n"
            "Atlas captures a full-scene keyframe at t=0 (UI parity) so the animation starts from a fixed baseline (no scene fallback).\n"
            "Use the returned animation_id for all subsequent animation_* tool calls."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "create_new": {
                    "type": "boolean",
                    "default": False,
                    "description": "When true, create a new Animation3D object (instead of reusing an existing one).",
                },
                "name": {
                    "type": "string",
                    "default": "",
                    "description": "Optional display name for the created animation (empty means default).",
                },
            },
        },
        preconditions=(require_engine_ready,),
        handler=_tool_handler("animation_ensure_animation"),
    ),
    tool_from_schema(
        name="animation_set_duration",
        description="Set animation duration in seconds.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "seconds": {"type": "number"},
            },
            "required": ["animation_id", "seconds"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_set_duration"),
    ),
    tool_from_schema(
        name="animation_set_key_param",
        description="Add a parameter keyframe by id (0=camera unsupported here, ≥4 objects; 1/2/3 groups) with json_key and a native JSON value. Note on composite object types (e.g., 3DTransform): animation keys store a full value for that key. If you pass a partial object (e.g., {'Translation Vec3':[...]} only), omitted subfields will remain at defaults for that key (they do NOT automatically inherit the current scene). If you intend to 'change only one subfield but keep the rest', read the current value first (scene_get_values or existing key) and write a full object.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {
                    "type": "string",
                    "description": "Canonical json_key; if omitted, 'name' is used to resolve.",
                },
                "name": {
                    "type": "string",
                    "description": "Display name to resolve to json_key when json_key is not provided.",
                },
                "time": {"type": "number"},
                "easing": {"type": "string", "default": "Linear"},
                "value": JSON_VALUE_SCHEMA,
            },
            "required": ["animation_id", "id", "time", "value"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_set_key_param"),
    ),
    tool_from_schema(
        name="animation_replace_key_param_at_times",
        description=(
            "Replace (or set) keys at the specified times for a parameter by id "
            "(1=background,2=axis,3=global,≥4=objects). "
            "Behavior: for each requested time, remove any existing key within tolerance (if present) "
            "and then set a new key at exactly that time. This WILL create new keys when none existed."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {"type": "string"},
                "times": {"type": "array", "items": {"type": "number"}},
                "value": JSON_VALUE_SCHEMA,
                "easing": {"type": "string", "default": "Linear"},
                "tolerance": {"type": "number", "default": 1e-3},
            },
            "required": ["animation_id", "id", "json_key", "times", "value"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_replace_key_param_at_times"),
    ),
    tool_from_schema(
        name="animation_clear_keys",
        description="Clear all keys for a parameter or camera by id (0=camera).",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {"type": "string"},
            },
            "required": ["animation_id", "id"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_clear_keys"),
    ),
    tool_from_schema(
        name="animation_remove_key",
        description="Remove a key at a specific time for a parameter by id.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {
                    "type": "integer",
                    "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                },
                "json_key": {"type": "string"},
                "time": {"type": "number"},
            },
            "required": ["animation_id", "id", "json_key", "time"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_remove_key"),
    ),
    tool_from_schema(
        name="animation_batch",
        description="Batch multiple SetKey and RemoveKey operations atomically. Non-camera only (ids ≥ 1); do not include camera (id=0) keys here. For camera motion, use animation_camera_solve_and_apply.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "set_keys": {
                    "type": "array",
                    "items": dict(ANIMATION_BATCH_SET_KEY_ENTRY_SCHEMA),
                    "description": "List of SetKey operations (non-camera parameters).",
                },
                "remove_keys": {
                    "type": "array",
                    "items": dict(ANIMATION_BATCH_REMOVE_KEY_ENTRY_SCHEMA),
                    "description": "List of RemoveKey operations (non-camera parameters).",
                },
                "commit": {
                    "type": "boolean",
                    "default": True,
                    "description": "Commit immediately if true",
                },
            },
            "required": ["animation_id", "set_keys", "remove_keys"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_batch"),
    ),
    tool_from_schema(
        name="animation_set_time",
        description="Set current timeline time (seconds).",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "seconds": {"type": "number", "description": "Timeline seconds"},
                "cancel": {
                    "type": "boolean",
                    "default": False,
                    "description": "When true, cancel any in-progress rendering/export tied to the animation view.",
                },
            },
            "required": ["animation_id", "seconds"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_set_time"),
    ),
    tool_from_schema(
        name="animation_save_keyframe",
        description=(
            "Capture a full-scene keyframe at the given time (UI 'Save Key Frame' parity):\n"
            "- Writes keys for all parameters (including camera) at `time`.\n"
            "- Use this to ensure the animation defines the FULL state (avoid scene fallback).\n"
            "- Useful as an authoring workflow to consider: set up the scene at a beat time, call this tool, repeat for other beats, then rely on interpolation.\n"
            "- Especially important after loading/adding new objects while authoring an animation: call at time=0 to seed their baseline keys."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "time": {"type": "number", "description": "Keyframe time (seconds)."},
                "cancel_rendering": {
                    "type": "boolean",
                    "default": True,
                    "description": "When true, cancel any in-progress long rendering before applying time.",
                },
            },
            "required": ["animation_id", "time"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_save_keyframe"),
    ),
    tool_from_schema(
        name="animation_save_animation",
        description="Save the current animation to a .animation3d path.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "path": {"type": "string"},
            },
            "required": ["animation_id", "path"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_save_animation"),
    ),
    tool_from_schema(
        name="animation_export_video",
        description="Export the current Animation3D to MP4 using the Atlas headless exporter. This tool saves the live animation (animation_id) to a temporary .animation3d file first. Note: video export is slower/heavier than previews; prefer scene_screenshot or animation_render_preview for verification.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "out": {"type": "string", "description": "Output .mp4 path"},
                "fps": {
                    "type": "number",
                    "default": 30,
                    "description": "Frames per second",
                },
                "start": {
                    "type": "integer",
                    "default": 0,
                    "description": "Start frame (inclusive)",
                },
                "end": {
                    "type": "integer",
                    "default": -1,
                    "description": "End frame (inclusive, -1 = duration)",
                },
                "width": {
                    "type": "integer",
                    "default": 1920,
                    "description": "Output width",
                },
                "height": {
                    "type": "integer",
                    "default": 1080,
                    "description": "Output height",
                },
                "overwrite": {
                    "type": "boolean",
                    "default": True,
                    "description": "Overwrite output if exists",
                },
            },
            "required": ["animation_id", "out"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_export_video"),
    ),
    tool_from_schema(
        name="animation_render_preview",
        description=(
            "Render exactly one PNG preview frame for an animation time by saving the current .animation3d and invoking headless Atlas. "
            "This is primarily for verifying animation-at-time behavior. For static scene screenshots, prefer scene_screenshot (lighter; does not involve animation export).\n"
            "Intended for model-based visual inspection (the runtime may upload the image to the model when consent is enabled). "
            "Do NOT ask the user to open the temp file path; if a human check is still needed, ask them to check in the Atlas UI."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "time": {"type": "number", "description": "Preview time in seconds"},
                "fps": {
                    "type": "number",
                    "default": 30,
                    "description": "Frames per second",
                },
                "width": {"type": "integer", "description": "Image width"},
                "height": {"type": "integer", "description": "Image height"},
            },
            "required": ["animation_id", "time", "width", "height"],
        },
        preconditions=(
            require_screenshot_consent,
            require_animation_id,
            require_engine_ready,
        ),
        handler=_tool_handler("animation_render_preview"),
    ),
]

_GROUP_ID_TO_NAME = {1: "Background", 2: "Axis", 3: "Global"}


def _coerce_conflict_policy(v: Any) -> str:
    s = str(v or "error").strip().lower()
    if s in ("error", "overwrite", "skip"):
        return s
    return "error"


def _resolve_track_json_key(
    *,
    id: int,
    json_key: Any,
    name: Any,
    _resolve_json_key,
) -> str | None:
    if int(id) == 0:
        return ""
    cand = str(json_key).strip() if json_key is not None else ""
    nm = str(name).strip() if name is not None else ""
    if not cand and not nm:
        return None
    if cand:
        try:
            jk = _resolve_json_key(int(id), candidate=cand)
            if not jk:
                jk = _resolve_json_key(int(id), name=cand)
            if jk:
                return jk
        except Exception:
            pass
    if nm:
        try:
            jk = _resolve_json_key(int(id), name=nm)
            if not jk:
                jk = _resolve_json_key(int(id), candidate=nm)
            if jk:
                return jk
        except Exception:
            pass
    return None


def _save_animation_to_temp(client, *, animation_id: int) -> dict:
    with tempfile.TemporaryDirectory(prefix="atlas_agent_anim_") as td:
        p = Path(td) / "current.animation3d"
        ok = client.save_animation(animation_id=int(animation_id), path=p)
        if not ok:
            raise RuntimeError("SaveAnimation failed (no animation available?)")
        return load_animation(p)


def _extract_track_keys(anim: dict, *, id: int, json_key: str) -> list[dict[str, Any]]:
    """Return key instances for a single track as [{idx,time,easing,value}, ...]."""
    keys_list: Any = None
    if int(id) == 0:
        cam = anim.get("Camera 3DCamera") or {}
        if isinstance(cam, dict):
            keys_list = cam.get("keys")
    elif int(id) in _GROUP_ID_TO_NAME:
        grp = anim.get(_GROUP_ID_TO_NAME[int(id)]) or {}
        if isinstance(grp, dict):
            track = grp.get(str(json_key)) or {}
            if isinstance(track, dict):
                keys_list = track.get("keys")
    else:
        obj = anim.get(str(int(id))) or {}
        if isinstance(obj, dict):
            track = obj.get(str(json_key)) or {}
            if isinstance(track, dict):
                keys_list = track.get("keys")
    if not isinstance(keys_list, list):
        return []
    out: list[dict[str, Any]] = []
    for idx, k in enumerate(keys_list):
        if not isinstance(k, dict):
            continue
        try:
            tm = float(k.get("time", 0.0))
        except Exception:
            tm = 0.0
        easing = str(k.get("type", "Linear"))
        out.append(
            {
                "idx": int(idx),
                "time": float(tm),
                "easing": easing,
                "value": k.get("value"),
            }
        )
    return out


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    client = ctx.client
    atlas_dir = ctx.atlas_dir
    dispatch = ctx.dispatch
    _param_to_dict = ctx.param_to_dict
    _resolve_json_key = ctx.resolve_json_key
    _json_key_exists = ctx.json_key_exists
    _schema_validator_cache = ctx.schema_validator_cache

    # Camera timeline tools rely on the stable look-at + distance interpolation convention.
    # Enforce the engine-side setting so authored camera keys always evaluate deterministically
    # (and stay robust if engine/UI defaults change in the future).
    def _ensure_camera_center_interpolation(animation_id: int) -> None:
        try:
            client.set_camera_interpolation_method(
                animation_id=animation_id, method="center"
            )
        except Exception:
            pass

    if name == "animation_set_param_by_name":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        name_str = str(args.get("name", ""))
        type_hint = args.get("type_hint")
        time_v = float(args.get("time", 0.0))
        easing = _normalize_easing_name(args.get("easing"))
        value_native = args.get("value")
        # Resolve param json_key by name and optional type
        pl = client.list_params(id=id)
        target_jk = None
        target_type = None
        lname = name_str.lower().strip()

        def match(pname: str) -> bool:
            ps = (pname or "").lower().strip()
            return ps == lname or ps.startswith(lname)

        for p in pl.params:
            if match(p.name) and (type_hint is None or p.type == type_hint):
                target_jk = p.json_key
                target_type = p.type
                break
        if target_jk is None:
            # fallback: try json_key match
            for p in pl.params:
                if match(p.json_key) and (type_hint is None or p.type == type_hint):
                    target_jk = p.json_key
                    target_type = p.type
                    break
        if target_jk is None:
            return json.dumps(
                {
                    "ok": False,
                    "error": f"parameter '{name_str}' not found for id={id}",
                }
            )
        # Coerce common types
        t = (target_type or "").lower()
        if "bool" in t and isinstance(value_native, str):
            s = value_native.strip().lower()
            if s in ("true", "1", "yes", "on"):
                value_native = True
            elif s in ("false", "0", "no", "off"):
                value_native = False
        if t.endswith("vec4") and isinstance(value_native, list):
            if len(value_native) == 3:
                value_native = [
                    float(value_native[0]),
                    float(value_native[1]),
                    float(value_native[2]),
                    1.0,
                ]
            elif len(value_native) == 4:
                value_native = [float(x) for x in value_native]
        if (t == "float" or t == "double") and isinstance(value_native, str):
            try:
                value_native = float(value_native)
            except Exception:
                pass
        # Typed SetKey
        try:
            ok = client.set_key_param(
                animation_id=animation_id,
                target_id=id,
                json_key=target_jk,
                time=time_v,
                easing=easing,
                value=value_native,
            )
            return json.dumps({"ok": ok, "json_key": target_jk, "type": target_type})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps(
                {
                    "ok": False,
                    "error": msg,
                    "json_key": target_jk,
                    "type": target_type,
                }
            )

    if name == "animation_remove_key_param_at_time":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        if id == 0:
            return json.dumps(
                {
                    "ok": False,
                    "error": "camera uses camera tools; use animation_replace_key_camera",
                }
            )
        json_key = str(args.get("json_key"))
        time_v = float(args.get("time", 0.0))
        tol = float(args.get("tolerance", 1e-3))
        # Verify parameter exists
        if not _json_key_exists(id, json_key):
            return json.dumps({"ok": False, "error": "json_key not found for id"})
        try:
            lr = client.list_keys(
                animation_id=animation_id,
                target_id=id,
                json_key=json_key,
                include_values=False,
            )
            times = [k.time for k in getattr(lr, "keys", [])]
            to_remove = [t for t in times if abs(t - time_v) <= tol]
            removed = 0
            for t in to_remove:
                ok = client.remove_key(
                    animation_id=animation_id, target_id=id, json_key=json_key, time=t
                )
                if ok:
                    removed += 1
            return json.dumps({"ok": True, "removed": removed})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_replace_key_param":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        if id == 0:
            return json.dumps(
                {"ok": False, "error": "use animation_replace_key_camera for id=0"}
            )
        json_key = str(args.get("json_key"))
        time_v = float(args.get("time", 0.0))
        easing = _normalize_easing_name(args.get("easing"))
        value = args.get("value")
        tol = float(args.get("tolerance", 1e-3))
        # Verify parameter exists
        if not _json_key_exists(id, json_key):
            return json.dumps({"ok": False, "error": "json_key not found for id"})
        try:
            # Remove keys within tolerance
            rm = json.loads(
                dispatch(
                    "animation_remove_key_param_at_time",
                    json.dumps(
                        {
                            "animation_id": animation_id,
                            "id": id,
                            "json_key": json_key,
                            "time": time_v,
                            "tolerance": tol,
                        }
                    ),
                )
            )
            ok = client.set_key_param(
                animation_id=animation_id,
                target_id=id,
                json_key=json_key,
                time=time_v,
                easing=easing,
                value=value,
            )
            return json.dumps({"ok": ok, "removed": rm.get("removed", 0)})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_replace_key_camera":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        time_v = float(args.get("time", 0.0))
        easing = _normalize_easing_name(args.get("easing"))
        allow_jump_cut = bool(args.get("allow_jump_cut", False))
        try:
            max_switch_jump_fraction = float(
                args.get(
                    "max_switch_jump_fraction",
                    DEFAULT_MAX_SWITCH_CAMERA_JUMP_FRACTION,
                )
            )
        except Exception:
            max_switch_jump_fraction = DEFAULT_MAX_SWITCH_CAMERA_JUMP_FRACTION
        if (
            not math.isfinite(max_switch_jump_fraction)
            or max_switch_jump_fraction < 0.0
        ):
            return json.dumps(
                {
                    "ok": False,
                    "error": "max_switch_jump_fraction must be a finite number >= 0",
                }
            )
        value = args.get("value")
        if not isinstance(value, dict):
            return json.dumps(
                {
                    "ok": False,
                    "error": "value must be a typed camera object (dict)",
                }
            )
        tol = float(args.get("tolerance", 1e-3))
        ids = args.get("ids") or []
        if not ids:
            try:
                ids = client.fit_candidates()
            except Exception:
                ids = []
        constraints = args.get("constraints") or {
            "keep_visible": True,
            "min_frame_coverage": 0.0,
        }
        # First pass policies allow adjustments
        policies1 = {
            "adjust_distance": True,
        }
        # Second pass: strict verification without adjustments
        policies2 = {
            "adjust_distance": False,
        }
        try:
            _ensure_camera_center_interpolation(animation_id)

            def _read_vec3(cam: dict, key: str) -> tuple[float, float, float] | None:
                raw = cam.get(key)
                if not isinstance(raw, list) or len(raw) != 3:
                    return None
                try:
                    x = float(raw[0])
                    y = float(raw[1])
                    z = float(raw[2])
                except Exception:
                    return None
                if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(z)):
                    return None
                return (x, y, z)

            def _sub(
                a: tuple[float, float, float], b: tuple[float, float, float]
            ) -> tuple[float, float, float]:
                return (a[0] - b[0], a[1] - b[1], a[2] - b[2])

            def _norm(v: tuple[float, float, float]) -> float:
                return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])

            # Gather removals (but do not mutate yet; we want to fail-fast on validation/guardrails).
            to_remove: list[float] = []
            try:
                lr = client.list_keys(
                    animation_id=animation_id, target_id=0, include_values=False
                )
                times = [k.time for k in getattr(lr, "keys", [])]
                to_remove = [t for t in times if abs(t - time_v) <= tol]
            except Exception:
                to_remove = []

            # Guardrail: prevent accidental camera jump-cuts (easing='Switch') unless explicitly
            # opted in. We compare against the *evaluated timeline camera* at this time (pre-write).
            old_eye: tuple[float, float, float] | None = None
            old_center: tuple[float, float, float] | None = None
            ref_eye_center_dist: float | None = None
            if easing == "Switch" and not allow_jump_cut:
                try:
                    samples = client.camera_sample(
                        animation_id=animation_id, times=[time_v]
                    )
                except Exception as e:
                    msg = str(e)
                    try:
                        msg = e.details()  # type: ignore[attr-defined]
                    except Exception:
                        pass
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "animation_replace_key_camera: cannot validate Switch-key continuity "
                                f"(camera_sample failed: {msg}). If you intended a cut, pass allow_jump_cut=true."
                            ),
                        }
                    )
                if not samples or not isinstance(samples[0].get("value"), dict):
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "animation_replace_key_camera: cannot validate Switch-key continuity "
                                "(camera_sample returned no value). If you intended a cut, pass allow_jump_cut=true."
                            ),
                        }
                    )
                old_value = samples[0].get("value") or {}
                old_eye = _read_vec3(old_value, "Eye Position Vec3")
                old_center = _read_vec3(old_value, "Center Position Vec3")
                if not (old_eye and old_center):
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "animation_replace_key_camera: Switch-key continuity check requires "
                                "Eye Position Vec3 and Center Position Vec3 in the sampled timeline camera value. "
                                "If you intended a cut, pass allow_jump_cut=true."
                            ),
                        }
                    )
                ref_eye_center_dist = _norm(_sub(old_eye, old_center))
                if (
                    ref_eye_center_dist is None
                    or not math.isfinite(ref_eye_center_dist)
                    or ref_eye_center_dist <= 0.0
                ):
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "animation_replace_key_camera: Switch-key continuity check failed "
                                "(invalid sampled eye↔center distance). If you intended a cut, pass allow_jump_cut=true."
                            ),
                        }
                    )

            try:
                vr = client.camera_validate(
                    animation_id=animation_id,
                    ids=ids,
                    times=[time_v],
                    values=[value],
                    constraints=constraints,
                    policies=policies1,
                )
                vals = vr.get("results") or []
                if vals and vals[0].get("adjusted") and vals[0].get("adjusted_value"):
                    value = vals[0].get("adjusted_value")
            except Exception:
                pass

            if easing == "Switch" and not allow_jump_cut:
                new_eye = _read_vec3(value, "Eye Position Vec3")
                new_center = _read_vec3(value, "Center Position Vec3")
                if not (
                    new_eye
                    and new_center
                    and old_eye
                    and old_center
                    and ref_eye_center_dist
                ):
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "animation_replace_key_camera: Switch-key continuity check requires "
                                "Eye Position Vec3 and Center Position Vec3 in both the sampled and new camera values. "
                                "If you intended a cut, pass allow_jump_cut=true."
                            ),
                        }
                    )
                delta_eye = _norm(_sub(new_eye, old_eye))
                delta_center = _norm(_sub(new_center, old_center))
                delta_frac = max(delta_eye, delta_center) / ref_eye_center_dist
                if delta_frac > max_switch_jump_fraction:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "animation_replace_key_camera: refusing to write an easing='Switch' key that would create "
                                "a large camera jump (jump cut). If you intended an instantaneous cut, pass allow_jump_cut=true. "
                                "Otherwise, use easing='Linear' with an explicit transition segment, or use "
                                "animation_camera_solve_and_apply(mode='STATIC') for holds."
                            ),
                            "time": float(time_v),
                            "jump_fraction": float(delta_frac),
                            "max_switch_jump_fraction": float(max_switch_jump_fraction),
                            "delta_eye": float(delta_eye),
                            "delta_center": float(delta_center),
                            "reference_eye_center_dist": float(ref_eye_center_dist),
                        }
                    )

            # Remove camera keys within tolerance (now safe to mutate).
            removed = 0
            for t in to_remove:
                try:
                    if client.remove_key(
                        animation_id=animation_id, target_id=0, json_key="", time=t
                    ):
                        removed += 1
                except Exception:
                    pass
            ok = client.set_key_camera(
                animation_id=animation_id, time=time_v, easing=easing, value=value
            )
            # Re-validate strictly
            final_ok = ok
            try:
                vr2 = client.camera_validate(
                    animation_id=animation_id,
                    ids=ids,
                    times=[time_v],
                    values=[value],
                    constraints=constraints,
                    policies=policies2,
                )
                final_ok = bool(vr2.get("ok", False))
                reason = (vr2.get("results") or [{}])[0].get("reason")
            except Exception:
                reason = None
            return json.dumps(
                {
                    "ok": bool(final_ok and ok),
                    "removed": removed,
                    **({"reason": reason} if reason else {}),
                }
            )
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_camera_solve_and_apply":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        try:
            mode = str(args.get("mode"))
            ids = args.get("ids") or []
            t0 = float(args.get("t0", 0.0))
            t1 = float(args.get("t1", 0.0))
            if not isinstance(ids, list) or any(
                not isinstance(i, (int, float)) for i in ids
            ):
                return json.dumps(
                    {"ok": False, "error": "ids must be an array of numbers"}
                )
            mode_up = mode.strip().upper()
            if mode_up not in ("FIT", "ORBIT", "DOLLY", "STATIC"):
                return json.dumps(
                    {
                        "ok": False,
                        "error": "mode must be one of FIT|ORBIT|DOLLY|STATIC",
                    }
                )
            if mode_up in ("ORBIT", "DOLLY") and not (t1 > t0):
                return json.dumps(
                    {"ok": False, "error": "t1 must be > t0 for ORBIT/DOLLY"}
                )
            _ensure_camera_center_interpolation(animation_id)
            constraints = args.get("constraints") or {
                "keep_visible": True,
                "min_frame_coverage": 0.0,
            }
            params = args.get("params") or {}
            # DOLLY requires explicit distances to produce any motion. Both values <= 0
            # (or missing) means "keep the base camera distance" for both endpoints,
            # which is effectively a STATIC hold but with more confusing intent.
            if mode_up == "DOLLY":
                if not isinstance(params, dict):
                    return json.dumps(
                        {
                            "ok": False,
                            "error": "params must be an object for DOLLY (use start_dist/end_dist)",
                        }
                    )

                def _read_dist(key: str) -> float | None:
                    if key not in params:
                        return None
                    try:
                        v = float(params.get(key))
                    except Exception:
                        raise ValueError(f"params.{key} must be a number")
                    if not math.isfinite(v):
                        raise ValueError(f"params.{key} must be finite")
                    return v

                try:
                    start_dist = _read_dist("start_dist")
                    end_dist = _read_dist("end_dist")
                except ValueError as e:
                    return json.dumps({"ok": False, "error": str(e)})

                if (start_dist is None or start_dist <= 0.0) and (
                    end_dist is None or end_dist <= 0.0
                ):
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "DOLLY requires params.start_dist and/or params.end_dist > 0 (absolute eye→center distance). "
                                "If you want a hold, use mode='STATIC'. "
                                "If you want a relative dolly (bbox-scaled), use animation_camera_walkthrough_apply with "
                                "look_at_policy='bbox_center' and a move.forward/back segment."
                            ),
                        }
                    )
            # Defaults for ORBIT
            if mode_up == "ORBIT":
                params.setdefault("axis", "y")
                # `params` is reserved for mode-specific knobs. For ORBIT, degrees and
                # key density are top-level arguments to avoid conflicting sources.
                if isinstance(params, dict) and "degrees" in params:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "animation_camera_solve_and_apply(mode='ORBIT') does not accept params.degrees. "
                                "Use top-level `degrees`."
                            ),
                        }
                    )
                if isinstance(params, dict) and "max_step_degrees" in params:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": (
                                "animation_camera_solve_and_apply(mode='ORBIT') does not accept params.max_step_degrees. "
                                "Use top-level `max_step_degrees`."
                            ),
                        }
                    )

                deg_top = args.get("degrees", None) if "degrees" in args else None

                def _as_finite_float(v: Any, default: float | None) -> float | None:
                    if v is None:
                        return default
                    try:
                        x = float(v)
                    except Exception:
                        return default
                    return x if math.isfinite(x) else default

                deg_top_f = _as_finite_float(deg_top, None)
                deg = deg_top_f if deg_top_f is not None else 360.0
                params["degrees"] = float(deg)

                # Optional: control key density by limiting per-step rotation.
                msd_top = (
                    args.get("max_step_degrees", None)
                    if "max_step_degrees" in args
                    else None
                )
                msd_top_f = _as_finite_float(msd_top, None)
                msd = msd_top_f
                if msd is not None:
                    if msd <= 0.0:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "max_step_degrees must be a finite number > 0",
                            }
                        )
                    params["max_step_degrees"] = float(msd)
            tol = float(args.get("tolerance", 1e-3))
            easing = _normalize_easing_name(args.get("easing"))
            clear_range = bool(args.get("clear_range", True))
            # Ensure the engine state matches the timeline at t0 before solving.
            #
            # `camera_solve` snapshots the *current engine camera + bbox* as the base for
            # FIT/ORBIT/DOLLY/STATIC. When authoring a timeline in multiple adjacent
            # segments (e.g., ORBIT [0,6.5] then STATIC [6.5,8]), failing to sync the
            # engine to t0 can cause discontinuities: the solver would base the second
            # segment on whatever camera the UI currently has (often the t=0 pose),
            # overwriting the boundary key and producing a visible “reset”.
            if not client.set_time(
                animation_id=animation_id, seconds=t0, cancel_rendering=True
            ):
                return json.dumps(
                    {
                        "ok": False,
                        "error": "failed to set animation time to t0 before solving",
                        "t0": float(t0),
                    }
                )
            keys = client.camera_solve(
                mode=mode_up,
                ids=ids,
                t0=t0,
                t1=t1,
                constraints=constraints,
                params=params,
            )
            if not keys:
                return json.dumps(
                    {"ok": False, "error": "camera_solve returned no keys"}
                )

            def _has_time(arr: list[dict], t: float, eps: float) -> bool:
                for kk in arr:
                    try:
                        tv = float(kk.get("time", 0.0))
                    except Exception:
                        continue
                    if abs(tv - t) <= eps:
                        return True
                return False

            # Invariants: ORBIT/DOLLY are interval solves and must include endpoints.
            if mode_up in ("ORBIT", "DOLLY") and t1 > t0:
                if not _has_time(keys, float(t0), tol):
                    times_dbg = []
                    try:
                        times_dbg = [float(k.get("time", 0.0)) for k in (keys or [])]
                    except Exception:
                        times_dbg = []
                    return json.dumps(
                        {
                            "ok": False,
                            "error": "camera_solve did not return a key at t0",
                            "t0": float(t0),
                            "t1": float(t1),
                            "times": times_dbg,
                        }
                    )
                if not _has_time(keys, float(t1), tol):
                    times_dbg = []
                    try:
                        times_dbg = [float(k.get("time", 0.0)) for k in (keys or [])]
                    except Exception:
                        times_dbg = []
                    return json.dumps(
                        {
                            "ok": False,
                            "error": "camera_solve did not return a key at t1",
                            "t0": float(t0),
                            "t1": float(t1),
                            "times": times_dbg,
                        }
                    )

            # ORBIT contract: when key density is controlled via max_step_degrees, the
            # solver should segment the path accordingly (segments+1 keys). If this
            # invariant changes, we'd rather fail fast than silently author a sparse
            # timeline that will interpolate differently downstream.
            if mode_up == "ORBIT" and t1 > t0:
                try:
                    deg_val = float(params.get("degrees", 360.0))
                except Exception:
                    deg_val = 360.0
                try:
                    msd_val = float(params.get("max_step_degrees", 90.0))
                except Exception:
                    msd_val = 90.0
                if msd_val > 0.0 and math.isfinite(msd_val) and math.isfinite(deg_val):
                    expected_segments = max(
                        1, int(math.ceil(abs(float(deg_val)) / float(msd_val)))
                    )
                    expected_keys = expected_segments + 1
                    if len(keys) != expected_keys:
                        times_dbg = []
                        try:
                            times_dbg = [
                                float(k.get("time", 0.0)) for k in (keys or [])
                            ]
                        except Exception:
                            times_dbg = []
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "camera_solve returned unexpected key count for ORBIT",
                                "degrees": float(deg_val),
                                "max_step_degrees": float(msd_val),
                                "expected_segments": int(expected_segments),
                                "expected_keys": int(expected_keys),
                                "got_keys": int(len(keys)),
                                "times": times_dbg,
                            }
                        )
            # Optionally clear existing keys in [t0, t1] (with tolerance), excluding solver times
            if clear_range:
                try:
                    lr = client.list_keys(
                        animation_id=animation_id, target_id=0, include_values=False
                    )
                    existing = [float(k.time) for k in getattr(lr, "keys", [])]
                except Exception:
                    existing = []
                tmin, tmax = (t0, t1) if t0 <= t1 else (t1, t0)
                # Build set of solver times for matching
                solved_times = [float(k.get("time", 0.0)) for k in (keys or [])]

                def _near_any(x: float, arr: list[float], eps: float) -> bool:
                    for v in arr:
                        if abs(x - v) <= eps:
                            return True
                    return False

                for old_t in existing:
                    if old_t + tol < tmin or old_t - tol > tmax:
                        continue
                    if _near_any(old_t, solved_times, tol):
                        continue
                    try:
                        client.remove_key(
                            animation_id=animation_id,
                            target_id=0,
                            json_key="",
                            time=old_t,
                        )
                    except Exception:
                        pass
            applied: list[float] = []
            apply_errors: list[dict[str, Any]] = []
            for k in keys or []:
                try:
                    tv = float(k.get("time", 0.0))
                    vv = k.get("value") or {}
                    # Use replace to remove near times and validate; pass ids for validation inside the function
                    payload = {
                        "animation_id": animation_id,
                        "time": tv,
                        "easing": easing,
                        "value": vv,
                        "tolerance": tol,
                        "strict": False,
                        "ids": ids,
                        "constraints": constraints,
                    }
                    rr = json.loads(
                        dispatch("animation_replace_key_camera", json.dumps(payload))
                        or "{}"
                    )
                    if rr.get("ok"):
                        applied.append(tv)
                    else:
                        apply_errors.append(
                            {
                                "time": float(tv),
                                "error": rr.get("error")
                                or rr.get("reason")
                                or "unknown",
                            }
                        )
                except Exception:
                    try:
                        apply_errors.append(
                            {"time": float(k.get("time", 0.0)), "error": "exception"}
                        )
                    except Exception:
                        apply_errors.append({"error": "exception"})
                    continue

            # If we didn't apply everything we solved, treat that as a failure so
            # the agent doesn't proceed with an incomplete/ambiguous camera track.
            if len(applied) != len(keys):
                return json.dumps(
                    {
                        "ok": False,
                        "error": "failed to apply one or more solved camera keys",
                        "applied": sorted(applied),
                        "total": int(len(applied)),
                        "expected": int(len(keys)),
                        "apply_errors": apply_errors,
                    }
                )

            return json.dumps(
                {"ok": True, "applied": sorted(applied), "total": len(applied)}
            )
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_camera_validate":
        try:
            animation_id = int(args.get("animation_id", 0) or 0)
            if animation_id <= 0:
                return json.dumps({"ok": False, "error": "animation_id is required"})
            ids = args.get("ids") or []
            times = args.get("times") or []
            # Values are optional; the server can sample from animation when omitted.
            values = args.get("values") or []
            if not times:
                return json.dumps({"ok": False, "error": "times must be non-empty"})
            constraints = args.get("constraints") or {}
            policies = args.get("policies") or {}
            res = client.camera_validate(
                animation_id=animation_id,
                ids=ids,
                times=times,
                values=values,
                constraints=constraints,
                policies=policies,
            )
            results = res.get("results") or []

            return json.dumps(
                {
                    "ok": bool(res.get("ok", False)),
                    "results": results,
                }
            )
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_camera_sample":
        try:
            animation_id = int(args.get("animation_id", 0) or 0)
            if animation_id <= 0:
                return json.dumps({"ok": False, "error": "animation_id is required"})
            times_in = args.get("times") or []
            if not isinstance(times_in, list) or not times_in:
                return json.dumps(
                    {"ok": False, "error": "times must be a non-empty list"}
                )
            times = [float(t) for t in times_in]
            samples = client.camera_sample(animation_id=animation_id, times=times)
            return json.dumps({"ok": True, "samples": samples})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_camera_waypoint_spline_apply":
        try:
            animation_id = int(args.get("animation_id", 0) or 0)
            if animation_id <= 0:
                return json.dumps({"ok": False, "error": "animation_id is required"})
            _ensure_camera_center_interpolation(animation_id)
            ids = args.get("ids") or []
            after_clipping = bool(args.get("after_clipping", True))
            t0 = float(args.get("t0", 0.0))
            t1 = float(args.get("t1", 0.0))
            if not (math.isfinite(t0) and math.isfinite(t1)):
                return json.dumps({"ok": False, "error": "t0 and t1 must be finite"})
            if t0 < 0.0 or t1 < 0.0:
                return json.dumps({"ok": False, "error": "t0 and t1 must be >= 0"})
            if not (t1 > t0):
                return json.dumps({"ok": False, "error": "t1 must be > t0"})
            waypoints_in = args.get("waypoints") or []
            if not isinstance(waypoints_in, list) or not waypoints_in:
                return json.dumps(
                    {"ok": False, "error": "waypoints must be a non-empty list"}
                )
            if len(waypoints_in) < 2:
                return json.dumps(
                    {"ok": False, "error": "at least 2 waypoints are required"}
                )
            easing = _normalize_easing_name(args.get("easing"))
            tol = float(args.get("tolerance", 1e-3))
            if not math.isfinite(tol) or tol < 0.0:
                return json.dumps(
                    {"ok": False, "error": "tolerance must be a finite number >= 0"}
                )
            clear_range = bool(args.get("clear_range", True))
            constraints = args.get("constraints") or {
                "keep_visible": True,
                "min_frame_coverage": 0.0,
            }
            base_value = args.get("base_value")
            if not isinstance(base_value, dict) or not base_value:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "base_value is required (typed camera object). Tip: use animation_camera_sample(animation_id,[t0]) to sample from the timeline.",
                    }
                )
            look_at_policy = (
                str(
                    args.get("look_at_policy", "preserve_direction")
                    or "preserve_direction"
                )
                .strip()
                .lower()
            )
            if look_at_policy not in ("preserve_direction", "bbox_center"):
                return json.dumps(
                    {
                        "ok": False,
                        "error": "look_at_policy must be one of: preserve_direction | bbox_center",
                    }
                )

            # Normalize waypoint times: allow either absolute time or u in [0,1].
            span = float(t1 - t0)
            waypoints: list[dict] = []
            used_times: set[float] = set()
            for w in waypoints_in:
                if not isinstance(w, dict):
                    return json.dumps(
                        {"ok": False, "error": "each waypoint must be an object"}
                    )
                time_raw = w.get("time")
                u_raw = w.get("u")
                if time_raw is not None:
                    tm = float(time_raw)
                elif u_raw is not None:
                    u = float(u_raw)
                    if not math.isfinite(u) or u < 0.0 or u > 1.0:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "when using 'u', it must be a finite number in [0,1]. For out-of-range times, use explicit 'time'.",
                            }
                        )
                    tm = t0 + u * span
                else:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": "each waypoint must include either 'time' (seconds) or 'u' (0..1)",
                        }
                    )
                if not math.isfinite(tm) or tm < 0.0:
                    return json.dumps(
                        {"ok": False, "error": "waypoint time must be finite and >= 0"}
                    )
                if tm < t0 - tol or tm > t1 + tol:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": f"waypoint time {tm} is outside the apply window [{t0},{t1}] (tolerance={tol}). Adjust t0/t1 or use different waypoint times.",
                        }
                    )
                if tm in used_times:
                    return json.dumps(
                        {"ok": False, "error": f"duplicate waypoint time: {tm}"}
                    )
                used_times.add(tm)

                entry: dict[str, Any] = {"time": tm}

                eye = w.get("eye")
                if eye is not None:
                    if not isinstance(eye, dict):
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "waypoint.eye must be an object or null",
                            }
                        )
                    world_raw = eye.get("world")
                    frac_raw = eye.get("bbox_fraction")
                    modes = int(world_raw is not None) + int(frac_raw is not None)
                    if modes == 0:
                        # Treat a fully-null eye object as "omitted".
                        pass
                    elif modes != 1:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "waypoint.eye must have exactly one non-null of: world | bbox_fraction",
                            }
                        )
                    elif world_raw is not None:
                        v = world_raw
                        if not (isinstance(v, list) and len(v) == 3):
                            return json.dumps(
                                {"ok": False, "error": "eye.world must be [x,y,z]"}
                            )
                        if not all(math.isfinite(float(x)) for x in v):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "eye.world must contain finite numbers",
                                }
                            )
                        entry["eye"] = {
                            "world": [float(v[0]), float(v[1]), float(v[2])]
                        }
                    else:
                        v = frac_raw
                        if not (isinstance(v, list) and len(v) == 3):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "eye.bbox_fraction must be [fx,fy,fz]",
                                }
                            )
                        vv = [float(v[0]), float(v[1]), float(v[2])]
                        if not all(math.isfinite(x) for x in vv):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "eye.bbox_fraction must contain finite numbers",
                                }
                            )
                        if any((x < 0.0 or x > 1.0) for x in vv):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "eye.bbox_fraction values must be in [0,1]. For out-of-bbox points, use eye.world.",
                                }
                            )
                        entry["eye"] = {"bbox_fraction": vv}

                look = w.get("look_at")
                if look is not None:
                    if not isinstance(look, dict):
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "waypoint.look_at must be an object or null",
                            }
                        )
                    world_raw = look.get("world")
                    frac_raw = look.get("bbox_fraction")
                    bbox_center = look.get("bbox_center") is True
                    modes = (
                        int(world_raw is not None)
                        + int(frac_raw is not None)
                        + int(bbox_center)
                    )
                    if modes == 0:
                        # Treat a fully-null look_at object as "omitted".
                        look = None
                    elif modes != 1:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "waypoint.look_at must have exactly one of: world | bbox_center:true | bbox_fraction",
                            }
                        )
                    elif world_raw is not None:
                        v = world_raw
                        if not (isinstance(v, list) and len(v) == 3):
                            return json.dumps(
                                {"ok": False, "error": "look_at.world must be [x,y,z]"}
                            )
                        if not all(math.isfinite(float(x)) for x in v):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "look_at.world must contain finite numbers",
                                }
                            )
                        entry["look_at"] = {
                            "world": [float(v[0]), float(v[1]), float(v[2])]
                        }
                    elif bbox_center:
                        entry["look_at"] = {"bbox_center": True}
                    else:
                        v = frac_raw
                        if not (isinstance(v, list) and len(v) == 3):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "look_at.bbox_fraction must be [fx,fy,fz]",
                                }
                            )
                        vv = [float(v[0]), float(v[1]), float(v[2])]
                        if not all(math.isfinite(x) for x in vv):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "look_at.bbox_fraction must contain finite numbers",
                                }
                            )
                        if any((x < 0.0 or x > 1.0) for x in vv):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "look_at.bbox_fraction values must be in [0,1]. For out-of-bbox points, use look_at.world.",
                                }
                            )
                        entry["look_at"] = {"bbox_fraction": vv}

                if ("look_at" not in entry) and (look_at_policy == "bbox_center"):
                    entry["look_at"] = {"bbox_center": True}

                waypoints.append(entry)

            # Solve typed camera keys (no writes) from waypoints.
            base_cam = base_value
            keys = client.camera_path_solve(
                ids=ids,
                after_clipping=after_clipping,
                base_value=base_cam,
                waypoints=waypoints,
            )
            if not keys:
                return json.dumps(
                    {"ok": False, "error": "camera_path_solve returned no keys"}
                )

            # Optionally clear existing keys in [t0,t1] before applying.
            if clear_range:
                clear_res = json.loads(
                    dispatch(
                        "animation_clear_keys_range",
                        json.dumps(
                            {
                                "animation_id": animation_id,
                                "id": 0,
                                "t0": float(t0),
                                "t1": float(t1),
                                "tolerance": float(tol),
                                "include_times": False,
                            }
                        ),
                    )
                    or "{}"
                )
                if isinstance(clear_res, dict) and clear_res.get("ok") is False:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": f"failed to clear camera keys in range: {clear_res.get('error')}",
                        }
                    )

            applied: list[float] = []
            failed: list[dict[str, Any]] = []
            for k in keys:
                try:
                    tv = float(k.get("time", 0.0))
                    vv = k.get("value") or {}
                    payload = {
                        "animation_id": animation_id,
                        "time": tv,
                        "easing": easing,
                        "value": vv,
                        "tolerance": tol,
                        "strict": False,
                        "ids": ids,
                        "constraints": constraints,
                    }
                    rr = json.loads(
                        dispatch("animation_replace_key_camera", json.dumps(payload))
                        or "{}"
                    )
                    if rr.get("ok"):
                        applied.append(tv)
                    else:
                        failed.append(
                            {
                                "time": tv,
                                "error": rr.get("error")
                                or rr.get("reason")
                                or "apply_failed",
                            }
                        )
                except Exception as e:
                    failed.append(
                        {"time": float(k.get("time", 0.0) or 0.0), "error": str(e)}
                    )

            payload: dict[str, Any] = {
                "ok": not bool(failed),
                "applied": sorted(applied),
                "total": len(applied),
            }
            if failed:
                payload["failed"] = failed
            return json.dumps(payload)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_camera_walkthrough_apply":
        try:
            animation_id = int(args.get("animation_id", 0) or 0)
            if animation_id <= 0:
                return json.dumps({"ok": False, "error": "animation_id is required"})
            _ensure_camera_center_interpolation(animation_id)
            ids = args.get("ids") or []
            after_clipping = bool(args.get("after_clipping", True))
            t0 = float(args.get("t0", 0.0))
            t1 = float(args.get("t1", 0.0))
            if not (math.isfinite(t0) and math.isfinite(t1)):
                return json.dumps({"ok": False, "error": "t0 and t1 must be finite"})
            if t0 < 0.0 or t1 < 0.0:
                return json.dumps({"ok": False, "error": "t0 and t1 must be >= 0"})
            if not (t1 > t0):
                return json.dumps({"ok": False, "error": "t1 must be > t0"})

            segments_in = args.get("segments") or []
            if not isinstance(segments_in, list) or not segments_in:
                return json.dumps(
                    {"ok": False, "error": "segments must be a non-empty list"}
                )

            easing = _normalize_easing_name(args.get("easing"))
            tol = float(args.get("tolerance", 1e-3))
            if not math.isfinite(tol) or tol < 0.0:
                return json.dumps(
                    {"ok": False, "error": "tolerance must be a finite number >= 0"}
                )
            step_seconds = float(args.get("step_seconds", 1.0))
            if not math.isfinite(step_seconds) or step_seconds <= 0.0:
                return json.dumps(
                    {"ok": False, "error": "step_seconds must be a finite number > 0"}
                )
            clear_range = bool(args.get("clear_range", True))
            constraints = args.get("constraints") or {"keep_visible": False}
            base_value = args.get("base_value")
            if not isinstance(base_value, dict) or not base_value:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "base_value is required (typed camera object). Tip: use animation_camera_sample(animation_id,[t0]) to sample from the timeline.",
                    }
                )
            look_at_policy = (
                str(
                    args.get("look_at_policy", "preserve_direction")
                    or "preserve_direction"
                )
                .strip()
                .lower()
            )
            if look_at_policy not in ("preserve_direction", "bbox_center"):
                return json.dumps(
                    {
                        "ok": False,
                        "error": "look_at_policy must be one of: preserve_direction | bbox_center",
                    }
                )

            # Initial camera pose.
            cam: dict = base_value
            if look_at_policy == "bbox_center":
                cam = client.camera_look_at(
                    target_bbox_center=True,
                    ids=ids,
                    after_clipping=after_clipping,
                    base_value=cam,
                )

            # Normalize segments to time ranges inside [t0,t1].
            segs: list[dict[str, Any]] = []
            for s in segments_in:
                if not isinstance(s, dict):
                    return json.dumps(
                        {"ok": False, "error": "each segment must be an object"}
                    )
                segs.append(s)

            try:
                segs = expand_walkthrough_segments(segs)
            except Exception as e:
                return json.dumps(
                    {"ok": False, "error": f"segment template expansion failed: {e}"}
                )

            span = float(t1 - t0)
            any_u = any(
                (s.get("u0") is not None) or (s.get("u1") is not None) for s in segs
            )
            any_dur = any(s.get("duration") is not None for s in segs)
            if any_u and any_dur:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "segments must use exactly one timing mode: u0/u1 OR duration OR equal split (do not mix)",
                    }
                )

            time_ranges: list[tuple[float, float, dict[str, Any]]] = []
            if any_u:
                explicit: list[tuple[float, float, dict[str, Any]]] = []
                for s in segs:
                    if (s.get("u0") is None) or (s.get("u1") is None):
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "when using u0/u1 timing, every segment must include both u0 and u1",
                            }
                        )
                    u0 = float(s.get("u0"))
                    u1 = float(s.get("u1"))
                    if not (math.isfinite(u0) and math.isfinite(u1)):
                        return json.dumps(
                            {"ok": False, "error": "segment u0/u1 must be finite"}
                        )
                    if u0 < 0.0 or u1 < 0.0 or u0 > 1.0 or u1 > 1.0:
                        return json.dumps(
                            {"ok": False, "error": "segment u0/u1 must be in [0,1]"}
                        )
                    if not (u1 > u0):
                        return json.dumps(
                            {"ok": False, "error": "segment u1 must be > u0"}
                        )
                    explicit.append((u0, u1, s))
                explicit.sort(key=lambda it: float(it[0]))

                # Fill gaps with pause segments so interpolation doesn't move during gaps.
                eps = 1e-9
                ucur = 0.0
                normalized: list[tuple[float, float, dict[str, Any]]] = []
                for u0, u1, s in explicit:
                    if u0 < (ucur - eps):
                        return json.dumps(
                            {"ok": False, "error": "segments overlap in u-space"}
                        )
                    if u0 > (ucur + eps):
                        normalized.append((ucur, u0, {"pause": True, "label": "pause"}))
                    normalized.append((u0, u1, s))
                    ucur = u1
                if ucur < (1.0 - eps):
                    normalized.append((ucur, 1.0, {"pause": True, "label": "pause"}))

                for u0, u1, s in normalized:
                    ta = t0 + u0 * span
                    tb = t0 + u1 * span
                    time_ranges.append((float(ta), float(tb), s))
            elif any_dur:
                durs: list[float] = []
                for s in segs:
                    if s.get("duration") is None:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "when using duration timing, every segment must include duration",
                            }
                        )
                    d = float(s.get("duration"))
                    if not math.isfinite(d) or d <= 0.0:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "segment duration must be a finite number > 0",
                            }
                        )
                    durs.append(d)
                total = float(sum(durs))
                if not (total > 0.0):
                    return json.dumps(
                        {"ok": False, "error": "total duration must be > 0"}
                    )
                tcur = float(t0)
                for i, s in enumerate(segs):
                    dt = span * float(durs[i]) / total
                    ta = tcur
                    tb = tcur + dt
                    tcur = tb
                    time_ranges.append((ta, tb, s))
                # Ensure exact end at t1 (avoid drift from floating-point summation).
                if time_ranges:
                    ta, _, s = time_ranges[-1]
                    time_ranges[-1] = (ta, float(t1), s)
            else:
                # Equal split
                n = len(segs)
                dt = span / float(n)
                tcur = float(t0)
                for i, s in enumerate(segs):
                    ta = tcur
                    tb = (t0 + (i + 1) * dt) if (i < n - 1) else float(t1)
                    time_ranges.append((float(ta), float(tb), s))
                    tcur = float(tb)

            if not time_ranges:
                return json.dumps(
                    {"ok": False, "error": "no valid segments after normalization"}
                )

            def _coerce_float(x: Any, *, field: str) -> float:
                try:
                    v = float(x)
                except Exception:
                    raise ValueError(f"{field} must be a number")
                if not math.isfinite(v):
                    raise ValueError(f"{field} must be finite")
                return v

            def _parse_move(seg: dict[str, Any]) -> tuple[float, float, float]:
                if bool(seg.get("pause", False)):
                    return (0.0, 0.0, 0.0)
                move = seg.get("move")
                if move is None:
                    return (0.0, 0.0, 0.0)
                if not isinstance(move, dict):
                    raise ValueError("segment.move must be an object")
                forward = 0.0
                right = 0.0
                up = 0.0
                for k, v in move.items():
                    if v is None:
                        continue
                    key = str(k or "").strip().lower()
                    val = _coerce_float(v, field=f"move.{k}")
                    if key in {"forward", "fwd"}:
                        forward += val
                    elif key in {"back", "backward"}:
                        forward -= val
                    elif key == "right":
                        right += val
                    elif key == "left":
                        right -= val
                    elif key == "up":
                        up += val
                    elif key == "down":
                        up -= val
                    else:
                        raise ValueError(
                            "segment.move keys must be one of: forward, back, right, left, up, down"
                        )
                return (forward, right, up)

            def _parse_rotate(seg: dict[str, Any]) -> tuple[float, float, float]:
                if bool(seg.get("pause", False)):
                    return (0.0, 0.0, 0.0)
                rot = seg.get("rotate")
                if rot is None:
                    return (0.0, 0.0, 0.0)
                if not isinstance(rot, dict):
                    raise ValueError("segment.rotate must be an object")
                yaw = 0.0
                pitch = 0.0
                roll = 0.0
                for k, v in rot.items():
                    if v is None:
                        continue
                    key = str(k or "").strip().lower()
                    val = _coerce_float(v, field=f"rotate.{k}")
                    if key == "yaw":
                        yaw += val
                    elif key == "pitch":
                        pitch += val
                    elif key == "roll":
                        roll += val
                    else:
                        raise ValueError(
                            "segment.rotate keys must be one of: yaw, pitch, roll"
                        )
                return (yaw, pitch, roll)

            def _apply_move(
                cam_value: dict, *, forward: float, right: float, up: float
            ) -> dict:
                cur = cam_value
                move_center = look_at_policy != "bbox_center"
                if abs(forward) > 1e-12:
                    cur = client.camera_move_local(
                        op=("FORWARD" if forward >= 0.0 else "BACK"),
                        distance=abs(float(forward)),
                        distance_is_fraction_of_bbox_radius=True,
                        ids=ids,
                        after_clipping=after_clipping,
                        move_center=move_center,
                        base_value=cur,
                    )
                if abs(right) > 1e-12:
                    cur = client.camera_move_local(
                        op=("RIGHT" if right >= 0.0 else "LEFT"),
                        distance=abs(float(right)),
                        distance_is_fraction_of_bbox_radius=True,
                        ids=ids,
                        after_clipping=after_clipping,
                        move_center=move_center,
                        base_value=cur,
                    )
                if abs(up) > 1e-12:
                    cur = client.camera_move_local(
                        op=("UP" if up >= 0.0 else "DOWN"),
                        distance=abs(float(up)),
                        distance_is_fraction_of_bbox_radius=True,
                        ids=ids,
                        after_clipping=after_clipping,
                        move_center=move_center,
                        base_value=cur,
                    )
                return cur

            def _apply_rotate(
                cam_value: dict, *, yaw: float, pitch: float, roll: float
            ) -> dict:
                cur = cam_value
                if abs(yaw) > 1e-12:
                    cur = client.camera_rotate(
                        op=("AZIMUTH" if look_at_policy == "bbox_center" else "YAW"),
                        degrees=float(yaw),
                        base_value=cur,
                    )
                if abs(pitch) > 1e-12:
                    cur = client.camera_rotate(
                        op=(
                            "ELEVATION" if look_at_policy == "bbox_center" else "PITCH"
                        ),
                        degrees=float(pitch),
                        base_value=cur,
                    )
                if abs(roll) > 1e-12:
                    cur = client.camera_rotate(
                        op="ROLL", degrees=float(roll), base_value=cur
                    )
                return cur

            # Build camera key values by integrating segments sequentially.
            keys: list[dict[str, Any]] = [{"time": float(t0), "value": cam}]
            last_time = float(t0)
            for ta, tb, seg in time_ranges:
                if not (math.isfinite(ta) and math.isfinite(tb)):
                    return json.dumps(
                        {"ok": False, "error": "segment time range must be finite"}
                    )
                if tb <= ta:
                    continue
                try:
                    total_forward, total_right, total_up = _parse_move(seg)
                    total_yaw, total_pitch, total_roll = _parse_rotate(seg)
                except ValueError as e:
                    return json.dumps({"ok": False, "error": str(e)})

                is_pause = (
                    abs(total_forward) <= 1e-12
                    and abs(total_right) <= 1e-12
                    and abs(total_up) <= 1e-12
                    and abs(total_yaw) <= 1e-12
                    and abs(total_pitch) <= 1e-12
                    and abs(total_roll) <= 1e-12
                )

                dt = float(tb - ta)
                n_steps = 1 if is_pause else max(1, int(math.ceil(dt / step_seconds)))
                for i in range(1, n_steps + 1):
                    # Integrate small steps to approximate simultaneous movement + rotation.
                    frac = 1.0 / float(n_steps)
                    cam = _apply_move(
                        cam,
                        forward=total_forward * frac,
                        right=total_right * frac,
                        up=total_up * frac,
                    )
                    cam = _apply_rotate(
                        cam,
                        yaw=total_yaw * frac,
                        pitch=total_pitch * frac,
                        roll=total_roll * frac,
                    )
                    tm = float(ta + dt * (float(i) / float(n_steps)))
                    if tm > last_time + 1e-9:
                        keys.append({"time": tm, "value": cam})
                        last_time = tm

            # Ensure a final key at t1 (exact).
            if abs(last_time - float(t1)) > 1e-9:
                keys.append({"time": float(t1), "value": cam})

            if len(keys) < 2:
                return json.dumps(
                    {"ok": False, "error": "walkthrough produced fewer than 2 keys"}
                )

            # Optionally clear existing keys in [t0,t1] before applying.
            if clear_range:
                clear_res = json.loads(
                    dispatch(
                        "animation_clear_keys_range",
                        json.dumps(
                            {
                                "animation_id": animation_id,
                                "id": 0,
                                "t0": float(t0),
                                "t1": float(t1),
                                "tolerance": float(tol),
                                "include_times": False,
                            }
                        ),
                    )
                    or "{}"
                )
                if isinstance(clear_res, dict) and clear_res.get("ok") is False:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": f"failed to clear camera keys in range: {clear_res.get('error')}",
                        }
                    )

            applied: list[float] = []
            failed: list[dict[str, Any]] = []
            for k in keys:
                try:
                    tv = float(k.get("time", 0.0))
                    vv = k.get("value") or {}
                    payload = {
                        "animation_id": animation_id,
                        "time": tv,
                        "easing": easing,
                        "value": vv,
                        "tolerance": tol,
                        "strict": False,
                        "ids": ids,
                        "constraints": constraints,
                    }
                    rr = json.loads(
                        dispatch("animation_replace_key_camera", json.dumps(payload))
                        or "{}"
                    )
                    if rr.get("ok"):
                        applied.append(tv)
                    else:
                        failed.append(
                            {
                                "time": tv,
                                "error": rr.get("error")
                                or rr.get("reason")
                                or "apply_failed",
                            }
                        )
                except Exception as e:
                    failed.append(
                        {"time": float(k.get("time", 0.0) or 0.0), "error": str(e)}
                    )

            out: dict[str, Any] = {
                "ok": not bool(failed),
                "applied": sorted(applied),
                "total": len(applied),
                "planned_total": len(keys),
                "step_seconds": float(step_seconds),
            }
            if failed:
                out["failed"] = failed
            return json.dumps(out)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_get_time":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        try:
            ts = client.get_time(animation_id=animation_id)
            return json.dumps(
                {
                    "ok": True,
                    "seconds": getattr(ts, "seconds", 0.0),
                    "duration": getattr(ts, "duration", 0.0),
                }
            )
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_list_keys":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        json_key = args.get("json_key") or None
        if isinstance(json_key, str) and json_key.strip() == "":
            json_key = None
        include_values = bool(args.get("include_values", False))
        lr = client.list_keys(
            animation_id=animation_id,
            target_id=id,
            json_key=json_key,
            include_values=include_values,
        )
        keys = [
            {"time": k.time, "type": k.type, "value": getattr(k, "value_json", "")}
            for k in lr.keys
        ]
        return json.dumps({"ok": True, "keys": keys})

    if name == "animation_clear_keys_range":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        t0 = float(args.get("t0", 0.0))
        t1 = float(args.get("t1", 0.0))
        tol = float(args.get("tolerance", 1e-3))
        include_times = bool(args.get("include_times", False))
        if tol < 0:
            return json.dumps({"ok": False, "error": "tolerance must be >= 0"})
        tmin, tmax = (t0, t1) if t0 <= t1 else (t1, t0)
        jk = _resolve_track_json_key(
            id=id,
            json_key=args.get("json_key"),
            name=args.get("name"),
            _resolve_json_key=_resolve_json_key,
        )
        if id != 0 and not jk:
            return json.dumps({"ok": False, "error": "json_key or name required"})
        jk = jk or ""
        try:
            lr = client.list_keys(
                animation_id=animation_id,
                target_id=id,
                json_key=(jk or None),
                include_values=False,
            )
            times = [float(k.time) for k in getattr(lr, "keys", []) or []]
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

        to_remove = [t for t in times if (t >= (tmin - tol) and t <= (tmax + tol))]
        remove_keys = [
            {
                "id": int(id),
                "json_key": ("" if int(id) == 0 else str(jk)),
                "time": float(t),
            }
            for t in to_remove
        ]
        if not remove_keys:
            return json.dumps(
                {
                    "ok": True,
                    "id": int(id),
                    "json_key": ("" if int(id) == 0 else str(jk)),
                    "t0": float(t0),
                    "t1": float(t1),
                    "tolerance": float(tol),
                    "removed": 0,
                    **({"removed_times": []} if include_times else {}),
                }
            )
        try:
            ok = client.batch(
                animation_id=animation_id,
                set_keys=[],
                remove_keys=remove_keys,
                commit=True,
            )
            payload = {
                "ok": bool(ok),
                "id": int(id),
                "json_key": ("" if int(id) == 0 else str(jk)),
                "t0": float(t0),
                "t1": float(t1),
                "tolerance": float(tol),
                "removed": int(len(remove_keys)),
            }
            if include_times:
                payload["removed_times"] = to_remove
            return json.dumps(payload)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_shift_keys_range":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        t0 = float(args.get("t0", 0.0))
        t1 = float(args.get("t1", 0.0))
        delta = float(args.get("delta", 0.0))
        tol = float(args.get("tolerance", 1e-3))
        include_times = bool(args.get("include_times", False))
        on_conflict = _coerce_conflict_policy(args.get("on_conflict"))
        if tol < 0:
            return json.dumps({"ok": False, "error": "tolerance must be >= 0"})
        tmin, tmax = (t0, t1) if t0 <= t1 else (t1, t0)
        jk = _resolve_track_json_key(
            id=id,
            json_key=args.get("json_key"),
            name=args.get("name"),
            _resolve_json_key=_resolve_json_key,
        )
        if id != 0 and not jk:
            return json.dumps({"ok": False, "error": "json_key or name required"})
        jk = jk or ""
        try:
            anim = _save_animation_to_temp(client, animation_id=animation_id)
            all_keys = _extract_track_keys(anim, id=id, json_key=jk)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

        in_range = [
            k
            for k in all_keys
            if (k["time"] >= (tmin - tol) and k["time"] <= (tmax + tol))
        ]
        if not in_range:
            return json.dumps(
                {
                    "ok": True,
                    "id": int(id),
                    "json_key": ("" if int(id) == 0 else str(jk)),
                    "moved": 0,
                    "skipped": 0,
                    "overwritten": 0,
                    "delta": float(delta),
                }
            )
        in_idx = {int(k["idx"]) for k in in_range}
        outside = [k for k in all_keys if int(k["idx"]) not in in_idx]

        def new_time_for(k: dict[str, Any]) -> float:
            return float(k["time"]) + float(delta)

        moved_mappings: list[dict[str, float]] = []
        skipped_times: list[float] = []
        overwritten_times: list[float] = []
        remove_keys: list[dict[str, Any]] = []
        set_keys: list[dict[str, Any]] = []

        # Fail fast on negative target times
        for k in in_range:
            nt = new_time_for(k)
            if nt < 0:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "shift produces negative key time",
                        "time": float(k["time"]),
                        "new_time": float(nt),
                        "delta": float(delta),
                    }
                )

        if on_conflict == "skip":
            # Direction-aware processing prevents collisions with skipped keys.
            proc = sorted(
                in_range, key=lambda kk: float(kk["time"]), reverse=delta >= 0
            )
            occupied = [float(k["time"]) for k in outside]
            for k in proc:
                nt = new_time_for(k)
                if any(abs(nt - t) <= tol for t in occupied):
                    skipped_times.append(float(k["time"]))
                    occupied.append(float(k["time"]))
                    continue
                # Move this key
                remove_keys.append(
                    {
                        "id": int(id),
                        "json_key": ("" if int(id) == 0 else str(jk)),
                        "time": float(k["time"]),
                    }
                )
                set_keys.append(
                    {
                        "id": int(id),
                        "json_key": ("" if int(id) == 0 else str(jk)),
                        "time": float(nt),
                        "easing": str(k.get("easing", "Linear")),
                        "value": k.get("value"),
                    }
                )
                moved_mappings.append({"from": float(k["time"]), "to": float(nt)})
                occupied.append(float(nt))
        else:
            # Preflight conflicts vs outside keys and collisions among moved keys.
            moved = []
            for k in in_range:
                nt = new_time_for(k)
                moved.append((float(nt), k))
            moved.sort(key=lambda x: x[0])
            for i in range(1, len(moved)):
                if abs(moved[i][0] - moved[i - 1][0]) <= tol:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": "shift would place multiple keys at the same time (within tolerance)",
                            "tolerance": float(tol),
                            "times": [moved[i - 1][0], moved[i][0]],
                        }
                    )
            conflicts: list[dict[str, float]] = []
            for nt, _k in moved:
                for okk in outside:
                    if abs(float(okk["time"]) - nt) <= tol:
                        conflicts.append(
                            {"new_time": float(nt), "existing_time": float(okk["time"])}
                        )
            if conflicts and on_conflict == "error":
                return json.dumps(
                    {
                        "ok": False,
                        "error": "shift conflicts with existing keys (use on_conflict=overwrite or skip)",
                        "tolerance": float(tol),
                        "conflicts": conflicts,
                    }
                )
            conflict_idx = set()
            if conflicts and on_conflict == "overwrite":
                for okk in outside:
                    for nt, _k in moved:
                        if abs(float(okk["time"]) - float(nt)) <= tol:
                            conflict_idx.add(int(okk["idx"]))
                            break
                for okk in outside:
                    if int(okk["idx"]) in conflict_idx:
                        remove_keys.append(
                            {
                                "id": int(id),
                                "json_key": ("" if int(id) == 0 else str(jk)),
                                "time": float(okk["time"]),
                            }
                        )
                        overwritten_times.append(float(okk["time"]))
            # Remove originals + set shifted keys
            for nt, k in moved:
                remove_keys.append(
                    {
                        "id": int(id),
                        "json_key": ("" if int(id) == 0 else str(jk)),
                        "time": float(k["time"]),
                    }
                )
                set_keys.append(
                    {
                        "id": int(id),
                        "json_key": ("" if int(id) == 0 else str(jk)),
                        "time": float(nt),
                        "easing": str(k.get("easing", "Linear")),
                        "value": k.get("value"),
                    }
                )
                moved_mappings.append({"from": float(k["time"]), "to": float(nt)})

        if not remove_keys and not set_keys:
            return json.dumps(
                {
                    "ok": True,
                    "id": int(id),
                    "json_key": ("" if int(id) == 0 else str(jk)),
                    "moved": 0,
                    "skipped": int(len(skipped_times)),
                    "overwritten": int(len(overwritten_times)),
                    "delta": float(delta),
                    **({"skipped_times": skipped_times} if include_times else {}),
                }
            )
        try:
            ok = client.batch(
                animation_id=animation_id,
                set_keys=set_keys,
                remove_keys=remove_keys,
                commit=True,
            )
            out = {
                "ok": bool(ok),
                "id": int(id),
                "json_key": ("" if int(id) == 0 else str(jk)),
                "moved": int(len(set_keys)),
                "skipped": int(len(skipped_times)),
                "overwritten": int(len(overwritten_times)),
                "delta": float(delta),
            }
            if include_times:
                out["moved_mappings"] = moved_mappings
                out["skipped_times"] = skipped_times
                out["overwritten_times"] = overwritten_times
            return json.dumps(out)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_scale_keys_range":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        t0 = float(args.get("t0", 0.0))
        t1 = float(args.get("t1", 0.0))
        scale = float(args.get("scale", 1.0))
        anchor_mode = str(args.get("anchor", "t0") or "t0").strip().lower()
        tol = float(args.get("tolerance", 1e-3))
        include_times = bool(args.get("include_times", False))
        on_conflict = _coerce_conflict_policy(args.get("on_conflict"))
        if tol < 0:
            return json.dumps({"ok": False, "error": "tolerance must be >= 0"})
        if scale <= 0:
            return json.dumps({"ok": False, "error": "scale must be > 0"})
        tmin, tmax = (t0, t1) if t0 <= t1 else (t1, t0)
        if anchor_mode not in ("t0", "center"):
            anchor_mode = "t0"
        anchor_time = float(tmin if anchor_mode == "t0" else (tmin + tmax) * 0.5)
        if abs(scale - 1.0) < 1e-12:
            return json.dumps(
                {
                    "ok": True,
                    "id": int(id),
                    "json_key": (
                        "" if int(id) == 0 else str(args.get("json_key") or "")
                    ),
                    "moved": 0,
                    "skipped": 0,
                    "overwritten": 0,
                    "scale": float(scale),
                    "anchor": anchor_mode,
                }
            )
        jk = _resolve_track_json_key(
            id=id,
            json_key=args.get("json_key"),
            name=args.get("name"),
            _resolve_json_key=_resolve_json_key,
        )
        if id != 0 and not jk:
            return json.dumps({"ok": False, "error": "json_key or name required"})
        jk = jk or ""
        try:
            anim = _save_animation_to_temp(client, animation_id=animation_id)
            all_keys = _extract_track_keys(anim, id=id, json_key=jk)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

        in_range = [
            k
            for k in all_keys
            if (k["time"] >= (tmin - tol) and k["time"] <= (tmax + tol))
        ]
        if not in_range:
            return json.dumps(
                {
                    "ok": True,
                    "id": int(id),
                    "json_key": ("" if int(id) == 0 else str(jk)),
                    "moved": 0,
                    "skipped": 0,
                    "overwritten": 0,
                    "scale": float(scale),
                    "anchor": anchor_mode,
                }
            )
        in_idx = {int(k["idx"]) for k in in_range}
        outside = [k for k in all_keys if int(k["idx"]) not in in_idx]

        def new_time_for(k: dict[str, Any]) -> float:
            return anchor_time + (float(k["time"]) - anchor_time) * float(scale)

        # Fail fast on negative target times
        for k in in_range:
            nt = new_time_for(k)
            if nt < 0:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "scale produces negative key time",
                        "time": float(k["time"]),
                        "new_time": float(nt),
                        "scale": float(scale),
                        "anchor_time": float(anchor_time),
                    }
                )

        moved_mappings: list[dict[str, float]] = []
        skipped_times: list[float] = []
        overwritten_times: list[float] = []
        remove_keys: list[dict[str, Any]] = []
        set_keys: list[dict[str, Any]] = []

        if on_conflict == "skip":
            above = [k for k in in_range if float(k["time"]) >= anchor_time]
            below = [k for k in in_range if float(k["time"]) < anchor_time]
            above_proc = sorted(
                above, key=lambda kk: float(kk["time"]), reverse=scale > 1.0
            )
            below_proc = sorted(
                below, key=lambda kk: float(kk["time"]), reverse=scale < 1.0
            )
            occupied = [float(k["time"]) for k in outside]

            def proc_key(k: dict[str, Any]):
                nt = new_time_for(k)
                if any(abs(nt - t) <= tol for t in occupied):
                    skipped_times.append(float(k["time"]))
                    occupied.append(float(k["time"]))
                    return
                remove_keys.append(
                    {
                        "id": int(id),
                        "json_key": ("" if int(id) == 0 else str(jk)),
                        "time": float(k["time"]),
                    }
                )
                set_keys.append(
                    {
                        "id": int(id),
                        "json_key": ("" if int(id) == 0 else str(jk)),
                        "time": float(nt),
                        "easing": str(k.get("easing", "Linear")),
                        "value": k.get("value"),
                    }
                )
                moved_mappings.append({"from": float(k["time"]), "to": float(nt)})
                occupied.append(float(nt))

            for k in above_proc:
                proc_key(k)
            for k in below_proc:
                proc_key(k)
        else:
            moved = []
            for k in in_range:
                nt = new_time_for(k)
                moved.append((float(nt), k))
            moved.sort(key=lambda x: x[0])
            for i in range(1, len(moved)):
                if abs(moved[i][0] - moved[i - 1][0]) <= tol:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": "scale would place multiple keys at the same time (within tolerance)",
                            "tolerance": float(tol),
                            "times": [moved[i - 1][0], moved[i][0]],
                        }
                    )
            conflicts: list[dict[str, float]] = []
            for nt, _k in moved:
                for okk in outside:
                    if abs(float(okk["time"]) - nt) <= tol:
                        conflicts.append(
                            {"new_time": float(nt), "existing_time": float(okk["time"])}
                        )
            if conflicts and on_conflict == "error":
                return json.dumps(
                    {
                        "ok": False,
                        "error": "scale conflicts with existing keys (use on_conflict=overwrite or skip)",
                        "tolerance": float(tol),
                        "conflicts": conflicts,
                    }
                )
            conflict_idx = set()
            if conflicts and on_conflict == "overwrite":
                for okk in outside:
                    for nt, _k in moved:
                        if abs(float(okk["time"]) - float(nt)) <= tol:
                            conflict_idx.add(int(okk["idx"]))
                            break
                for okk in outside:
                    if int(okk["idx"]) in conflict_idx:
                        remove_keys.append(
                            {
                                "id": int(id),
                                "json_key": ("" if int(id) == 0 else str(jk)),
                                "time": float(okk["time"]),
                            }
                        )
                        overwritten_times.append(float(okk["time"]))
            for nt, k in moved:
                remove_keys.append(
                    {
                        "id": int(id),
                        "json_key": ("" if int(id) == 0 else str(jk)),
                        "time": float(k["time"]),
                    }
                )
                set_keys.append(
                    {
                        "id": int(id),
                        "json_key": ("" if int(id) == 0 else str(jk)),
                        "time": float(nt),
                        "easing": str(k.get("easing", "Linear")),
                        "value": k.get("value"),
                    }
                )
                moved_mappings.append({"from": float(k["time"]), "to": float(nt)})

        if not remove_keys and not set_keys:
            return json.dumps(
                {
                    "ok": True,
                    "id": int(id),
                    "json_key": ("" if int(id) == 0 else str(jk)),
                    "moved": 0,
                    "skipped": int(len(skipped_times)),
                    "overwritten": int(len(overwritten_times)),
                    "scale": float(scale),
                    "anchor": anchor_mode,
                    **({"skipped_times": skipped_times} if include_times else {}),
                }
            )
        try:
            ok = client.batch(
                animation_id=animation_id,
                set_keys=set_keys,
                remove_keys=remove_keys,
                commit=True,
            )
            out = {
                "ok": bool(ok),
                "id": int(id),
                "json_key": ("" if int(id) == 0 else str(jk)),
                "moved": int(len(set_keys)),
                "skipped": int(len(skipped_times)),
                "overwritten": int(len(overwritten_times)),
                "scale": float(scale),
                "anchor": anchor_mode,
                "anchor_time": float(anchor_time),
            }
            if include_times:
                out["moved_mappings"] = moved_mappings
                out["skipped_times"] = skipped_times
                out["overwritten_times"] = overwritten_times
            return json.dumps(out)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_duplicate_keys_range":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        t0 = float(args.get("t0", 0.0))
        t1 = float(args.get("t1", 0.0))
        dest_t0 = float(args.get("dest_t0", 0.0))
        tol = float(args.get("tolerance", 1e-3))
        include_times = bool(args.get("include_times", False))
        on_conflict = _coerce_conflict_policy(args.get("on_conflict"))
        if tol < 0:
            return json.dumps({"ok": False, "error": "tolerance must be >= 0"})
        tmin, tmax = (t0, t1) if t0 <= t1 else (t1, t0)
        jk = _resolve_track_json_key(
            id=id,
            json_key=args.get("json_key"),
            name=args.get("name"),
            _resolve_json_key=_resolve_json_key,
        )
        if id != 0 and not jk:
            return json.dumps({"ok": False, "error": "json_key or name required"})
        jk = jk or ""
        try:
            anim = _save_animation_to_temp(client, animation_id=animation_id)
            all_keys = _extract_track_keys(anim, id=id, json_key=jk)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

        in_range = [
            k
            for k in all_keys
            if (k["time"] >= (tmin - tol) and k["time"] <= (tmax + tol))
        ]
        if not in_range:
            return json.dumps(
                {
                    "ok": True,
                    "id": int(id),
                    "json_key": ("" if int(id) == 0 else str(jk)),
                    "created": 0,
                    "skipped": 0,
                    "overwritten": 0,
                    "dest_t0": float(dest_t0),
                }
            )

        existing_times = [float(k["time"]) for k in all_keys]

        def new_time_for(k: dict[str, Any]) -> float:
            return float(dest_t0) + (float(k["time"]) - float(tmin))

        # Fail fast on negative target times
        for k in in_range:
            nt = new_time_for(k)
            if nt < 0:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "duplicate produces negative key time",
                        "time": float(k["time"]),
                        "new_time": float(nt),
                        "dest_t0": float(dest_t0),
                    }
                )

        remove_keys: list[dict[str, Any]] = []
        set_keys: list[dict[str, Any]] = []
        created_mappings: list[dict[str, float]] = []
        skipped_mappings: list[dict[str, float]] = []
        overwritten_times: list[float] = []

        # Identify conflicts vs any existing key time.
        conflicts: list[tuple[float, dict[str, Any]]] = []
        for k in in_range:
            nt = new_time_for(k)
            if any(abs(nt - t) <= tol for t in existing_times):
                conflicts.append((float(nt), k))

        if conflicts and on_conflict == "error":
            return json.dumps(
                {
                    "ok": False,
                    "error": "duplicate conflicts with existing keys (use on_conflict=overwrite or skip)",
                    "tolerance": float(tol),
                    "conflicts": [
                        {
                            "new_time": float(nt),
                            "existing_times": [
                                t for t in existing_times if abs(t - nt) <= tol
                            ],
                        }
                        for nt, _k in conflicts
                    ],
                }
            )

        conflict_idx = set()
        if conflicts and on_conflict == "overwrite":
            for okk in all_keys:
                for nt, _k in conflicts:
                    if abs(float(okk["time"]) - float(nt)) <= tol:
                        conflict_idx.add(int(okk["idx"]))
                        break
            for okk in all_keys:
                if int(okk["idx"]) in conflict_idx:
                    remove_keys.append(
                        {
                            "id": int(id),
                            "json_key": ("" if int(id) == 0 else str(jk)),
                            "time": float(okk["time"]),
                        }
                    )
                    overwritten_times.append(float(okk["time"]))

        for k in in_range:
            nt = new_time_for(k)
            if on_conflict == "skip" and any(
                abs(nt - t) <= tol for t in existing_times
            ):
                skipped_mappings.append({"from": float(k["time"]), "to": float(nt)})
                continue
            set_keys.append(
                {
                    "id": int(id),
                    "json_key": ("" if int(id) == 0 else str(jk)),
                    "time": float(nt),
                    "easing": str(k.get("easing", "Linear")),
                    "value": k.get("value"),
                }
            )
            created_mappings.append({"from": float(k["time"]), "to": float(nt)})

        if not remove_keys and not set_keys:
            return json.dumps(
                {
                    "ok": True,
                    "id": int(id),
                    "json_key": ("" if int(id) == 0 else str(jk)),
                    "created": 0,
                    "skipped": int(len(skipped_mappings)),
                    "overwritten": int(len(overwritten_times)),
                    "dest_t0": float(dest_t0),
                    **({"skipped_mappings": skipped_mappings} if include_times else {}),
                }
            )
        try:
            ok = client.batch(
                animation_id=animation_id,
                set_keys=set_keys,
                remove_keys=remove_keys,
                commit=True,
            )
            out = {
                "ok": bool(ok),
                "id": int(id),
                "json_key": ("" if int(id) == 0 else str(jk)),
                "created": int(len(set_keys)),
                "skipped": int(len(skipped_mappings)),
                "overwritten": int(len(overwritten_times)),
                "dest_t0": float(dest_t0),
            }
            if include_times:
                out["created_mappings"] = created_mappings
                out["skipped_mappings"] = skipped_mappings
                out["overwritten_times"] = overwritten_times
            return json.dumps(out)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_describe_file":
        schema_dir = args.get("schema_dir")
        sd, searched = discover_schema_dir(schema_dir, atlas_dir)
        try:
            anim = load_animation(Path(str(args.get("path"))))
            caps = {}
            if sd:
                try:
                    caps = load_capabilities(Path(sd))
                except Exception:
                    caps = {}
            style = str(args.get("style", "short"))
            text = summarize_animation(anim, caps, style=style)
            return json.dumps(
                {"ok": True, "summary": text, "schema_dir": str(sd) if sd else None}
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e), "searched": searched})

    if name == "animation_ensure_animation":
        create_new = bool(args.get("create_new", False))
        nm = args.get("name")
        nm_s = str(nm) if isinstance(nm, str) else ""
        resp = client.ensure_animation(create_new=create_new, name=nm_s or None)
        out = {
            "ok": bool(getattr(resp, "ok", False)),
            "animation_id": int(getattr(resp, "animation_id", 0) or 0),
            "created": bool(getattr(resp, "created", False)),
        }
        err = str(getattr(resp, "error", "") or "").strip()
        if err:
            out["error"] = err
        if out["ok"] and out["animation_id"] > 0:
            try:
                ctx.runtime_state["current_animation_id"] = out["animation_id"]
            except Exception:
                pass
        return json.dumps(out)

    if name == "animation_set_duration":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        seconds = float(args.get("seconds", 0.0))
        return json.dumps(
            {"ok": client.set_duration(animation_id=animation_id, seconds=seconds)}
        )

    if name == "animation_set_key_param":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        # Expect native JSON value. Resolve json_key by name if needed; coerce common mistakes.
        id = int(args.get("id"))
        if id == 0:
            return json.dumps(
                {
                    "ok": False,
                    "error": "camera uses camera tools; use animation_replace_key_camera or animation_camera_solve_and_apply",
                }
            )
        json_key = args.get("json_key")
        time_v = float(args.get("time", 0.0))
        easing = _normalize_easing_name(args.get("easing"))
        value_native = args.get("value")
        # Resolve json_key via list_params by display name
        if not json_key:
            name = str(args.get("name") or "").strip()
            if not name:
                return json.dumps({"ok": False, "error": "json_key or name required"})
            try:
                pl = client.list_params(id=id)
                for p in pl.params:
                    if getattr(p, "name", None) == name:
                        json_key = getattr(p, "json_key", None)
                        break
            except Exception:
                json_key = None
            if not json_key:
                return json.dumps(
                    {
                        "ok": False,
                        "error": f"could not resolve json_key for name='{name}'",
                    }
                )
        json_key = str(json_key)
        # Look up param meta and verify existence
        try:
            pl = client.list_params(id=id)
            meta = None
            for p in pl.params:
                if p.json_key == json_key:
                    meta = p
                    break
        except Exception:
            meta = None
        # If not found, try assistive resolution (treat provided json_key as candidate/display name)
        if meta is None and json_key:
            try:
                jk2 = _resolve_json_key(id, candidate=json_key)
                if not jk2:
                    # As a last attempt, allow passing same string as 'name'
                    jk2 = _resolve_json_key(id, name=json_key)
                if jk2:
                    json_key = jk2
                    # refresh meta
                    pl = client.list_params(id=id)
                    for p in pl.params:
                        if p.json_key == json_key:
                            meta = p
                            break
            except Exception:
                pass
        if meta is None:
            return json.dumps({"ok": False, "error": "json_key not found for id"})
        # Coerce booleans if needed
        if meta is not None:
            t = (getattr(meta, "type", "") or "").lower()
            if "bool" in t and isinstance(value_native, str):
                s = value_native.strip().lower()
                if s in ("true", "1", "yes", "on"):
                    value_native = True
                elif s in ("false", "0", "no", "off"):
                    value_native = False
            # Normalize numeric vectors by length if Vec3/Vec4
            if (
                t.endswith("vec4")
                and isinstance(value_native, list)
                and len(value_native) == 4
            ):
                value_native = [float(v) for v in value_native]
            if (
                t.endswith("vec3")
                and isinstance(value_native, list)
                and len(value_native) == 3
            ):
                value_native = [float(v) for v in value_native]
        try:
            ok = client.set_key_param(
                animation_id=animation_id,
                target_id=id,
                json_key=json_key,
                time=time_v,
                easing=easing,
                value=value_native,
            )
            return json.dumps(
                {
                    "ok": ok,
                    "id": id,
                    "json_key": json_key,
                    "time": time_v,
                    "easing": easing,
                }
            )
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_replace_key_param_at_times":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        if id == 0:
            return json.dumps(
                {
                    "ok": False,
                    "error": "camera uses camera tools; use animation_replace_key_camera",
                }
            )
        json_key = str(args.get("json_key"))
        times = args.get("times") or []
        value = args.get("value")
        easing = _normalize_easing_name(args.get("easing"))
        tol = float(args.get("tolerance", 1e-3))
        if not times:
            return json.dumps({"ok": False, "error": "times required"})
        # Verify parameter exists for id
        if not _json_key_exists(id, json_key):
            return json.dumps({"ok": False, "error": "json_key not found for id"})
        try:
            per_time: list[dict[str, Any]] = []
            all_ok = True
            for t in times:
                rm = json.loads(
                    dispatch(
                        "animation_remove_key_param_at_time",
                        json.dumps(
                            {
                                "animation_id": animation_id,
                                "id": id,
                                "json_key": json_key,
                                "time": t,
                                "tolerance": tol,
                            }
                        ),
                    )
                )
                sk = json.loads(
                    dispatch(
                        "animation_set_key_param",
                        json.dumps(
                            {
                                "animation_id": animation_id,
                                "id": id,
                                "json_key": json_key,
                                "time": t,
                                "easing": easing,
                                "value": value,
                            }
                        ),
                    )
                )
                rm_ok = bool(rm.get("ok")) if isinstance(rm, dict) else False
                sk_ok = bool(sk.get("ok")) if isinstance(sk, dict) else False
                all_ok = all_ok and rm_ok and sk_ok
                per_time.append(
                    {
                        "time": float(t),
                        "remove": rm if isinstance(rm, dict) else {"ok": False},
                        "set": sk if isinstance(sk, dict) else {"ok": False},
                    }
                )

            if not all_ok:
                # Do not silently report success when any per-time operation failed.
                return json.dumps(
                    {
                        "ok": False,
                        "error": "one or more times failed (see per_time results)",
                        "count": len(times),
                        "per_time": per_time,
                    }
                )
            return json.dumps({"ok": True, "count": len(times), "per_time": per_time})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_clear_keys":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        if id == 0:
            ok = client.clear_keys(animation_id=animation_id, target_id=0, json_key="")
        else:
            ok = client.clear_keys(
                animation_id=animation_id,
                target_id=id,
                json_key=str(args.get("json_key") or ""),
            )
        return json.dumps({"ok": ok})

    if name == "animation_remove_key":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        if id == 0:
            return json.dumps(
                {
                    "ok": False,
                    "error": "camera uses camera tools; use animation_replace_key_camera",
                }
            )
        json_key = str(args.get("json_key"))
        time_v = float(args.get("time", 0.0))
        # Verify parameter exists
        if not _json_key_exists(id, json_key):
            return json.dumps({"ok": False, "error": "json_key not found for id"})
        ok = client.remove_key(
            animation_id=animation_id, target_id=id, json_key=json_key, time=time_v
        )
        return json.dumps({"ok": ok})

    if name == "animation_batch":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        set_keys = args.get("set_keys") or []
        remove_keys = args.get("remove_keys") or []
        # Normalize per-entry easing aliases (common LLM guesses like "ease-in-out").
        if isinstance(set_keys, list):
            for sk in set_keys:
                if not isinstance(sk, dict):
                    continue
                if "easing" in sk:
                    ez = sk.get("easing")
                    if ez is None:
                        sk.pop("easing", None)
                    else:
                        sk["easing"] = _normalize_easing_name(ez)
        if not set_keys and not remove_keys:
            return json.dumps(
                {
                    "ok": False,
                    "error": "animation_batch called with empty set/remove. Build concrete SetKey entries or use animation_replace_key_param/animation_replace_key_camera (or animation_camera_solve_and_apply).",
                }
            )
        # Verify that each set_key references a valid json_key for its id (non-camera only)
        invalid: list[dict] = []
        params_cache: dict[int, set] = {}
        try:
            for sk in set_keys:
                id = int(sk.get("id", -1))
                if id == 0:
                    continue
                jk = sk.get("json_key")
                if not jk:
                    invalid.append({"reason": "missing json_key", "entry": sk})
                    continue
                if id not in params_cache:
                    try:
                        pl = client.list_params(id=id)
                        params_cache[id] = {
                            getattr(p, "json_key", "") for p in pl.params
                        }
                    except Exception:
                        params_cache[id] = set()
                if str(jk) not in params_cache[id]:
                    invalid.append(
                        {
                            "reason": "json_key not found for id",
                            "json_key": jk,
                            "id": id,
                        }
                    )
        except Exception:
            invalid = []
        if invalid:
            return json.dumps(
                {
                    "ok": False,
                    "error": "validation failed for set_keys",
                    "invalid": invalid,
                }
            )
        ok = client.batch(
            animation_id=animation_id,
            set_keys=set_keys,
            remove_keys=remove_keys,
            commit=bool(args.get("commit", True)),
        )
        if not ok:
            return json.dumps({"ok": False, "error": "batch failed"})

        # Post-verify: ensure all requested SetKey operations actually exist.
        # This avoids silent partial timelines (which are extremely hard for the
        # agent to debug downstream).
        try:
            # Snapshot duration to warn when keys lie outside the clip range (UI
            # often won't show them, and playback won't reach them).
            duration: float | None = None
            try:
                ts = client.get_time(animation_id=animation_id)
                d = getattr(ts, "duration", None)
                if (
                    isinstance(d, (int, float))
                    and math.isfinite(float(d))
                    and float(d) >= 0
                ):
                    duration = float(d)
            except Exception:
                duration = None

            # Group set_keys by track so we only list keys once per track.
            track_to_times: dict[tuple[int, str], list[float]] = {}
            max_set_time: float | None = None
            for s in set_keys:
                if not isinstance(s, dict):
                    continue
                target_id = int(s.get("target_id", s.get("id", -1)))
                jk = "" if target_id == 0 else str(s.get("json_key") or "")
                t = float(s.get("time", 0.0))
                track_to_times.setdefault((target_id, jk), []).append(t)
                if max_set_time is None or t > max_set_time:
                    max_set_time = t

            missing: list[dict[str, Any]] = []
            eps = 1e-6  # must match engine epsilon (kKeyTimeEpsSec)
            for (target_id, jk), want_times in track_to_times.items():
                lr = client.list_keys(
                    animation_id=animation_id,
                    target_id=target_id,
                    json_key=jk,
                    include_values=False,
                )
                have_times = [float(k.time) for k in getattr(lr, "keys", [])]
                for t in want_times:
                    if not any(abs(float(t) - float(ht)) < eps for ht in have_times):
                        missing.append(
                            {"id": int(target_id), "json_key": jk, "time": float(t)}
                        )

            if missing:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "batch returned ok but some keys were not found after verify",
                        "missing": missing,
                        **(
                            {"duration": float(duration)}
                            if duration is not None
                            else {}
                        ),
                    }
                )

            resp: dict[str, Any] = {
                "ok": True,
                "set_count": int(len(set_keys)) if isinstance(set_keys, list) else 0,
                "remove_count": int(len(remove_keys))
                if isinstance(remove_keys, list)
                else 0,
            }
            if duration is not None:
                resp["duration"] = float(duration)
            if (
                duration is not None
                and max_set_time is not None
                and max_set_time > float(duration) + eps
            ):
                resp["warning"] = (
                    "some keys are beyond the current animation duration; they may not be visible in the UI timeline and playback won't reach them"
                )
                resp["max_set_time"] = float(max_set_time)
                resp["suggested_duration"] = float(max_set_time)
            return json.dumps(resp)
        except Exception as e:
            # Batch itself succeeded; verification is best-effort.
            return json.dumps({"ok": True, "warning": f"batch verify failed: {e}"})

    if name == "animation_set_time":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        ok = client.set_time(
            animation_id=animation_id,
            seconds=float(args.get("seconds", 0.0)),
            cancel_rendering=bool(args.get("cancel", False)),
        )
        return json.dumps({"ok": ok})

    if name == "animation_save_keyframe":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        time_v = float(args.get("time", 0.0))
        cancel = bool(args.get("cancel_rendering", True))
        try:
            ok = client.add_key_frame(
                animation_id=animation_id,
                time=time_v,
                cancel_rendering=cancel,
            )
            return json.dumps({"ok": bool(ok)})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_save_animation":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        p = Path(str(args.get("path") or ""))
        ok = bool(client.save_animation(animation_id=animation_id, path=p))
        if ok:
            try:
                if ctx.session_store is not None:
                    ctx.session_store.set_meta(
                        last_animation_save_path=str(p.expanduser().resolve())
                    )
            except Exception:
                pass
        return json.dumps({"ok": ok})

    if name == "animation_export_video":
        # Export .animation3d to MP4 by invoking headless Atlas

        out = args.get("out")
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        if not out:
            return json.dumps({"ok": False, "error": "out is required"})
        # Resolve Atlas binary
        atlas_bin = None
        if atlas_dir:
            try:
                ab, _ = compute_paths_from_atlas_dir(Path(atlas_dir))
                atlas_bin = ab
            except Exception:
                atlas_bin = None
        if atlas_bin is None:
            for d in default_install_dirs():
                ab, _ = compute_paths_from_atlas_dir(d)
                if ab.exists():
                    atlas_bin = ab
                    break
        if atlas_bin is None:
            return json.dumps(
                {
                    "ok": False,
                    "error": "Atlas binary not found; ensure Atlas is installed",
                }
            )
        # Save the current animation to a stable temp file and export from that file.
        try:
            if ctx.session_store is not None:
                tdir = ctx.session_store.root / "artifacts"
                tdir.mkdir(parents=True, exist_ok=True)
            else:
                tdir = Path(tempfile.mkdtemp(prefix="atlas_export_"))
        except Exception:
            tdir = Path(tempfile.mkdtemp(prefix="atlas_export_"))
        anim_path = tdir / "export.animation3d"
        ok_save = bool(client.save_animation(animation_id=animation_id, path=anim_path))
        if not ok_save:
            return json.dumps(
                {"ok": False, "error": "failed to save animation for export"}
            )
        rc = export_video(
            atlas_bin=str(atlas_bin),
            animation_path=anim_path,
            output_video=Path(out),
            fps=int(args.get("fps", 30)),
            start=int(args.get("start", 0)),
            end=int(args.get("end", -1)),
            width=int(args.get("width", 1920)),
            height=int(args.get("height", 1080)),
            overwrite=bool(args.get("overwrite", True)),
            use_gpu_devices=None,
        )
        return json.dumps(
            {
                "ok": rc == 0,
                "exit_code": rc,
                "animation_path": str(anim_path),
                "out": str(out),
            }
        )

    if name == "animation_render_preview":
        # Privacy/consent gate: requires an explicit per-session user decision.
        allow = False
        try:
            if ctx.session_store is not None:
                allow = ctx.session_store.get_consent("screenshots") is True
        except Exception:
            allow = False
        if not allow:
            return json.dumps(
                {
                    "ok": False,
                    "error": "screenshots not permitted for this session",
                }
            )

        fps = int(float(args.get("fps", 30)))
        if fps < 1:
            fps = 1
        tsec_requested = float(args.get("time", 0.0))
        if not math.isfinite(tsec_requested):
            tsec_requested = 0.0
        width = int(args.get("width", 1600))
        height = int(args.get("height", 900))
        requested_frame_idx = int(math.floor(tsec_requested * fps + 1e-9))
        # Resolve Atlas binary
        atlas_bin = None
        if atlas_dir:
            try:
                ab, _ = compute_paths_from_atlas_dir(Path(atlas_dir))
                atlas_bin = ab
            except Exception:
                atlas_bin = None
        if atlas_bin is None:
            for d in default_install_dirs():
                ab, _ = compute_paths_from_atlas_dir(d)
                if ab.exists():
                    atlas_bin = ab
                    break
        if atlas_bin is None:
            return json.dumps(
                {
                    "ok": False,
                    "error": "Atlas binary not found; ensure Atlas is installed",
                }
            )
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        # Save animation to a temp file and render a single frame
        tdir = Path(tempfile.mkdtemp(prefix="atlas_preview_"))
        anim_path = tdir / "preview.animation3d"
        ok_save = client.save_animation(animation_id=animation_id, path=anim_path)
        if not ok_save:
            return json.dumps(
                {"ok": False, "error": "failed to save temporary animation"}
            )

        # Headless previews are frame-index based. Map the requested time to a
        # renderable frame index, clamping when the request is at/after the clip
        # end time (common case: time == Duration).
        duration_s: float | None = None
        try:
            anim = load_animation(anim_path)
            d = anim.get("Duration") if isinstance(anim, dict) else None
            if (
                isinstance(d, (int, float))
                and math.isfinite(float(d))
                and float(d) >= 0
            ):
                duration_s = float(d)
        except Exception:
            duration_s = None

        frame_idx = requested_frame_idx
        if duration_s is not None:
            total_frames = int(math.floor(duration_s * fps + 1e-9))
            max_frame_idx = max(0, total_frames - 1) if total_frames > 0 else 0
            frame_idx = min(frame_idx, max_frame_idx)
        frame_idx = max(0, frame_idx)
        tsec_resolved = float(frame_idx) / float(fps)
        time_adjusted = frame_idx != requested_frame_idx

        frames_dir = tdir / "frames"
        frames_dir.mkdir(parents=True, exist_ok=True)
        rc = preview_frames(
            atlas_bin=str(atlas_bin),
            animation_path=anim_path,
            out_dir=frames_dir,
            fps=fps,
            start=frame_idx,
            end=frame_idx,
            width=width,
            height=height,
            overwrite=True,
            dummy_output=str(tdir / "dummy.mp4"),
            # Prefer a single deterministic PNG output for model inspection.
            tile_size=0,
            tile_border=0,
        )
        if rc != 0:
            return json.dumps(
                {"ok": False, "exit_code": rc, "error": "preview renderer failed"}
            )
        # Find the produced image. The exporter writes PNG; enforce that we return
        # exactly one PNG so callers can rely on stable behavior.
        images = sorted(glob.glob(str(frames_dir / "*.png")))
        if not images:
            return json.dumps({"ok": False, "error": "no PNG image produced"})
        if len(images) != 1:
            return json.dumps(
                {
                    "ok": False,
                    "error": "expected exactly one PNG image but found multiple",
                    "paths": images,
                }
            )
        out: dict[str, Any] = {
            "ok": True,
            "path": images[0],
            "width": int(width),
            "height": int(height),
        }
        out["fps"] = int(fps)
        out["requested_time"] = float(tsec_requested)
        out["resolved_time"] = float(tsec_resolved)
        out["frame_idx"] = int(frame_idx)
        if duration_s is not None:
            out["duration"] = float(duration_s)
        if time_adjusted:
            out["note"] = "preview time was mapped/clamped to a renderable frame index"
        return json.dumps(out)

    return None
