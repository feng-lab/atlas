from __future__ import annotations

import itertools
import json
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Protocol

CONTEXT_TRIM_MAX_RETRIES = 32

# Best-effort retry for transient network/proxy issues during streaming.
#
# Rationale: some OpenAI-compatible providers occasionally terminate HTTP/1.1 chunked
# responses early ("incomplete chunked read"). This is not a request error and can
# usually be resolved by retrying the same request.
TRANSIENT_NETWORK_MAX_RETRIES = 3
TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.6


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


def _normalize_tool_parameters_schema(params: Any) -> dict[str, Any]:
    """Best-effort normalization for provider compatibility.

    Some OpenAI-compatible providers validate tool schemas using a strict subset
    of JSON Schema (similar to OpenAI Structured Outputs), requiring:
    - object schemas to set additionalProperties=false
    - required to be present and include every key in properties
    - array schemas to include items

    We apply this *only* to schemas that declare properties (i.e., structured
    objects). We intentionally do not "tighten" generic JSON objects (object
    schemas without properties), since those often represent arbitrary payloads
    such as typed scene values.
    """

    if not isinstance(params, dict):
        return {"type": "object", "properties": {}}
    out: dict[str, Any] = dict(params)
    if "type" not in out:
        out["type"] = "object"
    if out.get("type") == "object" and not isinstance(out.get("properties"), dict):
        out["properties"] = {}

    def _tighten(node: Any) -> Any:
        if not isinstance(node, dict):
            return node
        fixed: dict[str, Any] = dict(node)

        for comb in ("anyOf", "oneOf", "allOf"):
            v = fixed.get(comb)
            if isinstance(v, list):
                fixed[comb] = [_tighten(x) for x in v]

        t = fixed.get("type")
        types: set[str] = set()
        if isinstance(t, str):
            types.add(t)
        elif isinstance(t, list):
            for it in t:
                if isinstance(it, str):
                    types.add(it)

        if "array" in types:
            items = fixed.get("items")
            if items is None:
                fixed["items"] = {}
            else:
                fixed["items"] = _tighten(items)

        if "object" in types and isinstance(fixed.get("properties"), dict):
            props = fixed.get("properties") or {}
            if isinstance(props, dict):
                fixed["properties"] = {str(k): _tighten(v) for k, v in props.items()}
                fixed["required"] = list(props.keys())
                fixed["additionalProperties"] = False

        return fixed

    tightened = _tighten(out)
    return tightened if isinstance(tightened, dict) else out


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
            params = _normalize_tool_parameters_schema(fn.get("parameters"))
            converted: dict[str, Any] = {
                "type": "function",
                "name": name,
                "parameters": params,
                "strict": bool(fn.get("strict", False)),
            }
            desc = fn.get("description")
            if isinstance(desc, str) and desc.strip():
                converted["description"] = desc.strip()
            out.append(converted)
            continue

        # Already Responses-style tool shape.
        fixed = dict(t)
        fixed["parameters"] = _normalize_tool_parameters_schema(fixed.get("parameters"))
        if "strict" not in fixed:
            fixed["strict"] = False
        out.append(fixed)

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

    # Providers vary in what extra fields appear on output items (e.g., status).
    # Rather than trying to strip an ever-growing denylist, rebuild a minimal,
    # provider-compatible input item shape by type.
    itype = str(item.get("type") or "")

    if itype == "message":
        role = str(item.get("role") or "").strip() or "assistant"
        is_assistant = role == "assistant"
        content_in = item.get("content")
        content: list[dict[str, Any]] = []
        if isinstance(content_in, list):
            for part in content_in:
                if not isinstance(part, dict):
                    continue
                ptype = str(part.get("type") or "")
                if ptype in {"output_text", "input_text", "text"}:
                    text = part.get("text")
                    if isinstance(text, str) and text:
                        # Providers generally expect:
                        # - user input parts: input_text
                        # - assistant history parts: output_text
                        content.append(
                            {"type": ("output_text" if is_assistant else "input_text"), "text": text}
                        )
                elif ptype == "refusal":
                    refusal = part.get("refusal")
                    if isinstance(refusal, str) and refusal:
                        content.append({"type": "refusal", "refusal": refusal})
                elif ptype in {"input_image", "output_image"}:
                    # Best-effort: keep images by URL/data URL when present.
                    # For assistant history, some providers only accept output_text/refusal.
                    if is_assistant:
                        continue
                    image_url = None
                    if isinstance(part.get("image_url"), str):
                        image_url = part.get("image_url")
                    elif isinstance(part.get("image_url"), dict):
                        iu = part.get("image_url")
                        if isinstance(iu.get("url"), str):
                            image_url = iu.get("url")
                    if isinstance(image_url, str) and image_url.strip():
                        content.append({"type": "input_image", "image_url": image_url})

        if not content:
            return {}
        out: dict[str, Any] = {"type": "message", "role": role, "content": content}
        name = item.get("name")
        if isinstance(name, str) and name.strip():
            out["name"] = name.strip()
        return out

    if itype == "function_call":
        name = str(item.get("name") or "").strip()
        call_id = str(item.get("call_id") or "").strip()
        arguments = item.get("arguments")
        if not isinstance(arguments, str):
            try:
                arguments = json.dumps(arguments, ensure_ascii=False)
            except Exception:
                arguments = str(arguments)
        if not name or not call_id:
            return {}
        return {"type": "function_call", "call_id": call_id, "name": name, "arguments": arguments}

    if itype == "function_call_output":
        call_id = str(item.get("call_id") or "").strip()
        output = item.get("output")
        if not isinstance(output, str):
            try:
                output = json.dumps(output, ensure_ascii=False)
            except Exception:
                output = str(output)
        if not call_id:
            return {}
        return {"type": "function_call_output", "call_id": call_id, "output": output}

    # Unknown item types are dropped.
    return {}


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


