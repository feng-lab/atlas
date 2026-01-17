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
from ...tool_registry import Tool, tool_from_schema

# Fail-fast for internal exporter helpers
from ...exporter import export_video, preview_frames
from .context import ToolDispatchContext
from .preconditions import (
    require_animation_id,
    require_engine_ready,
    require_screenshot_consent,
)

JSON_VALUE_SCHEMA: Dict[str, Any] = {
    "description": "Native JSON value (supports object/array/scalars; nested structures allowed).",
    # Use anyOf rather than a "type": [...] union. Some providers validate a more
    # restrictive subset of JSON Schema and require explicit items for any
    # schema that can be an array.
    "anyOf": [
        {"type": "object"},
        {"type": "array", "items": {}},
        {"type": "number"},
        {"type": "string"},
        {"type": "boolean"},
        {"type": "null"},
    ],
}

ANIMATION_ID_PARAM_SCHEMA: Dict[str, Any] = {
    "type": "integer",
    "description": "Animation3D object id. Use animation_ensure_animation to create/select an animation and obtain this id.",
}


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


TOOLS: List[Tool] = [
    tool_from_schema(
        name="animation_describe_file",
        description="Parse an .animation3d file and return a concise natural-language summary (uses capabilities.json for names).",
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Path to a .animation3d file."},
                "schema_dir": {"type": ["string", "null"], "description": "Optional schema directory override for capabilities.json lookups."},
                "style": {"type": "string", "enum": ["short", "long"], "default": "short", "description": "Summary style."},
            },
            "required": ["path"],
        },
        preconditions=(),
        handler=_tool_handler("animation_describe_file"),
    ),
    tool_from_schema(
        name="animation_camera_solve_and_apply",
        description="Animation timeline: solve camera (FIT|ORBIT|DOLLY|STATIC) and write validated keys. Clears existing camera keys in [t0,t1] by default (tolerance-aware). Not for scene-only FIT. Returns {applied:[times...], total}.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "mode": {"type": "string", "enum": ["FIT", "ORBIT", "DOLLY", "STATIC"], "description": "Solve mode for generating camera keys."},
                "ids": {"type": "array", "items": {"type": "integer"}, "description": "Target ids; empty uses fit_candidates()."},
                "t0": {"type": "number", "description": "Start time (seconds) of the write window."},
                "t1": {"type": "number", "description": "End time (seconds) of the write window."},
                "constraints": {"type": "object", "description": "Visibility/coverage constraints (keep_visible, margin, min_coverage, fov policy)."},
                "params": {"type": "object", "description": "Mode-specific parameters (e.g., axis for ORBIT)."},
                "degrees": {"type": "number", "description": "ORBIT: total rotation in degrees (default 360)."},
                "tolerance": {"type": "number", "default": 1e-3, "description": "Time tolerance used when clearing/replacing keys."},
                "easing": {"type": "string", "default": "Linear", "description": "Easing to assign to written keys."},
                "clear_range": {"type": "boolean", "default": True, "description": "Remove existing camera keys inside [t0,t1] (within tolerance) before applying new keys."},
            },
            "required": ["animation_id", "mode", "ids", "t0", "t1"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_camera_solve_and_apply"),
    ),
    # Camera interpolation method tools are intentionally disabled for now.
    # Only the default "Center" mode is considered stable.
    tool_from_schema(
        name="animation_camera_waypoint_spline_apply",
        description=(
            "Guided waypoint camera path (timeline): solve typed camera keys from bbox/world waypoints, optionally clear existing camera keys in [t0,t1], "
            "then write validated camera keys.\n\n"
            "Note: camera interpolation methods beyond the default Center mode are currently disabled. "
            "For smoother motion from sparse waypoints, provide additional waypoints or use the walkthrough tool (which samples a denser key sequence)."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "ids": {"type": "array", "items": {"type": "integer"}, "description": "Target ids for bbox computations and validation. Empty → fit_candidates() for validation; bbox computations use all visual objects."},
                "after_clipping": {"type": "boolean", "default": True, "description": "Use clipped bbox for bbox-fraction waypoints."},
                "t0": {"type": "number", "description": "Start time (seconds)."},
                "t1": {"type": "number", "description": "End time (seconds)."},
                "waypoints": {"type": "array", "items": {"type": "object"}, "description": "Waypoints with either absolute time or normalized u in [0,1]. Each waypoint: {u?:number, time?:number, eye?:{world:[x,y,z]|bbox_fraction:[fx,fy,fz]}, look_at?:{world:[x,y,z]|bbox_center:true|bbox_fraction:[fx,fy,fz]}}. If look_at is omitted, the solver preserves the previous view direction and center distance."},
                "easing": {"type": "string", "default": "Linear", "description": "Easing assigned to written camera keys (note: spline path uses waypoint geometry)."},
                "clear_range": {"type": "boolean", "default": True, "description": "Remove existing camera keys inside [t0,t1] (tolerance-aware) before applying new keys."},
                "tolerance": {"type": "number", "default": 1e-3, "description": "Time tolerance used when clearing/replacing keys."},
                "constraints": {"type": "object", "description": "Camera validation constraints. For interior walkthroughs, set keep_visible=false (disables the coverage requirement)."},
                "base_value": {"type": "object", "description": "Optional base camera value used as defaults for projection/fov/up and for the initial direction when look_at is omitted."},
            },
            "required": ["animation_id", "t0", "t1", "waypoints"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_camera_waypoint_spline_apply"),
    ),
    tool_from_schema(
        name="animation_camera_walkthrough_apply",
        description=(
            "First-person walkthrough authoring: build a smooth camera path from high-level motion segments (local moves + yaw/pitch/roll), "
            "optionally clear existing camera keys in [t0,t1], then write validated camera keys. "
            "This is intended for 'fly/drone inside the object' requests where users describe motion in words and the agent invents segments.\n\n"
            "Segment timing modes (choose ONE):\n"
            "- Explicit: every segment has u0/u1 in [0,1]\n"
            "- Duration: every segment has duration (seconds); durations are normalized to fill [t0,t1]\n"
            "- Equal: no u0/u1/duration provided; segments split [t0,t1] evenly\n\n"
            "Motion units:\n"
            "- move distances are fractions of the target bbox enclosing-sphere radius (so no world-unit guessing).\n"
            "- rotation is in degrees.\n"
            "Sampling:\n"
            "- Motion is approximated by sampling each segment at step_seconds; decrease step_seconds for higher-fidelity curved motion."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "ids": {"type": "array", "items": {"type": "integer"}, "description": "Target ids for bbox computations and validation. Empty → fit_candidates() for validation; bbox computations use all visual objects."},
                "after_clipping": {"type": "boolean", "default": True, "description": "Use clipped bbox for bbox-scaled movement."},
                "t0": {"type": "number", "description": "Start time (seconds)."},
                "t1": {"type": "number", "description": "End time (seconds)."},
                "segments": {"type": "array", "items": {"type": "object"}, "description": "Motion segments. Each segment may include: u0/u1 (0..1) OR duration (seconds) OR neither (equal split); move: {forward?, back?, right?, left?, up?, down?} (fractions of bbox radius; signed values allowed); rotate: {yaw?, pitch?, roll?} (degrees); pause:true; template:'enter'|'turn_right'|'pause'; amount/distance/degrees; label."},
                "step_seconds": {"type": "number", "default": 1.0, "description": "Sampling step used to approximate motion inside each segment. Smaller → more keys (smoother curved motion)."},
                "easing": {"type": "string", "default": "Linear", "description": "Easing assigned to written camera keys (note: spline path uses key geometry)."},
                "clear_range": {"type": "boolean", "default": True, "description": "Remove existing camera keys inside [t0,t1] (tolerance-aware) before applying new keys."},
                "tolerance": {"type": "number", "default": 1e-3, "description": "Time tolerance used when clearing/replacing keys."},
                "constraints": {"type": "object", "description": "Camera validation constraints. For interior walkthroughs, set keep_visible=false (disables the coverage requirement)."},
                "base_value": {"type": "object", "description": "Optional base camera value used as the initial camera pose (projection/fov/up defaults)."},
            },
            "required": ["animation_id", "t0", "t1", "segments"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_camera_walkthrough_apply"),
    ),
    tool_from_schema(
        name="animation_camera_validate",
        description="Animation timeline: validate camera values against visibility/coverage constraints. Values are optional; when omitted, the server samples the current animation camera at the given times.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "ids": {"type": "array", "items": {"type": "integer"}, "description": "Target ids to validate against (typically fit_candidates)."},
                "times": {"type": "array", "items": {"type": "number"}, "description": "Times (seconds) to validate."},
                "values": {"type": "array", "items": {"type": "object"}, "description": "Optional: typed camera values aligned with times. If omitted or shorter than times, the server fills by sampling."},
                "constraints": {"type": "object", "description": "Visibility/coverage constraints (keep_visible, margin, min_coverage, etc.)."},
                "policies": {"type": "object", "description": "Adjustment policies (adjust_fov, adjust_distance, adjust_clipping)."},
            },
            "required": ["animation_id", "ids", "times"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_camera_validate"),
    ),
    tool_from_schema(
        name="animation_set_param_by_name",
        description="Set a parameter by display name (case-insensitive) by id. Resolves json_key via scene_list_params, then calls animation_set_key_param. Id map: 1=background, 2=axis, 3=global, ≥4=objects.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
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
                "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
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
                "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
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
        description="Replace (or set) a camera key at time: remove any camera key within tolerance then set a new camera value. Use for explicit single-time edits. If you already used animation_camera_solve_and_apply for this segment, do NOT call this afterward to 'finalize' — keys are already written.",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "time": {"type": "number"},
                "easing": {"type": "string", "default": "Linear"},
                "value": {"type": "object"},
                "ids": {"type": "array", "items": {"type": "integer"}, "description": "Optional ids for camera validation. When omitted/empty, uses fit_candidates()."},
                "constraints": {"type": "object", "description": "Optional camera validation constraints. When omitted, defaults to keep_visible=true and min_coverage=0.95."},
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
                "id": {"type": "integer", "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                "json_key": {"type": "string", "description": "Parameter json_key (ignored for camera); use canonical names from scene_list_params(id)"},
                "include_values": {"type": "boolean", "description": "True to include value_json samples for each key"},
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
                "id": {"type": "integer", "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                "json_key": {"type": "string", "description": "Canonical json_key (ignored for camera)."},
                "name": {"type": "string", "description": "Optional display name to resolve to json_key when json_key is not provided (ignored for camera)."},
                "t0": {"type": "number", "description": "Range start time (seconds)."},
                "t1": {"type": "number", "description": "Range end time (seconds)."},
                "tolerance": {"type": "number", "default": 1e-3, "description": "Time tolerance used for range boundary inclusion and conflict matching."},
                "include_times": {"type": "boolean", "default": False, "description": "When true, include the full list of removed key times (no truncation)."},
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
                "id": {"type": "integer", "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                "json_key": {"type": "string", "description": "Canonical json_key (ignored for camera)."},
                "name": {"type": "string", "description": "Optional display name to resolve to json_key when json_key is not provided (ignored for camera)."},
                "t0": {"type": "number", "description": "Range start time (seconds)."},
                "t1": {"type": "number", "description": "Range end time (seconds)."},
                "delta": {"type": "number", "description": "Time shift in seconds (can be negative)."},
                "tolerance": {"type": "number", "default": 1e-3, "description": "Time tolerance used for range boundary inclusion and conflict matching."},
                "on_conflict": {"type": "string", "enum": ["error", "overwrite", "skip"], "default": "error", "description": "If shifted keys land on existing key times: error (abort), overwrite (remove existing keys), or skip (leave conflicting keys unmoved)."},
                "include_times": {"type": "boolean", "default": False, "description": "When true, include the full list of moved/skipped mappings (no truncation)."},
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
                "id": {"type": "integer", "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                "json_key": {"type": "string", "description": "Canonical json_key (ignored for camera)."},
                "name": {"type": "string", "description": "Optional display name to resolve to json_key when json_key is not provided (ignored for camera)."},
                "t0": {"type": "number", "description": "Range start time (seconds)."},
                "t1": {"type": "number", "description": "Range end time (seconds)."},
                "scale": {"type": "number", "description": "Scale factor (>0)."},
                "anchor": {"type": "string", "enum": ["t0", "center"], "default": "t0", "description": "Anchor mode for scaling: t0 uses the range start; center uses (t0+t1)/2."},
                "tolerance": {"type": "number", "default": 1e-3, "description": "Time tolerance used for range boundary inclusion and conflict matching."},
                "on_conflict": {"type": "string", "enum": ["error", "overwrite", "skip"], "default": "error", "description": "If scaled keys land on existing key times: error (abort), overwrite (remove existing keys), or skip (leave conflicting keys unmoved)."},
                "include_times": {"type": "boolean", "default": False, "description": "When true, include the full list of moved/skipped mappings (no truncation)."},
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
                "id": {"type": "integer", "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                "json_key": {"type": "string", "description": "Canonical json_key (ignored for camera)."},
                "name": {"type": "string", "description": "Optional display name to resolve to json_key when json_key is not provided (ignored for camera)."},
                "t0": {"type": "number", "description": "Source range start time (seconds)."},
                "t1": {"type": "number", "description": "Source range end time (seconds)."},
                "dest_t0": {"type": "number", "description": "Destination start time (seconds). Keys keep their relative offsets from the source range start."},
                "tolerance": {"type": "number", "default": 1e-3, "description": "Time tolerance used for range boundary inclusion and conflict matching."},
                "on_conflict": {"type": "string", "enum": ["error", "overwrite", "skip"], "default": "error", "description": "If duplicated keys land on existing key times: error (abort), overwrite (remove existing keys), or skip (do not create conflicting duplicates)."},
                "include_times": {"type": "boolean", "default": False, "description": "When true, include the full list of duplicated/skipped mappings (no truncation)."},
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
        description="Ensure a 3D animation exists and is bound/selected for editing. Returns {ok, animation_id, created}. Use the returned animation_id for all subsequent animation_* tool calls.",
        parameters_schema={"type": "object", "properties": {"create_new": {"type": "boolean", "default": False, "description": "When true, create a new Animation3D object (instead of reusing an existing one)."}, "name": {"type": "string", "default": "", "description": "Optional display name for the created animation (empty means default)."}}},
        preconditions=(require_engine_ready,),
        handler=_tool_handler("animation_ensure_animation"),
    ),
    tool_from_schema(
        name="animation_set_duration",
        description="Set animation duration in seconds.",
        parameters_schema={
            "type": "object",
            "properties": {"animation_id": dict(ANIMATION_ID_PARAM_SCHEMA), "seconds": {"type": "number"}},
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
                "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
                "json_key": {"type": "string", "description": "Canonical json_key; if omitted, 'name' is used to resolve."},
                "name": {"type": "string", "description": "Display name to resolve to json_key when json_key is not provided."},
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
        description="Replace keys at the specified times for a parameter by id (1=background,2=axis,3=global,≥4=objects).",
        parameters_schema={
            "type": "object",
            "properties": {
                "animation_id": dict(ANIMATION_ID_PARAM_SCHEMA),
                "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
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
                "id": {"type": "integer", "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
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
                "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
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
                "set_keys": {"type": "array", "items": {"type": "object"}, "description": "List of SetKey operations"},
                "remove_keys": {"type": "array", "items": {"type": "object"}, "description": "List of RemoveKey operations"},
                "commit": {"type": "boolean", "default": True, "description": "Commit immediately if true"},
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
                "cancel": {"type": "boolean", "default": False, "description": "When true, cancel any in-progress rendering/export tied to the animation view."},
            },
            "required": ["animation_id", "seconds"],
        },
        preconditions=(require_animation_id, require_engine_ready),
        handler=_tool_handler("animation_set_time"),
    ),
    tool_from_schema(
        name="animation_save_animation",
        description="Save the current animation to a .animation3d path.",
        parameters_schema={
            "type": "object",
            "properties": {"animation_id": dict(ANIMATION_ID_PARAM_SCHEMA), "path": {"type": "string"}},
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
                "fps": {"type": "number", "default": 30, "description": "Frames per second"},
                "start": {"type": "integer", "default": 0, "description": "Start frame (inclusive)"},
                "end": {"type": "integer", "default": -1, "description": "End frame (inclusive, -1 = duration)"},
                "width": {"type": "integer", "default": 1920, "description": "Output width"},
                "height": {"type": "integer", "default": 1080, "description": "Output height"},
                "overwrite": {"type": "boolean", "default": True, "description": "Overwrite output if exists"},
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
                "fps": {"type": "number", "default": 30, "description": "Frames per second"},
                "width": {"type": "integer", "description": "Image width"},
                "height": {"type": "integer", "description": "Image height"},
            },
            "required": ["animation_id", "time", "width", "height"],
        },
        preconditions=(require_screenshot_consent, require_animation_id, require_engine_ready),
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
        out.append({"idx": int(idx), "time": float(tm), "easing": easing, "value": k.get("value")})
    return out


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    client = ctx.client
    atlas_dir = ctx.atlas_dir
    dispatch = ctx.dispatch
    _param_to_dict = ctx.param_to_dict
    _resolve_json_key = ctx.resolve_json_key
    _json_key_exists = ctx.json_key_exists
    _schema_validator_cache = ctx.schema_validator_cache

    if name == "animation_set_param_by_name":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        id = int(args.get("id"))
        name_str = str(args.get("name", ""))
        type_hint = args.get("type_hint")
        time_v = float(args.get("time", 0.0))
        easing = str(args.get("easing", "Linear"))
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
            lr = client.list_keys(animation_id=animation_id, target_id=id, json_key=json_key, include_values=False)
            times = [k.time for k in getattr(lr, "keys", [])]
            to_remove = [t for t in times if abs(t - time_v) <= tol]
            removed = 0
            for t in to_remove:
                ok = client.remove_key(animation_id=animation_id, target_id=id, json_key=json_key, time=t)
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
        easing = str(args.get("easing", "Linear"))
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
        easing = str(args.get("easing", "Linear"))
        value = args.get("value") or {}
        tol = float(args.get("tolerance", 1e-3))
        ids = args.get("ids") or []
        if not ids:
            try:
                ids = client.fit_candidates()
            except Exception:
                ids = []
        constraints = args.get("constraints") or {
            "keep_visible": True,
            "min_coverage": 0.95,
        }
        # First pass policies allow adjustments
        policies1 = {
            "adjust_fov": True,
            "adjust_distance": True,
            "adjust_clipping": True,
        }
        # Second pass: strict verification without adjustments
        policies2 = {
            "adjust_fov": False,
            "adjust_distance": False,
            "adjust_clipping": False,
        }
        try:
            # Remove camera keys within tolerance
            try:
                lr = client.list_keys(animation_id=animation_id, target_id=0, include_values=False)
                times = [k.time for k in getattr(lr, "keys", [])]
                to_remove = [t for t in times if abs(t - time_v) <= tol]
                removed = 0
                for t in to_remove:
                    if client.remove_key(animation_id=animation_id, target_id=0, json_key="", time=t):
                        removed += 1
            except Exception:
                removed = 0
            # Validate and accept adjusted value if provided
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
            ok = client.set_key_camera(animation_id=animation_id, time=time_v, easing=easing, value=value)
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
            constraints = args.get("constraints") or {
                "keep_visible": True,
                "min_coverage": 0.95,
            }
            params = args.get("params") or {}
            # Defaults for ORBIT
            if mode_up == "ORBIT":
                params.setdefault("axis", "y")
                # Top-level degrees is the single agent-facing knob; backend expects 'degrees'
                try:
                    deg = float(args.get("degrees", 360.0))
                except Exception:
                    deg = 360.0
                params["degrees"] = deg
            tol = float(args.get("tolerance", 1e-3))
            easing = str(args.get("easing", "Linear"))
            clear_range = bool(args.get("clear_range", True))
            keys = client.camera_solve(
                mode=mode_up,
                ids=ids,
                t0=t0,
                t1=t1,
                constraints=constraints,
                params=params,
            )
            # Optionally clear existing keys in [t0, t1] (with tolerance), excluding solver times
            if clear_range:
                try:
                    lr = client.list_keys(animation_id=animation_id, target_id=0, include_values=False)
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
                        client.remove_key(animation_id=animation_id, target_id=0, json_key="", time=old_t)
                    except Exception:
                        pass
            applied: list[float] = []
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
                except Exception:
                    continue
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
            return json.dumps(
                {"ok": bool(res.get("ok", False)), "results": res.get("results")}
            )
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
                return json.dumps({"ok": False, "error": "waypoints must be a non-empty list"})
            if len(waypoints_in) < 2:
                return json.dumps({"ok": False, "error": "at least 2 waypoints are required"})
            easing = str(args.get("easing", "Linear"))
            tol = float(args.get("tolerance", 1e-3))
            if not math.isfinite(tol) or tol < 0.0:
                return json.dumps({"ok": False, "error": "tolerance must be a finite number >= 0"})
            clear_range = bool(args.get("clear_range", True))
            constraints = args.get("constraints") or {
                "keep_visible": True,
                "min_coverage": 0.95,
            }
            base_value = args.get("base_value")

            # Normalize waypoint times: allow either absolute time or u in [0,1].
            span = float(t1 - t0)
            waypoints: list[dict] = []
            used_times: set[float] = set()
            for w in waypoints_in:
                if not isinstance(w, dict):
                    return json.dumps(
                        {"ok": False, "error": "each waypoint must be an object"}
                    )
                if "time" in w:
                    tm = float(w.get("time", 0.0))
                elif "u" in w:
                    u = float(w.get("u", 0.0))
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
                    return json.dumps({"ok": False, "error": "waypoint time must be finite and >= 0"})
                if tm < t0 - tol or tm > t1 + tol:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": f"waypoint time {tm} is outside the apply window [{t0},{t1}] (tolerance={tol}). Adjust t0/t1 or use different waypoint times.",
                        }
                    )
                if tm in used_times:
                    return json.dumps({"ok": False, "error": f"duplicate waypoint time: {tm}"})
                used_times.add(tm)

                entry: dict[str, Any] = {"time": tm}

                eye = w.get("eye")
                if eye is not None:
                    if not isinstance(eye, dict):
                        return json.dumps({"ok": False, "error": "waypoint.eye must be an object"})
                    if ("world" in eye) and ("bbox_fraction" in eye):
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "waypoint.eye must have exactly one of: world | bbox_fraction",
                            }
                        )
                    if "world" in eye:
                        v = eye.get("world")
                        if not (isinstance(v, list) and len(v) == 3):
                            return json.dumps({"ok": False, "error": "eye.world must be [x,y,z]"})
                        if not all(math.isfinite(float(x)) for x in v):
                            return json.dumps({"ok": False, "error": "eye.world must contain finite numbers"})
                        entry["eye"] = {"world": [float(v[0]), float(v[1]), float(v[2])]}
                    elif "bbox_fraction" in eye:
                        v = eye.get("bbox_fraction")
                        if not (isinstance(v, list) and len(v) == 3):
                            return json.dumps({"ok": False, "error": "eye.bbox_fraction must be [fx,fy,fz]"})
                        vv = [float(v[0]), float(v[1]), float(v[2])]
                        if not all(math.isfinite(x) for x in vv):
                            return json.dumps({"ok": False, "error": "eye.bbox_fraction must contain finite numbers"})
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
                        return json.dumps({"ok": False, "error": "waypoint.look_at must be an object"})
                    modes = int("world" in look) + int("bbox_fraction" in look) + int(look.get("bbox_center") is True)
                    if modes != 1:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "waypoint.look_at must have exactly one of: world | bbox_center:true | bbox_fraction",
                            }
                        )
                    if "world" in look:
                        v = look.get("world")
                        if not (isinstance(v, list) and len(v) == 3):
                            return json.dumps({"ok": False, "error": "look_at.world must be [x,y,z]"})
                        if not all(math.isfinite(float(x)) for x in v):
                            return json.dumps({"ok": False, "error": "look_at.world must contain finite numbers"})
                        entry["look_at"] = {"world": [float(v[0]), float(v[1]), float(v[2])]}
                    elif look.get("bbox_center") is True:
                        entry["look_at"] = {"bbox_center": True}
                    else:
                        v = look.get("bbox_fraction")
                        if not (isinstance(v, list) and len(v) == 3):
                            return json.dumps({"ok": False, "error": "look_at.bbox_fraction must be [fx,fy,fz]"})
                        vv = [float(v[0]), float(v[1]), float(v[2])]
                        if not all(math.isfinite(x) for x in vv):
                            return json.dumps({"ok": False, "error": "look_at.bbox_fraction must contain finite numbers"})
                        if any((x < 0.0 or x > 1.0) for x in vv):
                            return json.dumps(
                                {
                                    "ok": False,
                                    "error": "look_at.bbox_fraction values must be in [0,1]. For out-of-bbox points, use look_at.world.",
                                }
                            )
                        entry["look_at"] = {"bbox_fraction": vv}

                waypoints.append(entry)

            # Solve typed camera keys (no writes) from waypoints.
            keys = client.camera_path_solve(
                ids=ids,
                after_clipping=after_clipping,
                base_value=base_value if isinstance(base_value, dict) else None,
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
                    return json.dumps({"ok": False, "error": f"failed to clear camera keys in range: {clear_res.get('error')}"})

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
                                "error": rr.get("error") or rr.get("reason") or "apply_failed",
                            }
                        )
                except Exception as e:
                    failed.append({"time": float(k.get("time", 0.0) or 0.0), "error": str(e)})

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
                return json.dumps({"ok": False, "error": "segments must be a non-empty list"})

            easing = str(args.get("easing", "Linear"))
            tol = float(args.get("tolerance", 1e-3))
            if not math.isfinite(tol) or tol < 0.0:
                return json.dumps({"ok": False, "error": "tolerance must be a finite number >= 0"})
            step_seconds = float(args.get("step_seconds", 1.0))
            if not math.isfinite(step_seconds) or step_seconds <= 0.0:
                return json.dumps({"ok": False, "error": "step_seconds must be a finite number > 0"})
            clear_range = bool(args.get("clear_range", True))
            constraints = args.get("constraints") or {"keep_visible": False}
            base_value = args.get("base_value")

            # Initial camera pose.
            cam: dict
            if isinstance(base_value, dict) and base_value:
                cam = base_value
            else:
                cam = client.camera_get()
            if not isinstance(cam, dict) or not cam:
                return json.dumps({"ok": False, "error": "failed to get a base camera value"})

            # Normalize segments to time ranges inside [t0,t1].
            segs: list[dict[str, Any]] = []
            for s in segments_in:
                if not isinstance(s, dict):
                    return json.dumps({"ok": False, "error": "each segment must be an object"})
                segs.append(s)

            try:
                segs = expand_walkthrough_segments(segs)
            except Exception as e:
                return json.dumps({"ok": False, "error": f"segment template expansion failed: {e}"})

            span = float(t1 - t0)
            any_u = any(("u0" in s) or ("u1" in s) for s in segs)
            any_dur = any("duration" in s for s in segs)
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
                    if ("u0" not in s) or ("u1" not in s):
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "when using u0/u1 timing, every segment must include both u0 and u1",
                            }
                        )
                    u0 = float(s.get("u0", 0.0))
                    u1 = float(s.get("u1", 0.0))
                    if not (math.isfinite(u0) and math.isfinite(u1)):
                        return json.dumps({"ok": False, "error": "segment u0/u1 must be finite"})
                    if u0 < 0.0 or u1 < 0.0 or u0 > 1.0 or u1 > 1.0:
                        return json.dumps({"ok": False, "error": "segment u0/u1 must be in [0,1]"})
                    if not (u1 > u0):
                        return json.dumps({"ok": False, "error": "segment u1 must be > u0"})
                    explicit.append((u0, u1, s))
                explicit.sort(key=lambda it: float(it[0]))

                # Fill gaps with pause segments so interpolation doesn't move during gaps.
                eps = 1e-9
                ucur = 0.0
                normalized: list[tuple[float, float, dict[str, Any]]] = []
                for u0, u1, s in explicit:
                    if u0 < (ucur - eps):
                        return json.dumps({"ok": False, "error": "segments overlap in u-space"})
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
                    if "duration" not in s:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "when using duration timing, every segment must include duration",
                            }
                        )
                    d = float(s.get("duration", 0.0))
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
                    return json.dumps({"ok": False, "error": "total duration must be > 0"})
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
                return json.dumps({"ok": False, "error": "no valid segments after normalization"})

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
                    key = str(k or "").strip().lower()
                    val = _coerce_float(v, field=f"rotate.{k}")
                    if key == "yaw":
                        yaw += val
                    elif key == "pitch":
                        pitch += val
                    elif key == "roll":
                        roll += val
                    else:
                        raise ValueError("segment.rotate keys must be one of: yaw, pitch, roll")
                return (yaw, pitch, roll)

            def _apply_move(cam_value: dict, *, forward: float, right: float, up: float) -> dict:
                cur = cam_value
                if abs(forward) > 1e-12:
                    cur = client.camera_move_local(
                        op=("FORWARD" if forward >= 0.0 else "BACK"),
                        distance=abs(float(forward)),
                        distance_is_fraction_of_bbox_radius=True,
                        ids=ids,
                        after_clipping=after_clipping,
                        move_center=True,
                        base_value=cur,
                    )
                if abs(right) > 1e-12:
                    cur = client.camera_move_local(
                        op=("RIGHT" if right >= 0.0 else "LEFT"),
                        distance=abs(float(right)),
                        distance_is_fraction_of_bbox_radius=True,
                        ids=ids,
                        after_clipping=after_clipping,
                        move_center=True,
                        base_value=cur,
                    )
                if abs(up) > 1e-12:
                    cur = client.camera_move_local(
                        op=("UP" if up >= 0.0 else "DOWN"),
                        distance=abs(float(up)),
                        distance_is_fraction_of_bbox_radius=True,
                        ids=ids,
                        after_clipping=after_clipping,
                        move_center=True,
                        base_value=cur,
                    )
                return cur

            def _apply_rotate(cam_value: dict, *, yaw: float, pitch: float, roll: float) -> dict:
                cur = cam_value
                if abs(yaw) > 1e-12:
                    cur = client.camera_rotate(op="YAW", degrees=float(yaw), base_value=cur)
                if abs(pitch) > 1e-12:
                    cur = client.camera_rotate(op="PITCH", degrees=float(pitch), base_value=cur)
                if abs(roll) > 1e-12:
                    cur = client.camera_rotate(op="ROLL", degrees=float(roll), base_value=cur)
                return cur

            # Build camera key values by integrating segments sequentially.
            keys: list[dict[str, Any]] = [{"time": float(t0), "value": cam}]
            last_time = float(t0)
            for ta, tb, seg in time_ranges:
                if not (math.isfinite(ta) and math.isfinite(tb)):
                    return json.dumps({"ok": False, "error": "segment time range must be finite"})
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
                return json.dumps({"ok": False, "error": "walkthrough produced fewer than 2 keys"})

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
                                "error": rr.get("error") or rr.get("reason") or "apply_failed",
                            }
                        )
                except Exception as e:
                    failed.append({"time": float(k.get("time", 0.0) or 0.0), "error": str(e)})

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
        lr = client.list_keys(animation_id=animation_id, target_id=id, json_key=json_key, include_values=include_values)
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
            lr = client.list_keys(animation_id=animation_id, target_id=id, json_key=(jk or None), include_values=False)
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
            {"id": int(id), "json_key": ("" if int(id) == 0 else str(jk)), "time": float(t)}
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
            ok = client.batch(animation_id=animation_id, set_keys=[], remove_keys=remove_keys, commit=True)
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
            proc = sorted(in_range, key=lambda kk: float(kk["time"]), reverse=delta >= 0)
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
                        conflicts.append({"new_time": float(nt), "existing_time": float(okk["time"])})
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
            ok = client.batch(animation_id=animation_id, set_keys=set_keys, remove_keys=remove_keys, commit=True)
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
                    "json_key": ("" if int(id) == 0 else str(args.get("json_key") or "")),
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
            above_proc = sorted(above, key=lambda kk: float(kk["time"]), reverse=scale > 1.0)
            below_proc = sorted(below, key=lambda kk: float(kk["time"]), reverse=scale < 1.0)
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
                        conflicts.append({"new_time": float(nt), "existing_time": float(okk["time"])})
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
            ok = client.batch(animation_id=animation_id, set_keys=set_keys, remove_keys=remove_keys, commit=True)
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
                        {"new_time": float(nt), "existing_times": [t for t in existing_times if abs(t - nt) <= tol]}
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
            if on_conflict == "skip" and any(abs(nt - t) <= tol for t in existing_times):
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
            ok = client.batch(animation_id=animation_id, set_keys=set_keys, remove_keys=remove_keys, commit=True)
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
        return json.dumps({"ok": client.set_duration(animation_id=animation_id, seconds=seconds)})

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
        easing = str(args.get("easing", "Linear"))
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
        easing = str(args.get("easing", "Linear"))
        tol = float(args.get("tolerance", 1e-3))
        if not times:
            return json.dumps({"ok": False, "error": "times required"})
        # Verify parameter exists for id
        if not _json_key_exists(id, json_key):
            return json.dumps({"ok": False, "error": "json_key not found for id"})
        try:
            for t in times:
                _ = json.loads(
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
                _ = json.loads(
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
            return json.dumps({"ok": True, "count": len(times)})
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
            ok = client.clear_keys(animation_id=animation_id, target_id=id, json_key=str(args.get("json_key") or ""))
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
        ok = client.remove_key(animation_id=animation_id, target_id=id, json_key=json_key, time=time_v)
        return json.dumps({"ok": ok})

    if name == "animation_batch":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        set_keys = args.get("set_keys") or []
        remove_keys = args.get("remove_keys") or []
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
        return json.dumps({"ok": ok})

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

    if name == "animation_save_animation":
        animation_id = int(args.get("animation_id", 0) or 0)
        if animation_id <= 0:
            return json.dumps({"ok": False, "error": "animation_id is required"})
        p = Path(str(args.get("path") or ""))
        ok = bool(client.save_animation(animation_id=animation_id, path=p))
        if ok:
            try:
                if ctx.session_store is not None:
                    ctx.session_store.set_meta(last_animation_save_path=str(p.expanduser().resolve()))
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
                {"ok": False, "error": "Atlas binary not found; ensure Atlas is installed"}
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
            return json.dumps({"ok": False, "error": "failed to save animation for export"})
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
                allow = (ctx.session_store.get_consent("screenshots") is True)
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
        width = int(args.get("width", 512))
        height = int(args.get("height", 512))
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
            if isinstance(d, (int, float)) and math.isfinite(float(d)) and float(d) >= 0:
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
        out: dict[str, Any] = {"ok": True, "path": images[0]}
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
