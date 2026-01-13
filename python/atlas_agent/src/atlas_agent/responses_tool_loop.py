from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Protocol


class ResponsesStreamingClient(Protocol):
    def responses_stream(
        self,
        *,
        instructions: str,
        input_items: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]] = None,
        temperature: float = 0.2,
        parallel_tool_calls: bool = False,
        reasoning_effort: str | None = "high",
        reasoning_summary: str | None = "detailed",
        text_verbosity: str | None = "high",
        on_event: Callable[[Dict[str, Any]], None] | None = None,
    ) -> Dict[str, Any]: ...


def _normalize_tools_for_responses_api(tools: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Return a Responses API compatible tool list.

    The OpenAI SDK's `responses.stream(...)` rejects Chat Completions-style
    function tools when they are plain dicts. In particular, tools shaped like:

      {"type":"function","function":{"name":"...","parameters":{...}}}

    must be converted to the Responses API style:

      {"type":"function","name":"...","parameters":{...},"strict":true}

    Our tool modules intentionally define simple dict specs (not Pydantic tool
    wrappers), so we normalize here to keep the streaming loop provider-agnostic.
    """

    out: list[dict[str, Any]] = []
    for t in tools or []:
        if not isinstance(t, dict):
            continue
        if str(t.get("type") or "") != "function":
            out.append(t)
            continue

        # Chat Completions tool shape: {"type":"function","function":{...}}
        fn = t.get("function")
        if isinstance(fn, dict):
            name = str(fn.get("name") or "").strip()
            if not name:
                continue
            params = fn.get("parameters")
            if not isinstance(params, dict):
                params = {"type": "object", "properties": {}}
            converted: dict[str, Any] = {
                "type": "function",
                "name": name,
                "parameters": params,
                "strict": bool(fn.get("strict", True)),
            }
            desc = fn.get("description")
            if isinstance(desc, str) and desc.strip():
                converted["description"] = desc.strip()
            out.append(converted)
            continue

        # Already Responses-style tool shape.
        out.append(t)

    return out


def _input_text_message(*, role: str, text: str) -> dict[str, Any]:
    return {
        "type": "message",
        "role": str(role),
        "content": [{"type": "input_text", "text": str(text)}],
    }


def _sanitize_response_item(item: dict[str, Any]) -> dict[str, Any]:
    """Make a ResponseItem safe to send back as input to the Responses API.

    The Responses API output items often include server-generated identifiers
    (e.g., `id`) that should not be echoed back as input items.
    """

    if not isinstance(item, dict):
        return {}
    out = dict(item)
    # Common server-side only fields
    out.pop("id", None)
    return out


def _extract_assistant_text_from_output_item(item: dict[str, Any]) -> str:
    if not isinstance(item, dict):
        return ""
    if str(item.get("type") or "") != "message":
        return ""
    if str(item.get("role") or "") != "assistant":
        return ""
    parts = item.get("content") or []
    out: list[str] = []
    if isinstance(parts, list):
        for p in parts:
            if not isinstance(p, dict):
                continue
            if str(p.get("type") or "") == "output_text":
                out.append(str(p.get("text") or ""))
    return "".join(out)


def _extract_function_calls(output_items: list[dict[str, Any]]) -> list[dict[str, Any]]:
    calls: list[dict[str, Any]] = []
    for item in output_items or []:
        if not isinstance(item, dict):
            continue
        if str(item.get("type") or "") != "function_call":
            continue
        name = str(item.get("name") or "")
        call_id = str(item.get("call_id") or "")
        arguments = item.get("arguments")
        if not name or not call_id:
            continue
        if not isinstance(arguments, str):
            # The Responses API delivers arguments as a JSON string in all normal
            # cases. Be defensive: convert to JSON to preserve data.
            try:
                arguments = json.dumps(arguments, ensure_ascii=False)
            except Exception:
                arguments = str(arguments)
        calls.append({"name": name, "call_id": call_id, "arguments": arguments})
    return calls


def _coerce_output_items(resp: dict[str, Any]) -> list[dict[str, Any]]:
    out = resp.get("output")
    if not isinstance(out, list):
        return []
    items: list[dict[str, Any]] = []
    for it in out:
        if isinstance(it, dict):
            items.append(it)
    return items


@dataclass(slots=True)
class ToolLoopCallbacks:
    """Optional streaming callbacks for rendering a streaming CLI."""

    on_phase_start: Callable[[str], None] | None = None
    on_phase_end: Callable[[str], None] | None = None
    on_reasoning_summary_delta: Callable[[str, int], None] | None = None
    on_reasoning_summary_part_added: Callable[[int], None] | None = None
    on_assistant_text_delta: Callable[[str], None] | None = None
    on_tool_call: Callable[[str, str, str], None] | None = None  # (name, args_json, call_id)
    on_tool_result: Callable[[str, str, str], None] | None = None  # (name, call_id, result_json)


@dataclass
class ToolLoopResult:
    assistant_text: str
    # One entry per model call. For debugging/persistence.
    reasoning_summaries: list[str]
    tool_calls: list[dict[str, Any]]
    # Full Responses API input items after the loop (sanitized), suitable to
    # feed into a subsequent phase/model call within the same user turn.
    input_items: list[dict[str, Any]]


def run_responses_tool_loop(
    *,
    llm: ResponsesStreamingClient,
    instructions: str,
    input_items: list[dict[str, Any]],
    tools: list[dict[str, Any]],
    dispatch: Callable[[str, str], str],
    post_tool_output: Callable[[str, str, str], list[dict[str, Any]]] | None = None,
    callbacks: ToolLoopCallbacks | None = None,
    temperature: float = 0.2,
    max_rounds: int = 24,
) -> ToolLoopResult:
    """Run a streaming Responses API tool loop.

    Design goals:
    - Uses the Responses API.
    - Streams reasoning summary deltas and assistant output deltas.
    - Executes function calls and feeds `function_call_output` back in.
    """

    cb = callbacks or ToolLoopCallbacks()
    # We keep full within-turn input items to avoid relying on server state.
    in_items: list[dict[str, Any]] = list(input_items or [])
    normalized_tools = _normalize_tools_for_responses_api(list(tools or []))

    all_tool_calls: list[dict[str, Any]] = []
    reasoning_summaries: list[str] = []
    final_assistant_text = ""

    def _append_message(role: str, text: str) -> None:
        in_items.append(_input_text_message(role=role, text=text))

    # Ensure there is at least one user message; Responses API requires input.
    if not any(isinstance(x, dict) and x.get("type") == "message" for x in in_items):
        _append_message("user", "(no prior messages)")

    for _round in range(max(1, int(max_rounds))):
        # Stream buffers for this call
        summary_parts: dict[int, list[str]] = {}
        assistant_chunks: list[str] = []

        def _on_event(ev: dict[str, Any]) -> None:
            et = str(ev.get("type") or "")
            if et == "response.reasoning_summary_text.delta":
                delta = str(ev.get("delta") or "")
                try:
                    summary_index = int(ev.get("summary_index", 0))
                except Exception:
                    summary_index = 0
                summary_parts.setdefault(summary_index, []).append(delta)
                if cb.on_reasoning_summary_delta is not None and delta:
                    cb.on_reasoning_summary_delta(delta, summary_index)
                return
            if et == "response.reasoning_summary_part.added":
                try:
                    summary_index = int(ev.get("summary_index", 0))
                except Exception:
                    summary_index = 0
                if cb.on_reasoning_summary_part_added is not None:
                    cb.on_reasoning_summary_part_added(summary_index)
                return
            if et == "response.output_text.delta":
                delta = str(ev.get("delta") or "")
                if delta:
                    assistant_chunks.append(delta)
                    if cb.on_assistant_text_delta is not None:
                        cb.on_assistant_text_delta(delta)
                return

        resp = llm.responses_stream(
            instructions=instructions,
            input_items=in_items,
            tools=normalized_tools,
            temperature=temperature,
            parallel_tool_calls=False,
            reasoning_effort="high",
            reasoning_summary="detailed",
            text_verbosity="high",
            on_event=_on_event,
        )

        output_items = _coerce_output_items(resp)
        # Persist output items into the loop input (full local history).
        for item in output_items:
            in_items.append(_sanitize_response_item(item))

        # Capture reasoning summary text for this call (merged across indices).
        merged = "\n\n".join(
            "".join(summary_parts[k]) for k in sorted(summary_parts.keys())
        ).strip()
        if merged:
            reasoning_summaries.append(merged)

        calls = _extract_function_calls(output_items)
        if calls:
            for call in calls:
                name = call["name"]
                call_id = call["call_id"]
                args_json = call["arguments"]
                all_tool_calls.append(call)
                if cb.on_tool_call is not None:
                    cb.on_tool_call(name, args_json, call_id)
                result_json = dispatch(name, args_json)
                if cb.on_tool_result is not None:
                    cb.on_tool_result(name, call_id, result_json)
                in_items.append(
                    {
                        "type": "function_call_output",
                        "call_id": call_id,
                        "output": result_json,
                    }
                )
                if post_tool_output is not None:
                    try:
                        extra = post_tool_output(name, args_json, result_json)
                        if isinstance(extra, list):
                            for it in extra:
                                if isinstance(it, dict):
                                    in_items.append(it)
                    except Exception:
                        # Hooks must not break the tool loop.
                        pass
            continue

        # No tool calls: we consider this a final assistant output.
        # Prefer streamed deltas (more faithful); fall back to response parsing.
        if assistant_chunks:
            final_assistant_text = "".join(assistant_chunks).strip()
        else:
            # There may be multiple assistant message items; concatenate.
            final_assistant_text = "".join(
                _extract_assistant_text_from_output_item(it) for it in output_items
            ).strip()
        break
    else:
        raise RuntimeError(
            f"Model did not converge after max_rounds={max_rounds} (still returning tool calls)."
        )

    return ToolLoopResult(
        assistant_text=final_assistant_text,
        reasoning_summaries=reasoning_summaries,
        tool_calls=all_tool_calls,
        input_items=list(in_items),
    )
