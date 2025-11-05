from __future__ import annotations

import json
import platform
import re
import subprocess
from pathlib import Path
from typing import Iterable, List, Dict, Any, Tuple, Optional

from .discovery import compute_paths_from_atlas_dir


# ---------- Repo detection ----------

REPO_SENTINELS = [
    Path("src/atlas/CMakeLists.txt"),
    Path("docs/AGENTS_GUIDE.md"),
]


def find_repo_root(start: Path | None = None, *, max_up: int = 5) -> Optional[Path]:
    cur = (start or Path.cwd()).resolve()
    for _ in range(max_up + 1):
        if all((cur / s).exists() for s in REPO_SENTINELS):
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    return None


def repo_schema_dir(repo_root: Path) -> Path:
    return (repo_root / "src/atlas/Resources/json/atlas").resolve()


def docs_inputs(repo_root: Path) -> List[Path]:
    # Single-source for agent knowledge base
    unified = (repo_root / "docs/AGENTS_GUIDE.md").resolve()
    return [unified] if unified.exists() else []


# ---------- KB build helpers ----------

def _read_text(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="ignore")


def _slugify(s: str) -> str:
    s = s.strip().lower()
    s = re.sub(r"[^a-z0-9\-_. ]+", "", s)
    s = s.replace(" ", "-")
    s = re.sub(r"-+", "-", s)
    return s[:64] or "item"


def _split_headings(md: str) -> List[Tuple[str, str]]:
    lines = md.splitlines()
    sections: List[Tuple[str, List[str]]] = []
    cur_title = ""
    cur_body: List[str] = []
    for ln in lines:
        if ln.startswith("# ") or ln.startswith("## "):
            if cur_body:
                sections.append((cur_title.strip(), cur_body))
                cur_body = []
            cur_title = ln.lstrip("# ").strip()
        else:
            cur_body.append(ln)
    if cur_body:
        sections.append((cur_title.strip(), cur_body))
    return [(t, "\n".join(b).strip()) for t, b in sections if (t or b)]


def _guess_tags(path: Path) -> List[str]:
    name = path.name.lower()
    tags: List[str] = []
    if "reference" in name:
        tags += ["tools", "reference"]
    if "contract" in name:
        tags += ["policy"]
    if "system" in name:
        tags += ["system"]
    if "developer" in name:
        tags += ["dev"]
    if "user" in name:
        tags += ["user"]
    return tags or ["docs"]


def _chunk_text(text: str, *, max_chars: int = 1800) -> List[str]:
    paras = [p.strip() for p in text.split("\n\n")]
    out: List[str] = []
    cur = []
    size = 0
    for p in paras:
        p2 = (p + "\n\n")
        if size + len(p2) > max_chars and cur:
            out.append("".join(cur).strip())
            cur = [p2]
            size = len(p2)
        else:
            cur.append(p2)
            size += len(p2)
    if cur:
        out.append("".join(cur).strip())
    return [c for c in out if c]


def build_kb_cards(paths: Iterable[Path]) -> List[Dict[str, Any]]:
    cards: List[Dict[str, Any]] = []
    for p in paths:
        if not p.exists():
            continue
        text = _read_text(p)
        base_tags = _guess_tags(p)
        for idx, (title, body) in enumerate(_split_headings(text)):
            if not body:
                continue
            for j, chunk in enumerate(_chunk_text(body)):
                card = {
                    "id": f"{_slugify(p.stem)}::{_slugify(title) or 'intro'}::{idx:02d}-{j:02d}",
                    "source": str(p),
                    "title": title or p.stem,
                    "tags": base_tags,
                    "text": chunk,
                }
                cards.append(card)
    return cards


_TOOL_LINE_RE = re.compile(r"^\s*-\s*([a-zA-Z0-9_]+)\s+\u2014\s+Required:\s*(.+?)\.(.*)$")


