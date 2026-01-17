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
        self.seen_input_items = []

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
        self.seen_input_items.append(list(input_items or []))
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


def test_reasoning_summary_complete_is_emitted_before_tool_execution():
    """The caller should be able to persist reasoning summaries *before* tool events.

    This preserves chronological ordering in the session log (reasoning → tools),
    matching the streaming CLI.
    """

    trace: list[str] = []

    def dispatch(name: str, args_json: str) -> str:
        trace.append(f"tool:{name}")
        return json.dumps({"ok": True})

    llm = DummyLLM(
        [
            {
                "events": [
                    {
                        "type": "response.reasoning_summary_text.delta",
                        "delta": "I will call a tool next.",
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
                            "content": [{"type": "output_text", "text": "Done."}],
                        }
                    ]
                }
            },
        ]
    )

    def on_reasoning_summary_complete(text: str, call_index: int) -> None:
        trace.append(f"summary:{call_index}:{text.strip()}")

    out = run_responses_tool_loop(
        llm=llm,
        instructions="system",
        input_items=[{"type": "message", "role": "user", "content": [{"type": "input_text", "text": "hi"}]}],
        tools=[{"type": "function", "function": {"name": "update_plan", "parameters": {"type": "object"}}}],
        dispatch=dispatch,
        callbacks=ToolLoopCallbacks(on_reasoning_summary_complete=on_reasoning_summary_complete),
        max_rounds=3,
    )

    assert out.assistant_text == "Done."
    assert llm.calls == 2
    assert trace
    # The summary completion must be recorded before the tool is executed.
    assert trace[0].startswith("summary:")
    assert trace[1] == "tool:update_plan"


def test_tool_loop_preserves_assistant_message_as_output_text_in_history():
    tool_called = []

    def dispatch(name: str, args_json: str) -> str:
        tool_called.append((name, args_json))
        return json.dumps({"ok": True})

    llm = DummyLLM(
        [
            {
                "response": {
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Planning..."}],
                            "status": "completed",
                        },
                        {
                            "type": "function_call",
                            "name": "update_plan",
                            "arguments": "{\"plan\":[]}",
                            "call_id": "call_1",
                        },
                    ]
                }
            },
            {
                "response": {
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Done."}],
                        }
                    ]
                }
            },
        ]
    )

    out = run_responses_tool_loop(
        llm=llm,
        instructions="system",
        input_items=[
            {"type": "message", "role": "user", "content": [{"type": "input_text", "text": "hi"}]}
        ],
        tools=[{"type": "function", "function": {"name": "update_plan", "parameters": {"type": "object"}}}],
        dispatch=dispatch,
        callbacks=ToolLoopCallbacks(),
        max_rounds=3,
    )

    assert out.assistant_text == "Done."
    assert llm.calls == 2

    # The second call should include assistant history as output_text (not input_text),
    # and should not include provider-specific fields like status.
    second_call_items = llm.seen_input_items[1]
    assistant_msgs = [it for it in second_call_items if it.get("type") == "message" and it.get("role") == "assistant"]
    assert assistant_msgs
    assert assistant_msgs[0]["content"][0]["type"] == "output_text"
    assert assistant_msgs[0]["content"][0]["text"] == "Planning..."
    assert "status" not in assistant_msgs[0]


def test_tool_loop_retries_on_incomplete_chunked_read():
    import atlas_agent.responses_tool_loop as rtl  # type: ignore

    class FlakyLLM:
        def __init__(self):
            self.calls = 0

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
            self.calls += 1
            if self.calls == 1:
                raise RuntimeError(
                    "peer closed connection without sending complete message body (incomplete chunked read)"
                )
            return {
                "output": [
                    {
                        "type": "message",
                        "role": "assistant",
                        "content": [{"type": "output_text", "text": "Done."}],
                    }
                ]
            }

    llm = FlakyLLM()

    prev_backoff = rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS
    rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.0
    try:
        out = run_responses_tool_loop(
            llm=llm,
            instructions="system",
            input_items=[
                {"type": "message", "role": "user", "content": [{"type": "input_text", "text": "hi"}]}
            ],
            tools=[],
            dispatch=lambda name, args_json: json.dumps({"ok": True}),
            callbacks=ToolLoopCallbacks(),
            max_rounds=3,
        )
    finally:
        rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS = prev_backoff

    assert out.assistant_text == "Done."
    assert llm.calls == 2


def test_tool_loop_prefers_parsed_output_when_stream_truncated():
    llm = DummyLLM(
        [
            {
                "events": [
                    {
                        "type": "response.output_text.delta",
                        "delta": "Hello",
                    }
                ],
                "response": {
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Hello world"}],
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

    assert out.assistant_text == "Hello world"


def test_tool_loop_auto_continues_when_response_incomplete_and_disables_tools():
    llm = DummyLLM(
        [
            {
                "response": {
                    "status": "incomplete",
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Part1"}],
                        }
                    ],
                }
            },
            {
                "response": {
                    "status": "completed",
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Part2"}],
                        }
                    ],
                }
            },
        ]
    )

    out = run_responses_tool_loop(
        llm=llm,
        instructions="system",
        input_items=[{"type": "message", "role": "user", "content": [{"type": "input_text", "text": "hi"}]}],
        tools=[{"type": "function", "function": {"name": "update_plan", "parameters": {"type": "object"}}}],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=6,
    )

    assert out.assistant_text == "Part1\n\nPart2"
    assert llm.calls == 2
    assert isinstance(llm.seen_tools[0], list) and llm.seen_tools[0]
    assert llm.seen_tools[1] == []


def test_tool_loop_auto_continues_when_final_message_empty():
    llm = DummyLLM(
        [
            {"response": {"status": "completed", "output": []}},
            {
                "response": {
                    "status": "completed",
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Done."}],
                        }
                    ],
                }
            },
        ]
    )

    out = run_responses_tool_loop(
        llm=llm,
        instructions="system",
        input_items=[{"type": "message", "role": "user", "content": [{"type": "input_text", "text": "hi"}]}],
        tools=[],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=6,
    )

    assert out.assistant_text == "Done."
    assert llm.calls == 2
