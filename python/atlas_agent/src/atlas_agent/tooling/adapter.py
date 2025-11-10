"""Tool schema adapters for different providers.

Transforms our tool specs (as produced in tools_agent.py) into provider‑specific
JSON Schemas that satisfy strict validators without changing runtime behavior.

Policy for 'openai_responses' (strict):
- Object schemas: required must include every key listed in properties.
- Optional properties are dropped from properties (kept implicit via defaults in dispatcher).
- Do not set additionalProperties=True (SDK rejects it); leave it absent or keep False.
- Arrays must declare items with a concrete type. When missing, use items.type
  = ["string","number","boolean","null","object","array"] as a safe superset.

This adapter allows us to keep canonical schemas with clean required vs optional
separation, while emitting the strict subset that providers demand.
"""

from copy import deepcopy
from typing import Any, Dict, List

Json = Dict[str, Any]


def _ensure_array_items(node: Any) -> Any:
    if isinstance(node, dict):
        out: Dict[str, Any] = {}
        for k, v in node.items():
            if k == "additionalProperties" and v is True:
                # Strict providers reject additionalProperties=True
                # Drop it; leaving absent is stricter than True and acceptable
                continue
            if k in ("properties", "definitions") and isinstance(v, dict):
                out[k] = {pn: _ensure_array_items(ps) for pn, ps in v.items()}
            elif k == "items":
                out[k] = _ensure_array_items(v)
            else:
                out[k] = _ensure_array_items(v)
        # If this schema is/contains an array, ensure items is present with a type
        typ = out.get("type")
        def _has_type(t, name: str) -> bool:
            return (isinstance(t, list) and name in t) or (t == name)
        if _has_type(typ, "array") and "items" not in out:
            out["items"] = {"type": ["string", "number", "boolean", "null", "object", "array"]}
        return out
    if isinstance(node, list):
        return [_ensure_array_items(x) for x in node]
    return node


def _required_equals_properties(schema: Json) -> Json:
    if not isinstance(schema, dict):
        return schema
    out = {}
    for k, v in schema.items():
        out[k] = v
    if schema.get("type") == "object" or ("properties" in schema):
        props = schema.get("properties") or {}
        if isinstance(props, dict):
            # Keep only required properties in 'properties' for ultra-strict providers
            req = schema.get("required") or []
            if isinstance(req, list):
                new_props = {k: props[k] for k in req if k in props}
            else:
                new_props = props
            out["properties"] = new_props
            out["required"] = list(new_props.keys())
        # Never emit additionalProperties=True
        if out.get("additionalProperties", None) is True:
            out.pop("additionalProperties", None)
    # Recurse into nested schemas
    for k in list(out.keys()):
        if k in ("properties", "definitions") and isinstance(out[k], dict):
            out[k] = {pn: _required_equals_properties(ps) for pn, ps in out[k].items()}
        elif k in ("items",):
            out[k] = _required_equals_properties(out[k])
    return out


def adapt_tools_for_provider(tools: List[Json], *, provider: str) -> List[Json]:
    if provider != "openai_responses":
        return tools
    out: List[Json] = []
    for t in tools:
        td = deepcopy(t)
        fn = td.get("function") if isinstance(td, dict) else None
        if isinstance(fn, dict):
            params = fn.get("parameters")
            if isinstance(params, dict):
                # Step 1: ensure arrays declare items types
                params2 = _ensure_array_items(params)
                # Step 2: enforce required == properties for strict providers
                params3 = _required_equals_properties(params2)
                fn["parameters"] = params3
        out.append(td)
    return out
