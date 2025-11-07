from __future__ import annotations

"""LLM Agent Tooling: tool specs + dispatcher for function-calling.

This module contains the curated tool list and dispatcher used by the
multi-agent system. It is the stable entry point for LLM function-calling.
"""

import json
from typing import Any, Dict, List, Tuple
from pathlib import Path

from ..discovery import discover_schema_dir

from ..scene_rpc import SceneClient
from ..capabilities_prompt import build_capabilities_prompt
from ..codegen_policy import is_codegen_enabled


def scene_tools_and_dispatcher(client: SceneClient, *, atlas_dir: str | None = None) -> Tuple[List[Dict[str, Any]], callable]:
    """Return (tool_specs, dispatcher) for OpenAI tool-calling.

    The dispatcher signature: (name: str, args_json: str) -> str
    Returns a compact JSON string result that the model can parse.
    """

    tools: List[Dict[str, Any]] = [
        {
            "type": "function",
            "function": {
                "name": "report_blocked",
                "description": "Implementer-only: declare that execution is blocked or not feasible. Use precise reason and details so Supervisor can inform the user.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "reason": {"type": "string", "description": "Short reason (e.g., json_key_not_found, option_invalid, tool_missing)"},
                        "details": {"type": "string", "description": "Specifics: id/json_key/value/time or missing option/label names"},
                        "suggestion": {"type": "string", "description": "Optional next step suggestion for the user"}
                    },
                    "required": ["reason"]
                },
            },
        },
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
                "description": "Return the full capabilities.json (parameter catalogs per object type and groups).",
                "parameters": {"type": "object", "properties": {}},
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
                "name": "animation_describe_file",
                "description": "Parse an .animation3d file and return a concise natural-language summary (uses capabilities.json for names).",
                "parameters": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
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
                        "id": {"type": "integer", "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "json_keys": {"type": "array", "items": {"type": "string"}, "description": "Param keys to read (empty = all)"}
                    },
                    "required": ["id", "json_keys"]
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_validate_apply",
                "description": "Scene (stateless): validate display parameter assignments (no time/easing). Note: timeline keys (if present) override these values during playback.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "set_params": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "id": {"type": "integer", "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                                    "json_key": {"type": "string"},
                                    "value": {"description": "Typed value. For 3DTransform, use an object with canonical subfields (e.g., {\"Translation Vec3\":[x,y,z],\"Rotation Vec4\":[ang,x,y,z],\"Scale Vec3\":[sx,sy,sz],\"Rotation Center Vec3\":[cx,cy,cz]}).",
                                               "type": ["object", "array", "number", "string", "boolean", "null"],
                                               "items": {"type": ["string", "number", "boolean", "null"]}}
                                },
                                "required": ["id", "json_key", "value"],
                                
                            }
                        }
                    },
                    "required": ["set_params"],
                    
                },
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
                                    "id": {"type": "integer", "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                                    "json_key": {"type": "string", "description": "Canonical parameter key (preferred)"},
                                    "name": {"type": "string", "description": "Display name; dispatcher resolves to json_key if provided"},
                                    "value": {"description": "Typed value. For 3DTransform, use an object with canonical subfields (e.g., {\"Translation Vec3\":[x,y,z],\"Rotation Vec4\":[ang,x,y,z],\"Scale Vec3\":[sx,sy,sz],\"Rotation Center Vec3\":[cx,cy,cz]}).",
                                               "type": ["object", "array", "number", "string", "boolean", "null"],
                                               "items": {"type": ["string", "number", "boolean", "null"]}}
                                },
                                "required": ["id", "value"],
                                
                            }
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
                "parameters": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
            },
        },
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
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "List of target object ids to focus"},
                        "after_clipping": {"type": "boolean", "default": True, "description": "Use clipped bbox (true) or full bbox (false)"},
                        "min_radius": {"type": "number", "default": 0.0, "description": "Minimum radius to avoid degenerate views"}
                    },
                    "required": ["ids", "after_clipping", "min_radius"]
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
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "List of target object ids to point to"},
                        "after_clipping": {"type": "boolean", "default": True, "description": "Use clipped bbox (true) or full bbox (false)"}
                    },
                    "required": ["ids", "after_clipping"]
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
                        "op": {"type": "string", "enum": ["AZIMUTH", "ELEVATION", "ROLL", "YAW", "PITCH", "FLIP"]},
                        "degrees": {"type": "number", "default": 90.0}
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
                        "mode": {"type": "string", "enum": ["XY", "XZ", "YZ", "RESET"], "default": "RESET", "description": "Preset view"},
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "Candidate ids for sizing (optional for RESET)"},
                        "after_clipping": {"type": "boolean", "default": True, "description": "Use clipped bbox (true) or full bbox (false)"},
                        "min_radius": {"type": "number", "default": 0.0, "description": "Minimum radius to avoid degenerate views"}
                    },
                    "required": ["mode", "ids", "after_clipping", "min_radius"]
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
                        "mode": {"type": "string", "enum": ["FIT", "ORBIT", "DOLLY", "STATIC"], "description": "Solve mode for generating camera keys."},
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "Target ids; empty uses fit_candidates()."},
                        "t0": {"type": "number", "description": "Start time (seconds) of the write window."},
                        "t1": {"type": "number", "description": "End time (seconds) of the write window."},
                        "constraints": {"type": "object", "description": "Visibility/coverage constraints (keep_visible, margin, min_coverage, fov policy)."},
                        "params": {"type": "object", "description": "Mode-specific parameters (e.g., axis for ORBIT)."},
                        "degrees": {"type": "number", "description": "ORBIT: total rotation in degrees (default 360)."},
                        "tolerance": {"type": "number", "default": 1e-3, "description": "Time tolerance used when clearing/replacing keys."},
                        "easing": {"type": "string", "default": "Linear", "description": "Easing to assign to written keys."},
                        "clear_range": {"type": "boolean", "default": True, "description": "Remove existing camera keys inside [t0,t1] (within tolerance) before applying new keys."}
                    },
                    "required": ["mode", "ids", "t0", "t1"]
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "animation_camera_validate",
                "description": "Animation timeline: validate typed camera values against visibility/coverage constraints.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "Target ids to validate against (typically fit_candidates)."},
                        "times": {"type": "array", "items": {"type": "number"}, "description": "Times (seconds) for each camera key."},
                        "values": {"type": "array", "items": {"type": "object"}, "description": "Typed camera values aligned with times."},
                        "constraints": {"type": "object", "description": "Visibility/coverage constraints (keep_visible, margin, min_coverage, etc.)."},
                        "policies": {"type": "object", "description": "Adjustment policies (adjust_fov, adjust_distance, adjust_clipping)."}
                    },
                    "required": ["ids", "times", "values"]
                },
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
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "Optional target ids; when omitted uses fit_candidates()."},
                        "after_clipping": {"type": "boolean", "default": True, "description": "Use clipped bbox (true) or full bbox (false)."},
                        "min_radius": {"type": "number", "default": 0.0, "description": "Minimum radius (world units) for the fit sphere; 0 disables."}
                    }
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
                        "value": {"type": "object", "description": "Typed camera value (object with camera fields)."}
                    },
                    "required": ["value"]
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
                        "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "name": {"type": "string"},
                        "type_hint": {"type": "string"},
                        "time": {"type": "number"},
                        "easing": {"type": "string", "default": "Linear"},
                        "value": {"description": "Native JSON value. For composite params like 3DTransform, pass an object with canonical subfields (e.g., {\"Translation Vec3\":[x,y,z],\"Rotation Vec4\":[ang,x,y,z],\"Scale Vec3\":[sx,sy,sz],\"Rotation Center Vec3\":[cx,cy,cz]}).",
                                   "type": ["object", "array", "number", "string", "boolean", "null"],
                                   "items": {"type": ["string", "number", "boolean", "null"]}},
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
                        "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "json_key": {"type": "string"},
                        "time": {"type": "number"},
                        "tolerance": {"type": "number", "default": 1e-3}
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
                        "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "json_key": {"type": "string"},
                        "time": {"type": "number"},
                        "easing": {"type": "string", "default": "Linear"},
                        "value": {"description": "Native JSON value (bool/number/string/array)",
                                   "type": ["string", "number", "boolean", "null", "array"],
                                   "items": {"type": ["string", "number", "boolean", "null"]}},
                        "tolerance": {"type": "number", "default": 1e-3},
                        "strict": {"type": "boolean", "default": False}
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
                        "strict": {"type": "boolean", "default": False}
                    },
                    "required": ["time", "value"],
                    
                }
            }
        },
        {
            "type": "function",
            "function": {
                "name": "system_info",
                "description": "Return OS/platform info and common paths so the agent can reason about file locations.",
                "parameters": {"type": "object", "properties": {}},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_expand_paths",
                "description": "Expand ~ and env vars and normalize paths. Returns expanded absolute-like paths per entry (relative kept relative).",
                "parameters": {"type": "object", "properties": {"paths": {"type": "array", "items": {"type": "string"}, "description": "Input paths (may contain ~ or env vars)"}}, "required": ["paths"]},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_check_paths",
                "description": "Check which of the given paths exist on the local filesystem. Returns {exists:[], missing:[]}.",
                "parameters": {"type": "object", "properties": {"paths": {"type": "array", "items": {"type": "string"}, "description": "Paths to check"}}, "required": ["paths"]},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_read_text",
                "description": "Read a UTF‑8 text file from disk. Returns {ok,text,bytes,encoding}. Pass max_bytes<=0 to read full file (no cap).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": {"type": "string", "description": "Path to a text file (supports ~ and env var expansion)"},
                        "max_bytes": {"type": "integer", "default": 268435456, "description": "Max bytes to read; <=0 reads full file. Default 256 MiB."}
                    },
                    "required": ["path"],
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_read_json",
                "description": "Read and parse a JSON file from disk. Returns {ok,data}. Pass max_bytes<=0 to read full file (no cap).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": {"type": "string", "description": "Path to a JSON file (supports ~ and env var expansion)"},
                        "max_bytes": {"type": "integer", "default": 268435456, "description": "Max bytes to read; <=0 reads full file. Default 256 MiB."}
                    },
                    "required": ["path"],
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_resolve_path",
                "description": "Resolve a possibly-typoed file/dir path using heuristics (expand ~, case-insensitive, pluralization, prefix match, repo search). Returns {ok,path?,candidates?,tried?}.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": {"type": "string", "description": "Possibly-typoed path to resolve"},
                        "kind": {"type": "string", "enum": ["file", "dir", "either"], "default": "either", "description": "Expected kind"},
                        "base_dirs": {"type": "array", "items": {"type": "string"}, "description": "Search bases"},
                        "max_candidates": {"type": "integer", "default": 10, "description": "Max results"}
                    },
                    "required": ["path"]
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "repo_search",
                "description": "Search the repo for a file/dir name (basename fuzzy scoring). Returns likely matches with scores.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "name": {"type": "string", "description": "Basename to search for"},
                        "type": {"type": "string", "enum": ["file", "dir", "either"], "default": "either", "description": "Kind to search"},
                        "max_depth": {"type": "integer", "default": 6, "description": "Max directory depth"},
                        "max_results": {"type": "integer", "default": 20, "description": "Max returned results"}
                    },
                    "required": ["name"]
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "fs_glob",
                "description": "List files matching a pattern inside a directory. Expands ~ and env vars. Example: dir='~/Documents/atlas_test/slice15', pattern='*.lsm'",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "dir": {"type": "string", "description": "Directory to glob"},
                        "pattern": {"type": "string", "default": "*", "description": "Glob pattern"},
                        "recursive": {"type": "boolean", "default": False, "description": "Recurse into subdirs"}
                    },
                    "required": ["dir"]
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "python_write_and_run",
                "description": "Write a Python script (string) to a temp file and run it with the repo root on PYTHONPATH. Returns stdout/stderr/exit_code, and optionally echoes the script.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "script": {"type": "string", "description": "Python source code"},
                        "filename": {"type": "string", "description": "Optional filename for the script (for logging)"},
                        "args": {"type": "array", "items": {"type": "string"}, "default": [], "description": "argv to pass to the script"},
                        "timeout_sec": {"type": "number", "default": 120.0, "description": "Execution timeout"},
                        "echo_script": {"type": "boolean", "default": True, "description": "Include script echo in response"},
                        "max_echo_chars": {"type": "integer", "default": 4000, "description": "Max script chars to echo"}
                    },
                    "required": ["script"]
                }
            }
        },
        {
            "type": "function",
            "function": {
                "name": "scene_load_files",
                "description": "Load one or more files into the GUI scene. Accepts absolute paths; ~ and env vars expanded. Prefer scene_ensure_loaded for idempotent behavior.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "files": {"type": "array", "items": {"type": "string"}, "description": "Absolute paths to load"},
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
                        "files": {"type": "array", "items": {"type": "string"}, "description": "Absolute paths to load (idempotent)"},
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
                        "names": {"type": "array", "items": {"type": "string"}, "description": "Basenames to search"},
                        "dir_hints": {"type": "array", "items": {"type": "string"}, "description": "Hint directories"},
                        "extensions": {"type": "array", "items": {"type": "string"}, "default": [".msh", ".obj", ".ply", ".swc", ".nii", ".tif", ".tiff"], "description": "Allowed extensions"},
                        "case_insensitive": {"type": "boolean", "default": True, "description": "Case-insensitive matching"}
                    },
                    "required": ["names"],
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
                        "dirs": {"type": "array", "items": {"type": "string"}, "description": "Search directories"},
                        "names": {"type": "array", "items": {"type": "string"}, "description": "Basenames to resolve"},
                        "extensions": {"type": "array", "items": {"type": "string"}, "default": [".msh", ".obj", ".ply", ".swc", ".nii", ".tif", ".tiff"], "description": "Allowed extensions"},
                        "case_insensitive": {"type": "boolean", "default": True, "description": "Case-insensitive matching"}
                    },
                    "required": ["dirs", "names"],
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_list_objects",
                "description": "List all objects in the current scene (id, type, name, visible).",
                "parameters": {"type": "object", "properties": {}},
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
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "Object ids (empty = all)"},
                        "after_clipping": {"type": "boolean", "default": False, "description": "Use clipped bbox (true) or full bbox (false)"}
                    },
                    "required": ["ids", "after_clipping"]
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
                        "id": {"type": "integer", "description": "Timeline target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "json_key": {"type": "string", "description": "Parameter json_key (ignored for camera); use canonical names from scene_list_params(id)"},
                        "include_values": {"type": "boolean", "description": "True to include value_json samples for each key"}
                    },
                    "required": ["id", "json_key", "include_values"]
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
                "name": "scene_capabilities",
                "description": "List parameter capabilities for background/axis/global and object types.",
                "parameters": {"type": "object", "properties": {}},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_list_params",
                "description": "List parameters by id. Id map: 0=camera, 1=background, 2=axis, 3=global, ≥4=objects.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "integer", "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"}
                    },
                    "required": ["id"]
                }
            },
        },
        {
            "type": "function",
            "function": {
                "name": "scene_validate_param_value",
                "description": "Validate a candidate value against the live parameter metadata (type and option_names) for a given id (0=camera, 1=background, 2=axis, 3=global, ≥4=objects).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "integer", "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "json_key": {"type": "string"},
                        "value": {"description": "Candidate JSON value (native types)",
                                   "type": ["string", "number", "boolean", "null", "array"],
                                   "items": {"type": ["string", "number", "boolean", "null"]}},
                    },
                    "required": ["id", "json_key", "value"],
                },
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
                "parameters": {"type": "object", "properties": {"seconds": {"type": "number"}}},
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
                        "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "json_key": {"type": "string", "description": "Canonical json_key; if omitted, 'name' is used to resolve."},
                        "name": {"type": "string", "description": "Display name to resolve to json_key when json_key is not provided."},
                        "time": {"type": "number"},
                        "easing": {"type": "string", "default": "Linear"},
                        "value": {"description": "Native JSON value (bool/number/string/array)",
                                   "type": ["string", "number", "boolean", "null", "array"],
                                   "items": {"type": ["string", "number", "boolean", "null"]}},
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
                        "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "json_key": {"type": "string"},
                        "times": {"type": "array", "items": {"type": "number"}},
                        "value": {"description": "Native JSON value",
                                   "type": ["string", "number", "boolean", "null", "array"],
                                   "items": {"type": ["string", "number", "boolean", "null"]}},
                        "easing": {"type": "string", "default": "Linear"},
                        "tolerance": {"type": "number", "default": 1e-3}
                    },
                    "required": ["id", "json_key", "times", "value"]
                }
            }
        },
        {
            "type": "function",
            "function": {
                "name": "animation_clear_keys",
                "description": "Clear all keys for a parameter or camera by id (0=camera).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "integer", "description": "Target id: 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids"},
                        "json_key": {"type": "string"},
                    },
                    "required": ["id"]
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
                        "id": {"type": "integer", "description": "Target id: 1=background, 2=axis, 3=global, ≥4=object ids"},
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
                        "set_keys": {"type": "array", "items": {"type": "object"}, "description": "List of SetKey operations"},
                        "remove_keys": {"type": "array", "items": {"type": "object"}, "description": "List of RemoveKey operations"},
                        "commit": {"type": "boolean", "default": True, "description": "Commit immediately if true"}
                    },
                    "required": ["set_keys", "remove_keys", "commit"]
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
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "Object ids"},
                        "on": {"type": "boolean", "description": "True to show, false to hide"}
                    },
                    "required": ["ids", "on"]
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
                        "ids": {"type": "array", "items": {"type": "integer"}, "description": "Ids to bound (empty = all)"},
                        "margin": {"type": "number", "default": 0.0, "description": "Extra normalized margin to expand box"},
                        "after_clipping": {"type": "boolean", "default": False, "description": "Use clipped bbox for computation"}
                    },
                    "required": ["ids", "margin", "after_clipping"]
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
                        "min": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3, "description": "Box min [x,y,z]"},
                        "max": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3, "description": "Box max [x,y,z]"},
                        "refit_camera": {"type": "boolean", "default": True, "description": "Refit camera after applying cut"}
                    },
                    "required": ["min", "max", "refit_camera"]
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
        {
            "type": "function",
            "function": {
                "name": "animation_set_time",
                "description": "Set current timeline time (seconds).",
                "parameters": {"type": "object", "properties": {"seconds": {"type": "number", "description": "Timeline seconds"}}, "required": ["seconds"]},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "animation_save_animation",
                "description": "Save the current animation to a .animation3d path.",
                "parameters": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
            },
        },
        {
            "type": "function",
            "function": {
                "name": "atlas_export_video",
                "description": "Export an .animation3d to MP4 using the Atlas headless exporter.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "animation": {"type": "string", "description": "Path to .animation3d file"},
                        "out": {"type": "string", "description": "Output .mp4 path"},
                        "fps": {"type": "number", "description": "Frames per second"},
                        "start": {"type": "integer", "description": "Start frame (inclusive)"},
                        "end": {"type": "integer", "description": "End frame (inclusive, -1 = duration)"},
                        "width": {"type": "integer", "description": "Output width"},
                        "height": {"type": "integer", "description": "Output height"},
                        "overwrite": {"type": "boolean", "description": "Overwrite output if exists"},
                        "use_gpu_devices": {"type": "string", "description": "Linux only: comma-separated GPU ids, e.g. '0,1'"}
                    },
                    "required": ["animation", "out", "fps", "start", "end", "width", "height", "overwrite", "use_gpu_devices"]
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
                        "time": {"type": "number", "description": "Preview time in seconds"},
                        "fps": {"type": "number", "description": "Frames per second"},
                        "width": {"type": "integer", "description": "Image width"},
                        "height": {"type": "integer", "description": "Image height"}
                    },
                    "required": ["time", "fps", "width", "height"]
                },
            },
        },
    ]

    # Conditionally expose codegen-related tools
    if is_codegen_enabled():
        tools.append({
            "type": "function",
            "function": {
                "name": "codegen_allowed_imports",
                "description": "Return the current codegen allowed import modules and whether each is importable in this environment.",
                "parameters": {"type": "object", "properties": {}},
            },
        })
    else:
        # When disabled, ensure any lingering codegen tools are removed (defensive if list changes elsewhere)
        tools = [
            t for t in tools
            if not (
                isinstance(t, dict)
                and isinstance(t.get("function"), dict)
                and t["function"].get("name") in {"python_write_and_run", "codegen_allowed_imports"}
            )
        ]

    # Per-dispatcher caches (persist during the Implementer run)
    _param_catalog_cache: dict[tuple, list] = {}
    _alias_cache: dict[tuple, dict[str, str]] = {}

    def _list_params_cached(id: int):
        id = int(id)
        key = ("id", id)
        if key in _param_catalog_cache:
            return _param_catalog_cache[key]
        pl = client.list_params(id=id)
        params = list(getattr(pl, "params", []))
        _param_catalog_cache[key] = params
        return params

    def _build_alias_map(params) -> dict[str, str]:
        alias: dict[str, str] = {}
        def norm(s: str) -> str:
            return (s or "").strip().lower()
        for p in params:
            jk = getattr(p, "json_key", "") or ""
            nm = getattr(p, "name", "") or ""
            ty = getattr(p, "type", "") or ""
            if jk:
                alias[norm(jk)] = jk
            if nm:
                alias[norm(nm)] = jk
            # If json_key is name + " " + type, expose the prefix as an alias as well
            try:
                if jk and ty and jk.endswith(" " + ty):
                    alias[norm(jk[: -(len(ty) + 1)])] = jk
            except Exception:
                pass
        return alias

    def _resolve_json_key(id: int, candidate: str | None = None, name: str | None = None) -> str | None:
        """Resolve to a canonical json_key using live params. Accepts either a candidate key or a display name.
        Returns canonical json_key or None.
        """
        if (candidate is None or str(candidate).strip() == "") and (name is None or str(name).strip() == ""):
            return None
        cand = (str(candidate) if candidate is not None else str(name))
        if not cand:
            return None
        cand_norm = cand.strip().lower()
        # Cache alias map per id
        key = ("id", int(id))
        if key not in _alias_cache:
            params = _list_params_cached(id=int(id))
            _alias_cache[key] = _build_alias_map(params)
        amap = _alias_cache.get(key, {})
        # Direct match (canonical)
        if cand_norm in amap:
            return amap[cand_norm]
        # Try to refresh aliases (avoid staleness)
        try:
            params = _list_params_cached(id=int(id))
            _alias_cache[key] = _build_alias_map(params)
            amap = _alias_cache.get(key, {})
        except Exception:
            pass
        if cand_norm in amap:
            return amap[cand_norm]
        # Fuzzy: prefix match on names and keys
        try:
            import difflib
            choices = list(amap.keys())
            # Try best close matches
            for m in difflib.get_close_matches(cand_norm, choices, n=1, cutoff=0.85):
                return amap[m]
            # Try relaxed prefix/contains
            for k in choices:
                if cand_norm in k or k in cand_norm:
                    return amap[k]
        except Exception:
            pass
        return None

    def dispatch(name: str, args_json: str) -> str:
        # Helpers
        def _param_to_dict(p) -> dict:
            """Format a Parameter proto to a JSON-serializable dict using only proto-defined fields.
            Includes optional enums and numeric/vector/span metadata when present.
            """
            entry = {
                "json_key": getattr(p, "json_key", ""),
                "name": getattr(p, "name", ""),
                "type": getattr(p, "type", ""),
                "supports_interpolation": getattr(p, "supports_interpolation", False),
            }
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
                if hasattr(p, "number_min") and hasattr(p, "number_max"):
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
            return entry

        def _json_key_exists(id: int, json_key: str) -> bool:
            try:
                pl = client.list_params(id=int(id))
                for p in pl.params:
                    if getattr(p, "json_key", None) == json_key:
                        return True
            except Exception:
                return False
            return False
        try:
            args = json.loads(args_json or "{}")
        except Exception:
            args = {}

        # Implementer-only: explicit blocked reporting so Supervisor can present exact reason
        if name == "report_blocked":
            out = {
                "ok": True,
                "reason": str(args.get("reason", "")),
                "details": str(args.get("details", "")),
                "suggestion": str(args.get("suggestion", "")),
            }
            return json.dumps(out)

        # Guard codegen execution pathway behind a feature flag
        if name == "python_write_and_run" and not is_codegen_enabled():
            return json.dumps({"ok": False, "error": "codegen disabled (enable with ATLAS_AGENT_ENABLE_CODEGEN=1)"})

        # System / FS helpers for LLM-driven resolution
        if name == "scene_animation_concepts":
            info = (
                "Scene (.scene): current objects and their display parameters across 2D/3D. Saving a scene lets you restore the view/state.\n"
                "Animation (.animation2d/.animation3d): extends the scene with a timeline. Each display parameter (and camera) has keys at specific times, with easing (e.g., Switch/Linear/Exp).\n"
                "Playback rule: When the timeline plays, animation keys override scene values for the affected parameters. To change what plays, write/replace keys.\n"
                "Scene tools (scene_*) are stateless (no time/easing); Animation tools (animation_*) manipulate keys on the timeline."
            )
            return json.dumps({"ok": True, "text": info})
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
        if name == "fs_read_text":
            import os as _os
            p = str(args.get("path") or "")
            maxb = int(args.get("max_bytes", 268435456))
            try:
                q = _os.path.expanduser(_os.path.expandvars(p))
                with open(q, "rb") as f:
                    if maxb is not None and maxb > 0:
                        data = f.read(maxb)
                    else:
                        data = f.read()
                text = data.decode("utf-8", errors="replace")
                return json.dumps({"ok": True, "text": text, "bytes": len(data), "encoding": "utf-8"})
            except Exception as e:
                return json.dumps({"ok": False, "error": str(e)})
        if name == "fs_read_json":
            import os as _os, json as _json
            p = str(args.get("path") or "")
            maxb = int(args.get("max_bytes", 268435456))
            try:
                q = _os.path.expanduser(_os.path.expandvars(p))
                with open(q, "rb") as f:
                    if maxb is not None and maxb > 0:
                        data = f.read(maxb)
                    else:
                        data = f.read()
                obj = _json.loads(data.decode("utf-8", errors="strict"))
                return json.dumps({"ok": True, "data": obj})
            except Exception as e:
                return json.dumps({"ok": False, "error": str(e)})
        if name == "fs_resolve_path":
            import os as _os
            import difflib as _dif
            from pathlib import Path as _Path
            path_in = str(args.get("path") or "")
            kind = str(args.get("kind")) if args.get("kind") else "either"
            max_candidates = int(args.get("max_candidates", 10))
            base_dirs = [str(p) for p in (args.get("base_dirs") or [])]
            tried: list[str] = []
            def _exists(p: str) -> bool:
                if kind == "file":
                    return _os.path.isfile(p)
                if kind == "dir":
                    return _os.path.isdir(p)
                return _os.path.exists(p)
            # Expand and normalize
            p0 = _os.path.expanduser(_os.path.expandvars(path_in))
            p0 = _os.path.normpath(p0)
            tried.append(p0)
            if _exists(p0):
                return json.dumps({"ok": True, "path": _os.path.abspath(p0), "tried": tried})
            # Heuristic 1: case-insensitive correction per segment
            def _case_fix(p: str) -> str | None:
                parts = _Path(p).parts
                if not parts:
                    return None
                cur = _Path(parts[0])
                for seg in parts[1:]:
                    parent = cur
                    if not parent.exists():
                        break
                    try:
                        entries = {e.name.lower(): e.name for e in parent.iterdir()}
                        name = entries.get(seg.lower())
                        cur = parent / (name if name else seg)
                    except Exception:
                        cur = parent / seg
                q = str(cur)
                return q if _exists(q) else None
            cf = _case_fix(p0)
            if cf and cf not in tried:
                tried.append(cf)
                if _exists(cf):
                    return json.dumps({"ok": True, "path": _os.path.abspath(cf), "tried": tried, "reason": "case-insensitive match"})
            # Heuristic 2: pluralization/singularization on each segment (single change)
            def _plural_variants(p: str):
                parts = list(_Path(p).parts)
                for i in range(len(parts)):
                    alt = parts.copy()
                    seg = alt[i]
                    if seg.endswith("s"):
                        alt[i] = seg[:-1]
                    else:
                        alt[i] = seg + "s"
                    yield str(_Path(*alt))
            for cand in _plural_variants(p0):
                c2 = _os.path.normpath(cand)
                if c2 not in tried:
                    tried.append(c2)
                    if _exists(c2):
                        return json.dumps({"ok": True, "path": _os.path.abspath(c2), "tried": tried, "reason": "pluralization"})
            # Heuristic 3: simple prefix match near failing leaf
            try:
                parent, leaf = _os.path.split(p0)
                if parent and _os.path.isdir(parent) and leaf:
                    for fname in _os.listdir(parent):
                        if fname.lower().startswith(leaf.lower()):
                            c3 = _os.path.join(parent, fname)
                            if c3 not in tried:
                                tried.append(c3)
                                if _exists(c3):
                                    return json.dumps({"ok": True, "path": _os.path.abspath(c3), "tried": tried, "reason": "prefix match"})
            except Exception:
                pass
            # Fallback: repo/base dir search by basename with fuzzy scores
            repo_root = _Path(__file__).resolve().parents[3]
            search_roots = [str(repo_root)] + base_dirs
            target = _Path(path_in).name
            candidates: list[tuple[float, str]] = []
            def _walk_with_depth(root: str, max_depth: int = 6):
                root_p = _Path(root)
                try:
                    for cur, dirs, files in os.walk(root):
                        rel = _Path(cur).relative_to(root_p)
                        if len(rel.parts) > max_depth:
                            dirs[:] = []
                            continue
                        for nm in dirs + files:
                            full = _os.path.join(cur, nm)
                            if kind == "file" and not _os.path.isfile(full):
                                continue
                            if kind == "dir" and not _os.path.isdir(full):
                                continue
                            score = _dif.SequenceMatcher(a=nm.lower(), b=target.lower()).ratio()
                            if score >= 0.5:
                                candidates.append((score, _os.path.abspath(full)))
                except Exception:
                    return
            import os
            for r in search_roots:
                if _os.path.isdir(r):
                    _walk_with_depth(r, 6)
            candidates.sort(key=lambda x: x[0], reverse=True)
            out = [{"path": p, "score": float(s)} for (s, p) in candidates[:max_candidates]]
            return json.dumps({"ok": False, "candidates": out, "tried": tried})
        if name == "repo_search":
            import os as _os
            import difflib as _dif
            from pathlib import Path as _Path
            query = str(args.get("name") or "")
            typ = str(args.get("type")) if args.get("type") else "either"
            max_depth = int(args.get("max_depth", 6))
            max_results = int(args.get("max_results", 20))
            repo_root = _Path(__file__).resolve().parents[3]
            target = query.lower()
            hits: list[tuple[float, str]] = []
            try:
                for cur, dirs, files in os.walk(str(repo_root)):
                    rel = _Path(cur).relative_to(repo_root)
                    if len(rel.parts) > max_depth:
                        dirs[:] = []
                        continue
                    def _consider(nm: str):
                        full = _os.path.join(cur, nm)
                        if typ == "file" and not _os.path.isfile(full):
                            return
                        if typ == "dir" and not _os.path.isdir(full):
                            return
                        score = _dif.SequenceMatcher(a=nm.lower(), b=target).ratio()
                        if score >= 0.5:
                            hits.append((score, _os.path.abspath(full)))
                    for nm in dirs:
                        _consider(nm)
                    for nm in files:
                        _consider(nm)
            except Exception:
                pass
            hits.sort(key=lambda x: x[0], reverse=True)
            out = [{"path": p, "score": float(s)} for (s, p) in hits[:max_results]]
            return json.dumps({"ok": True, "results": out})
        if name == "fs_glob":
            import os as _os
            import glob as _glob
            d = str(args.get("dir") or "")
            pattern = str(args.get("pattern") or "*")
            rec = bool(args.get("recursive", False))
            base = _os.path.expanduser(_os.path.expandvars(d))
            if not _os.path.isdir(base):
                return json.dumps({"ok": False, "error": f"not a directory: {base}"})
            pat = _os.path.join(base, pattern)
            matches = _glob.glob(pat, recursive=rec)
            files = [ _os.path.abspath(m) for m in matches if _os.path.isfile(m) ]
            return json.dumps({"ok": True, "files": files, "dir": base, "pattern": pattern})

        # Scene understanding / load
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
                return json.dumps({"ok": False, "error": f"parameter '{name_str}' not found for id={id}"})
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
                ok = client.set_key_param(id=id, json_key=target_jk, time=time_v, easing=easing, value=value_native)
                return json.dumps({"ok": ok, "json_key": target_jk, "type": target_type})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg, "json_key": target_jk, "type": target_type})

        if name == "animation_remove_key_param_at_time":
            id = int(args.get("id"))
            if id == 0:
                return json.dumps({"ok": False, "error": "camera uses camera tools; use animation_replace_key_camera"})
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
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})

        if name == "animation_replace_key_param":
            id = int(args.get("id"))
            if id == 0:
                return json.dumps({"ok": False, "error": "use animation_replace_key_camera for id=0"})
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
                rm = json.loads(dispatch("animation_remove_key_param_at_time", json.dumps({
                    "id": id,
                    "json_key": json_key,
                    "time": time_v,
                    "tolerance": tol,
                })))
                ok = client.set_key_param(id=id, json_key=json_key, time=time_v, easing=easing, value=value)
                return json.dumps({"ok": ok, "removed": rm.get("removed", 0)})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
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
            constraints = args.get("constraints") or {"keep_visible": True, "min_coverage": 0.95}
            # First pass policies allow adjustments
            policies1 = {"adjust_fov": True, "adjust_distance": True, "adjust_clipping": True}
            # Second pass: strict verification without adjustments
            policies2 = {"adjust_fov": False, "adjust_distance": False, "adjust_clipping": False}
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
                    vr = client.camera_validate(ids=ids, times=[time_v], values=[value], constraints=constraints, policies=policies1)
                    vals = vr.get("results") or []
                    if vals and vals[0].get("adjusted") and vals[0].get("adjusted_value"):
                        value = vals[0].get("adjusted_value")
                except Exception:
                    pass
                ok = client.set_key_camera(time=time_v, easing=easing, value=value)
                # Re-validate strictly
                final_ok = ok
                try:
                    vr2 = client.camera_validate(ids=ids, times=[time_v], values=[value], constraints=constraints, policies=policies2)
                    final_ok = bool(vr2.get("ok", False))
                    reason = (vr2.get("results") or [{}])[0].get("reason")
                except Exception:
                    reason = None
                return json.dumps({"ok": bool(final_ok and ok), "removed": removed, **({"reason": reason} if reason else {})})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})

        if name == "fit_candidates":
            try:
                ids = client.fit_candidates()
                return json.dumps({"ok": True, "ids": ids})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})

        if name == "animation_camera_solve_and_apply":
            try:
                mode = str(args.get("mode"))
                ids = args.get("ids") or []
                t0 = float(args.get("t0", 0.0))
                t1 = float(args.get("t1", 0.0))
                if not isinstance(ids, list) or any(not isinstance(i, (int, float)) for i in ids):
                    return json.dumps({"ok": False, "error": "ids must be an array of numbers"})
                mode_up = mode.strip().upper()
                if mode_up not in ("FIT", "ORBIT", "DOLLY", "STATIC"):
                    return json.dumps({"ok": False, "error": "mode must be one of FIT|ORBIT|DOLLY|STATIC"})
                if mode_up in ("ORBIT", "DOLLY") and not (t1 > t0):
                    return json.dumps({"ok": False, "error": "t1 must be > t0 for ORBIT/DOLLY"})
                constraints = args.get("constraints") or {"keep_visible": True, "min_coverage": 0.95}
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
                keys = client.camera_solve(mode=mode_up, ids=ids, t0=t0, t1=t1, constraints=constraints, params=params)
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
                        rr = json.loads(dispatch("animation_replace_key_camera", json.dumps(payload)) or "{}")
                        if rr.get("ok"):
                            applied.append(tv)
                    except Exception:
                        continue
                return json.dumps({"ok": True, "applied": sorted(applied), "total": len(applied)})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})

        if name == "animation_camera_validate":
            try:
                ids = args.get("ids") or []
                times = args.get("times") or []
                values = args.get("values") or []
                if not times or len(times) != len(values):
                    return json.dumps({"ok": False, "error": "times and values must be non-empty and same length"})
                constraints = args.get("constraints") or {}
                policies = args.get("policies") or {}
                res = client.camera_validate(ids=ids, times=times, values=values, constraints=constraints, policies=policies)
                return json.dumps({"ok": bool(res.get("ok", False)), "results": res.get("results")})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
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
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})

        if name == "camera_point_to":
            try:
                ids = args.get("ids") or []
                ac = bool(args.get("after_clipping", True))
                val = client.camera_point_to(ids=ids, after_clipping=ac)
                return json.dumps({"ok": True, "value": val})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
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
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})

        if name == "camera_reset_view":
            try:
                mode = str(args.get("mode", "RESET"))
                ids = args.get("ids") or []
                ac = bool(args.get("after_clipping", True))
                mr = float(args.get("min_radius", 0.0))
                val = client.camera_reset_view(mode=mode, ids=ids, after_clipping=ac, min_radius=mr)
                return json.dumps({"ok": True, "value": val})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})
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
                vals = client.camera_fit(ids=ids, all=False, after_clipping=after, min_radius=minr)
                cam = vals[0] if vals else None
                if not cam:
                    return json.dumps({"ok": False, "error": "camera_fit returned no value"})
                ok = client.apply_params([{"id": 0, "json_key": "Camera 3DCamera", "value": cam}])
                return json.dumps({"ok": bool(ok)})
            except Exception as e:
                return json.dumps({"ok": False, "error": str(e)})
        if name == "scene_camera_apply":
            try:
                cam = args.get("value")
                if not cam or not isinstance(cam, dict):
                    return json.dumps({"ok": False, "error": "value must be a typed camera object"})
                ok = client.apply_params([{"id": 0, "json_key": "Camera 3DCamera", "value": cam}])
                return json.dumps({"ok": bool(ok)})
            except Exception as e:
                return json.dumps({"ok": False, "error": str(e)})
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
        if name == "animation_get_time":
            ts = client.get_time()
            return json.dumps({"seconds": getattr(ts, "seconds", 0.0), "duration": getattr(ts, "duration", 0.0)})
        if name == "python_write_and_run":
            import tempfile as _tmp, os as _os, subprocess as _sp, sys as _sys
            from pathlib import Path as _P
            script = args.get("script") or ""
            fname = str(args.get("filename") or "agent_script.py")
            tdir = _tmp.mkdtemp(prefix="atlas_codegen_")
            pth = _os.path.join(tdir, fname)
            # Ensure file ends with newline
            if not script.endswith("\n"):
                script += "\n"
            with open(pth, "w", encoding="utf-8") as f:
                f.write(script)
            # Build env with repo root on PYTHONPATH
            env = dict(_os.environ)
            repo_root = str(_P(__file__).resolve().parents[2])
            pp = env.get("PYTHONPATH", "")
            env["PYTHONPATH"] = (repo_root + (_os.pathsep + pp if pp else ""))
            # Run
            args_list = args.get("args") or []
            timeout = float(args.get("timeout_sec", 120.0))
            try:
                cp = _sp.run([_sys.executable, pth, *args_list], capture_output=True, text=True, env=env, timeout=timeout)
                out = cp.stdout or ""
                err = cp.stderr or ""
                # Truncate for transport safety
                if len(out) > 8000: out = out[:8000] + "\n…[truncated]"
                if len(err) > 8000: err = err[:8000] + "\n…[truncated]"
                resp = {"ok": cp.returncode == 0, "exit_code": cp.returncode, "stdout": out, "stderr": err, "path": pth}
                if bool(args.get("echo_script", True)):
                    maxc = int(args.get("max_echo_chars", 4000))
                    scr = script if len(script) <= maxc else (script[:maxc] + "\n…[truncated]")
                    resp["script"] = scr
                return json.dumps(resp)
            except Exception as e:
                resp = {"ok": False, "error": str(e), "path": pth}
                if bool(args.get("echo_script", True)):
                    maxc = int(args.get("max_echo_chars", 4000))
                    scr = script if len(script) <= maxc else (script[:maxc] + "\n…[truncated]")
                    resp["script"] = scr
                return json.dumps(resp)
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

        if name == "animation_list_keys":
            id = int(args.get("id"))
            json_key = args.get("json_key") or None
            if isinstance(json_key, str) and json_key.strip() == "":
                json_key = None
            include_values = bool(args.get("include_values", False))
            lr = client.list_keys(id=id, json_key=json_key, include_values=include_values)
            keys = [{"time": k.time, "type": k.type, "value": getattr(k, "value_json", "")} for k in lr.keys]
            return json.dumps({"ok": True, "keys": keys})
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
        if name == "scene_capabilities":
            schema_dir = args.get("schema_dir")
            sd, searched = discover_schema_dir(schema_dir, atlas_dir)
            if not sd:
                return json.dumps({"ok": False, "error": "capabilities not found", "searched": searched})
            try:
                with open(Path(sd) / "capabilities.json", "r", encoding="utf-8") as f:
                    caps = json.load(f)
                return json.dumps({"ok": True, "capabilities": caps})
            except Exception as e:
                return json.dumps({"ok": False, "error": str(e)})
        if name == "scene_params_handbook":
            schema_dir = args.get("schema_dir")
            include_groups = bool(args.get("include_groups", True))
            max_types = int(args.get("max_types", 50))
            max_params_per_type = int(args.get("max_params_per_type", 200))
            sd, searched = discover_schema_dir(schema_dir, atlas_dir)
            if not sd:
                return json.dumps({"ok": False, "error": "capabilities not found", "searched": searched})
            try:
                import json as _json
                from pathlib import Path as _Path
                caps = _json.loads((_Path(sd) / "capabilities.json").read_text(encoding="utf-8"))
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
                            lines.append(f"- `{p.get('jsonKey','')}` — {p.get('type','')} (interp={p.get('supportsInterpolation',False)})")
                        lines.append("")
                # Object types
                objs = caps.get("objects") or {}
                count_types = 0
                for tname, obj in (objs.items() if isinstance(objs, dict) else []):
                    if count_types >= max_types:
                        break
                    plist = []
                    if isinstance(obj, dict):
                        plist = obj.get("parameters") or obj.get("params") or []
                    lines.append(f"## Type: {tname}")
                    for p in plist[:max_params_per_type]:
                        jk = p.get('jsonKey','') or p.get('json_key','')
                        ty = p.get('type','')
                        interp = p.get('supportsInterpolation', False)
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
                return json.dumps({"ok": False, "error": "schema not found", "searched": searched})
            try:
                with open(Path(sd) / "animation3d.schema.json", "r", encoding="utf-8") as f:
                    sch = json.load(f)
                return json.dumps({"ok": True, "schema": sch})
            except Exception as e:
                return json.dumps({"ok": False, "error": str(e)})
        if name == "animation_describe_file":
            from ..describe import load_animation, load_capabilities, summarize_animation
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
                return json.dumps({"ok": True, "summary": text, "schema_dir": str(sd) if sd else None})
            except Exception as e:
                return json.dumps({"ok": False, "error": str(e), "searched": searched})
        if name == "scene_capabilities_summary":
            schema_dir = args.get("schema_dir")
            max_lines = args.get("max_lines") or 140
            sd, searched = discover_schema_dir(schema_dir, atlas_dir)
            try:
                text = build_capabilities_prompt(Path(sd), max_lines=max_lines)
                return json.dumps({"ok": True, "summary": text})
            except Exception as e:
                # Return generic text on failure
                text = build_capabilities_prompt(Path("/does/not/exist"), max_lines=max_lines)
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
            for o in objs[:max(0, limit)]:
                lines.append(f"  - {o.get('id')}:{o.get('type')}:{o.get('name')} visible={o.get('visible')}")
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
                    # summarize up to 2 params per object
                    params = list(mp.items())[:2]
                    for jk, times in params:
                        lines.append(f"Keys {oid}:{jk}: times={list(times)}")
                    count += 1
            except Exception:
                pass
            # Optional current values
            if sid_opt is not None and jks:
                try:
                    vals = client.get_param_values(id=int(sid_opt), json_keys=[str(x) for x in jks])
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
        if name == "scene_validate_apply":
            set_params = args.get("set_params") or []
            # Normalize and validate id shapes early to avoid malformed writes
            norm_params: list[dict] = []
            for it in set_params:
                if not isinstance(it, dict):
                    return json.dumps({"ok": False, "error": "each set_params item must be an object"})
                jk = it.get("json_key")
                if not isinstance(jk, str) or not jk:
                    return json.dumps({"ok": False, "error": "each set_params item must include a non-empty 'json_key'"})
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
                    return json.dumps({"ok": False, "error": "each set_params item must be an object"})
                id = int(it.get("id"))
                jk_in = it.get("json_key")
                name_in = it.get("name")
                # Resolve json_key
                if id == 0:
                    if jk_in is not None and jk_in != "Camera 3DCamera":
                        return json.dumps({"ok": False, "error": "json_key not found: expected 'Camera 3DCamera' for id=0"})
                    jk = "Camera 3DCamera"
                else:
                    jk: str | None = None
                    if isinstance(jk_in, str) and jk_in.strip():
                        # If canonical exists, keep; else resolve aliases/names
                        pl = client.list_params(id=id)
                        jks = {getattr(p, "json_key", ""): True for p in getattr(pl, "params", [])}
                        if jk_in in jks:
                            jk = jk_in
                        else:
                            jk_resolved = _resolve_json_key(id, candidate=jk_in)
                            if jk_resolved:
                                jk = jk_resolved
                                notes.append({"remapped_param": {"from": jk_in, "to": jk, "id": id}})
                            else:
                                if isinstance(name_in, str) and name_in.strip():
                                    jk_name = _resolve_json_key(id, name=name_in)
                                    if jk_name:
                                        jk = jk_name
                                        notes.append({"resolved_by_name": {"name": name_in, "to": jk, "id": id}})
                                    else:
                                        return json.dumps({"ok": False, "error": f"json_key not found: '{jk_in}'", "hint": "Use scene_list_params(id) or provide 'name'", "id": id})
                                else:
                                    return json.dumps({"ok": False, "error": f"json_key not found: '{jk_in}'", "hint": "Use scene_list_params(id) or provide 'name'", "id": id})
                    else:
                        if not isinstance(name_in, str) or not name_in.strip():
                            return json.dumps({"ok": False, "error": "each set_params item must include 'json_key' or 'name'"})
                        jk_name = _resolve_json_key(id, name=name_in)
                        if not jk_name:
                            return json.dumps({"ok": False, "error": f"could not resolve json_key for name='{name_in}'"})
                        jk = jk_name
                        notes.append({"resolved_by_name": {"name": name_in, "to": jk, "id": id}})
                norm_params.append({"id": id, "json_key": jk, "value": it.get("value")})

            # Validate before apply; if validation fails for types, return details
            val = client.validate_apply(norm_params)
            if not bool(val.get("ok", False)):
                return json.dumps({"ok": False, "validate": val, **({"notes": notes} if notes else {})})
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
                            overrides.append({"id": id2, "json_key": jk2, "key_times": times})
            except Exception:
                pass
            ok = client.apply_params(norm_params)
            resp = {"ok": bool(ok)}
            if overrides:
                resp["warning"] = "animation keys exist for some params; during playback those keys override scene values"
                resp["overrides"] = overrides
            if notes:
                resp["notes"] = notes
            return json.dumps(resp)
        if name == "scene_save_scene":
            ok = client.save_scene(args.get("path"))
            return json.dumps({"ok": bool(ok)})
        if name == "codegen_allowed_imports":
            try:
                from ..codegen_policy import allowed_imports_status
                names, status = allowed_imports_status()
                installed = [s["name"] for s in status if s.get("ok")]
                missing = [s["name"] for s in status if not s.get("ok")]
                return json.dumps({"ok": True, "allowed": names, "installed": installed, "missing": missing, "status": status})
            except Exception as e:
                return json.dumps({"ok": False, "error": str(e)})
        # 'export_python_camera_segmented_orbit' tool has been removed by design; use python_write_and_run to handle codegen directly.
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
        if name == "animation_ensure_animation":
            return json.dumps({"ok": client.ensure_animation()})
        if name == "animation_set_duration":
            seconds = float(args.get("seconds", 0.0))
            return json.dumps({"ok": client.set_duration(seconds)})
        # direct camera set tool removed; prefer solve_and_apply or replace_key_camera
        if name == "animation_set_key_param":
            # Expect native JSON value. Resolve json_key by name if needed; coerce common mistakes.
            id = int(args.get("id"))
            if id == 0:
                return json.dumps({"ok": False, "error": "camera uses camera tools; use animation_replace_key_camera or animation_camera_solve_and_apply"})
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
                    return json.dumps({"ok": False, "error": f"could not resolve json_key for name='{name}'"})
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
                    if s in ("true", "1", "yes", "on"): value_native = True
                    elif s in ("false", "0", "no", "off"): value_native = False
                # Normalize numeric vectors by length if Vec3/Vec4
                if t.endswith("vec4") and isinstance(value_native, list) and len(value_native) == 4:
                    value_native = [float(v) for v in value_native]
                if t.endswith("vec3") and isinstance(value_native, list) and len(value_native) == 3:
                    value_native = [float(v) for v in value_native]
            try:
                ok = client.set_key_param(id=id, json_key=json_key, time=time_v, easing=easing, value=value_native)
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
                return json.dumps({"ok": False, "error": "camera uses camera tools; use animation_replace_key_camera"})
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
                    _ = json.loads(dispatch("animation_remove_key_param_at_time", json.dumps({
                        "id": id,
                        "json_key": json_key,
                        "time": t,
                        "tolerance": tol
                    })))
                    _ = json.loads(dispatch("animation_set_key_param", json.dumps({
                        "id": id,
                        "json_key": json_key,
                        "time": t,
                        "easing": easing,
                        "value": value
                    })))
                return json.dumps({"ok": True, "count": len(times)})
            except Exception as e:
                msg = str(e)
                try: msg = e.details()  # type: ignore[attr-defined]
                except Exception: pass
                return json.dumps({"ok": False, "error": msg})
        if name == "animation_clear_keys":
            id = int(args.get("id"))
            ok = client.clear_keys(id=id, json_key=args.get("json_key"))
            return json.dumps({"ok": ok})
        if name == "animation_remove_key":
            id = int(args.get("id"))
            if id == 0:
                return json.dumps({"ok": False, "error": "camera uses camera tools; use animation_replace_key_camera"})
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
                return json.dumps({"ok": False, "error": "animation_batch called with empty set/remove. Build concrete SetKey entries or use animation_replace_key_param/animation_replace_key_camera (or animation_camera_solve_and_apply)."})
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
                            params_cache[id] = {getattr(p, "json_key", "") for p in pl.params}
                        except Exception:
                            params_cache[id] = set()
                    if str(jk) not in params_cache[id]:
                        invalid.append({"reason": "json_key not found for id", "json_key": jk, "id": id})
            except Exception:
                invalid = []
            if invalid:
                return json.dumps({"ok": False, "error": "validation failed for set_keys", "invalid": invalid})
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
        if name == "animation_set_time":
            ok = client.set_time(float(args.get("seconds", 0.0)), cancel_rendering=bool(args.get("cancel", False)))
            return json.dumps({"ok": ok})
        if name == "animation_save_animation":
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

        if name == "animation_render_preview":
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

        

        return json.dumps({"error": f"unknown tool: {name}"})

    # Filter out deprecated/disabled tools from the advertised list
    filtered = []
    disabled = {}
    # Environment-gated tools
    try:
        import os as _os
        allow_screenshots = (_os.environ.get("ATLAS_AGENT_ALLOW_SCREENSHOTS", "").strip().lower() in ("1", "true", "yes"))
    except Exception:
        allow_screenshots = False
    for t in tools:
        try:
            nm = t.get("function", {}).get("name")
            # Hide preview tool when screenshots disabled
            if nm == "animation_render_preview" and not allow_screenshots:
                continue
            if nm not in disabled:
                filtered.append(t)
        except Exception:
            filtered.append(t)
    return filtered, dispatch
