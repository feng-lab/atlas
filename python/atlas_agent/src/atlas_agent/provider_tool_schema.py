from __future__ import annotations

from typing import Any, Dict, List, Optional


def normalize_tool_parameters_schema_for_provider(params: Any) -> dict[str, Any]:
    """Best-effort normalization for provider compatibility.

    Some OpenAI-compatible providers validate tool schemas using a strict subset
    of JSON Schema (similar to OpenAI Structured Outputs), requiring:
    - object schemas to set additionalProperties=false
    - required to be present and include every key in properties
    - array schemas to include items

    This function is intentionally *provider-border only* and should not be used
    for internal argument validation. Internal validation is handled separately
    in `tool_registry.py` to keep logical schemas clean.

    We apply this only to schemas that declare properties (i.e., structured
    objects). We intentionally do not "tighten" generic JSON objects (object
    schemas without properties), since those often represent arbitrary payloads
    such as typed scene values.
    """

    if not isinstance(params, dict):
        return {"type": "object", "properties": {}}

    out: dict[str, Any] = dict(params)
    if "type" not in out:
        out["type"] = "object"
    if out.get("type") == "object" and not isinstance(out.get("properties"), dict):
        out["properties"] = {}

    def _tighten(node: Any) -> Any:
        if not isinstance(node, dict):
            return node
        fixed: dict[str, Any] = dict(node)

        # Recurse into combinators first.
        for comb in ("anyOf", "oneOf", "allOf"):
            v = fixed.get(comb)
            if isinstance(v, list):
                fixed[comb] = [_tighten(x) for x in v]

        t = fixed.get("type")
        types: set[str] = set()
        if isinstance(t, str):
            types.add(t)
        elif isinstance(t, list):
            for it in t:
                if isinstance(it, str):
                    types.add(it)

        # Arrays: ensure items exists (empty schema is ok).
        if "array" in types:
            items = fixed.get("items")
            if items is None:
                fixed["items"] = {}
            else:
                fixed["items"] = _tighten(items)

        # Structured objects: if properties is a dict (even empty), force strictness.
        if "object" in types and isinstance(fixed.get("properties"), dict):
            props = fixed.get("properties") or {}
            if isinstance(props, dict):
                fixed["properties"] = {str(k): _tighten(v) for k, v in props.items()}
                fixed["required"] = list(props.keys())
                fixed["additionalProperties"] = False

        return fixed

    tightened = _tighten(out)
    return tightened if isinstance(tightened, dict) else out


def convert_tools_to_responses_wire(
    raw_tools: Optional[List[Dict[str, Any]]],
) -> Optional[List[Dict[str, Any]]]:
    """Convert Chat Completions-style tools to the Responses API tool shape.

    This is a *wire adapter only*:
    - It does not modify or "tighten" JSON Schemas.
    - It only rewrites the wrapper shape so downstream code can treat tools as
      Responses-style consistently:
        {"type":"function","name":"...","description":"...","parameters":{...},"strict":false}

    Some OpenAI-compatible providers accept either shape; others require
    Responses-style when calling `/v1/responses`.
    """

    if not raw_tools:
        return None

    out: list[dict[str, Any]] = []
    for t in raw_tools:
        if not isinstance(t, dict):
            continue
        if str(t.get("type") or "") != "function":
            # Non-function tools (if introduced later) pass through.
            out.append(t)
            continue

        # Chat Completions tool shape: {"type":"function","function":{...}}
        fn = t.get("function")
        if isinstance(fn, dict):
            name = str(fn.get("name") or "").strip()
            if not name:
                continue
            converted: dict[str, Any] = {
                "type": "function",
                "name": name,
                "parameters": fn.get("parameters"),
                "strict": bool(fn.get("strict", False)),
            }
            desc = fn.get("description")
            if isinstance(desc, str) and desc.strip():
                converted["description"] = desc.strip()
            out.append(converted)
            continue

        # Already Responses-style tool shape.
        fixed = dict(t)
        if "strict" not in fixed:
            fixed["strict"] = False
        out.append(fixed)

    return out or None


