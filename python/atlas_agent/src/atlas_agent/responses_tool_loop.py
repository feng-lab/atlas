from __future__ import annotations

import itertools
import json
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Protocol

from .provider_tool_schema import (
    convert_tools_to_responses_wire,
    tighten_tools_schema_for_provider,
)

CONTEXT_TRIM_MAX_RETRIES = 32

# Best-effort retry for transient network/proxy issues during streaming.
#
# Rationale: some OpenAI-compatible providers occasionally terminate HTTP/1.1 chunked
# responses early ("incomplete chunked read"). This is not a request error and can
# usually be resolved by retrying the same request.
TRANSIENT_NETWORK_MAX_RETRIES = 3
TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.6

# When the model returns a final response with status="incomplete" (max output tokens) or
# produces an empty final message, we automatically ask it to continue a few times to
# recover a complete user-facing answer without requiring the user to type "continue".
FINAL_OUTPUT_CONTINUE_MAX_CALLS = 8


class ResponsesStreamingClient(Protocol):
    def responses_stream(
        self,
        *,
        instructions: str,
        input_items: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]] = None,
        temperature: float | None = None,
        parallel_tool_calls: bool = False,
        reasoning_effort: str | None = "high",
        reasoning_summary: str | None = "detailed",
        text_verbosity: str | None = "high",
        on_event: Callable[[Dict[str, Any]], None] | None = None,
    ) -> Dict[str, Any]: ...


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
            ptype = str(p.get("type") or "")
            if ptype in {"output_text", "text"}:
                out.append(str(p.get("text") or ""))
            elif ptype == "refusal":
                out.append(str(p.get("refusal") or ""))
    return "".join(out)


def _choose_best_final_assistant_text(*, stream_text: str, parsed_text: str) -> str:
    """Pick the most complete assistant output between streaming deltas and parsed output items.

    Some OpenAI-compatible providers occasionally drop the tail of streamed text deltas
    while still returning the full output in the final response payload.
    """

    st = (stream_text or "").strip()
    pt = (parsed_text or "").strip()

    if not st:
        return pt
    if not pt:
        return st

    # If one contains the other, keep the longer "superset" to avoid truncation.
    if pt.startswith(st) and len(pt) >= len(st):
        return pt
    if st.startswith(pt) and len(st) >= len(pt):
        return st

    # Otherwise, prefer the longer one.
    return pt if len(pt) > len(st) else st


def _merge_continuation_text(*, prev: str, new: str) -> str:
    """Merge follow-up 'continue' text with previously collected assistant output."""

    a = (prev or "").rstrip()
    b = (new or "").lstrip()
    if not a:
        return b
    if not b:
        return a

    # Avoid common repetition patterns (provider retries may resend full/partial text).
    if b.startswith(a):
        return b
    if a.endswith(b):
        return a
    if b in a:
        return a
    if a in b:
        return b

    sep = "\n" if (a and not a.endswith("\n")) else ""
    return f"{a}{sep}\n{b}".strip()


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
    # Called once per Responses API call (per tool-loop round) after the response
    # payload has been received. Useful for logging provider metadata (e.g. the
    # actual model routed by an OpenAI-compatible gateway).
    on_response_meta: Callable[[dict[str, Any], int], None] | None = None  # (resp, call_index)
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
    temperature: float | None = None,
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
    # Tool normalization happens in two stages:
    # 1) Wire adapter: Chat Completions tool shape -> Responses tool shape.
    # 2) Provider-border schema tightening: satisfy strict JSON schema validators.
    tools_wire = convert_tools_to_responses_wire(list(tools or [])) or []
    normalized_tools = tighten_tools_schema_for_provider(tools_wire) or []

    all_tool_calls: list[dict[str, Any]] = []
    reasoning_summaries: list[str] = []
    final_assistant_text = ""
    final_continue_calls = 0
    internal_continue_prompts: list[str] = []

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
                        round_tools = [] if final_continue_calls > 0 else normalized_tools
                        resp = llm.responses_stream(
                            instructions=instructions,
                            input_items=in_items,
                            tools=round_tools,
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

        if cb.on_response_meta is not None:
            try:
                cb.on_response_meta(resp, int(_round))
            except Exception:
                # Meta callbacks must not break tool execution.
                pass

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
        stream_text = "".join(assistant_chunks).strip()
        # There may be multiple assistant message items; concatenate.
        parsed_text = "".join(_extract_assistant_text_from_output_item(it) for it in output_items).strip()
        candidate = _choose_best_final_assistant_text(
            stream_text=stream_text, parsed_text=parsed_text
        )

        if candidate:
            final_assistant_text = _merge_continuation_text(prev=final_assistant_text, new=candidate)

        # Some providers return a final assistant message with status="incomplete" when they hit
        # max output tokens, even though there are no tool calls. Auto-continue a few times so
        # the user still gets a complete recap without having to type "continue".
        status = str(resp.get("status") or "").strip().lower() if isinstance(resp, dict) else ""
        is_incomplete = status == "incomplete"
        needs_continue = bool(is_incomplete or not candidate)
        if needs_continue and final_continue_calls < FINAL_OUTPUT_CONTINUE_MAX_CALLS:
            final_continue_calls += 1
            if not candidate:
                prompt = (
                    "INTERNAL TOOLLOOP (not user intent): The previous assistant output was empty or truncated.\n"
                    "Tools are disabled for this continuation call. Produce a user-facing final answer now.\n"
                    "Do not ask the user for more input unless strictly required."
                )
            else:
                prompt = (
                    "INTERNAL TOOLLOOP (not user intent): Continue writing the final user-facing answer.\n"
                    "Tools are disabled for this continuation call. Do not repeat previously produced text.\n"
                    "Finish the final user-facing answer."
                )
            internal_continue_prompts.append(prompt)
            _append_message("user", prompt)
            continue

        if is_incomplete and final_assistant_text:
            final_assistant_text = (
                final_assistant_text.rstrip()
                + "\n\n(Note: output was truncated by the model/provider; reply “continue” to request the remaining text.)"
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

    # Do not leak internal toolloop control prompts into subsequent phases/turns.
    # These are orchestration directives, not user messages, and can confuse some
    # models/providers if left in the conversation history (e.g., the model may
    # incorrectly claim the user asked it to avoid tools).
    if internal_continue_prompts:
        internal_set = set(internal_continue_prompts)

        def _is_internal_continue_message(it: dict[str, Any]) -> bool:
            if not isinstance(it, dict):
                return False
            if str(it.get("type") or "") != "message":
                return False
            if str(it.get("role") or "") != "user":
                return False
            parts = it.get("content") or []
            if not isinstance(parts, list):
                return False
            text_parts: list[str] = []
            for p in parts:
                if isinstance(p, dict) and isinstance(p.get("text"), str):
                    text_parts.append(p.get("text") or "")
            msg_text = "".join(text_parts)
            return msg_text in internal_set

        in_items = [it for it in in_items if not _is_internal_continue_message(it)]

    return ToolLoopResult(
        assistant_text=final_assistant_text,
        reasoning_summaries=reasoning_summaries,
        tool_calls=all_tool_calls,
        input_items=list(in_items),
    )
