from __future__ import annotations

import json
from pathlib import Path
from typing import Dict, List, Tuple
from .codegen_policy import is_codegen_enabled


def _load_capabilities(schema_dir: Path) -> dict | None:
    try:
        cap_path = Path(schema_dir) / "capabilities.json"
        with open(cap_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


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
    caps = _load_capabilities(schema_dir)
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
                lines.append("  Parameters: " + ", ".join(names[:12]))

    # Global groups if present (flat list)
    globs = caps.get("globals") or {}
    for gname in ("Background", "Axis", "Global"):
        g = globs.get(gname) if isinstance(globs, dict) else None
        if isinstance(g, dict):
            plist = g.get("parameters") or []
            names = _param_names(plist if isinstance(plist, list) else [])
            if names:
                lines.append(f"{gname}:")
                lines.append("  Parameters: " + ", ".join(names[:12]))

    # Return full content without hard line limits. If a shorter version is required,
    # the caller should summarize via the LLM rather than truncating here.
    return "\n".join(lines)