def _parse_tools(md: str) -> Dict[str, Any]:
    manifest: Dict[str, Any] = {"tools": []}
    category = None
    for ln in md.splitlines():
        if ln and not ln.startswith(" ") and not ln.startswith("-") and not ln.startswith("#"):
            category = ln.strip()
            continue
        m = _TOOL_LINE_RE.match(ln)
        if not m:
            continue
        name, required_str, rest = m.groups()
        req_fields: List[Dict[str, str]] = []
        for part in required_str.split(","):
            part = part.strip()
            if not part or part.lower() == "none":
                continue
            mm = re.match(r"([a-zA-Z0-9_]+)\((.+)\)", part)
            if mm:
                req_fields.append({"name": mm.group(1), "type": mm.group(2)})
            else:
                req_fields.append({"name": part, "type": ""})
        manifest["tools"].append({
            "name": name,
            "category": category or "",
            "required": req_fields,
            "notes": rest.strip(),
        })
    return manifest


def build_tools_manifest(reference_md_path: Path) -> Dict[str, Any]:
    if not reference_md_path.exists():
        return {"tools": []}
    md = _read_text(reference_md_path)
    return _parse_tools(md)


def build_agent_facts() -> Dict[str, Any]:
    return {
        "ids": {"camera": 0, "background": 1, "axis": 2, "global": 3, "objects_start": 4},
        "contracts": {
            "scene_vs_timeline": "Scene (.scene) is stateless (no time/easing). Animation (.animation2d/.animation3d) holds keys. During playback, keys override scene values.",
            "camera_segment_rule": "For chained rotations (e.g., 360°), segment ≤90° steps and always base each step on the previous camera value.",
            "stateless_scene_apply": "scene_apply accepts set_params with id/json_key/value only; never include time/easing.",
        },
        "paths": {
            "schema_dir_relative": "Resources/json/atlas",
            "capabilities": "Resources/json/atlas/capabilities.json",
        },
    }


def _dump_schema_with_atlas(atlas_dir: Path, out_dir: Path) -> None:
    atlas_bin, _ = compute_paths_from_atlas_dir(atlas_dir)
    args = [str(atlas_bin), "--run_dump_animation3d_schema", "--dump_output_dir", str(out_dir)]
    if platform.system() in {"Windows", "Linux"}:
        args += ["-platform", "offscreen"]
    try:
        subprocess.run(args, check=False)
    except FileNotFoundError:
        pass


REQUIRED_FILES = [
    "animation3d.schema.json",
    "capabilities.json",
    "agent_tools.manifest.json",
    "agent_facts.json",
    "agent_kb.jsonl",
]


def missing_llm_docs(out_dir: Path) -> List[str]:
    return [name for name in REQUIRED_FILES if not (out_dir / name).exists()]


def ensure_llm_docs(repo_root: Path, *, atlas_dir: Optional[str] = None, out_dir: Optional[Path] = None, force_schema_dump: bool = False) -> Path:
    out = (out_dir or repo_schema_dir(repo_root))
    out.mkdir(parents=True, exist_ok=True)

    # Dump schema/caps if missing
    missing = set(missing_llm_docs(out))
    if force_schema_dump or ({"animation3d.schema.json", "capabilities.json"} & missing):
        if atlas_dir:
            _dump_schema_with_atlas(Path(atlas_dir), out)

    # Tools manifest (unified guide)
    ref_path = repo_root / "docs/AGENTS_GUIDE.md"
    manifest = build_tools_manifest(ref_path)
    (out / "agent_tools.manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")

    # High-signal facts
    (out / "agent_facts.json").write_text(json.dumps(build_agent_facts(), indent=2, ensure_ascii=False), encoding="utf-8")

    # Knowledge cards
    cards = build_kb_cards(docs_inputs(repo_root))
    with (out / "agent_kb.jsonl").open("w", encoding="utf-8") as f:
        for c in cards:
            f.write(json.dumps(c, ensure_ascii=False) + "\n")

    return out
