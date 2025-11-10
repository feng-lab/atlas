"""LLM Agent Tooling: tool specs + dispatcher for function-calling.

This module contains the curated tool list and dispatcher used by the
multi-agent system. It is the stable entry point for LLM function-calling.
"""

import difflib
import json
import os
from typing import Any, Dict, List, Tuple

from google.protobuf.json_format import MessageToDict  # type: ignore

from ..codegen_policy import is_codegen_enabled
from ..scene_rpc import SceneClient
from .tool_modules import TOOL_TO_MODULE, build_tool_list, general_tools
from .tool_modules.context import ToolDispatchContext


def scene_tools_and_dispatcher(
    client: SceneClient, *, atlas_dir: str | None = None
) -> Tuple[List[Dict[str, Any]], callable]:
    """Return (tool_specs, dispatcher) for OpenAI tool-calling.

    The dispatcher signature: (name: str, args_json: str) -> str
    Returns a compact JSON string result that the model can parse.
    """

    tools: List[Dict[str, Any]] = build_tool_list()

    # Conditionally expose codegen-related tools
    if is_codegen_enabled():
        tools.append(general_tools.CODEGEN_TOOL_SPEC)
    else:
        # When disabled, ensure any lingering codegen tools are removed (defensive if list changes elsewhere)
        tools = [
            t
            for t in tools
            if not (
                isinstance(t, dict)
                and isinstance(t.get("function"), dict)
                and t["function"].get("name")
                in {"python_write_and_run", "codegen_allowed_imports"}
            )
        ]

    # Per-dispatcher caches (persist during the Implementer run)
    _param_catalog_cache: dict[tuple, list] = {}
    _alias_cache: dict[tuple, dict[str, str]] = {}
    _schema_validator_cache: dict[str, object] = {}

    def _list_params_cached(id: int):
        id = int(id)
        key = ("id", id)
        if key in _param_catalog_cache:
            return _param_catalog_cache[key]
        pl = client.list_params(id=id)
        params = list(getattr(pl, "params", []))
        _param_catalog_cache[key] = params
        return params

    def _build_alias_map(params) -> dict[str, str]:
        alias: dict[str, str] = {}

        def norm(s: str) -> str:
            return (s or "").strip().lower()

        for p in params:
            jk = getattr(p, "json_key", "") or ""
            nm = getattr(p, "name", "") or ""
            ty = getattr(p, "type", "") or ""
            if jk:
                alias[norm(jk)] = jk
            if nm:
                alias[norm(nm)] = jk
            # If json_key is name + " " + type, expose the prefix as an alias as well
            try:
                if jk and ty and jk.endswith(" " + ty):
                    alias[norm(jk[: -(len(ty) + 1)])] = jk
            except Exception:
                pass
        return alias

    def _resolve_json_key(
        id: int, candidate: str | None = None, name: str | None = None
    ) -> str | None:
        """Resolve to a canonical json_key using live params. Accepts either a candidate key or a display name.
        Returns canonical json_key or None.
        """
        if (candidate is None or str(candidate).strip() == "") and (
            name is None or str(name).strip() == ""
        ):
            return None
        cand = str(candidate) if candidate is not None else str(name)
        if not cand:
            return None
        cand_norm = cand.strip().lower()
        # Cache alias map per id
        key = ("id", int(id))
        if key not in _alias_cache:
            params = _list_params_cached(id=int(id))
            _alias_cache[key] = _build_alias_map(params)
        amap = _alias_cache.get(key, {})
        # Direct match (canonical)
        if cand_norm in amap:
            return amap[cand_norm]
        # Try to refresh aliases (avoid staleness)
        try:
            params = _list_params_cached(id=int(id))
            _alias_cache[key] = _build_alias_map(params)
            amap = _alias_cache.get(key, {})
        except Exception:
            pass
        if cand_norm in amap:
            return amap[cand_norm]
        # Fuzzy: prefix match on names and keys
        try:
            choices = list(amap.keys())
            # Try best close matches
            for m in difflib.get_close_matches(cand_norm, choices, n=1, cutoff=0.85):
                return amap[m]
            # Try relaxed prefix/contains
            for k in choices:
                if cand_norm in k or k in cand_norm:
                    return amap[k]
        except Exception:
            pass
        return None

    def dispatch(name: str, args_json: str) -> str:
        # Helpers
        def _param_to_dict(p) -> dict:
            """Format a Parameter proto to a JSON-serializable dict using proto-defined fields.
            Includes description and value_schema (JSON Schema) when provided by the server.
            """
            entry = {
                "json_key": getattr(p, "json_key", ""),
                "name": getattr(p, "name", ""),
                "type": getattr(p, "type", ""),
                "supports_interpolation": getattr(p, "supports_interpolation", False),
            }
            # Optional human-readable description provided by the server (C++ ZParameter::description)
            try:
                desc = getattr(p, "description", "")
                if isinstance(desc, str) and desc.strip() != "":
                    entry["description"] = desc
            except Exception:
                pass
            # Include canonical JSON Schema emitted by server when available
            try:
                if hasattr(p, "HasField") and p.HasField("value_schema"):
                    entry["value_schema"] = MessageToDict(getattr(p, "value_schema"))
            except Exception:
                pass
            return entry

        def _json_key_exists(id: int, json_key: str) -> bool:
            try:
                pl = client.list_params(id=int(id))
                for p in pl.params:
                    if getattr(p, "json_key", None) == json_key:
                        return True
            except Exception:
                return False
            return False

        try:
            args = json.loads(args_json or "{}")
        except Exception:
            args = {}

        ctx = ToolDispatchContext(
            client=client,
            atlas_dir=atlas_dir,
            dispatch=dispatch,
            param_to_dict=_param_to_dict,
            resolve_json_key=_resolve_json_key,
            json_key_exists=_json_key_exists,
            schema_validator_cache=_schema_validator_cache,
        )

        module = TOOL_TO_MODULE.get(name)
        if module:
            result = module.handle(name, args, ctx)
            if result is not None:
                return result

        return json.dumps({"error": f"unknown tool: {name}"})

    # Filter out deprecated/disabled tools from the advertised list
    filtered = []
    disabled = {}
    # Environment-gated tools
    try:
        allow_screenshots = os.environ.get(
            "ATLAS_AGENT_ALLOW_SCREENSHOTS", ""
        ).strip().lower() in ("1", "true", "yes")
    except Exception:
        allow_screenshots = False
    for t in tools:
        try:
            nm = t.get("function", {}).get("name")
            # Hide preview tool when screenshots disabled
            if nm == "animation_render_preview" and not allow_screenshots:
                continue
            if nm not in disabled:
                filtered.append(t)
        except Exception:
            filtered.append(t)
    return filtered, dispatch
