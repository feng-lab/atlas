import json
import os
import re
from pathlib import Path
from typing import Any, Dict, List, Optional

from ...discovery import compute_docs_dir_from_atlas_dir
from ...repo import find_repo_root
from ...tool_registry import Tool, tool_from_schema
from .context import ToolDispatchContext

DOCS_LIST_DESCRIPTION = "List available Atlas markdown docs (from the installed Atlas app bundle when available, and/or from the monorepo docs/ when running in-repo)."
DOCS_LIST_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "docs_dir": {
            "type": ["string", "null"],
            "description": "Optional explicit docs dir override (otherwise derived from atlas_dir or repo).",
        },
        "max_depth": {
            "type": "integer",
            "default": -1,
            "description": "Directory recursion depth for listing markdown docs. Use -1 for unlimited (correctness-first).",
        },
    },
}

DOCS_SEARCH_DESCRIPTION = "Search Atlas docs for a query. Returns matching excerpts with file paths and line numbers. By default returns all matches (correctness-first); set max_results>0 to bound output."
DOCS_SEARCH_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "query": {"type": "string", "description": "Search query (substring or regex)."},
        "regex": {
            "type": "boolean",
            "default": False,
            "description": "When true, treat query as a regex pattern.",
        },
        "case_sensitive": {
            "type": "boolean",
            "default": False,
            "description": "Case sensitive search when true.",
        },
        "docs_dir": {"type": ["string", "null"], "description": "Optional explicit docs dir override."},
        "include_paths": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Optional list of doc basenames or paths to restrict search (e.g., ['SCENE_SERVER.md']).",
        },
        "context_lines": {
            "type": "integer",
            "default": 2,
            "description": "Number of surrounding lines to include before/after each match.",
        },
        "max_results": {
            "type": "integer",
            "default": 0,
            "description": "0=unlimited (correctness-first). If >0, returns only a window and sets total_matches to the full count.",
        },
        "offset": {
            "type": "integer",
            "default": 0,
            "description": "Skip the first N matches (for paging). Use with max_results to page through large result sets without truncation.",
        },
    },
    "required": ["query"],
}

DOCS_READ_DESCRIPTION = "Read a slice of a markdown doc by line range. Prefer targeted slices over whole-file reads."
DOCS_READ_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "path": {"type": ["string", "null"], "description": "Absolute path to doc (optional if doc_name is provided)."},
        "doc_name": {
            "type": ["string", "null"],
            "description": "Doc basename (e.g., 'SCENE_SERVER.md') to resolve within discovered docs dirs.",
        },
        "docs_dir": {
            "type": ["string", "null"],
            "description": "Optional explicit docs dir override for resolving doc_name.",
        },
        "start_line": {"type": "integer", "description": "0-based start line (use 0 for beginning)."},
        "line_count": {"type": "integer", "description": "Number of lines to return."},
    },
    "required": ["start_line", "line_count"],
}


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


TOOLS: List[Tool] = [
    tool_from_schema(
        name="docs_list",
        description=DOCS_LIST_DESCRIPTION,
        parameters_schema=DOCS_LIST_PARAMETERS,
        handler=_tool_handler("docs_list"),
    ),
    tool_from_schema(
        name="docs_search",
        description=DOCS_SEARCH_DESCRIPTION,
        parameters_schema=DOCS_SEARCH_PARAMETERS,
        handler=_tool_handler("docs_search"),
    ),
    tool_from_schema(
        name="docs_read",
        description=DOCS_READ_DESCRIPTION,
        parameters_schema=DOCS_READ_PARAMETERS,
        handler=_tool_handler("docs_read"),
    ),
]


def _collect_doc_roots(*, atlas_dir: str | None, docs_dir_override: str | None) -> list[Path]:
    roots: list[Path] = []
    if docs_dir_override:
        p = Path(os.path.expanduser(os.path.expandvars(docs_dir_override)))
        roots.append(p)
    else:
        if atlas_dir:
            try:
                roots.append(compute_docs_dir_from_atlas_dir(Path(atlas_dir)))
            except Exception:
                pass
        rr = find_repo_root()
        if rr:
            roots.append(rr / "docs")
    # De-dup while preserving order
    out: list[Path] = []
    seen: set[str] = set()
    for r in roots:
        try:
            key = str(r.resolve())
        except Exception:
            key = str(r)
        if key in seen:
            continue
        seen.add(key)
        out.append(r)
    return out