def _iter_exception_chain(err: BaseException) -> list[BaseException]:
    out: list[BaseException] = []
    seen: set[int] = set()
    cur: BaseException | None = err
    while cur is not None and id(cur) not in seen:
        seen.add(id(cur))
        out.append(cur)
        nxt = getattr(cur, "__cause__", None) or getattr(cur, "__context__", None)
        cur = nxt if isinstance(nxt, BaseException) else None
    return out


def _is_transient_network_error(err: BaseException) -> bool:
    """Return true for errors where retrying the same request is reasonable."""

    # Avoid importing optional deps at module import time; best-effort.
    try:
        import httpx  # type: ignore
    except Exception:  # pragma: no cover
        httpx = None
    try:
        from openai import APIConnectionError, APIError, APITimeoutError, RateLimitError  # type: ignore
    except Exception:  # pragma: no cover
        APIConnectionError = APIError = APITimeoutError = RateLimitError = None

    needles = (
        "incomplete chunked read",
        "peer closed connection",
        "server disconnected",
        "connection reset by peer",
        "connection aborted",
        "timed out",
        "timeout",
        "temporarily unavailable",
        "proxy error",
        "bad gateway",
        "service unavailable",
        "gateway timeout",
    )

    for e in _iter_exception_chain(err):
        msg = str(e or "").lower()
        if any(n in msg for n in needles):
            return True

        if httpx is not None:
            try:
                if isinstance(
                    e,
                    (
                        httpx.TimeoutException,
                        httpx.ReadError,
                        httpx.RemoteProtocolError,
                        httpx.ConnectError,
                    ),
                ):
                    return True
            except Exception:
                pass

        if APIConnectionError is not None:
            try:
                if isinstance(e, (APIConnectionError, APITimeoutError, RateLimitError)):
                    return True
            except Exception:
                pass
        if APIError is not None:
            try:
                if isinstance(e, APIError):
                    status = getattr(e, "status_code", None)
                    if isinstance(status, int) and status >= 500:
                        return True
            except Exception:
                pass

    return False


