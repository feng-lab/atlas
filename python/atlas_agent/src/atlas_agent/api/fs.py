"""Typed filesystem helpers used by Python scripts.

These mirror the LLM Agent Tooling equivalents but return Python values and
raise exceptions for error conditions.
"""

import difflib
import glob
import os
from pathlib import Path
from typing import Iterable

from ..repo import find_repo_root


def expand_paths(paths: Iterable[str]) -> list[str]:
    out: list[str] = []
    for p in paths:
        t = os.path.expanduser(os.path.expandvars(str(p)))
        t = os.path.normpath(t)
        out.append(t)
    return out


def check_paths(paths: Iterable[str]) -> tuple[list[str], list[str]]:
    exists: list[str] = []
    missing: list[str] = []
    for p in paths:
        (exists if os.path.exists(p) else missing).append(p)
    return exists, missing


def glob_dir(dir_path: str, pattern: str = "*", recursive: bool = False) -> list[str]:
    base = os.path.expanduser(os.path.expandvars(dir_path))
    if not os.path.isdir(base):
        raise NotADirectoryError(base)
    pat = os.path.join(base, pattern)
    matches = glob.glob(pat, recursive=recursive)
    return [os.path.abspath(m) for m in matches if os.path.isfile(m)]


def resolve_path(path: str, *, kind: str = "either", base_dirs: list[str] | None = None, max_candidates: int = 10) -> tuple[bool, str | None, list[dict]]:
    """Resolve a possibly-typoed file/dir path.

    Returns (ok, path, candidates). If ok is False, candidates contains likely
    matches sorted by score (desc).
    """
    def _exists(p: str) -> bool:
        if kind == "file":
            return os.path.isfile(p)
        if kind == "dir":
            return os.path.isdir(p)
        return os.path.exists(p)

    p0 = os.path.normpath(os.path.expanduser(os.path.expandvars(path)))
    if _exists(p0):
        return True, os.path.abspath(p0), []

    # Case-insensitive correction per segment
    parts = Path(p0).parts
    if parts:
        cur = Path(parts[0])
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
        if _exists(q):
            return True, os.path.abspath(q), []

    # Pluralization/singularization
    def _plural_variants(p: str):
        ps = list(Path(p).parts)
        for i in range(len(ps)):
            alt = ps.copy()
            seg = alt[i]
            if seg.endswith("s"):
                alt[i] = seg[:-1]
            else:
                alt[i] = seg + "s"
            yield str(Path(*alt))

    for cand in _plural_variants(p0):
        if _exists(cand):
            return True, os.path.abspath(cand), []

    # Prefix match in parent
    parent, leaf = os.path.split(p0)
    if parent and os.path.isdir(parent) and leaf:
        for fname in os.listdir(parent):
            if fname.lower().startswith(leaf.lower()):
                c3 = os.path.join(parent, fname)
                if _exists(c3):
                    return True, os.path.abspath(c3), []

    # Fallback: repo or base_dirs search by basename with fuzzy scores
    repo_root = find_repo_root() or Path.cwd()
    search_roots = [str(repo_root)] + (base_dirs or [])
    target = Path(path).name.lower()
    candidates: list[tuple[float, str]] = []

    def _walk_with_depth(root: str, max_depth: int = 6):
        root_p = Path(root)
        for cur, dirs, files in os.walk(root):
            try:
                rel = Path(cur).relative_to(root_p)
            except Exception:
                rel = Path("")
            if len(rel.parts) > max_depth:
                dirs[:] = []
                continue
            for nm in dirs + files:
                full = os.path.join(cur, nm)
                if kind == "file" and not os.path.isfile(full):
                    continue
                if kind == "dir" and not os.path.isdir(full):
                    continue
                score = difflib.SequenceMatcher(a=nm.lower(), b=target).ratio()
                if score >= 0.5:
                    candidates.append((score, os.path.abspath(full)))

    for r in search_roots:
        if os.path.isdir(r):
            _walk_with_depth(r, 6)
    candidates.sort(key=lambda x: x[0], reverse=True)
    out = [{"path": p, "score": float(s)} for (s, p) in candidates[:max_candidates]]
    return False, None, out


def repo_search(name: str, *, type: str = "either", max_depth: int = 6, max_results: int = 20) -> list[dict]:
    repo_root = find_repo_root() or Path.cwd()
    target = name.lower()
    hits: list[tuple[float, str]] = []
    for cur, dirs, files in os.walk(str(repo_root)):
        rel = Path(cur).relative_to(repo_root)
        if len(rel.parts) > max_depth:
            dirs[:] = []
            continue
        def _consider(nm: str):
            full = os.path.join(cur, nm)
            if type == "file" and not os.path.isfile(full):
                return
            if type == "dir" and not os.path.isdir(full):
                return
            score = difflib.SequenceMatcher(a=nm.lower(), b=target).ratio()
            if score >= 0.5:
                hits.append((score, os.path.abspath(full)))
        for nm in dirs:
            _consider(nm)
        for nm in files:
            _consider(nm)
    hits.sort(key=lambda x: x[0], reverse=True)
    return [{"path": p, "score": float(s)} for (s, p) in hits[:max_results]]
