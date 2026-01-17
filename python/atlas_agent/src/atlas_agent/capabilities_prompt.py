"""Build compact, factual grounding text for the agent.

This produces a deliberately small, non-exhaustive "primer" that helps the
model choose the right tools and workflows without relying on brittle prompt
knowledge.

Source of truth remains the live tool interface:
- scene_list_objects / scene_list_params / scene_get_values
- scene_capabilities / scene_schema
"""

import json
from pathlib import Path
from typing import List


def build_atlas_agent_primer() -> str:
    lines: List[str] = []
    lines.append("Atlas + atlas_agent Primer (factual)")
    lines.append(
        "Atlas is a desktop visualization/analysis app for large 2D and 3D datasets (images, ROI masks, region annotations, puncta, SWC trees, meshes, SVG overlays)."
    )
    lines.append(
        "Atlas typically has two windows: a 2D view window and a 3D view window."
    )
    lines.append(
        "Scene (.scene): a static, reproducible Atlas state consisting of a list of renderable objects plus rendering parameters for both the 2D and 3D views; it can be saved/restored."
    )
    lines.append(
        "Objects: each object has rendering parameters (per-view) such as transforms (translate/rotate/scale), appearance (color/style), visibility, and cuts/clipping."
    )
    lines.append(
        "Animation (.animation2d/.animation3d): extends the scene concept with a keyframed timeline. Each parameter is defined by keys like (time,value) with easing/interpolation (Switch/Linear/Ease-in/out)."
    )
    lines.append(
        "At any time t, Atlas evaluates keys to compute parameter values for objects/camera, yielding a reproducible animation; animations can be saved/restored."
    )
    lines.append(
        "Animation2D affects only the 2D view; Animation3D affects only the 3D view. 2D and 3D parameters differ even for the same object type, and some types are view-specific (e.g., meshes render in 3D, not 2D)."
    )
    lines.append(
        "Playback rule: during playback, animation keys override scene values for affected parameters; to change what plays, write/replace keys (not scene-only edits)."
    )
    lines.append(
        "Atlas exposes a local gRPC API so external tools can query the live scene state and apply changes deterministically."
    )
    lines.append(
        "atlas_agent is a CLI + tool-using agent runtime that uses the gRPC API to execute natural-language requests via tool calls (load data, inspect objects/params, edit scene values, write animation keys, save/export)."
    )
    lines.append(
        "Rule of thumb: any request with time/duration implies animation_* tools; otherwise prefer scene_* tools."
    )
    lines.append(
        "Camera motion can be authored as camera keyframes: high-level solve modes (fit/orbit/dolly), first-person walkthroughs (local move + yaw/pitch/roll), or guided waypoint splines."
    )
    lines.append(
        "Routing heuristic: explicit waypoints/points → waypoint spline; motion verbs (fly/turn/pause) → walkthrough segments. Prefer bbox-scaled distances and dense key sampling for smooth motion."
    )
    return "\n".join(lines)


def _load_json(path: Path) -> dict | None:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def _load_capabilities(schema_dir: Path) -> dict | None:
    return _load_json(Path(schema_dir) / "capabilities.json")


def _load_formats(schema_dir: Path) -> dict | None:
    return _load_json(Path(schema_dir) / "supported_file_formats.json")


def build_capabilities_prompt(schema_dir: Path, *, codegen_enabled: bool = False) -> str:
    caps = _load_capabilities(schema_dir) or {}
    lines: List[str] = []
    lines.extend(build_atlas_agent_primer().splitlines())
    lines.append("")
    lines.append("Atlas Capability Summary (schema-derived, compact)")
    lines.append(
        "This summary is intentionally non-exhaustive. For authoritative details use tools:"
    )
    lines.append("- scene_list_objects / scene_list_params / scene_get_values")
    lines.append("- scene_capabilities / scene_schema")
    lines.append("- animation_list_keys / animation_camera_validate")
    if bool(codegen_enabled):
        lines.append(
            "Advanced: codegen is enabled. For complex calculations, small Python helpers can be run via the codegen tool; prefer plan→validate→apply with verification."
        )

    # Object type names (small and stable); avoid listing all parameters here.
    objects = caps.get("objects") or {}
    if isinstance(objects, dict):
        type_names = [str(k) for k in objects.keys()]
        if type_names:
            lines.append(f"Object types ({len(type_names)}):")
            for nm in type_names:
                if nm:
                    lines.append(f"- {nm}")

    # Global groups if present (flat list)
    globs = caps.get("globals") or {}
    if isinstance(globs, dict):
        gnames = [str(k) for k in globs.keys()]
        if gnames:
            lines.append(f"Global groups ({len(gnames)}):")
            for nm in gnames:
                if nm:
                    lines.append(f"- {nm}")

    # Optional: supported file formats bullets (short, by category)
    fmts = _load_formats(schema_dir) or {}
    cats = fmts.get("categories") if isinstance(fmts, dict) else None
    if isinstance(cats, dict) and cats:
        lines.append("Supported file formats:")
        try:
            for name, d in cats.items():
                exts = (
                    ", ".join(sorted(d.get("extensions", [])))
                    if isinstance(d, dict)
                    else ""
                )
                if exts:
                    lines.append(f"- {name}: {exts}")
        except Exception:
            # Do not fail summarization on format read errors
            pass

    return "\n".join(lines)
