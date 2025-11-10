import glob
import json
import os
import tempfile
from pathlib import Path
from typing import Any, Dict, List

from ...describe import load_animation, load_capabilities, summarize_animation
from ...discovery import (
    compute_paths_from_atlas_dir,
    default_install_dirs,
    discover_schema_dir,
)

# Fail-fast for internal exporter helpers
from ...exporter import export_video, preview_frames
from .context import ToolDispatchContext

HANDLED_TOOLS = (
    "animation_set_param_by_name",
    "animation_remove_key_param_at_time",
    "animation_replace_key_param",
    "animation_replace_key_camera",
    "animation_camera_solve_and_apply",
    "animation_camera_validate",
    "animation_get_time",
    "animation_list_keys",
    "animation_describe_file",
    "animation_ensure_animation",
    "animation_set_duration",
    "animation_set_key_param",
    "animation_replace_key_param_at_times",
    "animation_clear_keys",
    "animation_remove_key",
    "animation_batch",
    "animation_set_time",
    "animation_save_animation",
    "animation_export_video",
    "animation_render_preview",
)

TOOL_SPECS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "animation_describe_file",
            "description": "Parse an .animation3d file and return a concise natural-language summary (uses capabilities.json for names).",
            "parameters": {
                "type": "object",
                "properties": {"path": {"type": "string"}},
                "required": ["path"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_camera_solve_and_apply",
            "description": "Animation timeline: solve camera (FIT|ORBIT|DOLLY|STATIC) and write validated keys. Clears existing camera keys in [t0,t1] by default (tolerance-aware). Not for scene-only FIT. Returns {applied:[times...], total}.",
            "parameters": {
                "type": "object",
                "properties": {
                    "mode": {
                        "type": "string",
                        "enum": ["FIT", "ORBIT", "DOLLY", "STATIC"],
                        "description": "Solve mode for generating camera keys.",
                    },
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Target ids; empty uses fit_candidates().",
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
                        "type": "object",
                        "description": "Visibility/coverage constraints (keep_visible, margin, min_coverage, fov policy).",
                    },
                    "params": {
                        "type": "object",
                        "description": "Mode-specific parameters (e.g., axis for ORBIT).",
                    },
                    "degrees": {
                        "type": "number",
                        "description": "ORBIT: total rotation in degrees (default 360).",
                    },
                    "tolerance": {
                        "type": "number",
                        "default": 1e-3,
                        "description": "Time tolerance used when clearing/replacing keys.",
                    },
                    "easing": {
                        "type": "string",
                        "default": "Linear",
                        "description": "Easing to assign to written keys.",
                    },
                    "clear_range": {
                        "type": "boolean",
                        "default": True,
                        "description": "Remove existing camera keys inside [t0,t1] (within tolerance) before applying new keys.",
                    },
                },
                "required": ["mode", "ids", "t0", "t1"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_camera_validate",
            "description": "Animation timeline: validate camera values against visibility/coverage constraints. Values are optional; when omitted, the server samples the current animation camera at the given times.",
            "parameters": {
                "type": "object",
                "properties": {
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Target ids to validate against (typically fit_candidates).",
                    },
                    "times": {
                        "type": "array",
                        "items": {"type": "number"},
                        "description": "Times (seconds) to validate.",
                    },
                    "values": {
                        "type": "array",
                        "items": {"type": "object"},
                        "description": "Optional: typed camera values aligned with times. If omitted or shorter than times, the server fills by sampling.",
                    },
                    "constraints": {
                        "type": "object",
                        "description": "Visibility/coverage constraints (keep_visible, margin, min_coverage, etc.).",
                    },
                    "policies": {
                        "type": "object",
                        "description": "Adjustment policies (adjust_fov, adjust_distance, adjust_clipping).",
                    },
                },
                "required": ["ids", "times"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_set_param_by_name",
            "description": "Set a parameter by display name (case-insensitive) by id. Resolves json_key via scene_list_params, then calls animation_set_key_param. Id map: 1=background, 2=axis, 3=global, ≥4=objects.",
            "parameters": {
                "type": "object",
                "properties": {
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
                        "type": [
                            "object",
                            "array",
                            "number",
                            "string",
                            "boolean",
                            "null",
                        ],
                        "items": {"type": ["string", "number", "boolean", "null"]},
                    },
                },
                "required": ["id", "name", "time", "value"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_remove_key_param_at_time",
            "description": "Remove one or more keys near a time for a parameter by json_key and id. Uses a tolerance window to match existing keys.",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "json_key": {"type": "string"},
                    "time": {"type": "number"},
                    "tolerance": {"type": "number", "default": 1e-3},
                },
                "required": ["id", "json_key", "time"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_replace_key_param",
            "description": "Replace (or set) a parameter key by json_key at time by id: remove any key within tolerance then set a new typed value.",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "json_key": {"type": "string"},
                    "time": {"type": "number"},
                    "easing": {"type": "string", "default": "Linear"},
                    "value": {
                        "description": "Native JSON value (bool/number/string/array)",
                        "type": ["string", "number", "boolean", "null", "array"],
                        "items": {"type": ["string", "number", "boolean", "null"]},
                    },
                    "tolerance": {"type": "number", "default": 1e-3},
                    "strict": {"type": "boolean", "default": False},
                },
                "required": ["id", "json_key", "time", "value"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_replace_key_camera",
            "description": "Replace (or set) a camera key at time: remove any camera key within tolerance then set a new camera value. Use for explicit single-time edits. If you already used animation_camera_solve_and_apply for this segment, do NOT call this afterward to 'finalize' — keys are already written.",
            "parameters": {
                "type": "object",
                "properties": {
                    "time": {"type": "number"},
                    "easing": {"type": "string", "default": "Linear"},
                    "value": {"type": "object"},
                    "tolerance": {"type": "number", "default": 1e-3},
                    "strict": {"type": "boolean", "default": False},
                },
                "required": ["time", "value"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_list_keys",
            "description": "Timeline only: list animation keys by id. Requires an existing Animation3D. Id map: 0=camera, 1=background, 2=axis, 3=global, ≥4=objects. For camera (id=0) json_key is ignored. Do NOT use for scene-only verification — read current camera via scene_get_values(id=0, 'Camera 3DCamera').",
            "parameters": {
                "type": "object",
                "properties": {
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
                "required": ["id", "json_key", "include_values"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_get_time",
            "description": "Animation (timeline): get current playback seconds and duration.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_ensure_animation",
            "description": "Ensure a 3D animation exists in the GUI.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_set_duration",
            "description": "Set animation duration in seconds.",
            "parameters": {
                "type": "object",
                "properties": {"seconds": {"type": "number"}},
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_set_key_param",
            "description": "Add a parameter key by id (0=camera unsupported here, ≥4 objects; 1/2/3 groups) with json_key and a native JSON value (bool/number/string/array/object).",
            "parameters": {
                "type": "object",
                "properties": {
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
                    "value": {
                        "description": "Native JSON value (bool/number/string/array)",
                        "type": ["string", "number", "boolean", "null", "array"],
                        "items": {"type": ["string", "number", "boolean", "null"]},
                    },
                },
                "required": ["id", "time", "value"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_replace_key_param_at_times",
            "description": "Replace keys at the specified times for a parameter by id (1=background,2=axis,3=global,≥4=objects).",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "json_key": {"type": "string"},
                    "times": {"type": "array", "items": {"type": "number"}},
                    "value": {
                        "description": "Native JSON value",
                        "type": ["string", "number", "boolean", "null", "array"],
                        "items": {"type": ["string", "number", "boolean", "null"]},
                    },
                    "easing": {"type": "string", "default": "Linear"},
                    "tolerance": {"type": "number", "default": 1e-3},
                },
                "required": ["id", "json_key", "times", "value"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_clear_keys",
            "description": "Clear all keys for a parameter or camera by id (0=camera).",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "json_key": {"type": "string"},
                },
                "required": ["id"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_remove_key",
            "description": "Remove a key at a specific time for a parameter by id.",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "json_key": {"type": "string"},
                    "time": {"type": "number"},
                },
                "required": ["id", "json_key", "time"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_batch",
            "description": "Batch multiple SetKey and RemoveKey operations atomically. Non-camera only (ids ≥ 1); do not include camera (id=0) keys here. For camera motion, use animation_camera_solve_and_apply.",
            "parameters": {
                "type": "object",
                "properties": {
                    "set_keys": {
                        "type": "array",
                        "items": {"type": "object"},
                        "description": "List of SetKey operations",
                    },
                    "remove_keys": {
                        "type": "array",
                        "items": {"type": "object"},
                        "description": "List of RemoveKey operations",
                    },
                    "commit": {
                        "type": "boolean",
                        "default": True,
                        "description": "Commit immediately if true",
                    },
                },
                "required": ["set_keys", "remove_keys", "commit"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_set_time",
            "description": "Set current timeline time (seconds).",
            "parameters": {
                "type": "object",
                "properties": {
                    "seconds": {"type": "number", "description": "Timeline seconds"}
                },
                "required": ["seconds"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_save_animation",
            "description": "Save the current animation to a .animation3d path.",
            "parameters": {
                "type": "object",
                "properties": {"path": {"type": "string"}},
                "required": ["path"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_export_video",
            "description": "Export an .animation3d to MP4 using the Atlas headless exporter.",
            "parameters": {
                "type": "object",
                "properties": {
                    "animation": {
                        "type": "string",
                        "description": "Path to .animation3d file",
                    },
                    "out": {"type": "string", "description": "Output .mp4 path"},
                    "fps": {"type": "number", "description": "Frames per second"},
                    "start": {
                        "type": "integer",
                        "description": "Start frame (inclusive)",
                    },
                    "end": {
                        "type": "integer",
                        "description": "End frame (inclusive, -1 = duration)",
                    },
                    "width": {"type": "integer", "description": "Output width"},
                    "height": {"type": "integer", "description": "Output height"},
                    "overwrite": {
                        "type": "boolean",
                        "description": "Overwrite output if exists",
                    },
                    "use_gpu_devices": {
                        "type": "string",
                        "description": "Linux only: comma-separated GPU ids, e.g. '0,1'",
                    },
                },
                "required": [
                    "animation",
                    "out",
                    "fps",
                    "start",
                    "end",
                    "width",
                    "height",
                    "overwrite",
                    "use_gpu_devices",
                ],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "animation_render_preview",
            "description": "Render a single preview frame by saving the current animation and invoking headless Atlas. Returns a path to the image in the OS temp directory.",
            "parameters": {
                "type": "object",
                "properties": {
                    "time": {
                        "type": "number",
                        "description": "Preview time in seconds",
                    },
                    "fps": {"type": "number", "description": "Frames per second"},
                    "width": {"type": "integer", "description": "Image width"},
                    "height": {"type": "integer", "description": "Image height"},
                },
                "required": ["time", "fps", "width", "height"],
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

    if name == "animation_set_param_by_name":
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
                id=id,
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
            lr = client.list_keys(id=id, json_key=json_key, include_values=False)
            times = [k.time for k in getattr(lr, "keys", [])]
            to_remove = [t for t in times if abs(t - time_v) <= tol]
            removed = 0
            for t in to_remove:
                ok = client.remove_key(id=id, json_key=json_key, time=t)
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
        strict = bool(args.get("strict", False))
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
                            "id": id,
                            "json_key": json_key,
                            "time": time_v,
                            "tolerance": tol,
                        }
                    ),
                )
            )
            ok = client.set_key_param(
                id=id, json_key=json_key, time=time_v, easing=easing, value=value
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
        time_v = float(args.get("time", 0.0))
        easing = str(args.get("easing", "Linear"))
        value = args.get("value") or {}
        tol = float(args.get("tolerance", 1e-3))
        strict = bool(args.get("strict", False))
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
                lr = client.list_keys(id=0, include_values=False)
                times = [k.time for k in getattr(lr, "keys", [])]
                to_remove = [t for t in times if abs(t - time_v) <= tol]
                removed = 0
                for t in to_remove:
                    if client.remove_key(id=0, json_key="", time=t):
                        removed += 1
            except Exception:
                removed = 0
            # Validate and accept adjusted value if provided
            try:
                vr = client.camera_validate(
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
            ok = client.set_key_camera(time=time_v, easing=easing, value=value)
            # Re-validate strictly
            final_ok = ok
            try:
                vr2 = client.camera_validate(
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
                    lr = client.list_keys(id=0, include_values=False)
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
                        client.remove_key(id=0, json_key="", time=old_t)
                    except Exception:
                        pass
            applied: list[float] = []
            for k in keys or []:
                try:
                    tv = float(k.get("time", 0.0))
                    vv = k.get("value") or {}
                    # Use replace to remove near times and validate; pass ids for validation inside the function
                    payload = {
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
            ids = args.get("ids") or []
            times = args.get("times") or []
            # Values are optional; the server can sample from animation when omitted.
            values = args.get("values") or []
            if not times:
                return json.dumps({"ok": False, "error": "times must be non-empty"})
            constraints = args.get("constraints") or {}
            policies = args.get("policies") or {}
            res = client.camera_validate(
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

    if name == "animation_get_time":
        ts = client.get_time()
        return json.dumps(
            {
                "seconds": getattr(ts, "seconds", 0.0),
                "duration": getattr(ts, "duration", 0.0),
            }
        )

    if name == "animation_list_keys":
        id = int(args.get("id"))
        json_key = args.get("json_key") or None
        if isinstance(json_key, str) and json_key.strip() == "":
            json_key = None
        include_values = bool(args.get("include_values", False))
        lr = client.list_keys(id=id, json_key=json_key, include_values=include_values)
        keys = [
            {"time": k.time, "type": k.type, "value": getattr(k, "value_json", "")}
            for k in lr.keys
        ]
        return json.dumps({"ok": True, "keys": keys})

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
        return json.dumps({"ok": client.ensure_animation()})

    if name == "animation_set_duration":
        seconds = float(args.get("seconds", 0.0))
        return json.dumps({"ok": client.set_duration(seconds)})

    if name == "animation_set_key_param":
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
                id=id,
                json_key=json_key,
                time=time_v,
                easing=easing,
                value=value_native,
            )
            return json.dumps({"ok": ok})
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg})

    if name == "animation_replace_key_param_at_times":
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
        id = int(args.get("id"))
        ok = client.clear_keys(id=id, json_key=args.get("json_key"))
        return json.dumps({"ok": ok})

    if name == "animation_remove_key":
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
        ok = client.remove_key(id=id, json_key=json_key, time=time_v)
        return json.dumps({"ok": ok})

    if name == "animation_batch":
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
            set_keys=set_keys,
            remove_keys=remove_keys,
            commit=bool(args.get("commit", True)),
        )
        return json.dumps({"ok": ok})

    if name == "animation_set_time":
        ok = client.set_time(
            float(args.get("seconds", 0.0)),
            cancel_rendering=bool(args.get("cancel", False)),
        )
        return json.dumps({"ok": ok})

    if name == "animation_save_animation":
        return json.dumps({"ok": client.save_animation(Path(args.get("path")))})

    if name == "animation_export_video":
        # Export .animation3d to MP4 by invoking headless Atlas
        

        anim = args.get("animation")
        out = args.get("out")
        if not anim or not out:
            return json.dumps({"ok": False, "error": "animation and out are required"})
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
                {"ok": False, "error": "Atlas binary not found; set atlas_dir"}
            )
        rc = export_video(
            atlas_bin=str(atlas_bin),
            animation_path=Path(anim),
            output_video=Path(out),
            fps=int(args.get("fps", 30)),
            start=int(args.get("start", 0)),
            end=int(args.get("end", -1)),
            width=int(args.get("width", 1920)),
            height=int(args.get("height", 1080)),
            overwrite=bool(args.get("overwrite", True)),
            use_gpu_devices=args.get("use_gpu_devices"),
        )
        return json.dumps({"ok": rc == 0, "exit_code": rc})

    if name == "animation_render_preview":
        # Privacy/consent gate: disabled unless explicitly allowed by env
        allow = os.environ.get("ATLAS_AGENT_ALLOW_SCREENSHOTS", "").strip().lower() in (
            "1",
            "true",
            "yes",
        )
        if not allow:
            return json.dumps(
                {
                    "ok": False,
                    "error": "screenshots disabled; set ATLAS_AGENT_ALLOW_SCREENSHOTS=1 to enable",
                }
            )
        

        fps = int(float(args.get("fps", 30)))
        tsec = float(args.get("time", 0.0))
        width = int(args.get("width", 512))
        height = int(args.get("height", 512))
        frame_idx = int(round(tsec * max(1, fps)))
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
                    "error": "Atlas binary not found; set atlas_dir or install Atlas",
                }
            )
        # Save animation to a temp file and render a single frame
        tdir = Path(tempfile.mkdtemp(prefix="atlas_preview_"))
        anim_path = tdir / "preview.animation3d"
        ok_save = client.save_animation(anim_path)
        if not ok_save:
            return json.dumps(
                {"ok": False, "error": "failed to save temporary animation"}
            )
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
        )
        if rc != 0:
            return json.dumps(
                {"ok": False, "exit_code": rc, "error": "preview renderer failed"}
            )
        # Find the produced image (exact naming depends on exporter; pick any image in frames_dir)
        images = []
        for ext in ("*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tif", "*.tiff"):
            images.extend(sorted(glob.glob(str(frames_dir / ext))))
        if not images:
            return json.dumps({"ok": False, "error": "no image produced"})
        return json.dumps({"ok": True, "path": images[0]})

    return None
