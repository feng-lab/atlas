from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.agent_team.base import LLMClient  # type: ignore  # noqa: E402


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
    assert converted[0]["strict"] is True
    assert "function" not in converted[0]
    assert converted[0]["parameters"]["type"] == "object"
