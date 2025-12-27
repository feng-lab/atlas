import json
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List

from ...codegen_policy import allowed_imports_status, is_codegen_enabled
from ...repo import find_repo_root  # type: ignore
from .context import ToolDispatchContext

HANDLED_TOOLS = (
    "report_blocked",
    "python_write_and_run",
    "system_info",
    "codegen_allowed_imports",
)

CODEGEN_TOOL_SPEC: Dict[str, Any] = {
    "type": "function",
    "function": {
        "name": "codegen_allowed_imports",
        "description": "Return the current codegen allowed import modules and whether each is importable in this environment.",
        "parameters": {"type": "object", "properties": {}},
    },
}

TOOL_SPECS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "report_blocked",
            "description": "Implementer-only: declare that execution is blocked or not feasible. Use precise reason and details so Supervisor can inform the user.",
            "parameters": {
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
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "system_info",
            "description": "Return OS/platform info and common paths so the agent can reason about file locations.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "python_write_and_run",
            "description": "Write a Python script (string) to a temp file and run it with the repo root on PYTHONPATH. Returns stdout/stderr/exit_code, and optionally echoes the script.",
            "parameters": {
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
                        "default": 120.0,
                        "description": "Execution timeout",
                    },
                    "echo_script": {
                        "type": "boolean",
                        "default": True,
                        "description": "Include script echo in response",
                    },
                    "max_echo_chars": {
                        "type": "integer",
                        "default": 4000,
                        "description": "Max script chars to echo",
                    },
                },
                "required": ["script"],
            },
        },
    },
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
        out = {
            "ok": True,
            "reason": str(args.get("reason", "")),
            "details": str(args.get("details", "")),
            "suggestion": str(args.get("suggestion", "")),
        }
        return json.dumps(out)

    if name == "python_write_and_run" and not is_codegen_enabled():
        return json.dumps(
            {
                "ok": False,
                "error": "codegen disabled (enable with ATLAS_AGENT_ENABLE_CODEGEN=1)",
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
        timeout = float(args.get("timeout_sec", 120.0))
        try:
            cp = subprocess.run(
                [sys.executable, pth, *args_list],
                capture_output=True,
                text=True,
                env=env,
                timeout=timeout,
            )
            out = cp.stdout or ""
            err = cp.stderr or ""
            # Truncate for transport safety
            if len(out) > 8000:
                out = out[:8000] + "\n…[truncated]"
            if len(err) > 8000:
                err = err[:8000] + "\n…[truncated]"
            resp = {
                "ok": cp.returncode == 0,
                "exit_code": cp.returncode,
                "stdout": out,
                "stderr": err,
                "path": pth,
            }
            if bool(args.get("echo_script", True)):
                maxc = int(args.get("max_echo_chars", 4000))
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
                maxc = int(args.get("max_echo_chars", 4000))
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
