import hashlib
import json
import logging
import os
import platform
from pathlib import Path
from typing import Any, Dict, List

# Fail-fast required third-party imports
from google.protobuf.json_format import MessageToDict

try:
    from jsonschema import Draft202012Validator as JsonSchemaValidator  # type: ignore
except Exception:
    from jsonschema import Draft7Validator as JsonSchemaValidator  # type: ignore

from ...capabilities_prompt import build_capabilities_prompt
from ...discovery import discover_schema_dir
from .context import ToolDispatchContext
from .file_formats import SCENE_LOAD_CATEGORIES, get_supported_extensions

HANDLED_TOOLS = (
    "scene_capabilities_summary",
    "scene_animation_concepts",
    "scene_params_handbook",
    "scene_facts_summary",
    "scene_capabilities",
    "scene_schema",
    "scene_get_values",
    "scene_validate_params",
    "scene_apply",
    "scene_save_scene",
    "scene_camera_fit",
    "scene_camera_apply",
    "scene_load_files",
    "scene_ensure_loaded",
    "scene_smart_load",
    "scene_list_objects",
    "scene_find_objects",
    "scene_bbox",
    "scene_list_params",
    "scene_param_info",
    "scene_validate_param_value",
    "scene_set_visibility",
    "scene_make_alias",
    "scene_cut_suggest_box",
    "scene_cut_set_box",
    "scene_cut_clear",
)

SCENE_SET_PARAMS_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "set_params": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "json_key": {"type": "string"},
                    "value": {
                        "description": 'Typed value. For 3DTransform, use an object with canonical subfields (e.g., {"Translation Vec3":[x,y,z],"Rotation Vec4":[ang,x,y,z],"Scale Vec3":[sx,sy,sz],"Rotation Center Vec3":[cx,cy,cz]}).',
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
                "required": ["id", "json_key", "value"],
            },
        }
    },
    "required": ["set_params"],
}

