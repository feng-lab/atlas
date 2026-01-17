from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.agent_team.base import LLMClient  # type: ignore  # noqa: E402
from atlas_agent.provider_tool_schema import (  # type: ignore  # noqa: E402
    convert_tools_to_chat_completions_wire,
    convert_tools_to_responses_wire,
    normalize_tools_for_responses_api,
    normalize_tools_for_chat_completions_api,
    tighten_tools_schema_for_provider,
)


def test_chat_completions_tools_are_normalized_for_responses():
    tools = [
        {
            "type": "function",
            "function": {
                "name": "foo",
                "description": "desc",
                "parameters": {"type": "object", "properties": {"x": {"type": "integer"}}},
            },
        }
    ]

    converted = LLMClient._normalize_tools_for_responses(tools)
    assert isinstance(converted, list)
    assert len(converted) == 1
    assert converted[0]["type"] == "function"
    assert converted[0]["name"] == "foo"
    assert converted[0]["description"] == "desc"
    assert converted[0]["strict"] is False
    assert "function" not in converted[0]
    assert converted[0]["parameters"]["type"] == "object"
    assert converted[0]["parameters"]["properties"]["x"]["type"] == "integer"
    # Tool schemas are tightened for providers that require Structured-Outputs-like
    # strictness (additionalProperties=false and all keys required).
    assert converted[0]["parameters"]["additionalProperties"] is False
    assert converted[0]["parameters"]["required"] == ["x"]


def test_responses_tool_loop_normalization_is_responses_compatible():
    tools = [
        {
            "type": "function",
            "function": {
                "name": "report_blocked",
                "description": "desc",
                "parameters": {"type": "object", "properties": {"reason": {"type": "string"}}},
            },
        }
    ]

    converted = normalize_tools_for_responses_api(tools)
    assert isinstance(converted, list)
    assert converted[0]["type"] == "function"
    assert converted[0]["name"] == "report_blocked"
    assert converted[0]["strict"] is False
    assert converted[0]["parameters"]["type"] == "object"
    assert converted[0]["parameters"]["properties"]["reason"]["type"] == "string"
    assert converted[0]["parameters"]["additionalProperties"] is False
    assert converted[0]["parameters"]["required"] == ["reason"]

def test_wire_adapter_does_not_tighten_schema():
    tools = [
        {
            "type": "function",
            "function": {
                "name": "foo",
                "parameters": {"type": "object", "properties": {"x": {"type": "integer"}}},
            },
        }
    ]

    converted = convert_tools_to_responses_wire(tools)
    assert isinstance(converted, list)
    assert converted[0]["type"] == "function"
    assert converted[0]["name"] == "foo"
    # Wire conversion should not inject strict-schema fields.
    assert "additionalProperties" not in converted[0]["parameters"]
    assert "required" not in converted[0]["parameters"]


def test_schema_tightening_is_separate_from_wire_adapter():
    tools = [
        {
            "type": "function",
            "name": "foo",
            "parameters": {"type": "object", "properties": {"x": {"type": "integer"}}},
        }
    ]

    tightened = tighten_tools_schema_for_provider(tools)
    assert isinstance(tightened, list)
    assert tightened[0]["type"] == "function"
    assert tightened[0]["name"] == "foo"
    assert tightened[0]["parameters"]["additionalProperties"] is False
    assert tightened[0]["parameters"]["required"] == ["x"]


def test_chat_wire_adapter_converts_from_responses_shape_without_tightening():
    tools = [
        {
            "type": "function",
            "name": "foo",
            "description": "desc",
            "parameters": {"type": "object", "properties": {"x": {"type": "integer"}}},
        }
    ]

    converted = convert_tools_to_chat_completions_wire(tools)
    assert isinstance(converted, list)
    assert converted[0]["type"] == "function"
    assert "function" in converted[0]
    assert converted[0]["function"]["name"] == "foo"
    assert converted[0]["function"]["description"] == "desc"
    assert converted[0]["function"]["parameters"]["properties"]["x"]["type"] == "integer"
    # Wire conversion should not inject strict-schema fields.
    assert "additionalProperties" not in converted[0]["function"]["parameters"]
    assert "required" not in converted[0]["function"]["parameters"]


def test_chat_normalization_tightens_schema():
    tools = [
        {
            "type": "function",
            "name": "foo",
            "parameters": {"type": "object", "properties": {"x": {"type": "integer"}}},
        }
    ]

    normalized = normalize_tools_for_chat_completions_api(tools)
    assert isinstance(normalized, list)
    assert normalized[0]["type"] == "function"
    assert normalized[0]["function"]["name"] == "foo"
    assert normalized[0]["function"]["parameters"]["additionalProperties"] is False
    assert normalized[0]["function"]["parameters"]["required"] == ["x"]


def test_tool_loop_normalization_preserves_nested_schemas():
    tools = [
        {
            "type": "function",
            "function": {
                "name": "update_plan",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "plan": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "step_id": {"type": "string"},
                                    "step": {"type": "string"},
                                    "status": {"type": "string"},
                                },
                                "required": ["step", "status"],
                                "additionalProperties": False,
                            },
                        }
                    },
                },
            },
        }
    ]

    converted = normalize_tools_for_responses_api(tools)
    items = converted[0]["parameters"]["properties"]["plan"]["items"]
    assert items["required"] == ["step_id", "step", "status"]
    assert items["additionalProperties"] is False
