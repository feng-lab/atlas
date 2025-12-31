"""Build compact, factual grounding text for the agent.

Includes:
- A short "what is Atlas / what is atlas_agent" primer (stable, schema-independent).
- A condensed capabilities view derived from the discovered schema directory.

This remains a lightweight summarizer with optional file-format bullets and an
optional max_lines truncation.
"""

import json
from pathlib import Path
from typing import List

from .codegen_policy import is_codegen_enabled


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
        "atlas_agent is a CLI + multi-agent system that uses the gRPC API to execute natural-language requests via tool calls (load data, inspect objects/params, edit scene values, write animation keys, save/export)."
    )
    lines.append(
        "Rule of thumb: any request with time/duration implies animation_* tools; otherwise prefer scene_* tools."
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


def _param_names(params: List[dict]) -> List[str]:
    names = [(p.get("name") or p.get("json_key") or "").strip() for p in params]
    # De-dup while preserving order
    seen = set()
    out: List[str] = []
    for n in names:
        if n and n not in seen:
            seen.add(n)
            out.append(n)
    return out


def build_capabilities_prompt(schema_dir: Path, *, max_lines: int | None = None) -> str:
    caps = _load_capabilities(schema_dir) or {}
    lines: List[str] = []
    lines.extend(build_atlas_agent_primer().splitlines())
    lines.append("")
    lines.append("Atlas Capabilities Overview (condensed)")
    lines.append("Use tools to inspect live params: scene_list_params(id); list keys via animation_list_keys(id,json_key).")
    if is_codegen_enabled():
        lines.append(
            "Advanced: codegen is enabled. For complex calculations, small Python helpers can be run via the codegen tool; prefer plan→validate→apply with verification."
        )

    # Summarize per object type (flat list, no major/advanced split)
    objects = caps.get("objects") or {}
    if isinstance(objects, dict):
        for tname, obj in objects.items():
            plist = []
            if isinstance(obj, dict):
                plist = obj.get("parameters") or obj.get("params") or []
            names = _param_names(plist if isinstance(plist, list) else [])
            if names:
                lines.append(f"{tname}:")
                lines.append("  Parameters: " + ", ".join(names))

    # Global groups if present (flat list)
    globs = caps.get("globals") or {}
    if isinstance(globs, dict):
        for gname in ("Background", "Axis", "Global"):
            g = globs.get(gname)
            if isinstance(g, dict):
                plist = g.get("parameters") or []
                names = _param_names(plist if isinstance(plist, list) else [])
                if names:
                    lines.append(f"{gname}:")
                    lines.append("  Parameters: " + ", ".join(names))

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

    text = "\n".join(lines)
    if isinstance(max_lines, int) and max_lines > 0:
        return "\n".join(text.splitlines()[:max_lines])
    return text