def _list_markdown_files(root: Path, *, max_depth: int) -> list[Path]:
    files: list[Path] = []
    try:
        root = root.resolve()
    except Exception:
        pass
    if not root.exists() or not root.is_dir():
        return []
    try:
        for cur, dirs, fnames in os.walk(str(root)):
            try:
                rel = Path(cur).relative_to(root)
                if max_depth >= 0 and len(rel.parts) > max_depth:
                    dirs[:] = []
                    continue
            except Exception:
                pass
            for fn in fnames:
                if fn.lower().endswith(".md"):
                    files.append(Path(cur) / fn)
    except Exception:
        return []
    files.sort(key=lambda p: (p.name.lower(), str(p)))
    return files


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    atlas_dir = ctx.atlas_dir
    docs_dir_override = args.get("docs_dir")
    max_depth = int(args.get("max_depth", -1))
    roots = _collect_doc_roots(atlas_dir=atlas_dir, docs_dir_override=docs_dir_override)

    if name == "docs_list":
        docs: list[dict] = []
        for r in roots:
            for p in _list_markdown_files(r, max_depth=max_depth):
                docs.append(
                    {
                        "name": p.name,
                        "path": str(p),
                        "root": str(r),
                    }
                )
        # De-dup by absolute path
        seen_paths: set[str] = set()
        uniq: list[dict] = []
        for d in docs:
            p = d.get("path")
            if not isinstance(p, str):
                continue
            if p in seen_paths:
                continue
            seen_paths.add(p)
            uniq.append(d)
        return json.dumps({"ok": True, "roots": [str(r) for r in roots], "docs": uniq})

    if name == "docs_search":
        query = str(args.get("query") or "")
        if not query:
            return json.dumps({"ok": False, "error": "query is required"})
        use_regex = bool(args.get("regex", False))
        case_sensitive = bool(args.get("case_sensitive", False))
        include_paths = args.get("include_paths") or []
        include_norm = set()
        for it in include_paths:
            if isinstance(it, str) and it.strip():
                include_norm.add(it.strip())
                include_norm.add(os.path.basename(it.strip()))
        context_lines = max(0, int(args.get("context_lines", 2)))
        max_results = max(0, int(args.get("max_results", 0)))
        offset = max(0, int(args.get("offset", 0)))

        flags = 0 if case_sensitive else re.IGNORECASE
        pattern: Optional[re.Pattern[str]] = None
        if use_regex:
            try:
                pattern = re.compile(query, flags=flags)
            except re.error as e:
                return json.dumps({"ok": False, "error": f"invalid regex: {e}"})

        all_files: list[Path] = []
        for r in roots:
            all_files.extend(_list_markdown_files(r, max_depth=max_depth))
        # Optional scope filter
        if include_norm:
            all_files = [
                p
                for p in all_files
                if (p.name in include_norm) or (str(p) in include_norm)
            ]

        matches: list[dict] = []
        total = 0

        def _match_line(line: str) -> bool:
            if pattern is not None:
                return bool(pattern.search(line))
            if case_sensitive:
                return query in line
            return query.lower() in line.lower()

        for p in all_files:
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except Exception:
                continue
            lines = text.splitlines()
            for idx, ln in enumerate(lines):
                if not _match_line(ln):
                    continue
                total += 1
                if total <= offset:
                    continue
                if max_results and len(matches) >= max_results:
                    continue
                start = max(0, idx - context_lines)
                end = min(len(lines), idx + context_lines + 1)
                excerpt = "\n".join(lines[start:end])
                matches.append(
                    {
                        "path": str(p),
                        "line": idx + 1,
                        "excerpt": excerpt,
                    }
                )

        return json.dumps(
            {
                "ok": True,
                "query": query,
                "regex": use_regex,
                "case_sensitive": case_sensitive,
                "roots": [str(r) for r in roots],
                "total_matches": total,
                "results": matches,
                "result_window": {
                    "offset": offset,
                    "max_results": max_results,
                    "context_lines": context_lines,
                },
                "limit_reached": bool(max_results and total > (offset + max_results)),
            }
        )

    if name == "docs_read":
        path = args.get("path")
        doc_name = args.get("doc_name")
        start_line = int(args.get("start_line", 0))
        line_count = int(args.get("line_count", 0))
        if line_count <= 0:
            return json.dumps({"ok": False, "error": "line_count must be > 0"})
        resolved: Optional[Path] = None
        if isinstance(path, str) and path.strip():
            resolved = Path(os.path.expanduser(os.path.expandvars(path.strip())))
        elif isinstance(doc_name, str) and doc_name.strip():
            want = os.path.basename(doc_name.strip())
            candidates: list[str] = []
            for r in roots:
                cand = r / want
                if cand.exists() and cand.is_file():
                    resolved = cand
                    break
                # Also try a slow scan (in case docs are nested)
                for p in _list_markdown_files(r, max_depth=max_depth):
                    if p.name == want:
                        candidates.append(str(p))
            if resolved is None and len(candidates) == 1:
                resolved = Path(candidates[0])
            if resolved is None and candidates:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "doc_name is ambiguous; provide an absolute path",
                        "candidates": candidates,
                    }
                )
            if resolved is None:
                return json.dumps({"ok": False, "error": f"doc not found: {want}"})
        else:
            return json.dumps({"ok": False, "error": "provide path or doc_name"})

        if not resolved.exists() or not resolved.is_file():
            return json.dumps({"ok": False, "error": f"not a file: {str(resolved)}"})
        try:
            text = resolved.read_text(encoding="utf-8", errors="replace")
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})
        lines = text.splitlines()
        if start_line < 0:
            start_line = max(0, len(lines) + start_line)
        end = min(len(lines), start_line + line_count)
        excerpt_lines = lines[start_line:end]
        # Include 1-based line numbers for anchoring
        numbered = "\n".join(
            [f"{i + 1 + start_line:6d}  {ln}" for i, ln in enumerate(excerpt_lines)]
        )
        return json.dumps(
            {
                "ok": True,
                "path": str(resolved),
                "start_line": start_line,
                "line_count": line_count,
                "total_lines": len(lines),
                "text": numbered,
            }
        )

    return None
