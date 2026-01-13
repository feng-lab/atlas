from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.responses_tool_loop import (  # type: ignore  # noqa: E402
    ToolLoopCallbacks,
    run_responses_tool_loop,
)


class DummyLLM:
    def __init__(self, scripted):
        self._scripted = list(scripted)
        self.calls = 0
        self.seen_tools = []

    def responses_stream(  # noqa: D401
        self,
        *,
        instructions: str,
        input_items,
        tools=None,
        temperature: float = 0.2,
        parallel_tool_calls: bool = False,
        reasoning_effort: str | None = "high",
        reasoning_summary: str | None = "detailed",
        text_verbosity: str | None = "high",
        on_event=None,
    ):
        self.seen_tools.append(tools)
        if self.calls >= len(self._scripted):
            raise AssertionError("DummyLLM scripted responses exhausted")
        entry = self._scripted[self.calls]
        self.calls += 1
        for ev in entry.get("events", []):
            if on_event is not None:
                on_event(ev)
        return entry.get("response", {})


def test_tool_loop_returns_assistant_text_when_no_tools_called():
    llm = DummyLLM(
        [
            {
                "events": [
                    {
                        "type": "response.reasoning_summary_text.delta",
                        "delta": "I will do X.",
                        "summary_index": 0,
                    }
                ],
                "response": {
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Done."}],
                        }
                    ]
                },
            }
        ]
    )

    out = run_responses_tool_loop(
        llm=llm,
        instructions="system",
        input_items=[{"type": "message", "role": "user", "content": [{"type": "input_text", "text": "hi"}]}],
        tools=[],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=3,
    )

    assert out.assistant_text == "Done."
    assert llm.calls == 1
    assert out.reasoning_summaries == ["I will do X."]


def test_tool_loop_executes_function_calls_then_continues():
    tool_called = []

    def dispatch(name: str, args_json: str) -> str:
        tool_called.append((name, args_json))
        return json.dumps({"ok": True})

    llm = DummyLLM(
        [
            {
                "events": [
                    {
                        "type": "response.reasoning_summary_text.delta",
                        "delta": "I will call a tool.",
                        "summary_index": 0,
                    }
                ],
                "response": {
                    "output": [
                        {
                            "type": "function_call",
                            "name": "update_plan",
                            "arguments": "{\"plan\":[]}",
                            "call_id": "call_1",
                        }
                    ]
                },
            },
            {
                "response": {
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Applied."}],
                        }
                    ]
                }
            },
        ]
    )

    seen_tool_calls = []

    def on_tool_call(name: str, args_json: str, call_id: str) -> None:
        seen_tool_calls.append((name, call_id))

    out = run_responses_tool_loop(
        llm=llm,
        instructions="system",
        input_items=[{"type": "message", "role": "user", "content": [{"type": "input_text", "text": "hi"}]}],
        tools=[{"type": "function", "function": {"name": "update_plan", "parameters": {"type": "object"}}}],
        dispatch=dispatch,
        callbacks=ToolLoopCallbacks(on_tool_call=on_tool_call),
        max_rounds=3,
    )

    assert out.assistant_text == "Applied."
    assert llm.calls == 2
    assert isinstance(llm.seen_tools[0], list)
    assert llm.seen_tools[0][0]["type"] == "function"
    assert llm.seen_tools[0][0]["name"] == "update_plan"
    assert "function" not in llm.seen_tools[0][0]
    assert tool_called == [("update_plan", "{\"plan\":[]}")]
    assert seen_tool_calls == [("update_plan", "call_1")]
