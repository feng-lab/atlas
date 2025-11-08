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
        p2 = p + "\n\n"
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


# Deprecated: building a manifest by parsing docs is removed. The canonical
# tool schemas live in code (tools_agent.scene_tools_and_dispatcher) and are
# provided to LLMs at runtime via the tools parameter.


def _dump_schema_with_atlas(atlas_dir: Path, out_dir: Path) -> None:
    atlas_bin, _ = compute_paths_from_atlas_dir(atlas_dir)
    args = [
        str(atlas_bin),
        "--run_dump_animation3d_schema",
        "--dump_output_dir",
        str(out_dir),
    ]
    if platform.system() in {"Windows", "Linux"}:
        args += ["-platform", "offscreen"]
    try:
        subprocess.run(args, check=False)
    except FileNotFoundError:
        pass


REQUIRED_FILES = [
    "animation3d.schema.json",
    "capabilities.json",
    "supported_file_formats.json",
    "agent_kb.jsonl",
]


def missing_llm_docs(out_dir: Path) -> List[str]:
    return [name for name in REQUIRED_FILES if not (out_dir / name).exists()]


def ensure_llm_docs(
    repo_root: Path,
    *,
    atlas_dir: Optional[str] = None,
    out_dir: Optional[Path] = None,
    force_schema_dump: bool = False,
) -> Path:
    out = out_dir or repo_schema_dir(repo_root)
    out.mkdir(parents=True, exist_ok=True)

    # Dump schema/caps if missing
    missing = set(missing_llm_docs(out))
    if force_schema_dump or (
        {"animation3d.schema.json", "capabilities.json", "supported_file_formats.json"}
        & missing
    ):
        if atlas_dir:
            _dump_schema_with_atlas(Path(atlas_dir), out)

    # Tools manifest generation removed. The canonical tool schemas are defined in
    # tools_agent.scene_tools_and_dispatcher() and exposed to LLMs at runtime via
    # the "tools" parameter. Keeping a second, static JSON manifest created from
    # docs was redundant and risked drift.

    # Knowledge cards
    cards = build_kb_cards(docs_inputs(repo_root))
    with (out / "agent_kb.jsonl").open("w", encoding="utf-8") as f:
        for c in cards:
            f.write(json.dumps(c, ensure_ascii=False) + "\n")

    return out
