import bisect
import difflib
import glob
import io
import json
import os
import re
from pathlib import Path
from typing import Any, Dict, List, Optional

# Required internal helper; fail fast if missing
from ...repo import find_repo_root  # type: ignore
from ...tool_registry import Tool, tool_from_schema
from .context import ToolDispatchContext
from .file_formats import SCENE_LOAD_CATEGORIES, get_supported_extensions

# Internal runtime policy: keep heuristics deterministic and stable.
#
# This is used only for a small NUL-byte binary heuristic, not as a read cap.
BOM_SAMPLE_BYTES_FOR_BINARY_HEURISTIC = 4096


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


TOOLS: List[Tool] = [
    tool_from_schema(
        name="fs_expand_paths",
        description="Expand ~ and env vars and normalize paths. Returns expanded absolute-like paths per entry (relative kept relative).",
        parameters_schema={
            "type": "object",
            "properties": {
                "paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Input paths (may contain ~ or env vars)",
                }
            },
            "required": ["paths"],
        },
        handler=_tool_handler("fs_expand_paths"),
    ),
    tool_from_schema(
        name="fs_check_paths",
        description="Check which of the given paths exist on the local filesystem. Returns {exists:[], missing:[]}.",
        parameters_schema={
            "type": "object",
            "properties": {
                "paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Paths to check",
                }
            },
            "required": ["paths"],
        },
        handler=_tool_handler("fs_check_paths"),
    ),
    tool_from_schema(
        name="fs_read_text",
        description="Advanced text read with byte ranges and optional line extraction/filtering. Prefer fs_tail_lines/fs_tail_bytes for simple tails. If both byte and line windows are provided (start/length together with start_line/line_count), the byte window is read first and the line slice is applied within that decoded window. This combination is allowed but discouraged — avoid mixing byte and line windows unless you explicitly want that behavior.",
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to a text file (supports ~ and env var expansion)",
                },
                "start_line": {
                    "type": ["integer", "null"],
                    "description": "Line index (0-based). Negative means offset from end (last line = -1). When combined with start/length, slicing applies within the byte window (discouraged unless intentional).",
                },
                "line_count": {
                    "type": ["integer", "null"],
                    "description": "Number of lines to return from start_line. Omit/null to read to the end. Requires start_line; to tail last N lines use start_line=-N.",
                },
                "regex": {
                    "type": ["string", "null"],
                    "description": "If set, filter returned lines by this regex (applied after slicing). Implies line mode. Discouraged to combine with byte windows unless intentional.",
                },
                "start": {
                    "type": ["integer", "null"],
                    "description": "Start byte offset. Negative values mean offset from end (EOF + start). When combined with start_line/line_count, the line slice is applied to this byte window (discouraged unless intentional).",
                },
                "length": {
                    "type": ["integer", "null"],
                    "description": "Number of bytes to read from start. Omit/null to read to EOF.",
                },
                "encoding": {
                    "type": ["string", "null"],
                    "description": "Force a specific text encoding (default: auto-detect BOM then utf-8).",
                },
                "errors": {
                    "type": "string",
                    "enum": ["strict", "ignore", "replace"],
                    "default": "replace",
                    "description": "Decode error policy.",
                },
            },
            "required": ["path"],
        },
        handler=_tool_handler("fs_read_text"),
    ),
    tool_from_schema(
        name="fs_tail_lines",
        description="Return exactly the last N lines of a text file. BOM-aware (UTF-8/UTF-16/UTF-32). Minimal, robust, no extra params.",
        parameters_schema={
            "type": "object",
            "properties": {"path": {"type": "string"}, "n": {"type": "integer", "default": 200}},
            "required": ["path"],
        },
        handler=_tool_handler("fs_tail_lines"),
    ),
    tool_from_schema(
        name="fs_tail_bytes",
        description="Return the last K bytes of a text file, decoded with BOM-aware detection (UTF-8/UTF-16/UTF-32).",
        parameters_schema={
            "type": "object",
            "properties": {"path": {"type": "string"}, "bytes": {"type": "integer", "default": 4096}},
            "required": ["path"],
        },
        handler=_tool_handler("fs_tail_bytes"),
    ),
    tool_from_schema(
        name="fs_search_text",
        description="Search a text file for a regex and return matches with byte offsets and surrounding line numbers. Correctness-first: searches the requested window (default entire file) without arbitrary caps. For large files, this may be expensive; specify start/length to constrain the window if desired.",
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to file (supports ~ and env var expansion)",
                },
                "regex": {
                    "type": "string",
                    "description": "Regular expression to search. Interpreted on encoded bytes; use case_sensitive=false for ASCII-insensitive search.",
                },
                "start": {
                    "type": ["integer", "null"],
                    "description": "Start byte offset to begin searching (default 0).",
                },
                "length": {
                    "type": ["integer", "null"],
                    "description": "Number of bytes to search from start (default: to EOF).",
                },
                "case_sensitive": {
                    "type": "boolean",
                    "default": True,
                    "description": "ASCII-insensitive match when false (bytes mode).",
                },
                "encoding": {
                    "type": ["string", "null"],
                    "description": "Optional known encoding; otherwise detect BOM then default to utf-8 for pattern encoding only.",
                },
                "max_matches": {
                    "type": "integer",
                    "default": 0,
                    "description": "0=unlimited (correctness-first). If >0, stops after this many matches and sets limit_reached=true.",
                },
            },
            "required": ["path", "regex"],
        },
        handler=_tool_handler("fs_search_text"),
    ),
    tool_from_schema(
        name="fs_read_json",
        description="Read and parse a JSON file from disk. Returns {ok,data}. Always reads the full file.",
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to a JSON file (supports ~ and env var expansion)",
                }
            },
            "required": ["path"],
        },
        handler=_tool_handler("fs_read_json"),
    ),
    tool_from_schema(
        name="fs_resolve_path",
        description="Resolve a possibly-typoed file/dir path using heuristics (expand ~, case-insensitive, pluralization, prefix match, repo search). Returns {ok,path?,candidates?,tried?}.",
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Possibly-typoed path to resolve"},
                "kind": {
                    "type": "string",
                    "enum": ["file", "dir", "either"],
                    "default": "either",
                    "description": "Expected kind",
                },
                "base_dirs": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional extra search bases",
                },
                "max_results": {"type": "integer", "default": 10, "description": "Max candidate results"},
                "max_depth": {
                    "type": "integer",
                    "default": 6,
                    "description": "Max recursive depth when searching anchors",
                },
            },
            "required": ["path"],
        },
        handler=_tool_handler("fs_resolve_path"),
    ),
    tool_from_schema(
        name="fs_hint_resolve",
        description=(
            "Resolve a likely file/dir path by searching caller-provided directories.\n"
            "The caller (LLM) must provide structured inputs:\n"
            "- expected_name: basename to score against (e.g., 'foo.txt')\n"
            "- possible_dirs: likely directories to search (e.g., ['~/Documents/atlas_test'])\n\n"
            "Search behavior:\n"
            "- Only searches within possible_dirs (no implicit hint parsing).\n"
            "- Returns the best-ranked candidate path and a ranked candidate list.\n"
            "- expected_name is used for scoring (not an exact-match contract). The result includes match='exact' when the best candidate basename matches expected_name case-insensitively.\n\n"
            "Return shape:\n"
            "- Found: {ok:true, path, match:'exact'|'best_candidate', expected_name, candidates, searched_dirs, missing_dirs, total_candidates, limit_reached, hint?}\n"
            "- Not found: {ok:false, error:'not_found', expected_name, searched_dirs, missing_dirs, hint}"
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "expected_name": {
                    "type": "string",
                    "description": "Basename to score against (e.g., foo.txt). Not treated as an exact-match contract.",
                },
                "kind": {"type": "string", "enum": ["file", "dir", "either"], "default": "file"},
                "possible_dirs": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Likely search directories (absolute or ~ expanded).",
                },
                "max_results": {"type": "integer", "default": 12},
                "max_depth": {"type": "integer", "default": 6},
            },
            "required": ["expected_name", "possible_dirs"],
        },
        handler=_tool_handler("fs_hint_resolve"),
    ),
    tool_from_schema(
        name="fs_glob",
        description="List files matching a pattern inside a directory. Expands ~ and env vars. Example: dir='~/Documents/atlas_test/slice15', pattern='*.lsm'",
        parameters_schema={
            "type": "object",
            "properties": {
                "dir": {"type": "string", "description": "Directory to glob"},
                "pattern": {"type": "string", "default": "*", "description": "Glob pattern"},
                "recursive": {"type": "boolean", "default": False, "description": "Recurse into subdirs"},
            },
            "required": ["dir"],
        },
        handler=_tool_handler("fs_glob"),
    ),
    tool_from_schema(
        name="fs_find_candidates",
        description="Resolve candidate file paths by trying directories and extensions; returns existing absolute paths.",
        parameters_schema={
            "type": "object",
            "properties": {
                "dirs": {"type": "array", "items": {"type": "string"}, "description": "Search directories"},
                "names": {"type": "array", "items": {"type": "string"}, "description": "Basenames to resolve"},
                "extensions": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Allowed extensions",
                },
                "schema_dir": {"type": "string", "description": "Optional schema directory override for extension catalogs"},
                "case_insensitive": {"type": "boolean", "default": True, "description": "Case-insensitive matching"},
            },
            "required": ["dirs", "names"],
        },
        handler=_tool_handler("fs_find_candidates"),
    ),
]


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    client = ctx.client
    atlas_dir = ctx.atlas_dir
    dispatch = ctx.dispatch
    _param_to_dict = ctx.param_to_dict
    _resolve_json_key = ctx.resolve_json_key
    _json_key_exists = ctx.json_key_exists
    _schema_validator_cache = ctx.schema_validator_cache

    if name == "fs_expand_paths":
        paths = [str(p) for p in (args.get("paths") or [])]
        out: list[str] = []
        for p in paths:
            t = os.path.expanduser(os.path.expandvars(p))
            # Normalize separators for current OS
            t = os.path.normpath(t)
            out.append(t)
        return json.dumps({"ok": True, "paths": out})

    if name == "fs_check_paths":
        paths = [str(p) for p in (args.get("paths") or [])]
        exists: list[str] = []
        missing: list[str] = []
        for p in paths:
            if os.path.exists(p):
                exists.append(p)
            else:
                missing.append(p)
        return json.dumps({"ok": True, "exists": exists, "missing": missing})

    if name == "fs_read_text":

        p = str(args.get("path") or "")

        # Helpers to coerce numeric args
        def _as_pos_int(v, default=None):
            try:
                i = int(v)
                return i if i >= 0 else default
            except Exception:
                return default

        start_raw = args.get("start")
        try:
            start = int(start_raw) if start_raw is not None else None
        except Exception:
            start = None
        length = _as_pos_int(args.get("length"), None)
        # New unified line options
        try:
            start_line = (
                int(args.get("start_line"))
                if args.get("start_line") is not None
                else None
            )
        except Exception:
            start_line = None
        line_count = _as_pos_int(args.get("line_count"), None)
        regex = args.get("regex")
        force_enc = args.get("encoding")
        errors = str(args.get("errors", "replace"))
        # Internal streaming chunk size (not user-configurable)
        block_size = 65536
        try:
            q = os.path.expanduser(os.path.expandvars(p))
            # Enforce symmetry: line_count requires start_line (use negative start_line for tail)
            if (line_count is not None) and (start_line is None):
                return json.dumps(
                    {
                        "ok": False,
                        "error": "line_count_requires_start_line",
                        "hint": "Provide start_line (use negative for tail) or use fs_tail_lines",
                    }
                )
            # Resolve byte window using seek/tell when possible so tail refers to current EOF
            rb_start: int = 0
            size: Optional[int] = None
            data: bytes
            with open(q, "rb") as f:
                # Try to get current size from file descriptor (may fail for special files)
                try:
                    size = os.fstat(f.fileno()).st_size
                except Exception:
                    size = None
                # Determine absolute start; negative means from EOF
                if start is None:
                    rb_start = 0
                else:
                    if start >= 0:
                        rb_start = min(start, size if isinstance(size, int) else start)
                    else:
                        # Negative start: offset from end
                        try:
                            f.seek(0, io.SEEK_END)
                            endpos = f.tell()
                            size = endpos
                        except Exception:
                            endpos = None
                        rb_start = max(
                            0, (size if isinstance(size, int) else 0) + start
                        )
                # Position the stream (seek or stream-skip)
                try:
                    f.seek(rb_start)
                except Exception:
                    remaining = rb_start
                    while remaining > 0:
                        chunk = f.read(min(block_size, remaining))
                        if not chunk:
                            break
                        remaining -= len(chunk)
                # Decide bytes to read
                if isinstance(length, int) and length >= 0:
                    to_read = length
                    data = f.read(max(0, to_read))
                else:
                    if isinstance(size, int) and size >= 0:
                        to_read = max(0, size - rb_start)
                        data = f.read(max(0, to_read))
                    else:
                        # Unknown size: read to EOF
                        data = f.read()
                        to_read = len(data)
            # Simple encoding detection: BOM, then utf-8 fallback
            enc = None
            if isinstance(force_enc, str) and force_enc:
                enc = force_enc
            else:
                if data.startswith(b"\xef\xbb\xbf"):
                    enc = "utf-8-sig"
                elif data.startswith(b"\xfe\xff") or data.startswith(b"\xff\xfe"):
                    enc = "utf-16"
                else:
                    # Try utf-8 strict, then fallback to utf-8 replace
                    try:
                        _ = data.decode("utf-8", errors="strict")
                        enc = "utf-8"
                    except Exception:
                        enc = "utf-8"
            # Binary heuristic
            sample = data[:BOM_SAMPLE_BYTES_FOR_BINARY_HEURISTIC]
            is_binary = b"\x00" in sample
            text = data.decode(
                enc or "utf-8",
                errors=(
                    errors if errors in ("strict", "ignore", "replace") else "replace"
                ),
            )
            lines = None
            want_lines = bool(
                (start_line is not None)
                or (line_count is not None)
                or (isinstance(regex, str) and regex)
            )
            if want_lines:
                try:
                    all_lines = text.splitlines()
                    # Compute slice by start_line/line_count on the current window
                    if start_line is not None:
                        idx0 = (
                            start_line
                            if start_line >= 0
                            else max(0, len(all_lines) + start_line)
                        )
                    else:
                        idx0 = 0
                    if line_count is not None and line_count >= 0:
                        idx1 = min(len(all_lines), idx0 + line_count)
                    else:
                        idx1 = len(all_lines)
                    sliced = all_lines[idx0:idx1]
                    # Apply regex after slicing (if provided)
                    regex_error = None
                    if isinstance(regex, str) and regex:
                        try:
                            rx = re.compile(regex)
                            sliced = [ln for ln in sliced if rx.search(ln)]
                        except Exception as _e:
                            regex_error = str(_e)
                    lines = sliced
                except Exception:
                    lines = text.splitlines()
            truncated = False
            # Determine truncation based on requested window
            # If no explicit window and read less than file size (i.e., did not read the full window), mark truncated
            read_from = rb_start
            read_to = rb_start + len(data)
            if isinstance(size, int) and size is not None and size >= 0:
                if isinstance(length, int) and length is not None and length >= 0:
                    window = length
                else:
                    window = size - rb_start
                if window is not None and len(data) < max(0, window):
                    truncated = True
            else:
                # Unknown size: cannot assert truncation
                pass
            out = {
                "ok": True,
                "path": q,
                "encoding": enc or "utf-8",
                "size_bytes": (size if isinstance(size, int) and size >= 0 else None),
                "bytes": len(data),
                "read_from": read_from,
                "read_to": read_to,
                "truncated": bool(truncated),
                "is_binary": bool(is_binary),
            }
            if want_lines:
                out["lines"] = lines or []
                if "regex_error" in locals() and regex_error:
                    out["regex_error"] = regex_error
            else:
                out["text"] = text
            return json.dumps(out)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "fs_tail_lines":

        p = str(args.get("path") or "")
        n = int(args.get("n", 200))
        if n <= 0:
            n = 1
        try:
            q = os.path.expanduser(os.path.expandvars(p))
            with open(q, "rb") as f:
                # Detect BOM/encoding from file start (not tail buffer)
                try:
                    f.seek(0, io.SEEK_SET)
                    _hdr = f.read(4)
                except Exception:
                    _hdr = b""
                if _hdr.startswith(b"\xef\xbb\xbf"):
                    enc = "utf-8-sig"
                elif _hdr.startswith(b"\xff\xfe\x00\x00") or _hdr.startswith(
                    b"\x00\x00\xfe\xff"
                ):
                    enc = "utf-32"
                elif _hdr.startswith(b"\xfe\xff") or _hdr.startswith(b"\xff\xfe"):
                    enc = "utf-16"
                else:
                    enc = "utf-8"
                try:
                    size = os.fstat(f.fileno()).st_size
                except Exception:
                    size = None
                # Scan from EOF until we have N+1 newlines or reach BOF (no arbitrary caps)
                f.seek(0, io.SEEK_END)
                endpos = f.tell()
                pos = endpos
                block_size = 65536
                buf = bytearray()
                lines_found = 0
                while pos > 0 and lines_found < (n + 1):
                    step = min(block_size, pos)
                    new_start = pos - step
                    f.seek(new_start)
                    chunk = f.read(step)
                    if not chunk:
                        break
                    buf[:0] = chunk
                    pos = new_start
                    lines_found = buf.count(b"\n")
                rb_start = endpos - len(buf)
                data = bytes(buf)
            # Decode using detected BOM-aware encoding, then take last N lines exactly
            text = data.decode(enc, errors="replace")
            lines_all = text.splitlines()
            lines_out = lines_all[-n:] if len(lines_all) > n else lines_all
            # No truncation in correctness-first mode: we scan to BOF or until N lines are found
            return json.dumps(
                {
                    "ok": True,
                    "path": q,
                    "encoding": enc,
                    "size_bytes": (
                        size if isinstance(size, int) and size >= 0 else None
                    ),
                    "bytes": len(data),
                    "read_from": rb_start,
                    "read_to": endpos,
                    "truncated": False,
                    "lines": lines_out,
                }
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "fs_tail_bytes":

        p = str(args.get("path") or "")
        k = int(args.get("bytes", 4096))
        if k <= 0:
            k = 1
        try:
            q = os.path.expanduser(os.path.expandvars(p))
            with open(q, "rb") as f:
                # Detect BOM/encoding from file start
                try:
                    f.seek(0, io.SEEK_SET)
                    _hdr = f.read(4)
                except Exception:
                    _hdr = b""
                if _hdr.startswith(b"\xef\xbb\xbf"):
                    enc = "utf-8-sig"
                elif _hdr.startswith(b"\xff\xfe\x00\x00") or _hdr.startswith(
                    b"\x00\x00\xfe\xff"
                ):
                    enc = "utf-32"
                elif _hdr.startswith(b"\xfe\xff") or _hdr.startswith(b"\xff\xfe"):
                    enc = "utf-16"
                else:
                    enc = "utf-8"
                f.seek(0, io.SEEK_END)
                endpos = f.tell()
                start = max(0, endpos - k)
                f.seek(start)
                data = f.read(endpos - start)
            text = data.decode(enc, errors="replace")
            return json.dumps(
                {
                    "ok": True,
                    "path": q,
                    "encoding": enc,
                    "bytes": len(data),
                    "read_from": start,
                    "read_to": endpos,
                    "text": text,
                }
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "fs_search_text":

        p = str(args.get("path") or "")
        pattern = str(args.get("regex") or "")
        start_raw = args.get("start")
        try:
            start = int(start_raw) if start_raw is not None else 0
        except Exception:
            start = 0
        length_raw = args.get("length")
        try:
            length = int(length_raw) if length_raw is not None else None
        except Exception:
            length = None
        case_sensitive = bool(args.get("case_sensitive", True))
        force_enc = args.get("encoding")
        max_matches = int(args.get("max_matches", 0))
        if max_matches < 0:
            max_matches = 0
        try:
            q = os.path.expanduser(os.path.expandvars(p))
            with open(q, "rb") as f:
                # Determine file size
                try:
                    size = os.fstat(f.fileno()).st_size
                except Exception:
                    size = None
                # Detect BOM/encoding from file start for pattern encoding only
                try:
                    f.seek(0, io.SEEK_SET)
                    _hdr = f.read(4)
                except Exception:
                    _hdr = b""
                if isinstance(force_enc, str) and force_enc:
                    enc = force_enc
                elif _hdr.startswith(b"\xef\xbb\xbf"):
                    enc = "utf-8-sig"
                elif _hdr.startswith(b"\xff\xfe\x00\x00") or _hdr.startswith(
                    b"\x00\x00\xfe\xff"
                ):
                    enc = "utf-32"
                elif _hdr.startswith(b"\xfe\xff") or _hdr.startswith(b"\xff\xfe"):
                    enc = "utf-16"
                else:
                    enc = "utf-8"
                # Compute search window
                if not isinstance(size, int):
                    # Unknown size: read to EOF from start, ignoring length if provided
                    if start < 0:
                        # Seek to end to compute absolute start from EOF
                        try:
                            f.seek(0, io.SEEK_END)
                            endpos = f.tell()
                        except Exception:
                            endpos = 0
                        scan_start = max(0, endpos + start)
                    else:
                        scan_start = max(0, start)
                    f.seek(scan_start)
                    buf = f.read()  # correctness-first
                    scan_end = scan_start + len(buf)
                    size_val = None
                else:
                    size_val = size
                    if start is None:
                        scan_start = 0
                    else:
                        if start >= 0:
                            scan_start = min(start, size)
                        else:
                            scan_start = max(0, size + start)
                    if isinstance(length, int) and length >= 0:
                        scan_end = min(size, scan_start + length)
                    else:
                        scan_end = size
                    f.seek(scan_start)
                    buf = f.read(max(0, scan_end - scan_start))
            # Prepare regex on bytes. Encode pattern with detected encoding.
            try:
                pat_b = pattern.encode(enc)
            except Exception as _e:
                return json.dumps(
                    {"ok": False, "error": f"pattern_encoding_error: {str(_e)}"}
                )
            flags = 0
            if not case_sensitive:
                flags |= re.IGNORECASE
            rx = re.compile(pat_b, flags)
            # Precompute newline byte positions in the window for line number math
            nl_positions = []
            try:
                # Fast scan for '\n' bytes
                idx = buf.find(b"\n")
                while idx != -1:
                    nl_positions.append(idx)
                    idx = buf.find(b"\n", idx + 1)
            except Exception:
                nl_positions = []
            # Base line number: count '\n' from BOF to scan_start in streaming fashion
            base_line = 0
            try:
                with open(q, "rb") as f2:
                    remaining = scan_start
                    block = 1 << 20  # 1 MiB blocks
                    while remaining > 0:
                        step = min(block, remaining)
                        chunk = f2.read(step)
                        if not chunk:
                            break
                        base_line += chunk.count(b"\n")
                        remaining -= len(chunk)
            except Exception:
                base_line = 0
            # Find matches
            matches = []
            limit_reached = False
            for m in rx.finditer(buf):
                b0 = scan_start + m.start()
                b1 = scan_start + m.end()
                # Start/end line numbers within window via binary search on nl_positions
                # Count of newlines strictly before m.start()
                nl_before_start = bisect.bisect_left(nl_positions, m.start())
                nl_before_end = bisect.bisect_left(nl_positions, m.end())
                s_line = base_line + nl_before_start
                e_line = base_line + nl_before_end
                matches.append(
                    {
                        "byte_start": int(b0),
                        "byte_end": int(b1),
                        "start_line": int(s_line),
                        "end_line": int(e_line),
                    }
                )
                if max_matches > 0 and len(matches) >= max_matches:
                    limit_reached = True
                    break
            out = {
                "ok": True,
                "path": q,
                "encoding": enc,
                "size_bytes": (size_val if isinstance(size_val, int) else None),
                "search_from": scan_start,
                "search_to": scan_end,
                "matches": matches,
            }
            if max_matches > 0:
                out["limit_reached"] = bool(limit_reached)
            return json.dumps(out)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "fs_read_json":
        p = str(args.get("path") or "")
        try:
            q = os.path.expanduser(os.path.expandvars(p))
            size = None
            try:
                size = os.stat(q).st_size
            except Exception:
                size = None
            with open(q, "rb") as f:
                data = f.read()
            # BOM-aware UTF-8 decoding for JSON
            if data.startswith(b"\xef\xbb\xbf"):
                text = data.decode("utf-8-sig", errors="strict")
            else:
                text = data.decode("utf-8", errors="strict")
            obj = json.loads(text)
            return json.dumps(
                {
                    "ok": True,
                    "data": obj,
                    "size_bytes": (size if isinstance(size, int) else None),
                }
            )
        except Exception as e:
            # Surface JSON errors clearly
            msg = str(e)
            return json.dumps({"ok": False, "error": msg})

    if name == "fs_resolve_path":

        path_in = str(args.get("path") or "")
        kind = str(args.get("kind")) if args.get("kind") else "either"
        max_results = int(args.get("max_results", 10))
        max_depth = int(args.get("max_depth", 6))
        base_dirs = [str(p) for p in (args.get("base_dirs") or [])]
        tried: list[str] = []

        def _exists(p: str) -> bool:
            if kind == "file":
                return os.path.isfile(p)
            if kind == "dir":
                return os.path.isdir(p)
            return os.path.exists(p)

        # Expand and normalize
        p0 = os.path.expanduser(os.path.expandvars(path_in))
        p0 = os.path.normpath(p0)
        tried.append(p0)
        if _exists(p0):
            return json.dumps({"ok": True, "path": os.path.abspath(p0), "tried": tried})

        # Heuristic 1: case-insensitive correction per segment
        def _case_fix(p: str) -> str | None:
            parts = Path(p).parts
            if not parts:
                return None
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
            return q if _exists(q) else None

        cf = _case_fix(p0)
        if cf and cf not in tried:
            tried.append(cf)
            if _exists(cf):
                return json.dumps(
                    {
                        "ok": True,
                        "path": os.path.abspath(cf),
                        "tried": tried,
                        "reason": "case-insensitive match",
                    }
                )

        # Heuristic 2: pluralization/singularization on each segment (single change)
        def _plural_variants(p: str):
            parts = list(Path(p).parts)
            for i in range(len(parts)):
                alt = parts.copy()
                seg = alt[i]
                if seg.endswith("s"):
                    alt[i] = seg[:-1]
                else:
                    alt[i] = seg + "s"
                yield str(Path(*alt))

        for cand in _plural_variants(p0):
            c2 = os.path.normpath(cand)
            if c2 not in tried:
                tried.append(c2)
                if _exists(c2):
                    return json.dumps(
                        {
                            "ok": True,
                            "path": os.path.abspath(c2),
                            "tried": tried,
                            "reason": "pluralization",
                        }
                    )
        # Heuristic 3: simple prefix match near failing leaf
        try:
            parent, leaf = os.path.split(p0)
            if parent and os.path.isdir(parent) and leaf:
                for fname in os.listdir(parent):
                    if fname.lower().startswith(leaf.lower()):
                        c3 = os.path.join(parent, fname)
                        if c3 not in tried:
                            tried.append(c3)
                            if _exists(c3):
                                return json.dumps(
                                    {
                                        "ok": True,
                                        "path": os.path.abspath(c3),
                                        "tried": tried,
                                        "reason": "prefix match",
                                    }
                                )
        except Exception:
            pass
        # Fallback: anchored search around nearest existing ancestor + common user dirs + optional bases
        repo_root = find_repo_root()
        # Find nearest existing ancestor directory of p0
        anc = Path(p0)
        while anc and not anc.exists():
            anc = anc.parent
        anchor_dirs: list[str] = []
        if anc and anc.exists():
            # Avoid scanning the home directory root; only add ancestor if it is not the home root
            try:
                _home = Path(os.path.expanduser("~")).resolve()
            except Exception:
                _home = None
            anc_dir = anc if anc.is_dir() else anc.parent
            if not (_home and anc_dir.resolve() == _home):
                anchor_dirs.append(str(anc_dir))
        # Common user dirs
        home = os.path.expanduser("~")
        docs = os.path.join(home, "Documents")
        dlds = os.path.join(home, "Downloads")
        desk = os.path.join(home, "Desktop")
        for d in (docs, dlds, desk):
            if os.path.isdir(d):
                anchor_dirs.append(d)
        # If the path mentions a subfolder (e.g., atlas_test), try doc/subfolder
        segs = [s for s in Path(p0).parts if s not in ("/", "\\")]
        for s in segs:
            cand = os.path.join(docs, s)
            if os.path.isdir(cand):
                anchor_dirs.append(cand)
        # Add optional bases and repo last
        anchor_dirs.extend(base_dirs)
        if repo_root:
            anchor_dirs.append(str(repo_root))
        # De-dupe while preserving order
        seen = set()
        search_roots = []
        for r in anchor_dirs:
            if r and r not in seen and os.path.isdir(r):
                seen.add(r)
                search_roots.append(r)
        target = Path(path_in).name
        candidates: list[tuple[float, str]] = []

        def _walk_with_depth(root: str, max_depth: int):
            root_p = Path(root)
            try:
                for cur, dirs, files in os.walk(root):
                    rel = Path(cur).relative_to(root_p)
                    if len(rel.parts) > max_depth:
                        dirs[:] = []
                        continue
                    for nm in files if kind != "dir" else dirs:
                        full = os.path.join(cur, nm)
                        if kind == "file" and not os.path.isfile(full):
                            continue
                        if kind == "dir" and not os.path.isdir(full):
                            continue
                        score = difflib.SequenceMatcher(
                            a=nm.lower(), b=target.lower()
                        ).ratio()
                        if score >= 0.5:
                            candidates.append((score, os.path.abspath(full)))
            except Exception:
                return
        # Normalize and filter invalid roots
        search_roots = [r for r in search_roots if r and os.path.isdir(r)]
        for r in search_roots:
            _walk_with_depth(r, max_depth)
            tried.append(r)
        candidates.sort(key=lambda x: x[0], reverse=True)
        out = [{"path": p, "score": float(s)} for (s, p) in candidates[:max_results]]
        return json.dumps({"ok": False, "candidates": out, "tried": tried})

    if name == "fs_hint_resolve":

        expected_name = str(args.get("expected_name") or "").strip()
        possible_dirs_raw = args.get("possible_dirs")
        kind = str(args.get("kind") or "file")
        max_results = int(args.get("max_results", 12))
        max_depth = int(args.get("max_depth", 6))

        if not expected_name:
            return json.dumps(
                {
                    "ok": False,
                    "error": "expected_name is required",
                }
            )
        if not isinstance(possible_dirs_raw, list) or not possible_dirs_raw:
            return json.dumps(
                {
                    "ok": False,
                    "error": "possible_dirs is required (non-empty array)",
                    "expected_name": expected_name,
                }
            )
        possible_dirs = [str(p) for p in possible_dirs_raw if isinstance(p, str) and p.strip()]
        if not possible_dirs:
            return json.dumps(
                {
                    "ok": False,
                    "error": "possible_dirs is required (non-empty array of strings)",
                    "expected_name": expected_name,
                }
            )

        # Expand and validate search roots. We keep both searched and missing dirs
        # for transparency/debugging.
        searched_dirs: list[str] = []
        missing_dirs: list[str] = []
        for d in possible_dirs:
            d2 = os.path.expanduser(os.path.expandvars(d))
            d2 = os.path.normpath(d2)
            if os.path.isdir(d2):
                searched_dirs.append(d2)
            else:
                missing_dirs.append(d2)
        # De-duplicate searched dirs while preserving order
        _seen_root: set[str] = set()
        searched_u: list[str] = []
        for r in searched_dirs:
            rn = os.path.normpath(r)
            if rn in _seen_root:
                continue
            _seen_root.add(rn)
            searched_u.append(rn)
        searched_dirs = searched_u
        if not searched_dirs:
            return json.dumps(
                {
                    "ok": False,
                    "error": "not_found",
                    "expected_name": expected_name,
                    "searched_dirs": [],
                    "missing_dirs": missing_dirs,
                    "hint": "None of the provided possible_dirs exist on disk.",
                }
            )

        target = expected_name.lower()
        candidates: list[tuple[float, str]] = []

        def _walk_with_depth(root: str, max_depth: int) -> None:
            root_p = Path(root)
            for cur, dirs, files in os.walk(root):
                rel = Path(cur).relative_to(root_p)
                if len(rel.parts) > max_depth:
                    dirs[:] = []
                    continue
                names = files if kind != "dir" else dirs
                for nm in names:
                    full = os.path.join(cur, nm)
                    if kind == "file" and not os.path.isfile(full):
                        continue
                    if kind == "dir" and not os.path.isdir(full):
                        continue
                    score = (
                        1.0
                        if nm.lower() == target
                        else difflib.SequenceMatcher(a=nm.lower(), b=target).ratio()
                    )
                    if score < 0.5:
                        continue
                    candidates.append((float(score), os.path.abspath(full)))

        try:
            for r in searched_dirs:
                _walk_with_depth(r, max_depth)
        except Exception as e:
            return json.dumps(
                {
                    "ok": False,
                    "error": str(e),
                    "expected_name": expected_name,
                    "searched_dirs": searched_dirs,
                    "missing_dirs": missing_dirs,
                }
            )

        # De-duplicate candidates by absolute path (keep best score).
        best_by_path: dict[str, float] = {}
        for score, p in candidates:
            prev = best_by_path.get(p)
            if prev is None or score > prev:
                best_by_path[p] = float(score)
        candidates_u = [(s, p) for (p, s) in best_by_path.items()]
        candidates_u.sort(key=lambda x: float(x[0]), reverse=True)

        total_candidates = len(candidates_u)
        limit_reached = bool(max_results and total_candidates > max_results)
        out = [{"path": p, "score": float(s)} for (s, p) in candidates_u[:max_results]]

        if not out:
            return json.dumps(
                {
                    "ok": False,
                    "error": "not_found",
                    "expected_name": expected_name,
                    "candidates": [],
                    "total_candidates": 0,
                    "limit_reached": False,
                    "searched_dirs": searched_dirs,
                    "missing_dirs": missing_dirs,
                    "hint": "No candidates were found under the searched_dirs. Consider increasing max_depth or providing more specific possible_dirs.",
                }
            )

        best = out[0]
        exact = bool(
            expected_name
            and os.path.basename(best["path"]).lower() == str(expected_name).lower()
        )
        match = "exact" if exact else "best_candidate"

        payload: dict[str, Any] = {
            "ok": True,
            "path": best["path"],
            "match": match,
            "expected_name": expected_name,
            "candidates": out,
            "total_candidates": total_candidates,
            "limit_reached": limit_reached,
            "searched_dirs": searched_dirs,
            "missing_dirs": missing_dirs,
        }
        if expected_name and not exact:
            payload["hint"] = (
                "Best candidate basename does not exactly match expected_name. "
                "Validate by reading/checking the file contents before using it."
            )
        return json.dumps(payload)

    if name == "repo_search":

        query = str(args.get("name") or "")
        typ = str(args.get("type")) if args.get("type") else "either"
        max_depth = int(args.get("max_depth", 6))
        max_results = int(args.get("max_results", 20))
        repo_root = find_repo_root()
        if not repo_root:
            try:
                repo_root = Path.cwd()
            except Exception:
                repo_root = Path(".")
        target = query.lower()
        hits: list[tuple[float, str]] = []
        try:
            for cur, dirs, files in os.walk(str(repo_root)):
                rel = Path(cur).relative_to(repo_root)
                if len(rel.parts) > max_depth:
                    dirs[:] = []
                    continue

                def _consider(nm: str):
                    full = os.path.join(cur, nm)
                    if typ == "file" and not os.path.isfile(full):
                        return
                    if typ == "dir" and not os.path.isdir(full):
                        return
                    score = difflib.SequenceMatcher(a=nm.lower(), b=target).ratio()
                    if score >= 0.5:
                        hits.append((score, os.path.abspath(full)))

                for nm in dirs:
                    _consider(nm)
                for nm in files:
                    _consider(nm)
        except Exception:
            pass
        hits.sort(key=lambda x: x[0], reverse=True)
        out = [{"path": p, "score": float(s)} for (s, p) in hits[:max_results]]
        return json.dumps({"ok": True, "results": out})

    if name == "fs_glob":

        d = str(args.get("dir") or "")
        pattern = str(args.get("pattern") or "*")
        rec = bool(args.get("recursive", False))
        base = os.path.expanduser(os.path.expandvars(d))
        if not os.path.isdir(base):
            return json.dumps({"ok": False, "error": f"not a directory: {base}"})
        pat = os.path.join(base, pattern)
        matches = glob.glob(pat, recursive=rec)
        files = [os.path.abspath(m) for m in matches if os.path.isfile(m)]
        return json.dumps({"ok": True, "files": files, "dir": base, "pattern": pattern})

    if name == "fs_find_candidates":

        dirs = args.get("dirs") or []
        names = args.get("names") or []
        schema_dir_override = args.get("schema_dir")
        exts_arg = args.get("extensions") or []
        exts = [str(e) for e in exts_arg if isinstance(e, str) and e]
        if not exts:
            exts = get_supported_extensions(schema_dir_override, atlas_dir, categories=SCENE_LOAD_CATEGORIES)
        ci = bool(args.get("case_insensitive", True))
        out: list[str] = []

        def variants(nm: str) -> list[str]:
            base, ext = os.path.splitext(nm)
            cand = [nm]
            if not ext:
                cand.extend([base + e for e in exts])
            return cand

        for d in dirs:
            d2 = os.path.expanduser(os.path.expandvars(str(d)))
            for nm in names:
                for cand in variants(nm):
                    p = os.path.join(d2, cand)
                    if os.path.exists(p):
                        out.append(os.path.abspath(p))
                    elif ci:
                        # Try case-insensitive match by scanning directory
                        try:
                            target = cand.lower()
                            for fname in os.listdir(d2):
                                if fname.lower() == target and os.path.exists(
                                    os.path.join(d2, fname)
                                ):
                                    out.append(os.path.abspath(os.path.join(d2, fname)))
                                    break
                        except Exception:
                            pass
        return json.dumps({"ok": True, "candidates": out})

    return None
