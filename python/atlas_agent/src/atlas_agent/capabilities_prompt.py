"""Build a compact, factual capabilities summary for grounding the agent.

This is a light cleanup of the prior implementation with the same behavior,
plus optional file-format bullets and an optional max_lines truncation.
"""

import json
from pathlib import Path
from typing import List

from .codegen_policy import is_codegen_enabled


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
    lines.append("Atlas Capabilities Overview (condensed)")
    lines.append("Use tools to inspect live params: scene_list_params(id); list keys via animation_list_keys(id,json_key).")
    if is_codegen_enabled():
        lines.append(
            "Advanced: codegen is enabled. For complex calculations, small Python helpers can be run via the codegen tool; prefer plan→validate→apply with verification."
        )
    # Scene vs Timeline contract (high-signal guidance for LLMs)
    lines.append(
        "Scene vs Timeline: Scene (.scene) = current display state (no time); Animation (.animation2d/.animation3d) = timeline keys per parameter. Change scene parameters will not affect animation. During playback, animation keys override scene values."
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
