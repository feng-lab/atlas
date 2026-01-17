from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence

from pydantic import BaseModel, ConfigDict, Field, ValidationError, create_model


Json = Dict[str, Any]
ToolHandler = Callable[[dict[str, Any], Any], Any]
ToolPrecondition = Callable[[dict[str, Any], Any], Optional[str]]


def _normalize_tool_parameters_schema(params: Any) -> dict[str, Any]:
    """Best-effort schema sanitization for local argument validation.

    This is intentionally *not* the same as provider-side "schema tightening".
    Some providers demand a strict subset for tool schemas (e.g. required must
    include every key in properties). We do NOT enforce those constraints here,
    because:
    - internally, optional fields (like update_plan.explanation / step_id) should
      remain optional for maintainability and backwards compatibility within the
      Python runtime;
    - tool handlers already implement additional semantic checks (e.g.
      animation_id must be >0).

    We only ensure the schema is well-formed enough for Pydantic model
    generation (e.g., arrays have items, objects have properties as dicts).
    """

    if not isinstance(params, dict):
        return {"type": "object", "properties": {}}
    out: dict[str, Any] = dict(params)
    if "type" not in out:
        out["type"] = "object"
    if out.get("type") == "object" and not isinstance(out.get("properties"), dict):
        out["properties"] = {}

    def _sanitize(node: Any) -> Any:
        if not isinstance(node, dict):
            return node
        fixed: dict[str, Any] = dict(node)

        # Recurse into combinators first.
        for comb in ("anyOf", "oneOf", "allOf"):
            v = fixed.get(comb)
            if isinstance(v, list):
                fixed[comb] = [_sanitize(x) for x in v]

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
                fixed["items"] = _sanitize(items)

        # Objects: recurse into structured properties when present.
        if "object" in types and isinstance(fixed.get("properties"), dict):
            props = fixed.get("properties") or {}
            if isinstance(props, dict):
                fixed["properties"] = {str(k): _sanitize(v) for k, v in props.items()}

        return fixed

    sanitized = _sanitize(out)
    return sanitized if isinstance(sanitized, dict) else out


def _type_from_schema(schema: Any, *, name_hint: str) -> Any:
    """Best-effort conversion from a JSON schema node to a Python type.

    This is used only for *input argument validation*; it is intentionally
    permissive and falls back to Any when we cannot represent the schema
    faithfully as a Pydantic model.
    """

    if not isinstance(schema, dict):
        return Any
    t = schema.get("type")
    if isinstance(t, list):
        # Union types: treat as Any for now (Pydantic union generation from JSON
        # Schema is non-trivial and provider schemas are often over-permissive).
        return Any
    if t == "string":
        return str
    if t == "integer":
        return int
    if t == "number":
        return float
    if t == "boolean":
        return bool
    if t == "array":
        items = schema.get("items")
        item_type = _type_from_schema(items, name_hint=name_hint + "Item")
        try:
            return list[item_type]  # py3.12+
        except Exception:
            return list
    if t == "object":
        props = schema.get("properties")
        if isinstance(props, dict) and props:
            model = _model_from_object_schema(schema, name_hint=name_hint)
            return model
        return dict[str, Any]
    return Any


def _model_from_object_schema(schema: dict[str, Any], *, name_hint: str) -> type[BaseModel]:
    props = schema.get("properties") or {}
    required = set(schema.get("required") or [])
    extra = "forbid" if schema.get("additionalProperties") is False else "allow"

    fields: dict[str, tuple[Any, Any]] = {}
    for key, sub in (props.items() if isinstance(props, dict) else []):
        k = str(key)
        typ = _type_from_schema(sub, name_hint=name_hint + "_" + k)
        desc = None
        if isinstance(sub, dict):
            d = sub.get("description")
            if isinstance(d, str) and d.strip():
                desc = d.strip()

        has_default = isinstance(sub, dict) and "default" in sub
        default_value = sub.get("default") if has_default else None
        if k in required:
            default = default_value if has_default else ...
            field_type = typ
        else:
            default = default_value if has_default else None
            field_type = typ | None
        fields[k] = (field_type, Field(default, description=desc))

    cfg = ConfigDict(extra=extra)
    return create_model(f"{name_hint}Args", __config__=cfg, **fields)  # type: ignore[arg-type]


