import json
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List

from ...codegen_policy import allowed_imports_status
from ...defaults import (
    DEFAULT_CODEGEN_MAX_ECHO_CHARS,
    DEFAULT_CODEGEN_STDIO_PREVIEW_CHARS,
    DEFAULT_CODEGEN_TIMEOUT_SEC,
)
from ...repo import find_repo_root  # type: ignore
from ...tool_registry import Tool, tool_from_schema
from .context import ToolDispatchContext
from .preconditions import require_codegen_enabled

REPORT_BLOCKED_DESCRIPTION = "Declare that execution is blocked or not feasible. Use precise reason/details so the user can take action."
REPORT_BLOCKED_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "reason": {
            "type": "string",
            "description": "Short reason (e.g., json_key_not_found, option_invalid, tool_missing)",
        },
        "details": {
            "type": "string",
            "description": "Specifics: id/json_key/value/time or missing option/label names",
        },
        "suggestion": {
            "type": "string",
            "description": "Optional next step suggestion for the user",
        },
    },
    "required": ["reason"],
}

SYSTEM_INFO_DESCRIPTION = "Return OS/platform info and common paths so the agent can reason about file locations."
SYSTEM_INFO_PARAMETERS: Dict[str, Any] = {"type": "object", "properties": {}}

PYTHON_WRITE_AND_RUN_DESCRIPTION = (
    "Write a Python script to a temp file and run it with the repo root on PYTHONPATH.\n"
    "Correctness-first: stdout/stderr are NOT truncated or dropped; full outputs are written to files and "
    "returned as stdout_path/stderr_path, along with small preview strings for convenience."
)
PYTHON_WRITE_AND_RUN_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "script": {
            "type": "string",
            "description": "Python source code",
        },
        "filename": {
            "type": "string",
            "description": "Optional filename for the script (for logging)",
        },
        "args": {
            "type": "array",
            "items": {"type": "string"},
            "default": [],
            "description": "argv to pass to the script",
        },
        "timeout_sec": {
            "type": "number",
            "default": DEFAULT_CODEGEN_TIMEOUT_SEC,
            "description": "Execution timeout",
        },
        "echo_script": {
            "type": "boolean",
            "default": True,
            "description": "Include script echo in response",
        },
        "max_echo_chars": {
            "type": "integer",
            "default": DEFAULT_CODEGEN_MAX_ECHO_CHARS,
            "description": "Max script chars to echo",
        },
    },
    "required": ["script"],
}

CODEGEN_ALLOWED_IMPORTS_DESCRIPTION = "Return the current codegen allowed import modules and whether each is importable in this environment."
CODEGEN_ALLOWED_IMPORTS_PARAMETERS: Dict[str, Any] = {"type": "object", "properties": {}}


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


