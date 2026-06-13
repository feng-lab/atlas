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
from atlas_agent.model_policy import DEFAULT_MODEL  # type: ignore  # noqa: E402


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
        reasoning_effort: str | None,
        reasoning_summary: str | None,
        text_verbosity: str | None,
        tools=None,
        temperature: float = 0.2,
        parallel_tool_calls: bool = False,
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
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ],
        tools=[],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=3,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
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
                            "arguments": '{"plan":[]}',
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
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ],
        tools=[
            {
                "type": "function",
                "function": {"name": "update_plan", "parameters": {"type": "object"}},
            }
        ],
        dispatch=dispatch,
        callbacks=ToolLoopCallbacks(on_tool_call=on_tool_call),
        max_rounds=3,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
    )

    assert out.assistant_text == "Applied."
    assert llm.calls == 2
    assert isinstance(llm.seen_tools[0], list)
    assert llm.seen_tools[0][0]["type"] == "function"
    assert llm.seen_tools[0][0]["name"] == "update_plan"
    assert "function" not in llm.seen_tools[0][0]
    assert tool_called == [("update_plan", '{"plan":[]}')]
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
                            "arguments": '{"plan":[]}',
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
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ],
        tools=[
            {
                "type": "function",
                "function": {"name": "update_plan", "parameters": {"type": "object"}},
            }
        ],
        dispatch=dispatch,
        callbacks=ToolLoopCallbacks(
            on_reasoning_summary_complete=on_reasoning_summary_complete
        ),
        max_rounds=3,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
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
                            "arguments": '{"plan":[]}',
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
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ],
        tools=[
            {
                "type": "function",
                "function": {"name": "update_plan", "parameters": {"type": "object"}},
            }
        ],
        dispatch=dispatch,
        callbacks=ToolLoopCallbacks(),
        max_rounds=3,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
    )

    assert out.assistant_text == "Done."
    assert llm.calls == 2

    # The second call should include assistant history as output_text (not input_text),
    # and should not include provider-specific fields like status.
    second_call_items = llm.seen_input_items[1]
    assistant_msgs = [
        it
        for it in second_call_items
        if it.get("type") == "message" and it.get("role") == "assistant"
    ]
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
            reasoning_effort: str | None,
            reasoning_summary: str | None,
            text_verbosity: str | None,
            tools=None,
            temperature: float = 0.2,
            parallel_tool_calls: bool = False,
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
                {
                    "type": "message",
                    "role": "user",
                    "content": [{"type": "input_text", "text": "hi"}],
                }
            ],
            tools=[],
            dispatch=lambda name, args_json: json.dumps({"ok": True}),
            callbacks=ToolLoopCallbacks(),
            max_rounds=3,
            reasoning_effort="high",
            reasoning_summary="detailed",
            text_verbosity="high",
        )
    finally:
        rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS = prev_backoff

    assert out.assistant_text == "Done."
    assert llm.calls == 2


def test_tool_loop_retries_when_gateway_model_missing_for_validated_models():
    import atlas_agent.responses_tool_loop as rtl  # type: ignore

    class GatewayFlakyLLM(DummyLLM):
        def __init__(self, scripted):
            super().__init__(scripted)
            # Simulate a model request where the gateway is expected to
            # report resp["model"].
            self.model = DEFAULT_MODEL

    llm = GatewayFlakyLLM(
        [
            {
                # Missing "model" should trigger a clean retry; this output must
                # not be appended to history.
                "response": {
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Discard me"}],
                        }
                    ]
                },
            },
            {
                "response": {
                    "model": DEFAULT_MODEL,
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

    seen_meta: list[dict] = []

    def on_response_meta(resp: dict, call_index: int) -> None:
        seen_meta.append({"call_index": call_index, "model": resp.get("model")})

    prev_backoff = rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS
    rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.0
    try:
        out = run_responses_tool_loop(
            llm=llm,
            instructions="system",
            input_items=[
                {
                    "type": "message",
                    "role": "user",
                    "content": [{"type": "input_text", "text": "hi"}],
                }
            ],
            tools=[],
            dispatch=lambda name, args_json: json.dumps({"ok": True}),
            callbacks=ToolLoopCallbacks(on_response_meta=on_response_meta),
            max_rounds=3,
            reasoning_effort="high",
            reasoning_summary="detailed",
            text_verbosity="high",
        )
    finally:
        rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS = prev_backoff

    assert out.assistant_text == "Done."
    assert llm.calls == 2
    # Only the accepted response should emit meta.
    assert seen_meta == [{"call_index": 0, "model": DEFAULT_MODEL}]
    # The second attempt should not include the discarded assistant output.
    assert llm.seen_input_items[1] == llm.seen_input_items[0]


def test_tool_loop_retries_when_gateway_model_mismatched_for_validated_models():
    import atlas_agent.responses_tool_loop as rtl  # type: ignore

    class GatewayMismatchLLM(DummyLLM):
        def __init__(self, scripted):
            super().__init__(scripted)
            self.model = DEFAULT_MODEL

    llm = GatewayMismatchLLM(
        [
            {
                # Wrong routed model should trigger a clean retry; this output must
                # not be appended to history.
                "response": {
                    "model": "gpt-4o",
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Discard me"}],
                        }
                    ],
                }
            },
            {
                "response": {
                    "model": DEFAULT_MODEL,
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

    seen_meta: list[dict] = []

    def on_response_meta(resp: dict, call_index: int) -> None:
        seen_meta.append({"call_index": call_index, "model": resp.get("model")})

    prev_backoff = rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS
    rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.0
    try:
        out = run_responses_tool_loop(
            llm=llm,
            instructions="system",
            input_items=[
                {
                    "type": "message",
                    "role": "user",
                    "content": [{"type": "input_text", "text": "hi"}],
                }
            ],
            tools=[],
            dispatch=lambda name, args_json: json.dumps({"ok": True}),
            callbacks=ToolLoopCallbacks(on_response_meta=on_response_meta),
            max_rounds=3,
            reasoning_effort="high",
            reasoning_summary="detailed",
            text_verbosity="high",
        )
    finally:
        rtl.TRANSIENT_NETWORK_BACKOFF_SECONDS = prev_backoff

    assert out.assistant_text == "Done."
    assert llm.calls == 2
    assert seen_meta == [{"call_index": 0, "model": DEFAULT_MODEL}]
    assert llm.seen_input_items[1] == llm.seen_input_items[0]


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
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ],
        tools=[],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=3,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
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
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ],
        tools=[
            {
                "type": "function",
                "function": {"name": "update_plan", "parameters": {"type": "object"}},
            }
        ],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=6,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
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
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ],
        tools=[],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=6,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
    )

    assert out.assistant_text == "Done."
    assert llm.calls == 2


