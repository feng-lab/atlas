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
            params = normalize_tool_parameters_schema_for_provider(fn.get("parameters"))
            converted: dict[str, Any] = {
                "type": "function",
                "name": name,
                "parameters": params,
                "strict": bool(fn.get("strict", False)),
            }
            desc = fn.get("description")
            if isinstance(desc, str) and desc.strip():
                converted["description"] = desc.strip()
            out.append(converted)
            continue

        # Already Responses-style tool shape.
        fixed = dict(t)
        fixed["parameters"] = normalize_tool_parameters_schema_for_provider(fixed.get("parameters"))
        if "strict" not in fixed:
            fixed["strict"] = False
        out.append(fixed)

    return out