def convert_tools_to_chat_completions_wire(
    raw_tools: Optional[List[Dict[str, Any]]],
) -> Optional[List[Dict[str, Any]]]:
    """Convert Responses-style tools to the Chat Completions tool shape.

    This is a *wire adapter only*:
    - It does not modify or "tighten" JSON Schemas.
    - It only rewrites the wrapper shape so downstream code can treat tools as
      Chat Completions-style consistently:
        {"type":"function","function":{"name":"...","description":"...","parameters":{...}}}

    Some OpenAI-compatible providers accept either shape; others require the
    Chat Completions wrapper when calling `/v1/chat/completions`.
    """

    if not raw_tools:
        return None

    out: list[dict[str, Any]] = []
    for t in raw_tools:
        if not isinstance(t, dict):
            continue
        if str(t.get("type") or "") != "function":
            out.append(t)
            continue

        # Already Chat Completions tool shape.
        fn = t.get("function")
        if isinstance(fn, dict):
            name = str(fn.get("name") or "").strip()
            if not name:
                continue
            out.append(t)
            continue

        # Responses tool shape: {"type":"function","name":"...","parameters":{...}}
        name = str(t.get("name") or "").strip()
        if not name:
            continue
        fn_out: dict[str, Any] = {"name": name}
        desc = t.get("description")
        if isinstance(desc, str) and desc.strip():
            fn_out["description"] = desc.strip()
        params = t.get("parameters")
        if isinstance(params, dict):
            fn_out["parameters"] = params
        out.append({"type": "function", "function": fn_out})

    return out or None


def tighten_tools_schema_for_provider(
    tools: Optional[List[Dict[str, Any]]],
) -> Optional[List[Dict[str, Any]]]:
    """Apply provider-border schema tightening to each tool's parameters.

    This is intentionally decoupled from wire-shape conversion so we can:
    - keep the logical tool schemas clean and provider-neutral, and
    - apply strictness only at the provider boundary.
    """

    if not tools:
        return None

    out: list[dict[str, Any]] = []
    for t in tools:
        if not isinstance(t, dict):
            continue
        if str(t.get("type") or "") != "function":
            out.append(t)
            continue

        # Responses-style: {"type":"function","name":"...","parameters":{...}}
        if "parameters" in t and "function" not in t:
            fixed = dict(t)
            fixed["parameters"] = normalize_tool_parameters_schema_for_provider(fixed.get("parameters"))
            if "strict" not in fixed:
                fixed["strict"] = False
            out.append(fixed)
            continue

        # Chat-style (rare at this point): {"type":"function","function":{...}}
        fn = t.get("function")
        if isinstance(fn, dict):
            fixed = dict(t)
            f2 = dict(fn)
            f2["parameters"] = normalize_tool_parameters_schema_for_provider(f2.get("parameters"))
            fixed["function"] = f2
            out.append(fixed)
            continue

        out.append(t)

    return out or None


def normalize_tools_for_chat_completions_api(
    raw_tools: Optional[List[Dict[str, Any]]],
) -> Optional[List[Dict[str, Any]]]:
    """Normalize tools for the Chat Completions API.

    Two-stage normalization:
    1) Wire adapter to Chat Completions tool shape.
    2) Provider-border schema tightening for strict validators.
    """

    converted = convert_tools_to_chat_completions_wire(raw_tools)
    return tighten_tools_schema_for_provider(converted)


def normalize_tools_for_responses_api(
    raw_tools: Optional[List[Dict[str, Any]]],
) -> Optional[List[Dict[str, Any]]]:
    """Convert Chat Completions-style tools to the Responses API tool shape.

    Tool definitions in Atlas are produced in Chat Completions format:
      {"type":"function","function":{"name":"...","description":"...","parameters":{...}}}

    Some providers and SDK versions require the Responses format:
      {"type":"function","name":"...","description":"...","parameters":{...},"strict":false}

    This helper also applies provider-border schema tightening to each tool's
    `parameters` to satisfy strict validators.
    """

    converted = convert_tools_to_responses_wire(raw_tools)
    return tighten_tools_schema_for_provider(converted)
