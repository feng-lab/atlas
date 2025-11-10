"""Codegen policy helpers (allowed imports and feature gating).

This module centralizes the allowlist of Python modules that codegen scripts
may import. The Implementer injects this list into the system prompt so LLMs
know what is safe/available.

Enable/disable the codegen pathway with ATLAS_AGENT_ENABLE_CODEGEN=1.
"""

import os
from importlib.util import find_spec
from pathlib import Path
from typing import Any, Dict, List, Tuple


def _read_allowlist_from_package() -> List[str]:
    here = Path(__file__).resolve()
    p = here.parent / "codegen_allowlist.txt"
    if not p.exists():
        return []
    out: List[str] = []
    for line in p.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        out.append(s)
    return out


def allowed_imports() -> List[str]:
    """Return the allowlist bundled with the package.

    No environment override, no merging. If the file is missing, returns [].
    """
    return _read_allowlist_from_package()


def allowed_imports_text() -> str:
    return ", ".join(allowed_imports())


def allowed_imports_status() -> Tuple[List[str], List[Dict[str, Any]]]:
    """Return (allowed_names, status_list) where status_list is [{name, ok, error?}]."""
    names = allowed_imports()
    status: List[Dict[str, Any]] = []
    for nm in names:
        ok = False
        err = ""
        try:
            ok = find_spec(nm) is not None
            if not ok:
                err = "module not found"
        except Exception as e:
            ok = False
            err = str(e)
        status.append({"name": nm, "ok": ok, **({"error": err} if err else {})})
    return names, status


def is_codegen_enabled() -> bool:
    """Return True when the codegen toolpath is enabled.

    Controlled by ATLAS_AGENT_ENABLE_CODEGEN env var. Accepts: 1/true/yes (case‑insensitive).
    Defaults to False (disabled) when unset.
    """
    val = os.environ.get("ATLAS_AGENT_ENABLE_CODEGEN", "").strip().lower()
    return val in ("1", "true", "yes", "on")
