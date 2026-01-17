"""LLM Agent Tooling: tool specs + dispatcher for function-calling.

This module contains the curated tool list and dispatcher used by the chat
runtime tool loop. It is the stable entry point for LLM function-calling.
"""

import difflib
import json
from typing import Any, Dict, List, Tuple

from google.protobuf.json_format import MessageToDict  # type: ignore

from ..scene_rpc import SceneClient
from ..session_store import SessionStore
from ..tool_registry import ToolRegistry
from .tool_modules import build_tools
from .tool_modules.context import ToolDispatchContext


def scene_tools_and_dispatcher(
    client: SceneClient,
    *,
    atlas_dir: str | None = None,
    session_store: SessionStore | None = None,
    runtime_state: dict[str, Any] | None = None,
    codegen_enabled: bool = False,
) -> Tuple[List[Dict[str, Any]], callable]:
    """Return (tool_specs, dispatcher) for OpenAI tool-calling.

    The dispatcher signature: (name: str, args_json: str) -> str
    Returns a compact JSON string result that the model can parse.
    """

    tool_objects = build_tools()
    # Hide tools that are intentionally disabled for this run. If a tool cannot
    # be used (feature-gated), it should not be advertised to the model at all
    # to avoid wasted rounds on blocked tool calls.
    if not bool(codegen_enabled):
        codegen_tool_names = {"python_write_and_run", "codegen_allowed_imports"}
        tool_objects = [t for t in tool_objects if t.name not in codegen_tool_names]
    tools: List[Dict[str, Any]] = [t.to_chat_tool_spec() for t in tool_objects]

    # Per-dispatcher caches (persist during the tool loop for a single user turn)
    _param_catalog_cache: dict[tuple, list] = {}
    _alias_cache: dict[tuple, dict[str, str]] = {}
    _schema_validator_cache: dict[str, object] = {}
    _runtime_state: dict[str, Any] = runtime_state if isinstance(runtime_state, dict) else {}

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

        chained_dispatch = dispatch
        try:
            maybe = _runtime_state.get("dispatch_proxy")
            if callable(maybe):
                chained_dispatch = maybe
        except Exception:
            chained_dispatch = dispatch

        ctx = ToolDispatchContext(
            client=client,
            atlas_dir=atlas_dir,
            codegen_enabled=bool(codegen_enabled),
            dispatch=chained_dispatch,
            param_to_dict=_param_to_dict,
            resolve_json_key=_resolve_json_key,
            json_key_exists=_json_key_exists,
            schema_validator_cache=_schema_validator_cache,
            session_store=session_store,
            runtime_state=_runtime_state,
        )
        return registry.dispatch(name=name, args_json=args_json, ctx=ctx)

    # Typed tool registry (Option C): validate tool args + run per-tool preconditions
    # before dispatching into the per-tool handlers.
    registry = ToolRegistry.from_tools(tool_objects)
    return tools, dispatch
