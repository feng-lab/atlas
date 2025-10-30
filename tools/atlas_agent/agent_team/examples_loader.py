from __future__ import annotations

from pathlib import Path
from typing import List


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _examples_dir() -> Path:
    return _repo_root() / "tools" / "atlas_agent" / "examples"


def load_compact_examples(max_count: int = 2) -> List[str]:
    """Load up to max_count compact few‑shot examples from tools/atlas_agent/examples.

    Each example is a short text file (plan + tools + facts). If no files are present,
    returns two built‑in examples.
    """
    ex_dir = _examples_dir()
    out: List[str] = []
    if ex_dir.exists() and ex_dir.is_dir():
        paths = sorted([p for p in ex_dir.glob("*.txt") if p.is_file()])[:max_count]
        for p in paths:
            try:
                out.append(p.read_text(encoding="utf-8").strip())
            except Exception:
                continue
    if out:
        return out
    # Built‑in fallbacks (compact patterns)
    builtins: List[str] = [
        (
            "User: Make the mesh red and orbit for 8 seconds.\n"
            "Plan:\n"
            "  1) Set Mesh to Single Color red at t=0.\n"
            "  2) Fit camera and set orbit start/end at t=0/8.\n"
            "  3) Set duration=8.\n"
            "Tools: scene_list_objects → scene_list_params → scene_set_param_by_name(‘Color Mode’, ‘Single Color’) → scene_set_param_by_name(‘Mesh Color’, [1,0,0,1]) → scene_camera_fit(ids=[meshId]) → scene_camera_orbit(ids=[meshId],axis=‘y’,angle=360) → scene_replace_key_camera(t=0, ...) → scene_replace_key_camera(t=8, ...) → scene_set_duration(8)\n"
            "Facts: camera=[0,8]; objects.{meshId}.Color Mode=[0]; objects.{meshId}.Mesh Color=[0]"
        ),
        (
            "User: Emphasize SWC 3 in magenta over 5 seconds, dim others.\n"
            "Plan:\n"
            "  1) SWC 3 → Color Mode=Single Color, Color=magenta at t=0.\n"
            "  2) Opacity ramp 0.3→1.0 from t=0→5; others set to 0.2 at t=0.\n"
            "Tools: scene_emphasize_object(id=3,color=magenta,opacity=1.0,dim_others=true,time=0) → scene_replace_key_param(json_key=‘Opacity’,time=0,value=0.3) → scene_replace_key_param(json_key=‘Opacity’,time=5,value=1.0)\n"
            "Facts: objects.3.Color Mode=[0]; objects.3.Color=[0]; objects.3.Opacity=[0,5]"
        ),
    ]
    return builtins[:max_count]

