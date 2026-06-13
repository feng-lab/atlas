from __future__ import annotations

import json
import copy
import sys
from pathlib import Path

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
        # Deep copy because the tool loop mutates in_items in place (e.g. stripping
        # ephemeral inline images after a call). We want to capture what was sent.
        self.seen_input_items.append(copy.deepcopy(list(input_items or [])))
        if self.calls >= len(self._scripted):
            raise AssertionError("DummyLLM scripted responses exhausted")
        entry = self._scripted[self.calls]
        self.calls += 1
        for ev in entry.get("events", []):
            if on_event is not None:
                on_event(ev)
        return entry.get("response", {})


def _find_tool_output(items, *, call_id: str):
    for it in items:
        if not isinstance(it, dict):
            continue
        if it.get("type") != "function_call_output":
            continue
        if str(it.get("call_id") or "") != str(call_id):
            continue
        return it
    return None


def test_tool_loop_inline_images_are_ephemeral() -> None:
    """Inline base64 images should only be sent once, then stripped from history."""

    llm = DummyLLM(
        [
            {
                "response": {
                    "model": DEFAULT_MODEL,
                    "output": [
                        {
                            "type": "function_call",
                            "name": "scene_screenshot",
                            "arguments": "{}",
                            "call_id": "call_1",
                        }
                    ],
                }
            },
            {
                # Next call sees the image once and asks for another tool.
                "response": {
                    "model": DEFAULT_MODEL,
                    "output": [
                        {
                            "type": "function_call",
                            "name": "scene_list_objects",
                            "arguments": "{}",
                            "call_id": "call_2",
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

    def dispatch(name: str, args_json: str) -> str:
        return json.dumps({"ok": True})

    def post_tool_output(name: str, args_json: str, result_json: str):
        if name != "scene_screenshot":
            return None
        # Simulate a tool output with an inline base64 data URL.
        return {
            "output": [
                {"type": "input_text", "text": "preview"},
                {"type": "input_image", "image_url": "data:image/png;base64,AAAA"},
            ]
        }

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
        dispatch=dispatch,
        callbacks=ToolLoopCallbacks(),
        max_rounds=10,
        reasoning_effort="high",
        reasoning_summary="detailed",
        text_verbosity="high",
        ephemeral_inline_images=True,
        post_tool_output=post_tool_output,
    )

    assert out.assistant_text == "Done."
    assert llm.calls == 3

    # Second request should include the inline image.
    second_items = llm.seen_input_items[1]
    out1 = _find_tool_output(second_items, call_id="call_1")
    assert out1 is not None
    assert isinstance(out1.get("output"), list)
    assert any(
        isinstance(p, dict)
        and p.get("type") == "input_image"
        and str(p.get("image_url") or "").startswith("data:image/")
        for p in (out1.get("output") or [])
    )

    # Third request should have the same tool output but without inline image.
    third_items = llm.seen_input_items[2]
    out1b = _find_tool_output(third_items, call_id="call_1")
    assert out1b is not None
    parts = out1b.get("output")
    assert isinstance(parts, list)
    assert not any(
        isinstance(p, dict)
        and p.get("type") == "input_image"
        and str(p.get("image_url") or "").startswith("data:image/")
        for p in parts
    )