@dataclass(slots=True)
class ToolLoopCallbacks:
    """Optional streaming callbacks for rendering a streaming CLI."""

    on_phase_start: Callable[[str], None] | None = None
    on_phase_end: Callable[[str], None] | None = None
    on_reasoning_summary_delta: Callable[[str, int], None] | None = None
    on_reasoning_summary_part_added: Callable[[int], None] | None = None
    # Called once per Responses API call (per tool-loop round), after the stream
    # completes and the final reasoning summary text has been assembled, and
    # BEFORE any tool calls from that response are executed.
    #
    # This lets callers persist reasoning summaries in the same chronological
    # order as tool call events.
    on_reasoning_summary_complete: Callable[[str, int], None] | None = None  # (text, call_index)
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


class ToolLoopNonConverged(RuntimeError):
    """Raised when the tool loop hits max_rounds without a final assistant message.

    This is not a fatal "agent error" by itself: callers may choose to continue
    with a larger/unbounded max_rounds, or run a forced finalization call with
    tools disabled (to produce a user-facing progress update).
    """

    def __init__(
        self,
        message: str,
        *,
        max_rounds: int,
        rounds_completed: int,
        reasoning_summaries: list[str],
        tool_calls: list[dict[str, Any]],
        input_items: list[dict[str, Any]],
    ):
        super().__init__(message)
        self.max_rounds = int(max_rounds)
        self.rounds_completed = int(rounds_completed)
        self.reasoning_summaries = list(reasoning_summaries or [])
        self.tool_calls = list(tool_calls or [])
        self.input_items = list(input_items or [])


def _is_context_length_error(exc: BaseException) -> bool:
    """Best-effort detection for context-window overflow errors.

    Providers and SDK versions format these errors differently. We use a simple
    string-based heuristic so the tool loop can trim older context and retry
    rather than hard-failing.
    """

    msg = str(exc or "").lower()
    needles = (
        "context length",
        "context_length",
        "context window",
        "maximum context",
        "too many tokens",
        "max tokens",
        "maximum number of tokens",
        "reduce the length",
        "input is too long",
        "request too large",
        "token limit",
    )
    return any(n in msg for n in needles)


def _drop_call_pair(in_items: list[dict[str, Any]], *, call_id: str) -> None:
    """Remove both sides of a function call/output pair (best-effort)."""

    cid = str(call_id or "")
    if not cid:
        return

    def _is_call_item(it: dict[str, Any]) -> bool:
        return str(it.get("type") or "") == "function_call" and str(it.get("call_id") or "") == cid

    def _is_output_item(it: dict[str, Any]) -> bool:
        return str(it.get("type") or "") == "function_call_output" and str(it.get("call_id") or "") == cid

    in_items[:] = [it for it in in_items if not (_is_call_item(it) or _is_output_item(it))]


