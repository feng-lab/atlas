from __future__ import annotations

import base64
import hashlib
import json
import os
import uuid
from pathlib import Path
from typing import Any, Dict, List

from ...tool_registry import Tool, tool_from_schema
from .context import ToolDispatchContext
from .preconditions import require_session_store


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


def _artifacts_dir(ctx: ToolDispatchContext) -> Path:
    ss = ctx.session_store
    if ss is None:
        raise RuntimeError("no session_store available")
    base = ss.root / "artifacts"
    base.mkdir(parents=True, exist_ok=True)
    return base


def _resolve_artifact_path(ctx: ToolDispatchContext, raw_path: Any) -> tuple[Path, str]:
    """Resolve a caller-provided relative path safely under the session artifacts dir."""

    s = str(raw_path or "").strip()
    if not s:
        raise ValueError("path is required")

    # Disallow absolute paths and drive-rooted paths.
    rel = Path(s)
    if rel.is_absolute():
        raise ValueError("path must be a relative path under the session artifacts dir")
    if rel.drive or rel.root:
        raise ValueError("path must not include a drive/root prefix")

    # Normalize and prevent traversal.
    base = _artifacts_dir(ctx)
    base_resolved = base.resolve()
    dest = (base / rel).resolve()
    if not dest.is_relative_to(base_resolved):
        raise ValueError("path resolves outside the session artifacts dir")

    rel_posix = rel.as_posix()
    if rel_posix in (".", ""):
        raise ValueError("path must include a filename")

    return dest, rel_posix


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _atomic_write_bytes(path: Path, data: bytes, *, overwrite: bool) -> None:
    if path.exists() and not overwrite:
        raise FileExistsError(f"refusing to overwrite existing file: {path}")

    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.parent / (".tmp_" + uuid.uuid4().hex)
    try:
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    finally:
        try:
            if tmp.exists():
                tmp.unlink()
        except Exception:
            pass


TOOLS: List[Tool] = [
    tool_from_schema(
        name="artifact_write_text",
        description=(
            "Write a UTF-8 text file under the current session artifacts directory.\n"
            "Safety: only relative paths are allowed; writes are confined to <session>/artifacts.\n"
            "Correctness-first: the file content is written exactly (no truncation)."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Relative path under the session artifacts dir (e.g., 'notes/plan.md').",
                },
                "text": {"type": "string", "description": "UTF-8 text content to write."},
                "overwrite": {
                    "type": "boolean",
                    "default": False,
                    "description": "Overwrite if the file already exists.",
                },
            },
            "required": ["path", "text"],
        },
        handler=_tool_handler("artifact_write_text"),
        preconditions=(require_session_store,),
    ),
    tool_from_schema(
        name="artifact_write_json",
        description=(
            "Write a JSON file under the current session artifacts directory.\n"
            "Safety: only relative paths are allowed; writes are confined to <session>/artifacts.\n"
            "Correctness-first: writes full JSON (no truncation)."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Relative path under the session artifacts dir (e.g., 'run/state.json').",
                },
                "data": {
                    "description": "JSON-serializable value to write (object/array/scalars).",
                    "anyOf": [
                        {"type": "object"},
                        {
                            "type": "array",
                            "items": {
                                "type": ["string", "number", "boolean", "null", "object"]
                            },
                        },
                        {"type": "number"},
                        {"type": "string"},
                        {"type": "boolean"},
                        {"type": "null"},
                    ],
                },
                "pretty": {
                    "type": "boolean",
                    "default": True,
                    "description": "Pretty-print (indent=2) when true; compact JSON otherwise.",
                },
                "overwrite": {
                    "type": "boolean",
                    "default": False,
                    "description": "Overwrite if the file already exists.",
                },
            },
            "required": ["path", "data"],
        },
        handler=_tool_handler("artifact_write_json"),
        preconditions=(require_session_store,),
    ),
    tool_from_schema(
        name="artifact_write_bytes_base64",
        description=(
            "Write a binary file under the current session artifacts directory.\n"
            "The payload is provided as base64.\n"
            "Safety: only relative paths are allowed; writes are confined to <session>/artifacts.\n"
            "Correctness-first: writes full bytes (no truncation)."
        ),
        parameters_schema={
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Relative path under the session artifacts dir (e.g., 'images/frame.png').",
                },
                "data_base64": {
                    "type": "string",
                    "description": "Base64-encoded bytes to write.",
                },
                "overwrite": {
                    "type": "boolean",
                    "default": False,
                    "description": "Overwrite if the file already exists.",
                },
            },
            "required": ["path", "data_base64"],
        },
        handler=_tool_handler("artifact_write_bytes_base64"),
        preconditions=(require_session_store,),
    ),
]


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    try:
        dest, rel_posix = _resolve_artifact_path(ctx, args.get("path"))
    except Exception as e:
        return json.dumps({"ok": False, "error": str(e)})

    overwrite = bool(args.get("overwrite", False))
    base = ""
    try:
        base = str(_artifacts_dir(ctx))
    except Exception:
        base = ""

    if name == "artifact_write_text":
        text = args.get("text")
        if not isinstance(text, str):
            return json.dumps({"ok": False, "error": "text must be a string"})
        try:
            data = text.encode("utf-8", errors="replace")
            _atomic_write_bytes(dest, data, overwrite=overwrite)
            return json.dumps(
                {
                    "ok": True,
                    "artifact_dir": base,
                    "relative_path": rel_posix,
                    "path": str(dest),
                    "bytes": int(len(data)),
                    "sha256": _sha256_bytes(data),
                },
                ensure_ascii=False,
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e), "path": str(dest)})

    if name == "artifact_write_json":
        data = args.get("data")
        pretty = bool(args.get("pretty", True))
        try:
            if pretty:
                text = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True)
            else:
                text = json.dumps(data, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        except Exception as e:
            return json.dumps({"ok": False, "error": f"failed to serialize JSON: {e}"})
        try:
            raw = text.encode("utf-8", errors="replace")
            _atomic_write_bytes(dest, raw, overwrite=overwrite)
            return json.dumps(
                {
                    "ok": True,
                    "artifact_dir": base,
                    "relative_path": rel_posix,
                    "path": str(dest),
                    "bytes": int(len(raw)),
                    "sha256": _sha256_bytes(raw),
                },
                ensure_ascii=False,
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e), "path": str(dest)})

    if name == "artifact_write_bytes_base64":
        b64 = args.get("data_base64")
        if not isinstance(b64, str) or not b64.strip():
            return json.dumps({"ok": False, "error": "data_base64 must be a non-empty string"})
        try:
            data = base64.b64decode(b64.encode("ascii", errors="ignore"), validate=False)
        except Exception as e:
            return json.dumps({"ok": False, "error": f"invalid base64: {e}"})
        try:
            _atomic_write_bytes(dest, data, overwrite=overwrite)
            return json.dumps(
                {
                    "ok": True,
                    "artifact_dir": base,
                    "relative_path": rel_posix,
                    "path": str(dest),
                    "bytes": int(len(data)),
                    "sha256": _sha256_bytes(data),
                },
                ensure_ascii=False,
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e), "path": str(dest)})

    return None
