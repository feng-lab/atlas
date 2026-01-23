from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.agent_team.tool_modules import build_tool_list  # type: ignore  # noqa: E402
from atlas_agent.provider_tool_schema import (  # type: ignore  # noqa: E402
    tighten_tools_schema_for_provider,
)


def _walk_schema(node, *, where: str) -> None:
    if isinstance(node, dict):
        t = node.get("type")
        types: set[str] = set()
        if isinstance(t, str):
            types.add(t)
        elif isinstance(t, list):
            for it in t:
                if isinstance(it, str):
                    types.add(it)

        if "array" in types:
            assert "items" in node, f"Array schema missing 'items' at {where}"
            items = node.get("items")
            assert isinstance(items, dict), f"Array schema items must be an object at {where}"
            assert (
                ("type" in items)
                or any(k in items for k in ("anyOf", "oneOf", "allOf"))
                or ("$ref" in items)
            ), f"Array schema items must declare a type at {where}"

        # Recurse into common schema fields
        for comb in ("anyOf", "oneOf", "allOf"):
            v = node.get(comb)
            if isinstance(v, list):
                for i, sub in enumerate(v):
                    _walk_schema(sub, where=f"{where}.{comb}[{i}]")

        props = node.get("properties")
        if isinstance(props, dict):
            for k, v in props.items():
                _walk_schema(v, where=f"{where}.properties.{k}")

        if "items" in node:
            _walk_schema(node.get("items"), where=f"{where}.items")

        ap = node.get("additionalProperties")
        if isinstance(ap, dict):
            _walk_schema(ap, where=f"{where}.additionalProperties")

        return

    if isinstance(node, list):
        for i, sub in enumerate(node):
            _walk_schema(sub, where=f"{where}[{i}]")


def test_provider_tightened_tool_schemas_have_typed_array_items() -> None:
    raw_tools = build_tool_list()
    tightened = tighten_tools_schema_for_provider(raw_tools) or []

    for t in tightened:
        if not isinstance(t, dict):
            continue
        if str(t.get("type") or "") != "function":
            # Ignore non-function tool types (if introduced later).
            continue

        if "function" in t:
            fn = t.get("function") if isinstance(t.get("function"), dict) else {}
            name = str(fn.get("name") or "<unknown>")
            params = fn.get("parameters")
        else:
            name = str(t.get("name") or "<unknown>")
            params = t.get("parameters")

        _walk_schema(params, where=f"tool={name}.parameters")

