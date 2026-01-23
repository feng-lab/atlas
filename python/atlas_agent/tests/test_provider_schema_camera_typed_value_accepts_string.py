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
    normalize_tools_for_chat_completions_api,
    normalize_tools_for_responses_api,
)


def _has_string_variant(schema) -> bool:
    if not isinstance(schema, dict):
        return False
    t = schema.get("type")
    if t == "string":
        return True
    if isinstance(t, list) and any(it == "string" for it in t):
        return True
    any_of = schema.get("anyOf")
    if isinstance(any_of, list):
        for sub in any_of:
            if isinstance(sub, dict) and sub.get("type") == "string":
                return True
    one_of = schema.get("oneOf")
    if isinstance(one_of, list):
        for sub in one_of:
            if isinstance(sub, dict) and sub.get("type") == "string":
                return True
    return False


def _assert_tool_value_param_accepts_string(tool, *, tool_name: str) -> None:
    assert isinstance(tool, dict)
    params = tool.get("parameters")
    assert isinstance(params, dict), f"{tool_name}: missing parameters"
    props = params.get("properties")
    assert isinstance(props, dict), f"{tool_name}: parameters.properties missing"
    assert "value" in props, f"{tool_name}: missing parameters.properties.value"
    value_schema = props.get("value")
    assert _has_string_variant(value_schema), (
        f"{tool_name}: value schema should accept a JSON string for provider compatibility; "
        f"got {value_schema!r}"
    )


def test_scene_camera_apply_accepts_json_string_value_in_responses_schema() -> None:
    raw_tools = build_tool_list()
    tools = normalize_tools_for_responses_api(raw_tools) or []
    found = next((t for t in tools if t.get("type") == "function" and t.get("name") == "scene_camera_apply"), None)
    assert found is not None, "scene_camera_apply not found in Responses tool list"
    _assert_tool_value_param_accepts_string(found, tool_name="scene_camera_apply")


def test_animation_replace_key_camera_accepts_json_string_value_in_responses_schema() -> None:
    raw_tools = build_tool_list()
    tools = normalize_tools_for_responses_api(raw_tools) or []
    found = next((t for t in tools if t.get("type") == "function" and t.get("name") == "animation_replace_key_camera"), None)
    assert found is not None, "animation_replace_key_camera not found in Responses tool list"
    _assert_tool_value_param_accepts_string(found, tool_name="animation_replace_key_camera")


def test_scene_camera_apply_accepts_json_string_value_in_chat_schema() -> None:
    raw_tools = build_tool_list()
    tools = normalize_tools_for_chat_completions_api(raw_tools) or []
    found = next(
        (
            t
            for t in tools
            if t.get("type") == "function"
            and isinstance(t.get("function"), dict)
            and t.get("function", {}).get("name") == "scene_camera_apply"
        ),
        None,
    )
    assert found is not None, "scene_camera_apply not found in Chat tool list"
    fn = found.get("function") if isinstance(found.get("function"), dict) else {}
    tool = {
        "parameters": fn.get("parameters"),
    }
    _assert_tool_value_param_accepts_string(tool, tool_name="scene_camera_apply")


def test_animation_replace_key_camera_accepts_json_string_value_in_chat_schema() -> None:
    raw_tools = build_tool_list()
    tools = normalize_tools_for_chat_completions_api(raw_tools) or []
    found = next(
        (
            t
            for t in tools
            if t.get("type") == "function"
            and isinstance(t.get("function"), dict)
            and t.get("function", {}).get("name") == "animation_replace_key_camera"
        ),
        None,
    )
    assert found is not None, "animation_replace_key_camera not found in Chat tool list"
    fn = found.get("function") if isinstance(found.get("function"), dict) else {}
    tool = {
        "parameters": fn.get("parameters"),
    }
    _assert_tool_value_param_accepts_string(tool, tool_name="animation_replace_key_camera")