TOOL_SPECS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "scene_capabilities_summary",
            "description": "Condensed capabilities overview. Background: Scene (.scene) is stateless current display state; Animation (.animation2d/.animation3d) adds timeline keys that override scene during playback.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_camera_fit",
            "description": "Scene-only: fit the scene camera to given ids (or all fit_candidates) without writing animation keys. Applies the typed camera via scene_apply(id=0).",
            "parameters": {
                "type": "object",
                "properties": {
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Optional target ids; when omitted uses fit_candidates().",
                    },
                    "after_clipping": {
                        "type": "boolean",
                        "default": True,
                        "description": "Use clipped bbox (true) or full bbox (false).",
                    },
                    "min_radius": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Minimum radius (world units) for the fit sphere; 0 disables.",
                    },
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_camera_apply",
            "description": "Scene-only: apply a typed camera value to the scene camera (id=0) without writing timeline keys.",
            "parameters": {
                "type": "object",
                "properties": {
                    "value": {
                        "type": "object",
                        "description": "Typed camera value (object with camera fields).",
                    }
                },
                "required": ["value"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_animation_concepts",
            "description": "Explain Atlas Scene vs Animation: Scene (.scene) captures current objects + display params (2D/3D). Animation (.animation2d/.animation3d) extends Scene with per-parameter timeline keys (easing: Switch/Linear/Exp/…). During playback, animation keys override scene values.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_params_handbook",
            "description": "Generate a Markdown handbook of parameters per object type and groups from capabilities.json (json_key, type, supports_interpolation, ranges).",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_facts_summary",
            "description": "Return a concise natural-language summary of current objects, keyframes, and time; optionally include selected parameter values.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_capabilities",
            "description": "Return the full capabilities.json (parameter catalogs per object type and groups) from the discovered schema directory.",
            "parameters": {
                "type": "object",
                "properties": {
                    "schema_dir": {
                        "type": ["string", "null"],
                        "description": "Optional schema directory override (defaults to discovery via atlas_dir/env).",
                    }
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_schema",
            "description": "Return the full Animation3D JSON Schema (animation3d.schema.json).",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_get_values",
            "description": "Scene (stateless): return current display values for json_keys by id. Id map: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids. For the scene camera, use json_key 'Camera 3DCamera' (or pass an empty json_keys array to retrieve it).",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "json_keys": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Param keys to read (empty = all)",
                    },
                },
                "required": ["id", "json_keys"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_validate_params",
            "description": "Scene (validate-only, no write): preflight display parameter assignments. Returns {ok:bool, results:[{json_key, ok, reason?, normalized_value?}]}. Use before scene_apply; during playback, timeline keys still override scene values.",
            "parameters": SCENE_SET_PARAMS_SCHEMA,
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_apply",
            "description": "Scene (stateless): apply display parameter assignments atomically (no time/easing). Accepts either canonical json_key or display name; resolves names via scene_list_params with caching. Targeting is by id (0=camera,1=background,2=axis,3=global,≥4=objects). Note: does not change animation; during playback, animation keys override scene values.",
            "parameters": {
                "type": "object",
                "properties": {
                    "set_params": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "id": {
                                    "type": "integer",
                                    "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                                },
                                "json_key": {
                                    "type": "string",
                                    "description": "Canonical parameter key (preferred)",
                                },
                                "name": {
                                    "type": "string",
                                    "description": "Display name; dispatcher resolves to json_key if provided",
                                },
                                "value": {
                                    "description": 'Typed value. For 3DTransform, use an object with canonical subfields (e.g., {"Translation Vec3":[x,y,z],"Rotation Vec4":[ang,x,y,z],"Scale Vec3":[sx,sy,sz],"Rotation Center Vec3":[cx,cy,cz]}).',
                                    "type": [
                                        "object",
                                        "array",
                                        "number",
                                        "string",
                                        "boolean",
                                        "null",
                                    ],
                                    "items": {
                                        "type": [
                                            "string",
                                            "number",
                                            "boolean",
                                            "null",
                                        ]
                                    },
                                },
                            },
                            "required": ["id", "value"],
                        },
                    }
                },
                "required": ["set_params"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_save_scene",
            "description": "Save current scene (.scene) to path.",
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
            "name": "scene_load_files",
            "description": "Load one or more files into the GUI scene. Accepts absolute paths; ~ and env vars expanded. Prefer scene_ensure_loaded for idempotent behavior.",
            "parameters": {
                "type": "object",
                "properties": {
                    "files": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Absolute paths to load",
                    },
                },
                "required": ["files"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_ensure_loaded",
            "description": "Idempotently ensure files are loaded: skips already loaded paths, validates existence, and loads only missing ones.",
            "parameters": {
                "type": "object",
                "properties": {
                    "files": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Absolute paths to load (idempotent)",
                    },
                },
                "required": ["files"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_smart_load",
            "description": "Resolve and load one or more files by name, searching typical user directories (Documents/Downloads/Desktop/CWD). Returns loaded object count and the resolved paths.",
            "parameters": {
                "type": "object",
                "properties": {
                    "names": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Basenames to search",
                    },
                    "dir_hints": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Hint directories",
                    },
                    "schema_dir": {
                        "type": "string",
                        "description": "Optional schema directory override for extension catalogs",
                    },
                    "extensions": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Allowed extensions",
                    },
                    "case_insensitive": {
                        "type": "boolean",
                        "default": True,
                        "description": "Case-insensitive matching",
                    },
                },
                "required": ["names"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_list_objects",
            "description": "List all objects in the current scene (id, type, name, path, visible).",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_find_objects",
            "description": "Find objects by substring filters over (type, name, path) with optional paging. If limit is omitted, returns all matches (no silent truncation).",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {
                        "type": "string",
                        "description": "Case-insensitive substring match over type/name/path/id.",
                    },
                    "type_contains": {
                        "type": "string",
                        "description": "Case-insensitive substring match over object type.",
                    },
                    "name_contains": {
                        "type": "string",
                        "description": "Case-insensitive substring match over object name.",
                    },
                    "path_contains": {
                        "type": "string",
                        "description": "Case-insensitive substring match over object path.",
                    },
                    "visible": {
                        "type": "boolean",
                        "description": "Optional visibility filter; when omitted, includes both visible and hidden objects.",
                    },
                    "limit": {
                        "type": "integer",
                        "minimum": 1,
                        "description": "Optional page size. When omitted, returns all results.",
                    },
                    "offset": {
                        "type": "integer",
                        "minimum": 0,
                        "default": 0,
                        "description": "Paging offset (0-based).",
                    },
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_bbox",
            "description": "Get bounding box for a set of ids. Pass an empty list for all objects.",
            "parameters": {
                "type": "object",
                "properties": {
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Object ids (empty = all)",
                    },
                    "after_clipping": {
                        "type": "boolean",
                        "default": False,
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
            "name": "scene_list_params",
            "description": "List parameters by id (includes value_schema). Id map: 0=camera, 1=background, 2=axis, 3=global, ≥4=objects.",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                    }
                },
                "required": ["id"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_param_info",
            "description": "Get one parameter's metadata by id (0=camera, 1=background, 2=axis, 3=global, ≥4=objects). Resolves json_key from either json_key or display name.",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "query": {
                        "type": "string",
                        "description": "Either a canonical json_key or a display name (case-insensitive).",
                    },
                    "json_key": {
                        "type": "string",
                        "description": "Canonical json_key (preferred when known).",
                    },
                    "name": {
                        "type": "string",
                        "description": "Display name to resolve to json_key (case-insensitive).",
                    },
                },
                "required": ["id"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_validate_param_value",
            "description": "Validate a candidate value against the live value_schema for a given id (0=camera, 1=background, 2=axis, 3=global, ≥4=objects).",
            "parameters": {
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids",
                    },
                    "json_key": {"type": "string"},
                    "value": {
                        "description": "Candidate JSON value (native types)",
                        "type": ["object", "array", "number", "string", "boolean", "null"],
                        "items": {"type": ["object", "array", "number", "string", "boolean", "null"]},
                    },
                },
                "required": ["id", "json_key", "value"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_set_visibility",
            "description": "Toggle visibility of a list of object ids.",
            "parameters": {
                "type": "object",
                "properties": {
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Object ids",
                    },
                    "on": {
                        "type": "boolean",
                        "description": "True to show, false to hide",
                    },
                },
                "required": ["ids", "on"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_make_alias",
            "description": "Create alias objects for given ids (shared backing data with independent view params).",
            "parameters": {
                "type": "object",
                "properties": {
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Object ids to alias; each produces a new alias id.",
                    }
                },
                "required": ["ids"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_cut_suggest_box",
            "description": "Suggest an axis-aligned cut box for given ids (or all).",
            "parameters": {
                "type": "object",
                "properties": {
                    "ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Ids to bound (empty = all)",
                    },
                    "margin": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Extra normalized margin to expand box",
                    },
                    "after_clipping": {
                        "type": "boolean",
                        "default": False,
                        "description": "Use clipped bbox for computation",
                    },
                },
                "required": ["ids", "margin", "after_clipping"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_cut_set_box",
            "description": "Apply a global cut box and optionally refit camera.",
            "parameters": {
                "type": "object",
                "properties": {
                    "min": {
                        "type": "array",
                        "items": {"type": "number"},
                        "minItems": 3,
                        "maxItems": 3,
                        "description": "Box min [x,y,z]",
                    },
                    "max": {
                        "type": "array",
                        "items": {"type": "number"},
                        "minItems": 3,
                        "maxItems": 3,
                        "description": "Box max [x,y,z]",
                    },
                    "refit_camera": {
                        "type": "boolean",
                        "default": True,
                        "description": "Refit camera after applying cut",
                    },
                },
                "required": ["min", "max", "refit_camera"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "scene_cut_clear",
            "description": "Clear global cuts.",
            "parameters": {"type": "object", "properties": {}},
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

    if name == "scene_animation_concepts":
        info = (
            "Scene (.scene): current objects and their display parameters across 2D/3D. Saving a scene lets you restore the view/state.\n"
            "Animation (.animation2d/.animation3d): extends the scene with a timeline. Each display parameter (and camera) has keys at specific times, with easing (e.g., Switch/Linear/Exp).\n"
            "Playback rule: When the timeline plays, animation keys override scene values for the affected parameters. To change what plays, write/replace keys.\n"
            "Scene tools (scene_*) are stateless (no time/easing); Animation tools (animation_*) manipulate keys on the timeline."
        )
        return json.dumps({"ok": True, "text": info})

    if name == "scene_load_files":
        files = args.get("files") or []
        try:
            resp = client.load_files(files)
            objs = [
                {"id": o.id, "type": o.type, "name": o.name, "visible": o.visible}
                for o in resp.objects
            ]
            return json.dumps({"ok": True, "objects": objs, "paths": files})
        except FileNotFoundError as e:
            # Hint the agent to use scene_smart_load next
            return json.dumps(
                {
                    "ok": False,
                    "error": str(e),
                    "hint": "Use scene_smart_load to resolve names in common dirs (Documents/Downloads/Desktop/cwd).",
                }
            )

    if name == "scene_ensure_loaded":
        files = args.get("files") or []
        summary = client.ensure_loaded(files)
        return json.dumps({"ok": True, **summary})

    if name == "scene_smart_load":

        names = args.get("names") or []
        dir_hints = args.get("dir_hints") or []
        schema_dir_override = args.get("schema_dir")
        exts_arg = args.get("extensions") or []
        exts = [str(e) for e in exts_arg if isinstance(e, str) and e]
        if not exts:
            exts = get_supported_extensions(
                schema_dir_override, atlas_dir, categories=SCENE_LOAD_CATEGORIES
            )
        ci = bool(args.get("case_insensitive", True))
        if not dir_hints:
            home = os.path.expanduser("~")
            # OS-specific common locations
            dirs: list[str] = []
            try:
                system = platform.system()
            except Exception:
                system = ""
            if system == "Windows":
                user = os.environ.get("USERPROFILE") or home
                for base in ["Documents", "Downloads", "Desktop"]:
                    dirs.append(os.path.join(user, base))
            elif system == "Darwin":
                for base in ["Documents", "Downloads", "Desktop"]:
                    dirs.append(os.path.join(home, base))
            else:
                # Linux/Unix
                for base in ["Documents", "Downloads", "Desktop"]:
                    dirs.append(os.path.join(home, base))
                # Common data mount points
                dirs += ["/data", "/mnt/data", "/srv/data"]
            # Always include cwd last
            dirs.append(os.getcwd())
            dir_hints = dirs

        def variants(nm: str) -> list[str]:
            base, ext = os.path.splitext(nm)
            cand = [nm]
            if not ext:
                cand.extend([base + e for e in exts])
            return cand

        resolved: list[str] = []
        tried: list[str] = []
        for d in dir_hints:
            d2 = os.path.expanduser(os.path.expandvars(str(d)))
            if not os.path.isdir(d2):
                continue
            for nm in names:
                for cand in variants(nm):
                    p = os.path.join(d2, cand)
                    tried.append(p)
                    if os.path.exists(p):
                        resolved.append(os.path.abspath(p))
                        continue
                    if ci:
                        try:
                            target = cand.lower()
                            for fname in os.listdir(d2):
                                if fname.lower() == target and os.path.exists(
                                    os.path.join(d2, fname)
                                ):
                                    resolved.append(
                                        os.path.abspath(os.path.join(d2, fname))
                                    )
                                    break
                        except Exception:
                            pass
        loaded = 0
        try:
            if resolved:
                resp = client.load_files(resolved)
                loaded = len(resp.objects)
            return json.dumps(
                {
                    "ok": True,
                    "loaded_objects": loaded,
                    "resolved": resolved,
                    "tried": tried,
                }
            )
        except Exception as e:
            return json.dumps(
                {
                    "ok": False,
                    "error": str(e),
                    "resolved": resolved,
                    "tried": tried,
                }
            )

    if name == "scene_list_objects":
        try:
            resp = client.list_objects()
            objs = [
                {"id": o.id, "type": o.type, "name": o.name, "path": o.path, "visible": o.visible}
                for o in resp.objects
            ]
            return json.dumps({"ok": True, "objects": objs})
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_find_objects":
        try:
            query = args.get("query")
            type_contains = args.get("type_contains")
            name_contains = args.get("name_contains")
            path_contains = args.get("path_contains")
            visible_filter = args.get("visible")
            limit = args.get("limit")
            offset = args.get("offset", 0)

            if limit is not None:
                try:
                    limit = int(limit)
                except Exception:
                    return json.dumps({"ok": False, "error": "limit must be an integer"})
                if limit < 1:
                    return json.dumps(
                        {"ok": False, "error": "limit must be >= 1 when provided"}
                    )
            try:
                offset = int(offset)
            except Exception:
                return json.dumps({"ok": False, "error": "offset must be an integer"})
            if offset < 0:
                return json.dumps({"ok": False, "error": "offset must be >= 0"})

            def norm(s: Any) -> str:
                return (str(s) if s is not None else "").lower()

            q = norm(query).strip()
            tc = norm(type_contains).strip()
            nc = norm(name_contains).strip()
            pc = norm(path_contains).strip()

            resp = client.list_objects()
            matches: list[dict[str, Any]] = []
            for o in getattr(resp, "objects", []) or []:
                entry = {
                    "id": int(getattr(o, "id", 0)),
                    "type": str(getattr(o, "type", "")),
                    "name": str(getattr(o, "name", "")),
                    "path": str(getattr(o, "path", "")),
                    "visible": bool(getattr(o, "visible", False)),
                }
                if isinstance(visible_filter, bool) and entry["visible"] != visible_filter:
                    continue
                if tc and tc not in entry["type"].lower():
                    continue
                if nc and nc not in entry["name"].lower():
                    continue
                if pc and pc not in entry["path"].lower():
                    continue
                if q:
                    hay = (
                        f'{entry["id"]} {entry["type"]} {entry["name"]} {entry["path"]}'
                    ).lower()
                    if q not in hay:
                        continue
                matches.append(entry)

            total = len(matches)
            if limit is None:
                return json.dumps(
                    {
                        "ok": True,
                        "total": total,
                        "offset": int(offset),
                        "limit": None,
                        "next_offset": None,
                        "results": matches,
                    }
                )
            results = matches[offset : offset + limit]
            next_offset = offset + limit if (offset + limit) < total else None
            return json.dumps(
                {
                    "ok": True,
                    "total": total,
                    "offset": int(offset),
                    "limit": int(limit),
                    "next_offset": next_offset,
                    "results": results,
                }
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_bbox":
        try:
            ids = args.get("ids") or []
            after = bool(args.get("after_clipping", False))
            req = client._pb2.BBoxRequest(ids=ids, after_clipping=after)
            resp = client._stub.BBox(req)
            b = resp.bbox
            return json.dumps(
                {
                    "ok": True,
                    "min": [b.min.x, b.min.y, b.min.z],
                    "max": [b.max.x, b.max.y, b.max.z],
                    "center": [b.center.x, b.center.y, b.center.z],
                    "size": [b.size.x, b.size.y, b.size.z],
                }
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_get_values":
        id = int(args.get("id"))
        jks = [str(x) for x in (args.get("json_keys") or [])]
        vals = client.get_param_values(id=id, json_keys=jks)
        return json.dumps({"ok": True, "values": vals})

    if name == "scene_list_params":
        id = int(args.get("id"))
        pl = client.list_params(id=id)
        params = [_param_to_dict(p) for p in pl.params]
        return json.dumps({"ok": True, "params": params})

    if name == "scene_param_info":
        id = int(args.get("id"))
        query = args.get("query")
        json_key_in = args.get("json_key")
        name_in = args.get("name")
        candidate = (
            str(json_key_in).strip()
            if json_key_in is not None and str(json_key_in).strip() != ""
            else (str(query).strip() if query is not None else None)
        )
        name_str = (
            str(name_in).strip()
            if name_in is not None and str(name_in).strip() != ""
            else (str(query).strip() if query is not None else None)
        )
        if (candidate is None or candidate == "") and (name_str is None or name_str == ""):
            return json.dumps({"ok": False, "error": "query, json_key, or name required"})
        try:
            jk = _resolve_json_key(id, candidate=candidate) if candidate else None
            if not jk and name_str:
                jk = _resolve_json_key(id, name=name_str)
            if not jk and candidate:
                jk = _resolve_json_key(id, name=candidate)
        except Exception:
            jk = None
        if not jk:
            return json.dumps(
                {
                    "ok": False,
                    "error": "could not resolve parameter",
                    "id": int(id),
                    **({"query": str(query)} if query is not None else {}),
                    **({"json_key": str(json_key_in)} if json_key_in is not None else {}),
                    **({"name": str(name_in)} if name_in is not None else {}),
                }
            )
        try:
            pl = client.list_params(id=id)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})
        target = None
        for p in getattr(pl, "params", []) or []:
            if getattr(p, "json_key", None) == jk:
                target = p
                break
        if target is None:
            return json.dumps(
                {"ok": False, "error": "parameter not found for id", "id": int(id), "json_key": jk}
            )
        info = _param_to_dict(target)

        def schema_summary(schema: Any) -> dict[str, Any]:
            if not isinstance(schema, dict):
                return {}
            out: dict[str, Any] = {}
            for k in ("type", "title", "description", "default", "minItems", "maxItems", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf"):
                if k in schema:
                    out[k] = schema.get(k)
            if "enum" in schema and isinstance(schema.get("enum"), list):
                out["enum"] = schema.get("enum")
            if "const" in schema:
                out["const"] = schema.get("const")
            if "properties" in schema and isinstance(schema.get("properties"), dict):
                out["properties"] = sorted([str(k) for k in schema.get("properties", {}).keys()])
            if "required" in schema and isinstance(schema.get("required"), list):
                out["required"] = [str(x) for x in schema.get("required") if isinstance(x, str)]
            if "oneOf" in schema and isinstance(schema.get("oneOf"), list):
                out["oneOf_count"] = len(schema.get("oneOf") or [])
            if "anyOf" in schema and isinstance(schema.get("anyOf"), list):
                out["anyOf_count"] = len(schema.get("anyOf") or [])
            if "allOf" in schema and isinstance(schema.get("allOf"), list):
                out["allOf_count"] = len(schema.get("allOf") or [])
            return out

        summary = schema_summary(info.get("value_schema"))
        return json.dumps(
            {
                "ok": True,
                "id": int(id),
                "json_key": jk,
                "param": info,
                **({"value_schema_summary": summary} if summary else {}),
            }
        )

    if name == "scene_capabilities":
        schema_dir = args.get("schema_dir")
        sd, searched = discover_schema_dir(schema_dir, atlas_dir)
        if not sd:
            return json.dumps(
                {
                    "ok": False,
                    "error": "capabilities not found",
                    "searched": searched,
                }
            )
        try:
            with open(Path(sd) / "capabilities.json", "r", encoding="utf-8") as f:
                caps = json.load(f)
            return json.dumps(
                {
                    "ok": True,
                    "schema_dir": str(sd),
                    "capabilities": caps,
                }
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_params_handbook":
        schema_dir = args.get("schema_dir")
        include_groups = bool(args.get("include_groups", True))
        max_types = int(args.get("max_types", 50))
        max_params_per_type = int(args.get("max_params_per_type", 200))
        sd, searched = discover_schema_dir(schema_dir, atlas_dir)
        if not sd:
            return json.dumps(
                {
                    "ok": False,
                    "error": "capabilities not found",
                    "searched": searched,
                }
            )
        try:
            caps = json.loads(
                (Path(sd) / "capabilities.json").read_text(encoding="utf-8")
            )
            lines: list[str] = []
            lines.append("# Atlas Parameters Handbook (from capabilities.json)")
            lines.append("")
            if include_groups:
                globs = caps.get("globals") or {}
                for gname in ("Background", "Axis", "Global"):
                    g = globs.get(gname) if isinstance(globs, dict) else None
                    if not isinstance(g, dict):
                        continue
                    plist = g.get("parameters") or []
                    lines.append(f"## Group: {gname}")
                    for p in plist[:max_params_per_type]:
                        lines.append(
                            f"- `{p.get('jsonKey','')}` — {p.get('type','')} (interp={p.get('supportsInterpolation',False)})"
                        )
                    lines.append("")
            # Object types
            objs = caps.get("objects") or {}
            count_types = 0
            for tname, obj in objs.items() if isinstance(objs, dict) else []:
                if count_types >= max_types:
                    break
                plist = []
                if isinstance(obj, dict):
                    plist = obj.get("parameters") or obj.get("params") or []
                lines.append(f"## Type: {tname}")
                for p in plist[:max_params_per_type]:
                    jk = p.get("jsonKey", "") or p.get("json_key", "")
                    ty = p.get("type", "")
                    interp = p.get("supportsInterpolation", False)
                    lines.append(f"- `{jk}` — {ty} (interp={interp})")
                lines.append("")
                count_types += 1
            md = "\n".join(lines)
            return json.dumps({"ok": True, "markdown": md})
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_schema":
        schema_dir = args.get("schema_dir")
        sd, searched = discover_schema_dir(schema_dir, atlas_dir)
        if not sd:
            return json.dumps(
                {"ok": False, "error": "schema not found", "searched": searched}
            )
        try:
            with open(Path(sd) / "animation3d.schema.json", "r", encoding="utf-8") as f:
                sch = json.load(f)
            return json.dumps({"ok": True, "schema": sch})
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_capabilities_summary":
        schema_dir = args.get("schema_dir")
        max_lines = args.get("max_lines") or 140
        sd, searched = discover_schema_dir(schema_dir, atlas_dir)
        try:
            text = build_capabilities_prompt(Path(sd), max_lines=max_lines)
            return json.dumps({"ok": True, "summary": text})
        except Exception as e:
            # Return generic text on failure
            text = build_capabilities_prompt(
                Path("/does/not/exist"), max_lines=max_lines
            )
            return json.dumps({"ok": False, "summary": text, "error": str(e)})

    if name == "scene_facts_summary":
        limit = int(args.get("sample_limit", 6))
        sid_opt = args.get("id")
        jks = args.get("json_keys") or []
        facts = client.scene_facts()
        lines: list[str] = []
        # Objects summary
        objs = facts.get("objects_list") or []
        lines.append(f"Objects: {len(objs)} total")
        for o in objs[: max(0, limit)]:
            lines.append(
                f"  - {o.get('id')}:{o.get('type')}:{o.get('name')} visible={o.get('visible')}"
            )
        # Time status
        try:
            ts = client.get_time()
            cur = float(getattr(ts, "seconds", 0.0) or 0.0)
            dur = float(getattr(ts, "duration", 0.0) or 0.0)
            lines.append(f"Time: t={cur:.3f}s / duration={dur:.3f}s")
        except Exception:
            pass
        # Camera keys
        try:
            cams = facts.get("keys", {}).get("camera") or []
            if cams:
                lines.append("Camera keys: " + ", ".join(str(float(t)) for t in cams))
        except Exception:
            pass
        # Per-object keys (sample)
        try:
            obj_keys = facts.get("keys", {}).get("objects", {}) or {}
            count = 0
            for oid, mp in obj_keys.items():
                if count >= limit:
                    break
                params = list(mp.items())
                for jk, times in params:
                    lines.append(f"Keys {oid}:{jk}: times={list(times)}")
                count += 1
        except Exception:
            pass
        # Optional current values
        if sid_opt is not None and jks:
            try:
                vals = client.get_param_values(
                    id=int(sid_opt), json_keys=[str(x) for x in jks]
                )
                lines.append("Values:")
                for k, v in vals.items():
                    try:
                        vv = json.dumps(v)
                    except Exception:
                        vv = str(v)
                    lines.append(f"  - {k} = {vv}")
            except Exception:
                pass
        return json.dumps({"ok": True, "summary": "\n".join(lines)})

    if name == "scene_validate_params":
        set_params = args.get("set_params") or []
        # Normalize and validate id shapes early to avoid malformed writes
        norm_params: list[dict] = []
        for it in set_params:
            if not isinstance(it, dict):
                return json.dumps(
                    {"ok": False, "error": "each set_params item must be an object"}
                )
            jk = it.get("json_key")
            if not isinstance(jk, str) or not jk:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "each set_params item must include a non-empty 'json_key'",
                    }
                )
            id = int(it.get("id"))
            norm_params.append({"id": id, "json_key": jk, "value": it.get("value")})
        res = client.validate_apply(norm_params)
        return json.dumps(res)

    if name == "scene_apply":
        set_params = args.get("set_params") or []
        # Normalize, resolve names→json_keys, and pre-verify ids/keys
        norm_params: list[dict] = []
        notes: list[dict] = []
        for it in set_params:
            if not isinstance(it, dict):
                return json.dumps(
                    {"ok": False, "error": "each set_params item must be an object"}
                )
            id = int(it.get("id"))
            jk_in = it.get("json_key")
            name_in = it.get("name")
            # Resolve json_key
            if id == 0:
                if jk_in is not None and jk_in != "Camera 3DCamera":
                    return json.dumps(
                        {
                            "ok": False,
                            "error": "json_key not found: expected 'Camera 3DCamera' for id=0",
                        }
                    )
                jk = "Camera 3DCamera"
            else:
                jk: str | None = None
                if isinstance(jk_in, str) and jk_in.strip():
                    # If canonical exists, keep; else resolve aliases/names
                    pl = client.list_params(id=id)
                    jks = {
                        getattr(p, "json_key", ""): True
                        for p in getattr(pl, "params", [])
                    }
                    if jk_in in jks:
                        jk = jk_in
                    else:
                        jk_resolved = _resolve_json_key(id, candidate=jk_in)
                        if jk_resolved:
                            jk = jk_resolved
                            notes.append(
                                {
                                    "remapped_param": {
                                        "from": jk_in,
                                        "to": jk,
                                        "id": id,
                                    }
                                }
                            )
                        else:
                            if isinstance(name_in, str) and name_in.strip():
                                jk_name = _resolve_json_key(id, name=name_in)
                                if jk_name:
                                    jk = jk_name
                                    notes.append(
                                        {
                                            "resolved_by_name": {
                                                "name": name_in,
                                                "to": jk,
                                                "id": id,
                                            }
                                        }
                                    )
                                else:
                                    return json.dumps(
                                        {
                                            "ok": False,
                                            "error": f"json_key not found: '{jk_in}'",
                                            "hint": "Use scene_list_params(id) or provide 'name'",
                                            "id": id,
                                        }
                                    )
                            else:
                                return json.dumps(
                                    {
                                        "ok": False,
                                        "error": f"json_key not found: '{jk_in}'",
                                        "hint": "Use scene_list_params(id) or provide 'name'",
                                        "id": id,
                                    }
                                )
                else:
                    if not isinstance(name_in, str) or not name_in.strip():
                        return json.dumps(
                            {
                                "ok": False,
                                "error": "each set_params item must include 'json_key' or 'name'",
                            }
                        )
                    jk_name = _resolve_json_key(id, name=name_in)
                    if not jk_name:
                        return json.dumps(
                            {
                                "ok": False,
                                "error": f"could not resolve json_key for name='{name_in}'",
                            }
                        )
                    jk = jk_name
                    notes.append(
                        {"resolved_by_name": {"name": name_in, "to": jk, "id": id}}
                    )
            norm_params.append({"id": id, "json_key": jk, "value": it.get("value")})

        # Validate before apply; if validation fails for types, return details
        val = client.validate_apply(norm_params)
        if not bool(val.get("ok", False)):
            return json.dumps(
                {
                    "ok": False,
                    "validate": val,
                    **({"notes": notes} if notes else {}),
                }
            )
        # Warn when timeline keys exist for the same id/json_key (scene values overridden during playback)
        overrides: list[dict] = []
        try:
            for p in norm_params:
                id2 = int(p.get("id", -1))
                jk2 = p.get("json_key")
                if id2 >= 0 and jk2:
                    lr = client.list_keys(id=id2, json_key=jk2, include_values=False)
                    times = [k.time for k in getattr(lr, "keys", [])]
                    if times:
                        overrides.append(
                            {"id": id2, "json_key": jk2, "key_times": times}
                        )
        except Exception:
            pass
        ok = client.apply_params(norm_params)
        resp = {"ok": bool(ok)}
        if overrides:
            resp["warning"] = (
                "animation keys exist for some params; during playback those keys override scene values"
            )
            resp["overrides"] = overrides
        if notes:
            resp["notes"] = notes
        return json.dumps(resp)

    if name == "scene_save_scene":
        ok = client.save_scene(args.get("path"))
        return json.dumps({"ok": bool(ok)})

    if name == "scene_camera_fit":
        # Use CameraFit (planning) and apply the first typed value to the scene camera (id=0)
        try:
            ids = args.get("ids") or []
            after = bool(args.get("after_clipping", True))
            minr = float(args.get("min_radius", 0.0))
            if not ids:
                try:
                    ids = client.fit_candidates()
                except Exception:
                    ids = []
            vals = client.camera_fit(
                ids=ids, all=False, after_clipping=after, min_radius=minr
            )
            cam = vals[0] if vals else None
            if not cam:
                return json.dumps(
                    {"ok": False, "error": "camera_fit returned no value"}
                )
            ok = client.apply_params(
                [{"id": 0, "json_key": "Camera 3DCamera", "value": cam}]
            )
            return json.dumps({"ok": bool(ok)})
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_camera_apply":
        try:
            cam = args.get("value")
            if not cam or not isinstance(cam, dict):
                return json.dumps(
                    {"ok": False, "error": "value must be a typed camera object"}
                )
            ok = client.apply_params(
                [{"id": 0, "json_key": "Camera 3DCamera", "value": cam}]
            )
            return json.dumps({"ok": bool(ok)})
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_validate_param_value":
        id = int(args.get("id"))
        json_key = str(args.get("json_key"))
        value = args.get("value")
        pl = client.list_params(id=id)
        meta = None
        for p in pl.params:
            if p.json_key == json_key:
                meta = p
                break
        if meta is None:
            return json.dumps({"ok": False, "error": "json_key not found for id"})
        # Prefer schema-based validation (protobuf -> dict)
        schema = {}
        try:
            if hasattr(meta, "HasField") and meta.HasField("value_schema"):
                schema = MessageToDict(getattr(meta, "value_schema")) or {}
        except Exception:
            schema = {}
        # Guard against unexpected non-mapping schemas from server (e.g., list)
        # jsonschema expects a mapping (or bool) at the root; otherwise validator will crash
        if not isinstance(schema, (dict, bool)):
            # Log full context for diagnosis and return a clear error
            try:
                logger = logging.getLogger("atlas_agent.tools")
                logger.error(
                    "invalid value_schema for id=%s json_key=%s: type=%s schema=%s",
                    id,
                    json_key,
                    type(schema).__name__,
                    schema,
                )
            except Exception:
                pass
            return json.dumps(
                {
                    "ok": False,
                    "error": "invalid_value_schema",
                    "details": {
                        "id": id,
                        "json_key": json_key,
                        "got_type": type(schema).__name__,
                        "schema": schema,
                    },
                }
            )
        # Cache compiled validator by (id, json_key, schema digest)
        digest = (
            hashlib.sha256(
                json.dumps(schema, sort_keys=True).encode("utf-8")
            ).hexdigest()
            if schema
            else ""
        )
        cache_key = f"{id}:{json_key}:{digest}"
        validator = _schema_validator_cache.get(cache_key)
        if validator is None:
            validator = JsonSchemaValidator(schema or {})
            _schema_validator_cache[cache_key] = validator
        errors = list(validator.iter_errors(value))
        if errors:
            e = errors[0]
            loc = "".join(
                [f"[{repr(p)}]" if isinstance(p, int) else f".{p}" for p in e.path]
            )
            msg = f"{loc}: {e.message}" if loc else e.message
            return json.dumps({"ok": False, "error": msg})
        return json.dumps({"ok": True})

    if name == "scene_set_visibility":
        ids = args.get("ids") or []
        on = bool(args.get("on", True))
        ok = client.set_visibility(ids, on)
        return json.dumps({"ok": ok})

    if name == "scene_make_alias":
        ids = [int(i) for i in (args.get("ids") or [])]
        if not ids:
            return json.dumps({"ok": False, "error": "ids must be a non-empty list"})
        try:
            res = client.make_alias(ids)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})
        return json.dumps(res)

    if name == "scene_cut_suggest_box":
        try:
            req = client._pb2.CutSuggestRequest(
                ids=args.get("ids") or [],
                margin=float(args.get("margin", 0.0)),
                after_clipping=bool(args.get("after_clipping", False)),
            )
            resp = client._stub.CutSuggest(req)
            box = resp.box
            return json.dumps(
                {
                    "ok": True,
                    "min": [box.min.x, box.min.y, box.min.z],
                    "max": [box.max.x, box.max.y, box.max.z],
                }
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "scene_cut_set_box":
        minv = args.get("min") or [0, 0, 0]
        maxv = args.get("max") or [0, 0, 0]
        ok = client.cut_set_box(
            (minv[0], minv[1], minv[2]),
            (maxv[0], maxv[1], maxv[2]),
            refit_camera=bool(args.get("refit_camera", True)),
        )
        return json.dumps({"ok": ok})

    if name == "scene_cut_clear":
        return json.dumps({"ok": client.cut_clear()})

    return None
