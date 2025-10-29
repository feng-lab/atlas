from __future__ import annotations

import json
from pathlib import Path
from typing import Dict, List, Tuple


def _load_capabilities(schema_dir: Path) -> dict | None:
    try:
        cap_path = Path(schema_dir) / "capabilities.json"
        with open(cap_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


MAJOR_HINTS = {
    "Swc": {"Rendering Mode", "Color Mode", "Color", "Opacity", "Size Scale"},
    "Mesh": {"Color Mode", "Mesh Color", "Wireframe Option", "Wireframe Color", "Opacity"},
    "Image": {"Opacity", "Size Scale"},
}


def _split_major_advanced(params: List[dict], type_name: str) -> Tuple[List[str], List[str]]:
    names = [(p.get("name") or p.get("json_key") or "").strip() for p in params]
    majors: List[str] = []
    adv: List[str] = []
    hints = MAJOR_HINTS.get(type_name, {"Opacity", "Color", "Color Mode", "Size Scale"})
    for n in names:
        (majors if any(h.lower() in n.lower() for h in hints) else adv).append(n)
    # De-dup and keep order
    def dedup(xs: List[str]) -> List[str]:
        seen = set()
        out = []
        for x in xs:
            if x and x not in seen:
                seen.add(x)
                out.append(x)
        return out
    return dedup(majors), dedup(adv)


def build_capabilities_prompt(schema_dir: Path, *, max_lines: int = 120) -> str:
    caps = _load_capabilities(schema_dir)
    lines: List[str] = []
    lines.append("Atlas Capabilities Overview (condensed)")
    lines.append("Use tools to inspect live params: scene_list_params, scene_capabilities, list per object id.")
    if not caps:
        # Minimal static knowledge
        lines += [
            "Swc (neurite trees): major = Rendering Mode (Normal|Line|Sphere|Cylinder), Color Mode (Single|Branch|Topology|Colormap), Color (Vec4), Opacity, Size Scale.",
            "Mesh: major = Color Mode (Single|Mesh Color), Mesh Color (Vec4), Wireframe Option (No/With/Only), Wireframe Color, Opacity.",
            "Groups: Background/Axis/Global have scene‑level parameters; Camera uses dedicated camera keys.",
            "Tip: Use scene_batch to atomically set multiple keys; prefer 'Single Color' then set the Vec4 color.",
        ]
        return "\n".join(lines[:max_lines])

    # Try to summarize per object type
    objects = caps.get("objects") or {}
    if isinstance(objects, dict):
        for tname, obj in objects.items():
            plist = []
            if isinstance(obj, dict):
                plist = obj.get("parameters") or obj.get("params") or []
            majors, adv = _split_major_advanced(plist if isinstance(plist, list) else [], tname)
            if majors or adv:
                lines.append(f"{tname}:")
            if majors:
                lines.append("  Major: " + ", ".join(majors[:12]))
            if adv:
                lines.append("  Advanced: " + ", ".join(adv[:12]))

    # Global groups if present
    globs = caps.get("globals") or {}
    for gname in ("Background", "Axis", "Global"):
        g = globs.get(gname) if isinstance(globs, dict) else None
        if isinstance(g, dict):
            plist = g.get("parameters") or []
            majors, adv = _split_major_advanced(plist if isinstance(plist, list) else [], gname)
            if majors or adv:
                lines.append(f"{gname}:")
            if majors:
                lines.append("  Major: " + ", ".join(majors[:12]))
            if adv:
                lines.append("  Advanced: " + ", ".join(adv[:12]))

    # Best practices
    lines += [
        "Camera: use CameraFit/Orbit/Dolly to derive camera values; write camera keys at start/end.",
        "Emphasis recipe: set Color Mode='Single Color', set Color=Vec4 RGBA, and adjust Opacity.",
        "Batch: prefer scene_batch to atomically write multiple keys across scopes.",
        "Cuts: suggest via scene_cut_suggest_box(ids, margin), then scene_cut_set_box(refit_camera=true).",
    ]
    return "\n".join(lines[:max_lines])

