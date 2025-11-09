import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
from .agent_team.base import LLMClient


def _safe_get(d: dict, *keys, default=None):
    cur = d
    for k in keys:
        if not isinstance(cur, dict) or k not in cur:
            return default
        cur = cur[k]
    return cur


def load_animation(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        j = json.load(f)
    if isinstance(j, dict) and "Animation3D" in j:
        return j["Animation3D"]
    # If file already contains the inner object
    return j


def load_capabilities(schema_dir: Path) -> dict:
    cap_path = Path(schema_dir) / "capabilities.json"
    with open(cap_path, "r", encoding="utf-8") as f:
        return json.load(f)


def _id_type_map(doc_map: Dict[str, Any]) -> Dict[str, str]:
    # Keys like "Mesh 2", "Image 1"
    m: Dict[str, str] = {}
    for k in doc_map.keys():
        if not isinstance(k, str):
            continue
        parts = k.split()
        if len(parts) == 2 and parts[1].isdigit():
            m[parts[1]] = parts[0]
    return m


def _friendly_param_name(json_key: str, caps: dict, target_type: Optional[str]) -> str:
    # Search in globals if target_type is Background/Axis/Global; else search per object type
    def find_in(lst):
        for p in lst:
            if p.get("jsonKey") == json_key:
                nm = p.get("name") or json_key
                tp = p.get("type") or ""
                return f"{nm} ({tp})"
        return json_key

    if target_type in {"Background", "Axis", "Global"}:
        g = caps.get("globals", {})
        entry = g.get(target_type)
        if entry and isinstance(entry, dict):
            return find_in(entry.get("parameters", []))
    elif target_type:
        obj = caps.get("objects", {}).get(target_type)
        if obj:
            return find_in(obj.get("parameters", []))
    return json_key


def _format_value(v: Any, max_len: int = 36) -> str:
    try:
        if isinstance(v, bool):
            return "on" if v else "off"
        if isinstance(v, (int, float)):
            return f"{v:.4g}"
        if isinstance(v, list):
            if len(v) <= 4 and all(isinstance(x, (int, float)) for x in v):
                return "[" + ", ".join(_format_value(x) for x in v) + "]"
            return f"array(len={len(v)})"
        s = json.dumps(v)
        return s if len(s) <= max_len else s[: max_len - 3] + "..."
    except Exception:
        return str(v)


def _analyze_param_animation(pmap: dict) -> List[str]:
    lines: List[str] = []
    keys = pmap.get("keys")
    if not isinstance(keys, list) or not keys:
        return lines
    times = [float(k.get("time", 0)) for k in keys]
    vals = [k.get("value") for k in keys]
    easing = [k.get("type") for k in keys]
    # First/last
    first = _format_value(vals[0])
    last = _format_value(vals[-1])
    if len(keys) == 1:
        lines.append(f"set {first} at t={times[0]:.3g}s")
        return lines
    # detect toggle/ramp
    if all(isinstance(v, bool) for v in vals):
        if vals[0] != vals[-1]:
            lines.append(
                f"toggle {first}→{last} from t={times[0]:.3g}s to {times[-1]:.3g}s ({easing[0]})"
            )
        else:
            lines.append(
                f"boolean sequence ({sum(1 for i in range(1,len(vals)) if vals[i]!=vals[i-1])} toggles)"
            )
        return lines
    if all(isinstance(v, (int, float)) for v in vals):
        if vals[0] != vals[-1]:
            lines.append(
                f"ramp {first}→{last} over {times[0]:.3g}–{times[-1]:.3g}s ({easing[0]})"
            )
        else:
            lines.append(f"multiple numeric keys (start={first}, end={last})")
        return lines
    lines.append(f"{len(keys)} keys (start={first}, end={last})")
    return lines


def _camera_angle_sweep(keys: List[dict]) -> Optional[float]:
    # Approximate total angle swept around the evolving center
    try:
        total = 0.0
        prev_vec = None
        for k in keys:
            val = k.get("value", {})
            eye = val.get("Eye Position") or val.get("Eye")
            center = val.get("Center Position") or val.get("Center")
            if not (isinstance(eye, list) and isinstance(center, list) and len(eye) >= 3 and len(center) >= 3):
                continue
            v = [eye[0] - center[0], eye[1] - center[1], eye[2] - center[2]]
            norm = math.sqrt(sum(x * x for x in v)) or 1.0
            v = [x / norm for x in v]
            if prev_vec is not None:
                dot = max(-1.0, min(1.0, sum(a * b for a, b in zip(prev_vec, v))))
                total += math.degrees(math.acos(dot))
            prev_vec = v
        return total
    except Exception:
        return None


def summarize_animation(anim: dict, caps: dict, style: str = "short") -> str:
    lines: List[str] = []

    duration = anim.get("Duration")
    if isinstance(duration, (int, float)):
        lines.append(f"Duration: {duration:.3g}s")

    # Doc summary
    doc_map = anim.get("Doc") or {}
    if isinstance(doc_map, dict) and doc_map:
        id_type = _id_type_map(doc_map)
        counts: Dict[str, int] = {}
        for t in id_type.values():
            counts[t] = counts.get(t, 0) + 1
        doc_parts = [f"{t} x{n}" for t, n in counts.items()]
        lines.append("Doc: " + ", ".join(sorted(doc_parts)))

    # Camera
    cam = anim.get("Camera 3DCamera") or {}
    ck = cam.get("keys") if isinstance(cam, dict) else None
    if isinstance(ck, list) and ck:
        sweep = _camera_angle_sweep(ck)
        if sweep is not None:
            lines.append(f"Camera: ~{sweep:.0f}° sweep, {len(ck)} key(s)")
        else:
            lines.append(f"Camera: {len(ck)} key(s)")

    # Groups
    for grp in ("Background", "Axis", "Global"):
        pmap = anim.get(grp)
        if not isinstance(pmap, dict) or not pmap:
            continue
        g_lines = []
        for json_key, v in pmap.items():
            if not isinstance(v, dict):
                continue
            friendly = _friendly_param_name(json_key, caps, grp)
            descs = _analyze_param_animation(v)
            for d in descs:
                g_lines.append(f"- {friendly}: {d}")
        if g_lines:
            lines.append(f"{grp}:")
            lines.extend(g_lines)

    # Objects
    if isinstance(doc_map, dict) and doc_map:
        id_type = _id_type_map(doc_map)
        for key, v in anim.items():
            if not isinstance(key, str) or not key.isdigit() or not isinstance(v, dict):
                continue
            typ = id_type.get(key, "Object")
            o_lines = []
            for json_key, vv in v.items():
                if not isinstance(vv, dict):
                    continue
                friendly = _friendly_param_name(json_key, caps, typ)
                for d in _analyze_param_animation(vv):
                    o_lines.append(f"- {friendly}: {d}")
            if o_lines:
                lines.append(f"{typ} {key}:")
                lines.extend(o_lines)

    if style == "short":
        # Return a concise overview (no silent truncation)
        return "\n".join(lines)
    return "\n".join(lines)


def summarize_with_llm(facts_text: str, *, api_key: Optional[str], model: str, max_tokens: int, temperature: float) -> str:
    if not api_key:
        return facts_text
    sys_prompt = (
        "You are an expert Atlas animation analyst. Given extracted facts about an Animation3D, "
        "write a clear, concise summary (5–8 sentences). Highlight: camera motion (orbit/dolly/zoom), major parameter changes, "
        "object fades/toggles, and a rough order of visual beats. Avoid jargon and code fences."
    )
    try:
        llm = LLMClient(api_key=api_key, model=model)
        text = llm.complete_text(system_prompt=sys_prompt, user_text=facts_text, temperature=temperature, max_tokens=max_tokens)
        return text or facts_text
    except Exception:
        return facts_text