TOOLS: List[Tool] = [
    tool_from_schema(
        name="report_blocked",
        description=REPORT_BLOCKED_DESCRIPTION,
        parameters_schema=REPORT_BLOCKED_PARAMETERS,
        handler=_tool_handler("report_blocked"),
    ),
    tool_from_schema(
        name="system_info",
        description=SYSTEM_INFO_DESCRIPTION,
        parameters_schema=SYSTEM_INFO_PARAMETERS,
        handler=_tool_handler("system_info"),
    ),
    tool_from_schema(
        name="python_write_and_run",
        description=PYTHON_WRITE_AND_RUN_DESCRIPTION,
        parameters_schema=PYTHON_WRITE_AND_RUN_PARAMETERS,
        handler=_tool_handler("python_write_and_run"),
        preconditions=(require_codegen_enabled,),
    ),
    tool_from_schema(
        name="codegen_allowed_imports",
        description=CODEGEN_ALLOWED_IMPORTS_DESCRIPTION,
        parameters_schema=CODEGEN_ALLOWED_IMPORTS_PARAMETERS,
        handler=_tool_handler("codegen_allowed_imports"),
        preconditions=(require_codegen_enabled,),
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

    if name == "report_blocked":
        # Mark this turn as blocked so the phase runner can avoid:
        # - post-write facts snapshots (which may trigger additional RPC calls)
        # - Verifier phase (which would likely fail/retry and add noise)
        #
        # This is especially important when the block reason is RPC-related (e.g., Atlas crashed).
        try:
            if isinstance(getattr(ctx, "runtime_state", None), dict):
                ctx.runtime_state["blocked"] = {
                    "reason": str(args.get("reason", "")),
                    "details": str(args.get("details", "")),
                    "suggestion": str(args.get("suggestion", "")),
                }
        except Exception:
            pass
        out = {
            "ok": True,
            "reason": str(args.get("reason", "")),
            "details": str(args.get("details", "")),
            "suggestion": str(args.get("suggestion", "")),
        }
        return json.dumps(out)

    if name == "python_write_and_run" and not bool(getattr(ctx, "codegen_enabled", False)):
        return json.dumps(
            {
                "ok": False,
                "error": "codegen disabled (enable with --enable-codegen)",
            }
        )

    if name == "system_info":
        try:
            system = platform.system()
            release = platform.release()
        except Exception:
            system = ""
            release = ""
        home = os.path.expanduser("~")
        cwd = os.getcwd()
        info = {
            "ok": True,
            "system": system,
            "release": release,
            "os_name": os.name,
            "home": home,
            "cwd": cwd,
            "common_dirs": {
                "Documents": os.path.join(home, "Documents"),
                "Downloads": os.path.join(home, "Downloads"),
                "Desktop": os.path.join(home, "Desktop"),
            },
        }
        return json.dumps(info)

    if name == "python_write_and_run":
        script = args.get("script") or ""
        fname = str(args.get("filename") or "agent_script.py")
        tdir = tempfile.mkdtemp(prefix="atlas_codegen_")
        pth = os.path.join(tdir, fname)
        # Ensure file ends with newline
        if not script.endswith("\n"):
            script += "\n"
        with open(pth, "w", encoding="utf-8") as f:
            f.write(script)
        # Build env with repo root on PYTHONPATH
        env = dict(os.environ)
        rr = find_repo_root()
        repo_root = str(rr) if rr else str(Path(__file__).resolve().parents[0])
        pp = env.get("PYTHONPATH", "")
        env["PYTHONPATH"] = repo_root + (os.pathsep + pp if pp else "")
        # Run
        args_list = args.get("args") or []
        timeout = float(args.get("timeout_sec", DEFAULT_CODEGEN_TIMEOUT_SEC))
        try:
            cp = subprocess.run(
                [sys.executable, pth, *args_list],
                capture_output=True,
                text=True,
                env=env,
                timeout=timeout,
            )
            out_full = cp.stdout or ""
            err_full = cp.stderr or ""

            # Persist full outputs to files so we never drop information.
            stdout_path = os.path.join(tdir, "stdout.txt")
            stderr_path = os.path.join(tdir, "stderr.txt")
            try:
                with open(stdout_path, "w", encoding="utf-8", errors="replace") as f:
                    f.write(out_full)
            except Exception:
                stdout_path = ""
            try:
                with open(stderr_path, "w", encoding="utf-8", errors="replace") as f:
                    f.write(err_full)
            except Exception:
                stderr_path = ""

            # Small previews for transport/model context; full output is on disk.
            preview_chars = int(DEFAULT_CODEGEN_STDIO_PREVIEW_CHARS)
            out_trunc = bool(preview_chars > 0 and len(out_full) > preview_chars)
            err_trunc = bool(preview_chars > 0 and len(err_full) > preview_chars)
            out_preview = (
                out_full
                if not out_trunc
                else (out_full[:preview_chars] + "\n…[truncated preview; see stdout_path]")
            )
            err_preview = (
                err_full
                if not err_trunc
                else (err_full[:preview_chars] + "\n…[truncated preview; see stderr_path]")
            )
            resp = {
                "ok": cp.returncode == 0,
                "exit_code": cp.returncode,
                "stdout_preview": out_preview,
                "stderr_preview": err_preview,
                "stdout_truncated": out_trunc,
                "stderr_truncated": err_trunc,
                "stdout_path": stdout_path,
                "stderr_path": stderr_path,
                "path": pth,
            }
            if bool(args.get("echo_script", True)):
                maxc = int(args.get("max_echo_chars", DEFAULT_CODEGEN_MAX_ECHO_CHARS))
                scr = (
                    script
                    if len(script) <= maxc
                    else (script[:maxc] + "\n…[truncated]")
                )
                resp["script"] = scr
            return json.dumps(resp)
        except Exception as e:
            resp = {"ok": False, "error": str(e), "path": pth}
            if bool(args.get("echo_script", True)):
                maxc = int(args.get("max_echo_chars", DEFAULT_CODEGEN_MAX_ECHO_CHARS))
                scr = (
                    script
                    if len(script) <= maxc
                    else (script[:maxc] + "\n…[truncated]")
                )
                resp["script"] = scr
            return json.dumps(resp)

    if name == "codegen_allowed_imports":
        try:
            names, status = allowed_imports_status()
            installed = [s["name"] for s in status if s.get("ok")]
            missing = [s["name"] for s in status if not s.get("ok")]
            return json.dumps(
                {
                    "ok": True,
                    "allowed": names,
                    "installed": installed,
                    "missing": missing,
                    "status": status,
                }
            )
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    return None
