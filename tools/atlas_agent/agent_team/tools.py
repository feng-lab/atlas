from __future__ import annotations

import json
from typing import Any, Dict, List, Tuple

from ..scene_rpc import SceneClient


def scene_tools_and_dispatcher(client: SceneClient, *, atlas_dir: str | None = None) -> Tuple[List[Dict[str, Any]], callable]:
    """Return (tool_specs, dispatcher) for OpenAI tool-calling.

    The dispatcher signature: (name: str, args_json: str) -> str
    Returns a compact JSON string result that the model can parse.
    """

    tools: List[Dict[str, Any]] = [
        {
            "type": "function",
            "function": {
                "name": "scene_guess_primary_object",
                "description": "Guess a primary target object id for simple commands. Returns the single Mesh id if exactly one Mesh exists; otherwise returns -1.",
                "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_recipe_orbit_focus",
                "description": "Recipe: orbit around target ids. Fits camera, writes start/end camera keys, and sets duration.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "ids": {"type": "array", "items": {"type": "integer"}},
                        "axis": {"type": "string", "enum": ["x", "y", "z"], "default": "y"},
                        "angle_degrees": {"type": "number", "default": 360.0},
                        "duration": {"type": "number", "default": 8.0},
                        "easing": {"type": "string", "default": "Linear"}
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_recipe_fade_emphasis",
                "description": "Recipe: emphasize one id with a color and fade opacity from start to end; optionally dim others.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "integer"},
                        "color": {"anyOf": [{"type": "string"}, {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 4}]},
                        "start_opacity": {"type": "number", "default": 0.3},
                        "end_opacity": {"type": "number", "default": 1.0},
                        "t0": {"type": "number", "default": 0.0},
                        "t1": {"type": "number", "default": 5.0},
                        "dim_others": {"type": "boolean", "default": True},
                        "easing": {"type": "string", "default": "Linear"}
                    },
                    "required": ["id"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_recipe_reveal_with_cut",
                "description": "Recipe: suggest cut box for ids (or all), apply cut with optional refit, and set a camera key.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "ids": {"type": "array", "items": {"type": "integer"}},
                        "margin": {"type": "number", "default": 0.0},
                        "refit_camera": {"type": "boolean", "default": True},
                        "time": {"type": "number", "default": 0.0},
                        "after_clipping": {"type": "boolean", "default": True},
                        "easing": {"type": "string", "default": "Linear"}
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_set_param_by_name",
                "description": "Set a parameter by display name (case-insensitive) for an object or group. Resolves json_key via scene_list_params, then calls scene_set_key_param.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                        "name": {"type": "string"},
                        "type_hint": {"type": "string"},
                        "time": {"type": "number"},
                        "easing": {"type": "string", "default": "Linear"},
                        "value": {"description": "Native JSON value (bool/number/string/array/object)"},
                    },
                    "required": ["name", "time", "value"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_remove_key_param_at_time",
                "description": "Remove one or more keys near a time for a parameter by json_key (object or group scope). Uses a tolerance window to match existing keys.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                        "json_key": {"type": "string"},
                        "time": {"type": "number"},
                        "tolerance": {"type": "number", "default": 1e-3}
                    },
                    "required": ["json_key", "time"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_replace_key_param",
                "description": "Replace (or set) a parameter key by json_key at time: remove any key within tolerance then set a new typed value.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                        "json_key": {"type": "string"},
                        "time": {"type": "number"},
                        "easing": {"type": "string", "default": "Linear"},
                        "value": {"description": "Native JSON value (bool/number/string/array/object)"},
                        "tolerance": {"type": "number", "default": 1e-3},
                        "strict": {"type": "boolean", "default": False}
                    },
                    "required": ["json_key", "time", "value"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_replace_key_camera",
                "description": "Replace (or set) a camera key at time: remove any camera key within tolerance then set a new camera value.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "time": {"type": "number"},
                        "easing": {"type": "string", "default": "Linear"},
                        "value": {"type": "object"},
                        "tolerance": {"type": "number", "default": 1e-3},
                        "strict": {"type": "boolean", "default": False}
                    },
                    "required": ["time", "value"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "system_info",
                "description": "Return OS/platform info and common paths so the agent can reason about file locations.",
                "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_expand_paths",
                "description": "Expand ~ and env vars and normalize paths. Returns expanded absolute-like paths per entry (relative kept relative).",
                "parameters": {"type": "object", "properties": {"paths": {"type": "array", "items": {"type": "string"}}}, "required": ["paths"], "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_check_paths",
                "description": "Check which of the given paths exist on the local filesystem. Returns {exists:[], missing:[]}.",
                "parameters": {"type": "object", "properties": {"paths": {"type": "array", "items": {"type": "string"}}}, "required": ["paths"], "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_emphasize_object",
                "description": "Emphasize an object by setting mode/color/opacity and optionally dimming others (atomic).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "integer"},
                        "mode": {"type": "string", "description": "Rendering mode hint; e.g., 'Sphere' for Swc"},
                        "color": {
                            "description": "Hex '#RRGGBB[AA]' or [r,g,b,a] floats",
                            "anyOf": [
                                {"type": "string"},
                                {
                                    "type": "array",
                                    "items": {"type": "number"},
                                    "minItems": 3,
                                    "maxItems": 4
                                }
                            ]
                        },
                        "opacity": {"type": "number"},
                        "dim_others": {"type": "boolean", "default": False},
                        "time": {"type": "number", "default": 0.0},
                        "easing": {"type": "string", "default": "Linear"},
                    },
                    "required": ["id"],
                    "additionalProperties": False,
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
                        "files": {"type": "array", "items": {"type": "string"}},
                    },
                    "required": ["files"],
                    "additionalProperties": False,
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
                        "files": {"type": "array", "items": {"type": "string"}},
                    },
                    "required": ["files"],
                    "additionalProperties": False,
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
                        "names": {"type": "array", "items": {"type": "string"}},
                        "dir_hints": {"type": "array", "items": {"type": "string"}},
                        "extensions": {"type": "array", "items": {"type": "string"}, "default": [".msh", ".obj", ".ply", ".swc", ".nii", ".tif", ".tiff"]},
                        "case_insensitive": {"type": "boolean", "default": True}
                    },
                    "required": ["names"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_find_candidates",
                "description": "Resolve candidate file paths by trying directories and extensions; returns existing absolute paths.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "dirs": {"type": "array", "items": {"type": "string"}},
                        "names": {"type": "array", "items": {"type": "string"}},
                        "extensions": {"type": "array", "items": {"type": "string"}, "default": [".msh", ".obj", ".ply", ".swc", ".nii", ".tif", ".tiff"]},
                        "case_insensitive": {"type": "boolean", "default": True}
                    },
                    "required": ["dirs", "names"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_list_objects",
                "description": "List all objects in the current scene (id, type, name, visible).",
                "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_bbox",
                "description": "Get bounding box for a set of ids. Omitting ids means all objects.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "ids": {"type": "array", "items": {"type": "integer"}},
                        "after_clipping": {"type": "boolean", "default": False},
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_list_keys",
                "description": "List keys for a target scope (camera|object|group) and optional json_key.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_camera": {"type": "boolean", "default": False},
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                        "json_key": {"type": "string"},
                        "include_values": {"type": "boolean", "default": False}
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_get_time",
                "description": "Get current timeline seconds and duration.",
                "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_capabilities",
                "description": "List parameter capabilities for background/axis/global and object types.",
                "parameters": {
                    "type": "object",
                    "properties": {"ids": {"type": "array", "items": {"type": "integer"}}},
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_list_params",
                "description": "List parameters for a given scope (camera|object|group).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_camera": {"type": "boolean", "default": False},
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_validate_param_value",
                "description": "Validate a candidate value against the live parameter metadata (type and option_names) for a given scope.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                        "json_key": {"type": "string"},
                        "value": {"description": "Candidate JSON value (native types)"},
                    },
                    "required": ["json_key", "value"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_ensure_animation",
                "description": "Ensure a 3D animation exists in the GUI.",
                "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_set_duration",
                "description": "Set animation duration in seconds.",
                "parameters": {"type": "object", "properties": {"seconds": {"type": "number"}}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_camera_fit",
                "description": "Suggest a camera value that frames selected objects.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "ids": {"type": "array", "items": {"type": "integer"}},
                        "all": {"type": "boolean", "default": False},
                        "after_clipping": {"type": "boolean", "default": False},
                        "min_radius": {"type": "number", "default": 0.0},
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_camera_orbit",
                "description": "Suggest start/end camera values to orbit around bbox center.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "ids": {"type": "array", "items": {"type": "integer"}},
                        "axis": {"type": "string", "enum": ["x", "y", "z"], "default": "y"},
                        "angle_degrees": {"type": "number", "default": 360.0},
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_camera_dolly",
                "description": "Suggest start/end camera values to dolly to given distances.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "ids": {"type": "array", "items": {"type": "integer"}},
                        "start_dist": {"type": "number", "default": 0.0},
                        "end_dist": {"type": "number", "default": 0.0},
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_set_key_camera",
                "description": "Add a camera key at a time with a given easing using a camera value JSON.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "time": {"type": "number"},
                        "easing": {"type": "string", "default": "Linear"},
                        "value": {"description": "Native JSON camera value object", "type": "object"},
                    },
                    "required": ["time", "value"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_set_key_param",
                "description": "Add a parameter key for object/group by json_key with a native JSON value (bool/number/string/array/object).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                        "json_key": {"type": "string"},
                        "time": {"type": "number"},
                        "easing": {"type": "string", "default": "Linear"},
                        "value": {"description": "Native JSON value (bool/number/string/array/object)"},
                    },
                    "required": ["json_key", "time", "value"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_clear_keys",
                "description": "Clear all keys for a parameter (or camera).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_camera": {"type": "boolean", "default": False},
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                        "json_key": {"type": "string"},
                    },
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_remove_key",
                "description": "Remove a key at a specific time for a parameter.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "scope_object": {"type": "integer"},
                        "scope_group": {"type": "string", "enum": ["background", "axis", "global"]},
                        "json_key": {"type": "string"},
                        "time": {"type": "number"},
                    },
                    "required": ["json_key", "time"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_batch",
                "description": "Batch multiple SetKey and RemoveKey operations atomically.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "set_keys": {"type": "array", "items": {"type": "object"}},
                        "remove_keys": {"type": "array", "items": {"type": "object"}},
                        "commit": {"type": "boolean", "default": True},
                    },
                    "additionalProperties": False,
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
                    "properties": {"ids": {"type": "array", "items": {"type": "integer"}}, "on": {"type": "boolean"}},
                    "required": ["ids", "on"],
                    "additionalProperties": False,
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
                    "properties": {"ids": {"type": "array", "items": {"type": "integer"}}, "margin": {"type": "number", "default": 0.0}, "after_clipping": {"type": "boolean", "default": False}},
                    "additionalProperties": False,
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
                        "min": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                        "max": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                        "refit_camera": {"type": "boolean", "default": True},
                    },
                    "required": ["min", "max"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_cut_clear",
                "description": "Clear global cuts.",
                "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_set_time",
                "description": "Set current timeline time (seconds).",
                "parameters": {"type": "object", "properties": {"seconds": {"type": "number"}, "cancel": {"type": "boolean", "default": False}}, "required": ["seconds"], "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_play",
                "description": "Start playback in GUI at given fps.",
                "parameters": {"type": "object", "properties": {"fps": {"type": "number", "default": 25.0}}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_pause",
                "description": "Pause playback in GUI.",
                "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_save_animation",
                "description": "Save the current animation to a .animation3d path.",
                "parameters": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"], "additionalProperties": False},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "atlas_export_video",
                "description": "Export an .animation3d to MP4 using the Atlas headless exporter. The agent chooses parameters (fps, size, range).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "animation": {"type": "string", "description": "Path to .animation3d file"},
                        "out": {"type": "string", "description": "Output .mp4 path"},
                        "fps": {"type": "number", "default": 30},
                        "start": {"type": "integer", "default": 0},
                        "end": {"type": "integer", "default": -1},
                        "width": {"type": "integer", "default": 1920},
                        "height": {"type": "integer", "default": 1080},
                        "overwrite": {"type": "boolean", "default": True},
                        "use_gpu_devices": {"type": "string", "description": "Linux only: comma-separated GPU ids e.g. '0,1'"},
                    },
                    "required": ["animation", "out"],
                    "additionalProperties": False,
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_render_preview",
                "description": "Render a single preview frame by saving the current animation and invoking headless Atlas. Returns a path to the image in the OS temp directory.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "time": {"type": "number", "description": "Preview time in seconds", "default": 0.0},
                        "fps": {"type": "number", "default": 30},
                        "width": {"type": "integer", "default": 512},
                        "height": {"type": "integer", "default": 512}
                    },
                    "additionalProperties": False,
                },
            },
        },
    ]

    def dispatch(name: str, args_json: str) -> str:
        try:
            args = json.loads(args_json or "{}")
        except Exception:
            args = {}

        # System / FS helpers for LLM-driven resolution
        if name == "system_info":
            import os as _os
            try:
                import platform as _plat
                system = _plat.system()
                release = _plat.release()
            except Exception:
                system = ""
                release = ""
            home = _os.path.expanduser("~")
            cwd = _os.getcwd()
            info = {
                "system": system,
                "release": release,
                "os_name": _os.name,
                "home": home,
                "cwd": cwd,
                "common_dirs": {
                    "Documents": _os.path.join(home, "Documents"),
                    "Downloads": _os.path.join(home, "Downloads"),
                    "Desktop": _os.path.join(home, "Desktop"),
                },
            }
            return json.dumps(info)
        if name == "fs_expand_paths":
            import os as _os
            paths = [str(p) for p in (args.get("paths") or [])]
            out: list[str] = []
            for p in paths:
                t = _os.path.expanduser(_os.path.expandvars(p))
                # Normalize separators for current OS
                t = _os.path.normpath(t)
                out.append(t)
            return json.dumps({"paths": out})
        if name == "fs_check_paths":
            import os as _os
            paths = [str(p) for p in (args.get("paths") or [])]
            exists: list[str] = []
            missing: list[str] = []
            for p in paths:
                if _os.path.exists(p):
                    exists.append(p)
                else:
                    missing.append(p)
            return json.dumps({"exists": exists, "missing": missing})

        # Scene understanding / load
        if name == "scene_guess_primary_object":
            objs = client.list_objects()
            mesh_ids = [int(o.id) for o in objs.objects if getattr(o, "type", "").lower() == "mesh"]
            oid = mesh_ids[0] if len(mesh_ids) == 1 else -1
            return json.dumps({"id": oid, "count_mesh": len(mesh_ids)})
        if name == "scene_set_param_by_name":
            scope_object = args.get("scope_object")
            if scope_object is None:
                # Try to guess a primary object
                g = json.loads(dispatch("scene_guess_primary_object", "{}"))
                if int(g.get("id", -1)) >= 0:
                    scope_object = int(g["id"])
            scope_group = args.get("scope_group")
            if scope_object is None and scope_group is None:
                return json.dumps({"ok": False, "error": "scope_object or scope_group required (or a unique Mesh must be present)"})
            name_str = str(args.get("name", ""))
            type_hint = args.get("type_hint")
            time_v = float(args.get("time", 0.0))
            easing = str(args.get("easing", "Linear"))
            value_native = args.get("value")
            # Resolve param json_key by name and optional type
            pl = client.list_params(scope_object=scope_object, scope_group=scope_group)
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
                return json.dumps({"ok": False, "error": f"parameter '{name_str}' not found in scope"})
            # Coerce common types
            t = (target_type or "").lower()
            if "bool" in t and isinstance(value_native, str):
                s = value_native.strip().lower()
                if s in ("true", "1", "yes", "on"): value_native = True
                elif s in ("false", "0", "no", "off"): value_native = False
            if t.endswith("vec4") and isinstance(value_native, list):
                if len(value_native) == 3:
                    value_native = [float(value_native[0]), float(value_native[1]), float(value_native[2]), 1.0]
                elif len(value_native) == 4:
                    value_native = [float(x) for x in value_native]
            if (t == "float" or t == "double") and isinstance(value_native, str):
                try:
                    value_native = float(value_native)
                except Exception:
                    pass
            # Typed SetKey
            try:
                ok = client.set_key_param(scope_object=scope_object, scope_group=scope_group, json_key=target_jk, time=time_v, easing=easing, value=value_native)
                return json.dumps({"ok": ok, "json_key": target_jk, "type": target_type})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg, "json_key": target_jk, "type": target_type})

        if name == "scene_remove_key_param_at_time":
            scope_object = args.get("scope_object")
            scope_group = args.get("scope_group")
            json_key = str(args.get("json_key"))
            time_v = float(args.get("time", 0.0))
            tol = float(args.get("tolerance", 1e-3))
            # Find existing keys near time and remove
            try:
                if scope_object is not None:
                    lr = client.list_keys(scope_object=int(scope_object), json_key=json_key, include_values=False)
                elif scope_group is not None:
                    lr = client.list_keys(scope_group=str(scope_group), json_key=json_key, include_values=False)
                else:
                    return json.dumps({"ok": False, "error": "scope_object or scope_group required"})
                times = [k.time for k in getattr(lr, "keys", [])]
                to_remove = [t for t in times if abs(t - time_v) <= tol]
                removed = 0
                for t in to_remove:
                    ok = client.remove_key(scope_object=scope_object, scope_group=scope_group, json_key=json_key, time=t)
                    if ok:
                        removed += 1
                return json.dumps({"ok": True, "removed": removed})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})

        if name == "scene_replace_key_param":
            scope_object = args.get("scope_object")
            scope_group = args.get("scope_group")
            json_key = str(args.get("json_key"))
            time_v = float(args.get("time", 0.0))
            easing = str(args.get("easing", "Linear"))
            value = args.get("value")
            tol = float(args.get("tolerance", 1e-3))
            strict = bool(args.get("strict", False))
            try:
                # Remove keys within tolerance
                rm = json.loads(dispatch("scene_remove_key_param_at_time", json.dumps({
                    "scope_object": scope_object,
                    "scope_group": scope_group,
                    "json_key": json_key,
                    "time": time_v,
                    "tolerance": tol,
                })))
                # If strict and nothing removed while a key exists far, we still set new one
                ok = client.set_key_param(scope_object=scope_object, scope_group=scope_group, json_key=json_key, time=time_v, easing=easing, value=value)
                return json.dumps({"ok": ok, "removed": rm.get("removed", 0)})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})

        if name == "scene_replace_key_camera":
            time_v = float(args.get("time", 0.0))
            easing = str(args.get("easing", "Linear"))
            value = args.get("value") or {}
            tol = float(args.get("tolerance", 1e-3))
            strict = bool(args.get("strict", False))
            try:
                # Remove camera keys within tolerance
                try:
                    lr = client.list_keys(scope_camera=True, include_values=False)
                    times = [k.time for k in getattr(lr, "keys", [])]
                    to_remove = [t for t in times if abs(t - time_v) <= tol]
                    for t in to_remove:
                        client.remove_key(json_key="", time=t)  # scope_camera handled in client.remove_key
                except Exception:
                    pass
                ok = client.set_key_camera(time=time_v, easing=easing, value=value)
                return json.dumps({"ok": ok})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})
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
                return json.dumps({"ok": False, "error": str(e), "hint": "Use scene_smart_load to resolve names in common dirs (Documents/Downloads/Desktop/cwd)."})
        if name == "scene_ensure_loaded":
            files = args.get("files") or []
            summary = client.ensure_loaded(files)
            return json.dumps({"ok": True, **summary})
        if name == "scene_list_keys":
            try:
                lr = client.list_keys(
                    scope_camera=bool(args.get("scope_camera", False)),
                    scope_object=args.get("scope_object"),
                    scope_group=args.get("scope_group"),
                    json_key=args.get("json_key"),
                    include_values=bool(args.get("include_values", False)),
                )
                keys = []
                for k in lr.keys:
                    # Server returns stringified values for verification today
                    keys.append({"time": k.time, "type": k.type, "value": getattr(k, "value_json", "")})
                return json.dumps({"ok": True, "keys": keys})
            except Exception as e:
                msg = str(e)
                try:
                    msg = e.details()  # type: ignore[attr-defined]
                except Exception:
                    pass
                return json.dumps({"ok": False, "error": msg})
        if name == "scene_get_time":
            ts = client.get_time()
            return json.dumps({"seconds": getattr(ts, "seconds", 0.0), "duration": getattr(ts, "duration", 0.0)})
        if name == "scene_smart_load":
            import os as _os
            names = args.get("names") or []
            dir_hints = args.get("dir_hints") or []
            exts = args.get("extensions") or []
            ci = bool(args.get("case_insensitive", True))
            if not dir_hints:
                home = _os.path.expanduser("~")
                # OS-specific common locations
                dirs: list[str] = []
                try:
                    import platform as _plat
                    system = _plat.system()
                except Exception:
                    system = ""
                if system == "Windows":
                    user = os.environ.get("USERPROFILE") or home
                    for base in ["Documents", "Downloads", "Desktop"]:
                        dirs.append(_os.path.join(user, base))
                elif system == "Darwin":
                    for base in ["Documents", "Downloads", "Desktop"]:
                        dirs.append(_os.path.join(home, base))
                else:
                    # Linux/Unix
                    for base in ["Documents", "Downloads", "Desktop"]:
                        dirs.append(_os.path.join(home, base))
                    # Common data mount points
                    dirs += ["/data", "/mnt/data", "/srv/data"]
                # Always include cwd last
                dirs.append(_os.getcwd())
                dir_hints = dirs
            def variants(nm: str) -> list[str]:
                base, ext = _os.path.splitext(nm)
                cand = [nm]
                if not ext:
                    cand.extend([base + e for e in exts])
                return cand
            resolved: list[str] = []
            tried: list[str] = []
            for d in dir_hints:
                d2 = _os.path.expanduser(_os.path.expandvars(str(d)))
                if not _os.path.isdir(d2):
                    continue
                for nm in names:
                    for cand in variants(nm):
                        p = _os.path.join(d2, cand)
                        tried.append(p)
                        if _os.path.exists(p):
                            resolved.append(_os.path.abspath(p))
                            continue
                        if ci:
                            try:
                                target = cand.lower()
                                for fname in _os.listdir(d2):
                                    if fname.lower() == target and _os.path.exists(_os.path.join(d2, fname)):
                                        resolved.append(_os.path.abspath(_os.path.join(d2, fname)))
                                        break
                            except Exception:
                                pass
            loaded = 0
            if resolved:
                resp = client.load_files(resolved)
                loaded = len(resp.objects)
            return json.dumps({"loaded_objects": loaded, "resolved": resolved, "tried": tried})
        if name == "scene_recipe_orbit_focus":
            ids = args.get("ids") or []
            axis = str(args.get("axis", "y"))
            angle = float(args.get("angle_degrees", 360.0))
            duration = float(args.get("duration", 8.0))
            easing = str(args.get("easing", "Linear"))
            # Derive ids if empty: try primary mesh; else all objects
            if not ids:
                try:
                    g = json.loads(dispatch("scene_guess_primary_object", "{}"))
                    if int(g.get("id", -1)) >= 0:
                        ids = [int(g["id"])]
                except Exception:
                    ids = []
            if not ids:
                try:
                    lo = client.list_objects()
                    ids = [int(o.id) for o in lo.objects]
                except Exception:
                    ids = []
            try:
                # Suggest orbit cameras and write start/end keys
                cams = client.camera_orbit(ids=ids or None, axis=axis, angle_degrees=angle)
                if not cams:
                    return json.dumps({"ok": False, "error": "no camera suggestions"})
                start = cams[0]
                end = cams[-1] if len(cams) > 1 else cams[0]
                ok1 = client.set_key_camera(time=0.0, easing=easing, value=start)
                ok2 = client.set_key_camera(time=duration, easing=easing, value=end)
                okd = client.set_duration(duration)
                return json.dumps({"ok": bool(ok1 and ok2 and okd), "ids": ids})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})
        if name == "scene_recipe_fade_emphasis":
            oid = int(args.get("id"))
            color = args.get("color", "magenta")
            t0 = float(args.get("t0", 0.0))
            t1 = float(args.get("t1", 5.0))
            easing = str(args.get("easing", "Linear"))
            start_op = float(args.get("start_opacity", 0.3))
            end_op = float(args.get("end_opacity", 1.0))
            dim_others = bool(args.get("dim_others", True))
            # Initialize emphasis at t0 (color + optional dim)
            _ = json.loads(dispatch("scene_emphasize_object", json.dumps({
                "id": oid, "time": t0, "color": color, "easing": easing, "dim_others": dim_others, "opacity": end_op
            })))
            # Find json_key for Opacity and write ramp
            try:
                pl = client.list_params(scope_object=oid)
                jk_op = None
                for p in pl.params:
                    if p.name == "Opacity" and p.type == "Float":
                        jk_op = p.json_key
                        break
                if jk_op is None:
                    return json.dumps({"ok": False, "error": "Opacity parameter not found"})
                r1 = json.loads(dispatch("scene_replace_key_param", json.dumps({
                    "scope_object": oid, "json_key": jk_op, "time": t0, "easing": easing, "value": start_op
                })))
                r2 = json.loads(dispatch("scene_replace_key_param", json.dumps({
                    "scope_object": oid, "json_key": jk_op, "time": t1, "easing": easing, "value": end_op
                })))
                return json.dumps({"ok": bool(r1.get("ok") and r2.get("ok"))})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})
        if name == "scene_recipe_reveal_with_cut":
            ids = args.get("ids") or []
            margin = float(args.get("margin", 0.0))
            refit = bool(args.get("refit_camera", True))
            time_v = float(args.get("time", 0.0))
            after_clip = bool(args.get("after_clipping", True))
            easing = str(args.get("easing", "Linear"))
            try:
                # Suggest cut box
                req = client._pb2.CutSuggestRequest(ids=ids, mode="box", margin=margin, after_clipping=after_clip)
                resp = client._stub.CutSuggest(req)
                box = resp.box
                ok_cut = client.cut_set_box((box.min.x, box.min.y, box.min.z), (box.max.x, box.max.y, box.max.z), refit_camera=refit)
                # Set a camera fit key after cut (best effort)
                cams = client.camera_fit(ids=ids or None, all=not bool(ids), after_clipping=True)
                if cams:
                    client.set_key_camera(time=time_v, easing=easing, value=cams[0])
                return json.dumps({"ok": bool(ok_cut)})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})
        if name == "scene_list_objects":
            resp = client.list_objects()
            objs = [
                {"id": o.id, "type": o.type, "name": o.name, "visible": o.visible}
                for o in resp.objects
            ]
            return json.dumps({"objects": objs})
        if name == "fs_find_candidates":
            import os as _os
            dirs = args.get("dirs") or []
            names = args.get("names") or []
            exts = args.get("extensions") or []
            ci = bool(args.get("case_insensitive", True))
            out: list[str] = []
            def variants(nm: str) -> list[str]:
                base, ext = _os.path.splitext(nm)
                cand = [nm]
                if not ext:
                    cand.extend([base + e for e in exts])
                return cand
            for d in dirs:
                d2 = _os.path.expanduser(_os.path.expandvars(str(d)))
                for nm in names:
                    for cand in variants(nm):
                        p = _os.path.join(d2, cand)
                        if _os.path.exists(p):
                            out.append(_os.path.abspath(p))
                        elif ci:
                            # Try case-insensitive match by scanning directory
                            try:
                                target = cand.lower()
                                for fname in _os.listdir(d2):
                                    if fname.lower() == target and _os.path.exists(_os.path.join(d2, fname)):
                                        out.append(_os.path.abspath(_os.path.join(d2, fname)))
                                        break
                            except Exception:
                                pass
            return json.dumps({"candidates": out})
        if name == "scene_bbox":
            ids = args.get("ids") or []
            after = bool(args.get("after_clipping", False))
            req = client._pb2.BBoxRequest(ids=ids, after_clipping=after)
            resp = client._stub.BBox(req)
            b = resp.bbox
            return json.dumps({
                "min": [b.min.x, b.min.y, b.min.z],
                "max": [b.max.x, b.max.y, b.max.z],
                "center": [b.center.x, b.center.y, b.center.z],
                "size": [b.size.x, b.size.y, b.size.z],
            })
        if name == "scene_capabilities":
            ids = args.get("ids") or []
            resp = client._stub.Capabilities(client._pb2.CapabilitiesRequest(ids=ids))
            # In Python protobuf, field named "global" becomes attribute "global_"
            global_params = getattr(resp, "global_", None)
            if global_params is None:
                try:
                    global_params = getattr(resp, "global")
                except Exception:
                    global_params = []
            # Return sizes only for brevity
            try:
                obj_keys = list(resp.objects.keys())
            except Exception:
                # Map fields sometimes need explicit cast
                obj_keys = list(dict(resp.objects).keys())
            return json.dumps({
                "background": len(resp.background),
                "axis": len(resp.axis),
                "global": len(global_params),
                "objects": obj_keys,
            })
        if name == "scene_list_params":
            obj = args.get("scope_object")
            try:
                if obj is not None and int(obj) < 0:
                    obj = None
            except Exception:
                obj = None
            resp = client.list_params(
                scope_camera=bool(args.get("scope_camera")),
                scope_object=obj,
                scope_group=args.get("scope_group"),
            )
            params = []
            for p in resp.params:
                entry = {"json_key": p.json_key, "name": p.name, "type": p.type, "supports_interpolation": p.supports_interpolation}
                # Option enums if provided by server
                try:
                    if getattr(p, "option_names", None):
                        entry["option_names"] = list(p.option_names)
                    if getattr(p, "option_data", None):
                        entry["option_data"] = list(p.option_data)
                except Exception:
                    pass
                # Numeric metadata
                try:
                    # Scalars
                    if hasattr(p, "number_min") and hasattr(p, "number_max"):
                        # protobuf default is 0; include only if range looks meaningful or type is numeric
                        entry["number_min"] = float(p.number_min)
                        entry["number_max"] = float(p.number_max)
                    if hasattr(p, "number_step"):
                        entry["number_step"] = float(p.number_step)
                    if hasattr(p, "decimal"):
                        entry["decimal"] = int(p.decimal)
                    # Vectors
                    if getattr(p, "vector_min", None):
                        entry["vector_min"] = list(p.vector_min)
                    if getattr(p, "vector_max", None):
                        entry["vector_max"] = list(p.vector_max)
                    # Spans
                    if hasattr(p, "span_min") and hasattr(p, "span_max"):
                        entry["span_min"] = float(p.span_min)
                        entry["span_max"] = float(p.span_max)
                    if hasattr(p, "span_step"):
                        entry["span_step"] = float(p.span_step)
                except Exception:
                    pass
                params.append(entry)
            return json.dumps({"params": params})
        if name == "scene_validate_param_value":
            scope_object = args.get("scope_object")
            try:
                if scope_object is not None and int(scope_object) < 0:
                    scope_object = None
            except Exception:
                scope_object = None
            scope_group = args.get("scope_group")
            json_key = str(args.get("json_key"))
            value = args.get("value")
            pl = client.list_params(scope_object=scope_object, scope_group=scope_group)
            meta = None
            for p in pl.params:
                if p.json_key == json_key:
                    meta = p
                    break
            if meta is None:
                return json.dumps({"ok": False, "error": "json_key not found in scope"})
            t = (meta.type or "").lower()
            # Basic type checks
            def _is_number(x):
                return isinstance(x, (int, float)) and not isinstance(x, bool)
            ok = True
            err = ""
            if "stringintoption" in t or "stringstringoption" in t or "intintoption" in t:
                if not isinstance(value, str):
                    ok, err = False, "option value must be string"
                else:
                    try:
                        opts = list(meta.option_names)
                        if opts and value not in opts:
                            ok, err = False, f"value '{value}' not in option_names"
                    except Exception:
                        pass
            elif t.endswith("vec4"):
                ok = isinstance(value, list) and len(value) == 4 and all(_is_number(v) for v in value)
                if not ok:
                    err = "Vec4 requires [n,n,n,n]"
            elif t.endswith("vec3"):
                ok = isinstance(value, list) and len(value) == 3 and all(_is_number(v) for v in value)
                if not ok:
                    err = "Vec3 requires [n,n,n]"
            elif t in ("float", "double"):
                ok = _is_number(value)
                if not ok:
                    err = "number required"
            elif t == "int":
                ok = isinstance(value, int) and not isinstance(value, bool)
                if not ok:
                    err = "integer required"
            elif t == "bool":
                ok = isinstance(value, bool)
                if not ok:
                    err = "bool required"
            # else: unknown types pass (e.g., 3DTransform)
            return json.dumps({"ok": ok, "error": err})

        # Timeline / camera
        if name == "scene_ensure_animation":
            return json.dumps({"ok": client.ensure_animation()})
        if name == "scene_set_duration":
            seconds = float(args.get("seconds", 0.0))
            return json.dumps({"ok": client.set_duration(seconds)})
        if name == "scene_camera_fit":
            vals = client.camera_fit(ids=args.get("ids"), all=bool(args.get("all", False)), after_clipping=bool(args.get("after_clipping", False)), min_radius=float(args.get("min_radius", 0.0)))
            return json.dumps({"values": vals})
        if name == "scene_camera_orbit":
            vals = client.camera_orbit(ids=args.get("ids"), axis=str(args.get("axis", "y")), angle_degrees=float(args.get("angle_degrees", 360.0)))
            return json.dumps({"values": vals})
        if name == "scene_camera_dolly":
            vals = client.camera_dolly(ids=args.get("ids"), start_dist=float(args.get("start_dist", 0.0)), end_dist=float(args.get("end_dist", 0.0)))
            return json.dumps({"values": vals})
        if name == "scene_set_key_camera":
            try:
                ok = client.set_key_camera(time=float(args.get("time", 0.0)), easing=str(args.get("easing", "Linear")), value=args.get("value") or {})
                return json.dumps({"ok": ok})
            except Exception as e:
                # Capture gRPC error details when available
                msg = str(e)
                try:
                    # grpc.RpcError has .details()
                    msg = e.details()  # type: ignore[attr-defined]
                except Exception:
                    pass
                return json.dumps({"ok": False, "error": msg})
        if name == "scene_set_key_param":
            # Expect native JSON value; coerce common mistakes (e.g., "true" -> true for Bool types).
            scope_object = args.get("scope_object")
            scope_group = args.get("scope_group")
            json_key = str(args.get("json_key"))
            time_v = float(args.get("time", 0.0))
            easing = str(args.get("easing", "Linear"))
            value_native = args.get("value")
            # Look up param meta to coerce types sensibly
            try:
                pl = client.list_params(scope_object=scope_object, scope_group=scope_group)
                meta = None
                for p in pl.params:
                    if p.json_key == json_key:
                        meta = p
                        break
            except Exception:
                meta = None
            # Coerce booleans if needed
            if meta is not None:
                t = (getattr(meta, "type", "") or "").lower()
                if "bool" in t and isinstance(value_native, str):
                    s = value_native.strip().lower()
                    if s in ("true", "1", "yes", "on"): value_native = True
                    elif s in ("false", "0", "no", "off"): value_native = False
                # Normalize numeric vectors by length if Vec3/Vec4
                if t.endswith("vec4") and isinstance(value_native, list) and len(value_native) == 4:
                    value_native = [float(v) for v in value_native]
                if t.endswith("vec3") and isinstance(value_native, list) and len(value_native) == 3:
                    value_native = [float(v) for v in value_native]
            # Serialize
            v_str = json.dumps(value_native)
            try:
                ok = client.set_key_param(
                    scope_object=scope_object,
                    scope_group=scope_group,
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
        if name == "scene_clear_keys":
            ok = client.clear_keys(
                scope_camera=bool(args.get("scope_camera", False)),
                scope_object=args.get("scope_object"),
                scope_group=args.get("scope_group"),
                json_key=args.get("json_key"),
            )
            return json.dumps({"ok": ok})
        if name == "scene_remove_key":
            ok = client.remove_key(
                scope_object=args.get("scope_object"),
                scope_group=args.get("scope_group"),
                json_key=str(args.get("json_key")),
                time=float(args.get("time", 0.0)),
            )
            return json.dumps({"ok": ok})
        if name == "scene_batch":
            set_keys = args.get("set_keys") or []
            remove_keys = args.get("remove_keys") or []
            if not set_keys and not remove_keys:
                return json.dumps({"ok": False, "error": "scene_batch called with empty set/remove. Build concrete SetKey entries or use scene_emphasize_object/scene_set_key_param/scene_set_key_camera first."})
            ok = client.batch(set_keys=set_keys, remove_keys=remove_keys, commit=bool(args.get("commit", True)))
            return json.dumps({"ok": ok})

        # Visibility / cuts
        if name == "scene_set_visibility":
            ids = args.get("ids") or []
            on = bool(args.get("on", True))
            ok = client.set_visibility(ids, on)
            return json.dumps({"ok": ok})
        if name == "scene_cut_suggest_box":
            req = client._pb2.CutSuggestRequest(ids=args.get("ids") or [], margin=float(args.get("margin", 0.0)), after_clipping=bool(args.get("after_clipping", False)))
            resp = client._stub.CutSuggest(req)
            box = resp.box
            return json.dumps({"min": [box.min.x, box.min.y, box.min.z], "max": [box.max.x, box.max.y, box.max.z]})
        if name == "scene_cut_set_box":
            minv = args.get("min") or [0, 0, 0]
            maxv = args.get("max") or [0, 0, 0]
            ok = client.cut_set_box((minv[0], minv[1], minv[2]), (maxv[0], maxv[1], maxv[2]), refit_camera=bool(args.get("refit_camera", True)))
            return json.dumps({"ok": ok})
        if name == "scene_cut_clear":
            return json.dumps({"ok": client.cut_clear()})

        # Playback / save
        if name == "scene_set_time":
            ok = client.set_time(float(args.get("seconds", 0.0)), cancel_rendering=bool(args.get("cancel", False)))
            return json.dumps({"ok": ok})
        if name == "scene_play":
            ok = client.play(float(args.get("fps", 25.0)))
            return json.dumps({"ok": ok})
        if name == "scene_pause":
            return json.dumps({"ok": client.pause()})
        if name == "scene_save_animation":
            from pathlib import Path as _P
            return json.dumps({"ok": client.save_animation(_P(args.get("path")))})

        if name == "atlas_export_video":
            # Export .animation3d to MP4 by invoking headless Atlas
            from pathlib import Path as _P
            from ..discovery import compute_paths_from_atlas_dir, default_install_dirs
            from ..exporter import export_video as _export_video
            anim = args.get("animation")
            out = args.get("out")
            if not anim or not out:
                return json.dumps({"ok": False, "error": "animation and out are required"})
            # Resolve Atlas binary
            atlas_bin = None
            if atlas_dir:
                try:
                    ab, _ = compute_paths_from_atlas_dir(_P(atlas_dir))
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
                return json.dumps({"ok": False, "error": "Atlas binary not found; set atlas_dir"})
            rc = _export_video(
                atlas_bin=str(atlas_bin),
                animation_path=_P(anim),
                output_video=_P(out),
                fps=int(args.get("fps", 30)),
                start=int(args.get("start", 0)),
                end=int(args.get("end", -1)),
                width=int(args.get("width", 1920)),
                height=int(args.get("height", 1080)),
                overwrite=bool(args.get("overwrite", True)),
                use_gpu_devices=args.get("use_gpu_devices"),
            )
            return json.dumps({"ok": rc == 0, "exit_code": rc})

        if name == "scene_render_preview":
            # Privacy/consent gate: disabled unless explicitly allowed by env
            import os as _os
            allow = (_os.environ.get("ATLAS_AGENT_ALLOW_SCREENSHOTS", "").strip().lower() in ("1", "true", "yes"))
            if not allow:
                return json.dumps({"ok": False, "error": "screenshots disabled; set ATLAS_AGENT_ALLOW_SCREENSHOTS=1 to enable"})
            from pathlib import Path as _P
            import tempfile as _tmp
            import glob as _glob
            from ..discovery import compute_paths_from_atlas_dir, default_install_dirs
            from ..exporter import preview_frames as _preview_frames
            fps = int(float(args.get("fps", 30)))
            tsec = float(args.get("time", 0.0))
            width = int(args.get("width", 512))
            height = int(args.get("height", 512))
            frame_idx = int(round(tsec * max(1, fps)))
            # Resolve Atlas binary
            atlas_bin = None
            if atlas_dir:
                try:
                    ab, _ = compute_paths_from_atlas_dir(_P(atlas_dir))
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
                return json.dumps({"ok": False, "error": "Atlas binary not found; set atlas_dir or install Atlas"})
            # Save animation to a temp file and render a single frame
            tdir = _P(_tmp.mkdtemp(prefix="atlas_preview_"))
            anim_path = tdir / "preview.animation3d"
            ok_save = client.save_animation(anim_path)
            if not ok_save:
                return json.dumps({"ok": False, "error": "failed to save temporary animation"})
            frames_dir = tdir / "frames"
            frames_dir.mkdir(parents=True, exist_ok=True)
            rc = _preview_frames(
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
                return json.dumps({"ok": False, "exit_code": rc, "error": "preview renderer failed"})
            # Find the produced image (exact naming depends on exporter; pick any image in frames_dir)
            images = []
            for ext in ("*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tif", "*.tiff"):
                images.extend(sorted(_glob.glob(str(frames_dir / ext))))
            if not images:
                return json.dumps({"ok": False, "error": "no image produced"})
            return json.dumps({"ok": True, "path": images[0]})

        if name == "scene_emphasize_object":
            oid = int(args.get("id"))
            time = float(args.get("time", 0.0))
            easing = str(args.get("easing", "Linear"))
            want_mode = args.get("mode")
            want_color = args.get("color")
            want_opacity = args.get("opacity")
            dim_others = bool(args.get("dim_others", False))

            # Resolve object type
            objs = client.list_objects()
            obj = None
            others = []
            for o in objs.objects:
                if int(o.id) == oid:
                    obj = o
                else:
                    others.append(o)
            if obj is None:
                return json.dumps({"error": f"object {oid} not found"})

            # Helper to find param json_key by name and optional type
            def find_key(scope_object: int, name: str, type_hint: str | None = None) -> str | None:
                pl = client.list_params(scope_object=scope_object)
                for p in pl.params:
                    if p.name == name and (type_hint is None or p.type == type_hint):
                        return p.json_key
                # Try fallback by matching prefix
                for p in pl.params:
                    if p.name.lower().startswith(name.lower()):
                        return p.json_key
                return None

            set_keys: list[dict] = []

            # Mode
            if want_mode and obj.type == "Swc":
                jk = find_key(oid, "Rendering Mode", "StringIntOption")
                if jk:
                    set_keys.append({
                        "scope": {"object": oid},
                        "json_key": jk,
                        "time": time,
                        "easing": easing,
                        "value": str(want_mode),
                    })

            # Color mapping
            def to_rgba(v) -> list[float]:
                if isinstance(v, str):
                    s = v.strip().lower()
                    named = {
                        "red": (1.0, 0.0, 0.0, 1.0),
                        "green": (0.0, 1.0, 0.0, 1.0),
                        "blue": (0.0, 0.0, 1.0, 1.0),
                        "white": (1.0, 1.0, 1.0, 1.0),
                        "black": (0.0, 0.0, 0.0, 1.0),
                        "yellow": (1.0, 1.0, 0.0, 1.0),
                        "magenta": (1.0, 0.0, 1.0, 1.0),
                        "cyan": (0.0, 1.0, 1.0, 1.0),
                    }
                    if s in named:
                        return list(named[s])
                    if s.startswith("#"):
                        h = s[1:]
                        if len(h) == 6:
                            r = int(h[0:2], 16) / 255.0
                            g = int(h[2:4], 16) / 255.0
                            b = int(h[4:6], 16) / 255.0
                            a = 1.0
                            return [r, g, b, a]
                        if len(h) == 8:
                            r = int(h[0:2], 16) / 255.0
                            g = int(h[2:4], 16) / 255.0
                            b = int(h[4:6], 16) / 255.0
                            a = int(h[6:8], 16) / 255.0
                            return [r, g, b, a]
                if isinstance(v, list) and len(v) >= 3:
                    if len(v) == 3:
                        return [float(v[0]), float(v[1]), float(v[2]), 1.0]
                    return [float(v[0]), float(v[1]), float(v[2]), float(v[3])]
                # default red
                return [1.0, 0.0, 0.0, 1.0]

            if want_color is not None:
                rgba = to_rgba(want_color)
                if obj.type == "Swc":
                    jk_mode = find_key(oid, "Color Mode", "StringIntOption")
                    jk_col = find_key(oid, "Color", "Vec4")
                    if jk_mode and jk_col:
                        set_keys.append({
                            "scope": {"object": oid},
                            "json_key": jk_mode,
                            "time": time,
                            "easing": easing,
                            "value": "Single Color",
                        })
                        set_keys.append({
                            "scope": {"object": oid},
                            "json_key": jk_col,
                            "time": time,
                            "easing": easing,
                            "value": rgba,
                        })
                elif obj.type == "Mesh":
                    jk_mode = find_key(oid, "Color Mode", "StringIntOption")
                    jk_col = find_key(oid, "Mesh Color", "Vec4")
                    if jk_mode and jk_col:
                        set_keys.append({
                            "scope": {"object": oid},
                            "json_key": jk_mode,
                            "time": time,
                            "easing": easing,
                            "value": "Single Color",
                        })
                        set_keys.append({
                            "scope": {"object": oid},
                            "json_key": jk_col,
                            "time": time,
                            "easing": easing,
                            "value": rgba,
                        })

            if want_opacity is not None:
                jk_op = find_key(oid, "Opacity", "Float")
                if jk_op:
                    set_keys.append({
                        "scope": {"object": oid},
                        "json_key": jk_op,
                        "time": time,
                        "easing": easing,
                        "value": float(want_opacity),
                    })

            # Dim others by lowering opacity
            if dim_others and others:
                for o in others:
                    jk_op2 = find_key(int(o.id), "Opacity", "Float")
                    if jk_op2:
                        set_keys.append({
                            "scope": {"object": int(o.id)},
                            "json_key": jk_op2,
                            "time": time,
                            "easing": easing,
                        "value": 0.2,
                        })

            if not set_keys:
                return json.dumps({"ok": False, "note": "no changes"})
            ok = client.batch(set_keys=set_keys, remove_keys=[], commit=True)
            return json.dumps({"ok": ok, "changed": len(set_keys)})

        return json.dumps({"error": f"unknown tool: {name}"})

    return tools, dispatch