def test_tool_loop_compacts_on_context_overflow_when_handler_provided():
    """When the provider rejects the request due to context length, the loop should
    prefer caller-provided compaction over blind trimming.
    """

    class OverflowOnceLLM:
        def __init__(self):
            self.calls = 0
            self.seen_input_items = []

        def responses_stream(  # noqa: D401
            self,
            *,
            instructions: str,
            input_items,
            reasoning_effort: str | None,
            reasoning_summary: str | None,
            text_verbosity: str | None,
            tools=None,
            temperature: float = 0.2,
            parallel_tool_calls: bool = False,
            on_event=None,
        ):
            self.calls += 1
            self.seen_input_items.append(list(input_items or []))
            if self.calls == 1:
                raise RuntimeError("maximum context length exceeded")
            return {
                "output": [
                    {
                        "type": "message",
                        "role": "assistant",
                        "content": [{"type": "output_text", "text": "Done."}],
                    }
                ]
            }

    llm = OverflowOnceLLM()

    def compact(in_items, _exc: BaseException) -> bool:
        # Replace older history with a single checkpoint message, preserving the
        # most recent user message.
        if not in_items:
            return False
        checkpoint = {
            "type": "message",
            "role": "user",
            "content": [{"type": "input_text", "text": "CONTEXT CHECKPOINT"}],
        }
        in_items[:] = [checkpoint, in_items[-1]]
        return True

    out = run_responses_tool_loop(
        llm=llm,
        instructions="system",
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            },
            {
                "type": "message",
                "role": "assistant",
                "content": [{"type": "output_text", "text": "older"}],
            },
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "latest"}],
            },
        ],
        tools=[],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=3,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
        on_context_overflow=compact,
    )

    assert out.assistant_text == "Done."
    assert llm.calls == 2
    second_call_items = llm.seen_input_items[1]
    assert second_call_items
    assert second_call_items[0]["type"] == "message"
    assert second_call_items[0]["role"] == "user"
    assert second_call_items[0]["content"][0]["text"] == "CONTEXT CHECKPOINT"


def test_tool_loop_proactively_compacts_when_estimate_near_context_window():
    """Proactive compaction should run before the provider rejects the request."""

    class SingleShotLLM:
        def __init__(self):
            self.calls = 0
            self.seen_input_items = []

        def responses_stream(  # noqa: D401
            self,
            *,
            instructions: str,
            input_items,
            reasoning_effort: str | None,
            reasoning_summary: str | None,
            text_verbosity: str | None,
            tools=None,
            temperature: float = 0.2,
            parallel_tool_calls: bool = False,
            on_event=None,
        ):
            self.calls += 1
            self.seen_input_items.append(list(input_items or []))
            return {
                "output": [
                    {
                        "type": "message",
                        "role": "assistant",
                        "content": [{"type": "output_text", "text": "Done."}],
                    }
                ]
            }

    llm = SingleShotLLM()
    compacted = {"calls": 0}

    def compact(in_items, _exc: BaseException) -> bool:
        compacted["calls"] += 1
        # Keep only a small checkpoint + the last user message.
        if not in_items:
            return False
        checkpoint = {
            "type": "message",
            "role": "user",
            "content": [{"type": "input_text", "text": "CONTEXT CHECKPOINT"}],
        }
        # Preserve the last user message if present; otherwise keep the last item.
        last = in_items[-1]
        for it in reversed(in_items):
            if it.get("type") == "message" and it.get("role") == "user":
                last = it
                break
        in_items[:] = [checkpoint, last]
        return True

    out = run_responses_tool_loop(
        llm=llm,
        instructions="system",
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "x" * 4000}],
            }
        ],
        tools=[],
        dispatch=lambda name, args_json: json.dumps({"ok": True}),
        callbacks=ToolLoopCallbacks(),
        max_rounds=3,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
        on_context_overflow=compact,
        # Force proactive compaction: tiny context window so our estimate crosses the threshold.
        effective_input_budget_tokens=200,
    )

    assert out.assistant_text == "Done."
    assert compacted["calls"] >= 1
    assert llm.calls == 1
    assert llm.seen_input_items
    assert llm.seen_input_items[0][0]["content"][0]["text"] == "CONTEXT CHECKPOINT"