@dataclass(frozen=True)
class Tool:
    name: str
    description: str
    parameters_schema: dict[str, Any]
    args_model: type[BaseModel]
    # Precondition checks run after schema validation, before the tool handler.
    # Return a string to block (treated as error), or None to allow.
    preconditions: Sequence[ToolPrecondition] = field(default_factory=tuple)
    handler: ToolHandler = field(
        default=lambda *_: json.dumps({"ok": False, "error": "unbound tool"})
    )

    def to_chat_tool_spec(self) -> dict[str, Any]:
        return {
            "type": "function",
            "function": {
                "name": self.name,
                "description": self.description,
                "parameters": self.parameters_schema,
            },
        }


def tool_from_schema(
    *,
    name: str,
    description: str,
    parameters_schema: dict[str, Any] | None,
    handler: ToolHandler,
    preconditions: Sequence[ToolPrecondition] | None = None,
) -> Tool:
    params = _normalize_tool_parameters_schema(parameters_schema)

    args_model: type[BaseModel]
    if isinstance(params, dict) and params.get("type") == "object" and isinstance(params.get("properties"), dict):
        args_model = _model_from_object_schema(params, name_hint=name)
    else:
        # Fallback: accept any dict-shaped args.
        args_model = create_model(  # type: ignore[assignment]
            f"{name}Args",
            __config__=ConfigDict(extra="allow"),
        )

    return Tool(
        name=str(name),
        description=str(description),
        parameters_schema=params,
        args_model=args_model,
        preconditions=tuple(preconditions or ()),
        handler=handler,
    )


class ToolRegistry:
    def __init__(self, tools: dict[str, Tool]):
        self._tools = dict(tools)

    @classmethod
    def from_tools(cls, tools: Iterable[Tool]) -> "ToolRegistry":
        mapping: dict[str, Tool] = {}
        for t in tools or []:
            if not isinstance(t, Tool):
                continue
            name = str(t.name or "").strip()
            if not name:
                continue
            if name in mapping:
                continue
            mapping[name] = t
        return cls(mapping)

    def has_tool(self, name: str) -> bool:
        return str(name or "") in self._tools

    def dispatch(self, *, name: str, args_json: str, ctx: Any) -> str:
        name = str(name or "").strip()
        tool = self._tools.get(name)
        if tool is None:
            return json.dumps({"ok": False, "error": f"unknown tool: {name}"})

        try:
            raw = json.loads(args_json or "{}")
        except Exception:
            raw = {}
        if not isinstance(raw, dict):
            return json.dumps({"ok": False, "error": "tool arguments must be a JSON object"})

        try:
            validated = tool.args_model.model_validate(raw)
        except ValidationError as e:
            return json.dumps(
                {
                    "ok": False,
                    "error": f"invalid arguments for {name}",
                    "validation_error": e.errors(),
                },
                ensure_ascii=False,
            )

        args = validated.model_dump()

        for check in tool.preconditions:
            try:
                msg = check(args, ctx)
            except Exception as e:
                msg = str(e)
            if isinstance(msg, str) and msg.strip():
                return json.dumps({"ok": False, "error": msg.strip()}, ensure_ascii=False)

        try:
            out = tool.handler(args, ctx)
            if out is None:
                return json.dumps({"ok": False, "error": f"tool returned no output: {name}"})
            if isinstance(out, str):
                return out
            return json.dumps(out, ensure_ascii=False)
        except Exception as e:
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return json.dumps({"ok": False, "error": msg}, ensure_ascii=False)