def _trim_oldest_item_for_retry(in_items: list[dict[str, Any]]) -> bool:
    """Trim one oldest non-essential item from in_items.

    Returns true when something was removed, false when we cannot trim further.
    """

    if not isinstance(in_items, list) or len(in_items) <= 1:
        return False

    # Try to keep at least one user message (Responses API requirement).
    last_user_idx: int | None = None
    for i in range(len(in_items) - 1, -1, -1):
        it = in_items[i]
        if not isinstance(it, dict):
            continue
        if str(it.get("type") or "") == "message" and str(it.get("role") or "") == "user":
            last_user_idx = i
            break

    # Remove from the front while avoiding the last user message when possible.
    rm_idx = 0
    if last_user_idx == 0 and len(in_items) > 1:
        rm_idx = 1
    if last_user_idx is not None and rm_idx == last_user_idx and len(in_items) > 1:
        rm_idx = 0 if last_user_idx != 0 else 1

    try:
        removed = in_items.pop(rm_idx)
    except Exception:
        return False

    if isinstance(removed, dict):
        rtype = str(removed.get("type") or "")
        if rtype in {"function_call", "function_call_output"}:
            cid = str(removed.get("call_id") or "")
            if cid:
                _drop_call_pair(in_items, call_id=cid)

    # Ensure there is still at least one user message; otherwise add a placeholder.
    if not any(
        isinstance(it, dict)
        and str(it.get("type") or "") == "message"
        and str(it.get("role") or "") == "user"
        for it in in_items
    ):
        in_items.append(_input_text_message(role="user", text="(context trimmed; continuing)"))

    return True


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
    reasoning_effort: str | None = "high",
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

    max_rounds_i = int(max_rounds)
    # max_rounds<=0 means "unbounded": keep going until the model stops emitting tool calls.
    round_iter = itertools.count() if max_rounds_i <= 0 else range(max(1, max_rounds_i))

    for _round in round_iter:
        resp: dict[str, Any] | None = None
        summary_parts: dict[int, list[str]] = {}
        assistant_chunks: list[str] = []

        # Retry transient network/proxy drops. This resets our local stream buffers so
        # the final merged assistant_text/reasoning summary does not duplicate.
        for net_try in range(max(1, int(TRANSIENT_NETWORK_MAX_RETRIES))):
            # Stream buffers for this attempt
            summary_parts = {}
            assistant_chunks = []

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

            # Best-effort retry loop for context window overflows: trim older items and retry.
            resp = None
            try:
                for _retry in range(CONTEXT_TRIM_MAX_RETRIES):
                    try:
                        resp = llm.responses_stream(
                            instructions=instructions,
                            input_items=in_items,
                            tools=normalized_tools,
                            temperature=temperature,
                            parallel_tool_calls=False,
                            reasoning_effort=reasoning_effort,
                            reasoning_summary="detailed",
                            text_verbosity="high",
                            on_event=_on_event,
                        )
                        break
                    except Exception as e:
                        if not _is_context_length_error(e):
                            raise
                        if not _trim_oldest_item_for_retry(in_items):
                            raise RuntimeError(
                                "Context window exceeded and could not trim further; start a new session/thread or reduce tool output sizes."
                            ) from e
            except Exception as e:
                if _is_transient_network_error(e) and net_try < TRANSIENT_NETWORK_MAX_RETRIES - 1:
                    import time

                    time.sleep(TRANSIENT_NETWORK_BACKOFF_SECONDS * (2**net_try))
                    continue
                raise
            if resp is not None:
                break
        if resp is None:
            raise RuntimeError("Responses stream failed without producing a response.")

        output_items = _coerce_output_items(resp)
        # Persist output items into the loop input (full local history).
        for item in output_items:
            sanitized = _sanitize_response_item(item)
            if sanitized:
                in_items.append(sanitized)

        # Capture reasoning summary text for this call (merged across indices).
        merged = "\n\n".join(
            "".join(summary_parts[k]) for k in sorted(summary_parts.keys())
        ).strip()
        if merged:
            reasoning_summaries.append(merged)
            if cb.on_reasoning_summary_complete is not None:
                try:
                    cb.on_reasoning_summary_complete(merged, int(_round))
                except Exception:
                    # Persistence callbacks must not break tool execution.
                    pass

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
        msg = (
            f"Model did not converge after max_rounds={max_rounds_i} (still returning tool calls). "
            "Consider increasing --max-rounds (or set it to 0 for unlimited), or improve tool batching."
        )
        raise ToolLoopNonConverged(
            msg,
            max_rounds=max_rounds_i,
            rounds_completed=max_rounds_i,
            reasoning_summaries=reasoning_summaries,
            tool_calls=all_tool_calls,
            input_items=list(in_items),
        )

    return ToolLoopResult(
        assistant_text=final_assistant_text,
        reasoning_summaries=reasoning_summaries,
        tool_calls=all_tool_calls,
        input_items=list(in_items),
    )
