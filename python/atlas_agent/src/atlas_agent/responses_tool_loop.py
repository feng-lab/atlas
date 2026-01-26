from __future__ import annotations

import itertools
import json
import logging
import math
import re
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Protocol

from .gateway_model import (
    gateway_model_matches_requested,
    openai_model_requires_gateway_model,
)

from .provider_tool_schema import (
    convert_tools_to_responses_wire,
    tighten_tools_schema_for_provider,
)

from .defaults import (
    CONTEXT_TRIM_MAX_RETRIES,
    FINAL_OUTPUT_CONTINUE_MAX_CALLS,
    GATEWAY_MODEL_DETECTION_MAX_RETRIES,
    PROACTIVE_CONTEXT_COMPACTION_MAX_ATTEMPTS_PER_CALL,
    PROACTIVE_CONTEXT_COMPACTION_TRIGGER_RATIO,
    TRANSIENT_NETWORK_BACKOFF_SECONDS,
    TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS,
    TRANSIENT_NETWORK_MAX_RETRIES,
)

from .llm_usage import extract_usage_delta_from_response

ContextOverflowHandler = Callable[[list[dict[str, Any]], BaseException], bool]

log = logging.getLogger(__name__)

ToolOutputPayload = str | list[dict[str, Any]]
PostToolOutputHook = Callable[[str, str, str], dict[str, Any] | None]


class PromptBudgetExceeded(RuntimeError):
    def __init__(self, *, estimated_tokens: int, limit_tokens: int):
        super().__init__(
            f"prompt budget exceeded (estimated_tokens={int(estimated_tokens)}, limit_tokens={int(limit_tokens)})"
        )
        self.estimated_tokens = int(estimated_tokens)
        self.limit_tokens = int(limit_tokens)


def _approx_token_count_from_bytes(nbytes: int) -> int:
    # Heuristic (provider-agnostic): token counts are roughly proportional to UTF-8 bytes.
    # We use 4 bytes/token as a conservative average.
    b = max(0, int(nbytes))
    return max(1, (b + 3) // 4)


def _is_inline_image_data_url(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    s = value.strip().lower()
    return s.startswith("data:image/")


def _redact_inline_image_data_urls(value: Any) -> Any:
    """Return a deep-copied JSON-serializable structure with inline image bytes removed.

    Proactive token estimation must not scale with `data:image/...;base64,...` text length.
    We only need a "large enough" estimate to detect context-window risk, not an exact count.
    """

    if _is_inline_image_data_url(value):
        return "data:image/...;base64,<omitted>"
    if isinstance(value, list):
        return [_redact_inline_image_data_urls(v) for v in value]
    if isinstance(value, dict):
        return {k: _redact_inline_image_data_urls(v) for k, v in value.items()}
    return value


@dataclass(frozen=True)
class _ImageTokenCostTileModel:
    base_tokens: int
    tile_tokens: int


@dataclass(frozen=True)
class _ImageTokenCostPatchModel:
    multiplier: float


def _image_token_cost_profile_for_model(
    model_name: str | None,
) -> _ImageTokenCostTileModel | _ImageTokenCostPatchModel | None:
    """Best-effort OpenAI image token profile for proactive estimation.

    Reference: OpenAI "Images and Vision" docs (Calculating costs).
    This is intentionally partial and only covers commonly used OpenAI models.
    Unknown models return None (caller may fall back to byte-based heuristics).
    """

    m = str(model_name or "").strip().lower()
    if not m:
        return None

    # Patch-based models (32px patches, capped at 1536 patches), with per-model multipliers.
    # Order matters: these prefixes overlap with broader families (e.g. gpt-5*).
    if m.startswith("gpt-4.1-mini"):
        return _ImageTokenCostPatchModel(multiplier=1.6)
    if m.startswith("gpt-4.1-nano"):
        return _ImageTokenCostPatchModel(multiplier=2.0)
    if m.startswith("o4-mini"):
        return _ImageTokenCostPatchModel(multiplier=1.72)
    if m.startswith("gpt-5-mini"):
        return _ImageTokenCostPatchModel(multiplier=1.62)
    if m.startswith("gpt-5-nano"):
        return _ImageTokenCostPatchModel(multiplier=2.0)

    # Tile-based models (512px tiles), with base + tile tokens.
    # Order matters: gpt-4o-mini must be checked before gpt-4o.
    if m.startswith("gpt-4o-mini"):
        return _ImageTokenCostTileModel(base_tokens=2833, tile_tokens=5667)
    if m.startswith("computer-use-preview"):
        return _ImageTokenCostTileModel(base_tokens=65, tile_tokens=129)
    if m.startswith("gpt-5-chat-latest"):
        return _ImageTokenCostTileModel(base_tokens=70, tile_tokens=140)
    if m.startswith("gpt-5"):
        return _ImageTokenCostTileModel(base_tokens=70, tile_tokens=140)
    if m.startswith("gpt-4.5"):
        return _ImageTokenCostTileModel(base_tokens=85, tile_tokens=170)
    if m.startswith("gpt-4.1"):
        return _ImageTokenCostTileModel(base_tokens=85, tile_tokens=170)
    if m.startswith("gpt-4o"):
        return _ImageTokenCostTileModel(base_tokens=85, tile_tokens=170)
    if m.startswith("o1-pro"):
        return _ImageTokenCostTileModel(base_tokens=75, tile_tokens=150)
    if m.startswith("o1"):
        return _ImageTokenCostTileModel(base_tokens=75, tile_tokens=150)
    if m.startswith("o3"):
        return _ImageTokenCostTileModel(base_tokens=75, tile_tokens=150)

    return None


def _estimate_image_tokens_tile_model(
    *,
    width_px: int,
    height_px: int,
    detail: str | None,
    base_tokens: int,
    tile_tokens: int,
) -> int:
    """Estimate image token cost for tile-based vision models."""

    w = max(1, int(width_px))
    h = max(1, int(height_px))

    d = str(detail or "auto").strip().lower()
    # For proactive estimation, treat auto as high: it is the safer upper bound
    # and avoids underestimating for typical screenshots/previews.
    if d == "low":
        return int(base_tokens)

    # High detail resizing rules (OpenAI docs):
    # 1) Scale to fit within a 2048x2048 square.
    # 2) Scale so the shortest side is 768px.
    # 3) Count 512px tiles in the resized image.
    # 4) Tokens = base + tile_tokens * tile_count.
    max_side = float(max(w, h))
    scale_2048 = 1.0
    if max_side > 2048.0:
        scale_2048 = 2048.0 / max_side
    w1 = float(w) * scale_2048
    h1 = float(h) * scale_2048

    min_side = float(min(w1, h1))
    if min_side <= 0:
        return int(base_tokens)
    scale_768 = 768.0 / min_side
    w2 = w1 * scale_768
    h2 = h1 * scale_768

    tiles_w = int(math.ceil(w2 / 512.0))
    tiles_h = int(math.ceil(h2 / 512.0))
    tiles = max(1, tiles_w) * max(1, tiles_h)
    return int(base_tokens) + int(tile_tokens) * int(tiles)


def _estimate_image_tokens_patch_model(
    *, width_px: int, height_px: int, multiplier: float
) -> int:
    """Estimate image token cost for patch-based vision models."""

    w = max(1, int(width_px))
    h = max(1, int(height_px))

    # Patch rules (OpenAI docs): split into 32px patches, cap at 1536 patches.
    def patch_count(width: int, height: int) -> int:
        return int(math.ceil(width / 32.0)) * int(math.ceil(height / 32.0))

    patches = patch_count(w, h)
    if patches > 1536:
        # Downscale so that patch_count <= 1536, preserving aspect ratio.
        # Start from the ideal scale factor, then shrink slightly if ceil() pushes us over.
        scale = math.sqrt(1536.0 / float(patches))
        w2 = max(1, int(math.floor(float(w) * scale)))
        h2 = max(1, int(math.floor(float(h) * scale)))
        patches = patch_count(w2, h2)

        # ceil() effects can still leave us slightly above; nudge down deterministically.
        guard = 0
        while patches > 1536 and guard < 4096 and (w2 > 1 or h2 > 1):
            guard += 1
            if w2 >= h2 and w2 > 1:
                w2 -= 1
            elif h2 > 1:
                h2 -= 1
            patches = patch_count(w2, h2)

    tokens = float(patches) * float(multiplier)
    return int(math.ceil(tokens))


def _estimate_image_tokens_openai(
    *,
    model_name: str | None,
    width_px: int | None,
    height_px: int | None,
    detail: str | None,
) -> int:
    """Best-effort OpenAI image token estimate for proactive prompt sizing."""

    profile = _image_token_cost_profile_for_model(model_name)
    if profile is None:
        return 0

    # If dimensions are unavailable, use a conservative "large enough" default.
    # - Tile models: 768x768 yields a stable upper bound for most aspect ratios.
    # - Patch models: use the maximum patch budget (1536) when unknown.
    if isinstance(profile, _ImageTokenCostPatchModel):
        if width_px is None or height_px is None or width_px <= 0 or height_px <= 0:
            return int(math.ceil(1536.0 * float(profile.multiplier)))
        return _estimate_image_tokens_patch_model(
            width_px=int(width_px),
            height_px=int(height_px),
            multiplier=profile.multiplier,
        )

    if width_px is None or height_px is None or width_px <= 0 or height_px <= 0:
        width_px = 768
        height_px = 768
    return _estimate_image_tokens_tile_model(
        width_px=int(width_px),
        height_px=int(height_px),
        detail=detail,
        base_tokens=profile.base_tokens,
        tile_tokens=profile.tile_tokens,
    )


def _extract_tool_call_args_by_call_id(
    input_items: list[dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    """Extract tool call args keyed by call_id (best-effort JSON parse)."""

    out: dict[str, dict[str, Any]] = {}
    for it in input_items or []:
        if not isinstance(it, dict):
            continue
        if str(it.get("type") or "") != "function_call":
            continue
        call_id = str(it.get("call_id") or "").strip()
        if not call_id:
            continue
        args_json = it.get("arguments")
        if not isinstance(args_json, str) or not args_json.strip():
            continue
        try:
            args = json.loads(args_json)
        except Exception:
            continue
        if isinstance(args, dict):
            out[call_id] = args
    return out


def _extract_image_dimensions_from_tool_call_args(
    args: dict[str, Any] | None,
) -> tuple[int | None, int | None]:
    if not isinstance(args, dict):
        return (None, None)
    try:
        w = int(args.get("width", 0) or 0)
    except Exception:
        w = 0
    try:
        h = int(args.get("height", 0) or 0)
    except Exception:
        h = 0
    if w > 0 and h > 0:
        return (w, h)
    return (None, None)


def _extract_image_dimensions_from_tool_output_parts(
    output_parts: Any,
) -> tuple[int | None, int | None]:
    """Extract width/height from our structured tool JSON result (best-effort).

    The tool loop often wraps tool results as:
      - input_text: "Tool JSON result:\\n{...json...}"
      - input_image: {image_url|file_id,...}

    We can parse that JSON reliably because we generated it (it is not free-form
    model text), and it reflects the actual output dimensions used by the tool.
    """

    if not isinstance(output_parts, list):
        return (None, None)
    for p in output_parts:
        if not isinstance(p, dict):
            continue
        if str(p.get("type") or "") != "input_text":
            continue
        text = p.get("text")
        if not isinstance(text, str) or not text:
            continue
        if not text.startswith("Tool JSON result:"):
            continue
        # Expected format: "Tool JSON result:\n{...}"
        newline = text.find("\n")
        if newline < 0:
            continue
        raw_json = text[newline + 1 :].strip()
        if not raw_json:
            continue
        try:
            obj = json.loads(raw_json)
        except Exception:
            continue
        if not isinstance(obj, dict):
            continue
        try:
            w = int(obj.get("width", 0) or 0)
        except Exception:
            w = 0
        try:
            h = int(obj.get("height", 0) or 0)
        except Exception:
            h = 0
        if w > 0 and h > 0:
            return (w, h)
    return (None, None)


def _extract_input_tokens_from_response(resp: dict[str, Any] | None) -> int | None:
    """Best-effort extraction of prompt/input token count from a Responses API response.

    This reuses the same parsing logic as the session meta tracking layer
    (extract_usage_delta_from_response) to avoid drift.
    """

    delta = extract_usage_delta_from_response(resp)
    if delta is None:
        return None
    try:
        n = int(delta.input_tokens) if delta.input_tokens is not None else None
    except Exception:
        n = None
    if n is None or n < 0:
        return None
    return int(n)


def _estimate_image_tokens_in_input_items(
    *, model_name: str | None, input_items: list[dict[str, Any]]
) -> int:
    """Estimate token cost of any input_image parts within input_items (best-effort)."""

    image_tokens = 0

    # Use structured tool call args (call_id → args dict) to estimate dimensions
    # for tool-produced images (e.g. scene_screenshot, animation_render_preview),
    # without parsing any natural-language text.
    call_args_by_id = _extract_tool_call_args_by_call_id(input_items or [])

    for it in input_items or []:
        if not isinstance(it, dict):
            continue
        itype = str(it.get("type") or "")

        if itype == "message":
            content = it.get("content")
            if not isinstance(content, list):
                continue
            for part in content:
                if not isinstance(part, dict):
                    continue
                if str(part.get("type") or "") != "input_image":
                    continue
                detail = part.get("detail")
                if not isinstance(detail, str):
                    detail = None
                image_tokens += _estimate_image_tokens_openai(
                    model_name=model_name,
                    width_px=None,
                    height_px=None,
                    detail=detail,
                )
            continue

        if itype == "function_call_output":
            call_id = str(it.get("call_id") or "").strip()
            outp = it.get("output")
            if not isinstance(outp, list):
                continue
            w, h = _extract_image_dimensions_from_tool_output_parts(outp)
            if w is None or h is None:
                w, h = _extract_image_dimensions_from_tool_call_args(
                    call_args_by_id.get(call_id)
                )
            for part in outp:
                if not isinstance(part, dict):
                    continue
                if str(part.get("type") or "") != "input_image":
                    continue
                detail = part.get("detail")
                if not isinstance(detail, str):
                    detail = None
                image_tokens += _estimate_image_tokens_openai(
                    model_name=model_name,
                    width_px=w,
                    height_px=h,
                    detail=detail,
                )

    return int(image_tokens)


def _estimate_input_items_delta_tokens(
    *, model_name: str | None, input_items: list[dict[str, Any]]
) -> int:
    """Estimate the token contribution of a list of newly appended input_items.

    This intentionally uses the same rough heuristics as _estimate_request_tokens,
    but scoped to only the delta items so we can add it to a known-accurate baseline
    from the model usage stats of the last successful call.
    """

    image_tokens = _estimate_image_tokens_in_input_items(
        model_name=model_name, input_items=input_items
    )

    redacted_items = _redact_inline_image_data_urls(list(input_items or []))
    try:
        raw = json.dumps(redacted_items, ensure_ascii=False, separators=(",", ":"))
    except Exception:
        raw = str(redacted_items)
    try:
        b = len(raw.encode("utf-8", errors="replace"))
    except Exception:
        b = len(raw)
    return _approx_token_count_from_bytes(b) + int(image_tokens)


def _estimate_request_tokens(
    *,
    model_name: str | None,
    instructions: str,
    input_items: list[dict[str, Any]],
    tools: list[dict[str, Any]],
) -> int:
    """Best-effort prompt token estimate for proactive compaction.

    This does NOT need to be exact; it only needs to detect "we are probably too large"
    early enough to compact and avoid a provider hard error.
    """

    image_tokens = _estimate_image_tokens_in_input_items(
        model_name=model_name, input_items=input_items
    )

    payload = {
        "instructions": str(instructions or ""),
        # Redact inline base64 image payloads so JSON size does not scale with them.
        "input_items": _redact_inline_image_data_urls(list(input_items or [])),
        "tools": list(tools or []),
    }
    try:
        # separators reduces overhead; ensure_ascii avoids byte inflation for unicode text.
        raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    except Exception:
        raw = str(payload)
    try:
        b = len(raw.encode("utf-8", errors="replace"))
    except Exception:
        b = len(raw)
    return _approx_token_count_from_bytes(b) + int(image_tokens)


class ResponsesStreamingClient(Protocol):
    def responses_stream(
        self,
        *,
        instructions: str,
        input_items: List[Dict[str, Any]],
        reasoning_effort: str | None,
        reasoning_summary: str | None,
        text_verbosity: str | None,
        tools: Optional[List[Dict[str, Any]]] = None,
        temperature: float | None = None,
        parallel_tool_calls: bool = False,
        on_event: Callable[[Dict[str, Any]], None] | None = None,
    ) -> Dict[str, Any]: ...


def _detect_gateway_model(resp: dict[str, Any] | None) -> str | None:
    if not isinstance(resp, dict):
        return None
    model = resp.get("model")
    if not isinstance(model, str) or not model.strip():
        return None
    return model.strip()


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
                            {
                                "type": (
                                    "output_text" if is_assistant else "input_text"
                                ),
                                "text": text,
                            }
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
        return {
            "type": "function_call",
            "call_id": call_id,
            "name": name,
            "arguments": arguments,
        }

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


def _looks_like_inline_image_url(value: Any) -> bool:
    """Return true for data URLs (base64 inline images).

    We treat these as high-cost prompt artifacts that should be ephemeral in the
    tool loop when Files API (file_id) is unavailable.
    """

    return _is_inline_image_data_url(value)


def _strip_inline_images_from_function_call_outputs(
    *, in_items: list[dict[str, Any]], call_ids: set[str]
) -> None:
    """Remove inline base64 images from selected function_call_output items.

    We keep all non-image parts (e.g. text headers + JSON results) so the model
    still has the semantic tool result in subsequent rounds without re-sending
    large base64 payloads.
    """

    if not call_ids:
        return
    for it in in_items:
        if not isinstance(it, dict):
            continue
        if str(it.get("type") or "") != "function_call_output":
            continue
        cid = str(it.get("call_id") or "").strip()
        if not cid or cid not in call_ids:
            continue
        out = it.get("output")
        if not isinstance(out, list):
            continue
        new_parts: list[dict[str, Any]] = []
        removed_any = False
        for p in out:
            if not isinstance(p, dict):
                continue
            if str(p.get("type") or "") == "input_image":
                image_url = p.get("image_url")
                if isinstance(image_url, str) and _looks_like_inline_image_url(
                    image_url
                ):
                    removed_any = True
                    continue
            new_parts.append(p)

        if removed_any:
            # Keep at least one part so the tool output isn't empty.
            it["output"] = (
                new_parts
                if new_parts
                else [
                    {
                        "type": "input_text",
                        "text": "(preview image omitted from prompt history to save context budget)",
                    }
                ]
            )


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


def _extract_web_search_calls(
    output_items: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Extract web_search_call output items (Responses API built-in tool).

    The exact payload varies by provider and SDK version. We keep this very
    small and only surface the action + status for logging/telemetry-free UX.
    """

    calls: list[dict[str, Any]] = []
    for item in output_items or []:
        if not isinstance(item, dict):
            continue
        if str(item.get("type") or "") != "web_search_call":
            continue
        status = item.get("status")
        if not isinstance(status, str) or not status.strip():
            status = None
        action_in = item.get("action")
        action: dict[str, Any] = {}
        if isinstance(action_in, dict):
            at = str(action_in.get("type") or "").strip()
            if at:
                action["type"] = at
            for k in ("query", "url", "pattern"):
                v = action_in.get(k)
                if isinstance(v, str) and v.strip():
                    action[k] = v.strip()
        calls.append({"status": status, "action": action})
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


def _extract_http_status_code(err: BaseException) -> int | None:
    """Best-effort HTTP status extraction from an exception chain."""

    for e in _iter_exception_chain(err):
        # OpenAI SDK errors commonly expose .status_code directly.
        status = getattr(e, "status_code", None)
        if isinstance(status, int):
            return status
        # Some wrappers carry an underlying response object.
        resp = getattr(e, "response", None)
        status2 = getattr(resp, "status_code", None)
        if isinstance(status2, int):
            return status2
    return None


def _is_gateway_http_5xx_error(err: BaseException) -> bool:
    """Return true for 5xx gateway/proxy errors worth extended retry.

    Rationale: many OpenAI-compatible gateways intermittently return 502/503/504,
    and a longer retry loop can recover without user intervention.
    """

    status = _extract_http_status_code(err)
    if status in {502, 503, 504}:
        return True

    # Fallback to message heuristics when status codes are not available.
    msg = str(err or "").lower()
    return any(
        n in msg
        for n in (
            "bad gateway",
            "service unavailable",
            "gateway timeout",
            "proxy error",
            "http/1.1 502",
            "http/1.1 503",
            "http/1.1 504",
        )
    )


def _is_rate_limit_error(err: BaseException) -> bool:
    """Return true for rate limiting (429 / token bucket / TPM).

    Note: Some OpenAI-compatible gateways return HTTP 200 with an error payload.
    In that case status codes may be missing, so we also rely on message
    heuristics.
    """

    status = _extract_http_status_code(err)
    if status == 429:
        return True

    # Best-effort: avoid hard dependency on the OpenAI SDK class.
    try:
        from openai import RateLimitError  # type: ignore
    except Exception:  # pragma: no cover
        RateLimitError = None

    for e in _iter_exception_chain(err):
        if RateLimitError is not None:
            try:
                if isinstance(e, RateLimitError):
                    return True
            except Exception:
                pass

        msg = str(e or "").lower()
        if any(
            n in msg
            for n in (
                "rate limit reached",
                "too many requests",
                "tpm",
                "tokens per min",
                "tokens per minute",
            )
        ):
            return True

    return False


def _extract_retry_after_seconds(err: BaseException) -> float | None:
    """Best-effort retry-after extraction for rate limit errors.

    We try, in order:
    - explicit exception attributes (retry_after/retry_after_seconds)
    - response headers (Retry-After)
    - error message patterns like "Please try again in 681ms".
    """

    def _coerce_float(v: Any) -> float | None:
        try:
            f = float(v)
        except Exception:
            return None
        if not math.isfinite(f):
            return None
        return f

    # 1) SDK-provided attributes / headers.
    for e in _iter_exception_chain(err):
        for attr in ("retry_after", "retry_after_seconds", "retry_after_ms"):
            v = getattr(e, attr, None)
            if v is None:
                continue
            f = _coerce_float(v)
            if f is None:
                continue
            if attr.endswith("_ms"):
                return f / 1000.0
            return f

        headers = getattr(e, "headers", None)
        if isinstance(headers, dict):
            ra = headers.get("retry-after") or headers.get("Retry-After")
            f = _coerce_float(ra)
            if f is not None and f >= 0.0:
                return f

        resp = getattr(e, "response", None)
        resp_headers = getattr(resp, "headers", None)
        if isinstance(resp_headers, dict):
            ra = resp_headers.get("retry-after") or resp_headers.get("Retry-After")
            f = _coerce_float(ra)
            if f is not None and f >= 0.0:
                return f

    # 2) Parse from message text (OpenAI frequently includes a recommended delay).
    msg = str(err or "")
    m = re.search(r"try again in\s+(\d+)\s*ms", msg, flags=re.IGNORECASE)
    if m:
        try:
            return max(0.0, float(m.group(1)) / 1000.0)
        except Exception:
            pass
    m = re.search(r"try again in\s+([0-9]*\.?[0-9]+)\s*s", msg, flags=re.IGNORECASE)
    if m:
        try:
            return max(0.0, float(m.group(1)))
        except Exception:
            pass

    return None


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
    on_reasoning_summary_complete: Callable[[str, int], None] | None = (
        None  # (text, call_index)
    )
    on_assistant_text_delta: Callable[[str], None] | None = None
    # Called once per Responses API call (per tool-loop round) after the response
    # payload has been received. Useful for logging provider metadata (e.g. the
    # actual model routed by an OpenAI-compatible gateway).
    on_response_meta: Callable[[dict[str, Any], int], None] | None = (
        None  # (resp, call_index)
    )
    on_tool_call: Callable[[str, str, str], None] | None = (
        None  # (name, args_json, call_id)
    )
    on_tool_result: Callable[[str, str, str], None] | None = (
        None  # (name, call_id, result_json)
    )
    # Called once per web search invocation observed in a model call.
    # Payload is a small dict: {"status":..., "action":{"type":"search|open_page|find_in_page", ...}}.
    on_web_search_call: Callable[[dict[str, Any], int], None] | None = (
        None  # (web_search_call, call_index)
    )


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
        return (
            str(it.get("type") or "") == "function_call"
            and str(it.get("call_id") or "") == cid
        )

    def _is_output_item(it: dict[str, Any]) -> bool:
        return (
            str(it.get("type") or "") == "function_call_output"
            and str(it.get("call_id") or "") == cid
        )

    in_items[:] = [
        it for it in in_items if not (_is_call_item(it) or _is_output_item(it))
    ]


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
        if (
            str(it.get("type") or "") == "message"
            and str(it.get("role") or "") == "user"
        ):
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
        in_items.append(
            _input_text_message(role="user", text="(context trimmed; continuing)")
        )

    return True


def run_responses_tool_loop(
    *,
    llm: ResponsesStreamingClient,
    instructions: str,
    input_items: list[dict[str, Any]],
    tools: list[dict[str, Any]],
    dispatch: Callable[[str, str], str],
    max_rounds: int,
    reasoning_effort: str | None,
    reasoning_summary: str | None,
    text_verbosity: str | None,
    ephemeral_inline_images: bool = False,
    post_tool_output: PostToolOutputHook | None = None,
    callbacks: ToolLoopCallbacks | None = None,
    temperature: float | None = None,
    on_context_overflow: ContextOverflowHandler | None = None,
    effective_input_budget_tokens: int | None = None,
    auto_compact_tokens: int | None = None,
) -> ToolLoopResult:
    """Run a streaming Responses API tool loop.

    Design goals:
    - Uses the Responses API.
    - Streams reasoning summary deltas and assistant output deltas.
    - Executes function calls and feeds `function_call_output` back in.
    """

    cb = callbacks or ToolLoopCallbacks()
    # effective_input_budget_tokens is the estimated usable token budget for the request payload we
    # send (instructions + input_items + tools). If a provider exposes only a *total* context window
    # (input + output), callers should compute an effective input budget by reserving headroom for
    # model output.
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
    # Ephemeral inline image attachments: when a tool output includes a base64
    # data URL, keep it only for the *next* model call. After the model has had a
    # chance to "see" the pixels once, strip the inline image from prompt history
    # to avoid bloating subsequent requests.
    pending_inline_image_call_ids: set[str] = set()

    # Proactive token estimation can be improved by using the model-reported
    # input token usage from the last successful call as a baseline, then only
    # estimating the contribution of newly appended input_items.
    last_call_input_tokens: int | None = None
    last_call_sent_in_items_len: int | None = None
    last_call_tools_id: int | None = None
    last_call_model: str | None = None

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

        requested_model = str(getattr(llm, "model", "") or "").strip()
        require_gateway_model = openai_model_requires_gateway_model(requested_model)

        # Retry transient network/proxy drops. Additionally, for OpenAI models we treat
        # missing `resp["model"]` as a gateway hiccup and retry cleanly (discarding the
        # payload, not persisting any meta, and not appending output items).
        transient_network_tries = 0
        gateway_http_tries = 0
        rate_limit_tries = 0
        gateway_model_miss_tries = 0
        while True:
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
                proactive_attempts = 0
                for _retry in range(CONTEXT_TRIM_MAX_RETRIES):
                    round_tools = [] if final_continue_calls > 0 else normalized_tools
                    estimated_tokens: int | None = None

                    # Proactive checkpoint compaction: if we can estimate that the next request
                    # is likely to exceed the model context window, compact BEFORE sending it.
                    if (
                        on_context_overflow is not None
                        and effective_input_budget_tokens is not None
                        and int(effective_input_budget_tokens) > 0
                    ):
                        # Clamp ratio to a sane range; this guards against accidental config edits.
                        try:
                            ratio = float(PROACTIVE_CONTEXT_COMPACTION_TRIGGER_RATIO)
                        except Exception:
                            ratio = 0.8
                        ratio = min(0.98, max(0.50, ratio))
                        # Two related thresholds:
                        # - `auto_compact_tokens`: a best-effort *hard* limit (provider metadata
                        #   when available, otherwise derived heuristics). Exceeding this tends to
                        #   correlate with provider context-window errors or upstream auto-truncation.
                        # - `PROACTIVE_CONTEXT_COMPACTION_TRIGGER_RATIO`: a softer, earlier trigger
                        #   for our proactive checkpoint compaction. Because token estimation is
                        #   intentionally rough, we start compacting before we are "right on the edge".
                        #
                        # Use the stricter (smaller) of the two so both signals remain meaningful.
                        ratio_limit = int(int(effective_input_budget_tokens) * ratio)
                        hard_limit = (
                            int(auto_compact_tokens)
                            if (
                                auto_compact_tokens is not None
                                and int(auto_compact_tokens) > 0
                            )
                            else None
                        )
                        limit = (
                            min(int(ratio_limit), int(hard_limit))
                            if hard_limit is not None
                            else int(ratio_limit)
                        )
                        if limit > 0 and proactive_attempts < int(
                            PROACTIVE_CONTEXT_COMPACTION_MAX_ATTEMPTS_PER_CALL
                        ):
                            # Prefer incremental estimation when we have an accurate baseline
                            # from the last call and the request shape is stable.
                            estimated_tokens = None
                            if (
                                last_call_input_tokens is not None
                                and last_call_sent_in_items_len is not None
                                and last_call_sent_in_items_len <= len(in_items)
                                and last_call_model == requested_model
                                and last_call_tools_id == id(round_tools)
                                # Inline-image ephemerality mutates historical items after a call.
                                # Keep the incremental path simple: fall back to full estimation.
                                and not bool(ephemeral_inline_images)
                            ):
                                delta_items = in_items[last_call_sent_in_items_len:]
                                delta_tokens = _estimate_input_items_delta_tokens(
                                    model_name=requested_model, input_items=delta_items
                                )
                                estimated_tokens = int(last_call_input_tokens) + int(
                                    delta_tokens
                                )
                            else:
                                estimated_tokens = _estimate_request_tokens(
                                    model_name=requested_model,
                                    instructions=instructions,
                                    input_items=in_items,
                                    tools=round_tools,
                                )
                            # The estimator is intentionally rough (provider-agnostic).
                            # Avoid overly aggressive compaction when we are only barely over
                            # the threshold; try the request and fall back to reactive trimming
                            # on a real provider context error.
                            margin = max(500, int(int(limit) * 0.05))
                            if estimated_tokens >= (limit + margin):
                                proactive_attempts += 1
                                try:
                                    did_compact = bool(
                                        on_context_overflow(
                                            in_items,
                                            PromptBudgetExceeded(
                                                estimated_tokens=int(estimated_tokens),
                                                limit_tokens=limit,
                                            ),
                                        )
                                    )
                                except Exception:
                                    did_compact = False
                                if did_compact:
                                    continue

                    try:
                        sent_in_items_len = len(in_items)
                        resp = llm.responses_stream(
                            instructions=instructions,
                            input_items=in_items,
                            reasoning_effort=reasoning_effort,
                            reasoning_summary=reasoning_summary,
                            text_verbosity=text_verbosity,
                            tools=round_tools,
                            temperature=temperature,
                            parallel_tool_calls=False,
                            on_event=_on_event,
                        )

                        # Record the accurate prompt token usage for the request we
                        # just sent, so subsequent proactive estimates can be incremental.
                        last_call_input_tokens = _extract_input_tokens_from_response(
                            resp
                        )
                        last_call_sent_in_items_len = int(sent_in_items_len)
                        last_call_tools_id = int(id(round_tools))
                        last_call_model = str(requested_model or "")
                        break
                    except Exception as e:
                        if not _is_context_length_error(e):
                            raise
                        # Prefer checkpoint compaction over blind dropping when
                        # the caller provides a handler.
                        if on_context_overflow is not None:
                            try:
                                if bool(on_context_overflow(in_items, e)):
                                    continue
                            except Exception:
                                # Compaction should never prevent fallback trimming.
                                pass
                        if not _trim_oldest_item_for_retry(in_items):
                            raise RuntimeError(
                                "Context window exceeded and could not trim further; start a new session/thread or reduce tool output sizes."
                            ) from e
            except Exception as e:
                # Some OpenAI-compatible gateways intermittently return 502/503/504
                # while they restart/rebalance. These usually recover quickly and are
                # worth a longer retry loop (similar to gateway-model detection).
                if _is_gateway_http_5xx_error(e) and gateway_http_tries < max(
                    0, int(GATEWAY_MODEL_DETECTION_MAX_RETRIES) - 1
                ):
                    import time

                    sleep_s = min(
                        float(TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS),
                        TRANSIENT_NETWORK_BACKOFF_SECONDS * (2.0**gateway_http_tries),
                    )
                    try:
                        status = _extract_http_status_code(e)
                        log.warning(
                            "Gateway/proxy HTTP %s; retrying "
                            "(requested_model=%s, attempt=%d/%d, sleep=%.2fs)",
                            str(status) if status is not None else "5xx",
                            requested_model or "?",
                            int(gateway_http_tries + 1),
                            int(GATEWAY_MODEL_DETECTION_MAX_RETRIES),
                            float(sleep_s),
                        )
                    except Exception:
                        pass
                    time.sleep(sleep_s)
                    gateway_http_tries += 1
                    continue

                # Handle 429 / TPM rate limit errors with a longer retry loop. Many
                # providers include an explicit "try again in Xms" hint; honor it
                # when available to recover automatically without user prompting.
                if _is_rate_limit_error(e) and rate_limit_tries < max(
                    0, int(GATEWAY_MODEL_DETECTION_MAX_RETRIES) - 1
                ):
                    import time

                    retry_after_s = _extract_retry_after_seconds(e)
                    if retry_after_s is None:
                        retry_after_s = TRANSIENT_NETWORK_BACKOFF_SECONDS * (
                            2.0**rate_limit_tries
                        )
                    # Add a small cushion so we don't wake up *exactly* at the
                    # boundary and immediately re-trigger the same limit.
                    retry_after_s = max(0.0, float(retry_after_s)) + 0.05
                    sleep_s = min(
                        float(TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS), retry_after_s
                    )
                    try:
                        status = _extract_http_status_code(e)
                        log.warning(
                            "Rate limited (HTTP %s); retrying "
                            "(requested_model=%s, attempt=%d/%d, sleep=%.2fs)",
                            str(status) if status is not None else "?",
                            requested_model or "?",
                            int(rate_limit_tries + 1),
                            int(GATEWAY_MODEL_DETECTION_MAX_RETRIES),
                            float(sleep_s),
                        )
                    except Exception:
                        pass
                    time.sleep(sleep_s)
                    rate_limit_tries += 1
                    continue

                if _is_transient_network_error(e) and transient_network_tries < max(
                    0, int(TRANSIENT_NETWORK_MAX_RETRIES) - 1
                ):
                    import time

                    time.sleep(
                        min(
                            float(TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS),
                            TRANSIENT_NETWORK_BACKOFF_SECONDS
                            * (2.0**transient_network_tries),
                        )
                    )
                    transient_network_tries += 1
                    continue
                raise
            if resp is None:
                raise RuntimeError(
                    "Responses stream failed without producing a response."
                )

            if require_gateway_model:
                gateway_model = _detect_gateway_model(resp)
                if (gateway_model is None) or (
                    not gateway_model_matches_requested(requested_model, gateway_model)
                ):
                    if gateway_model_miss_tries < max(
                        0, int(GATEWAY_MODEL_DETECTION_MAX_RETRIES) - 1
                    ):
                        try:
                            if gateway_model is None:
                                why = "missing"
                                got = "?"
                            else:
                                why = "mismatched"
                                got = str(gateway_model)
                            log.warning(
                                "Gateway model %s in provider response; retrying "
                                "(requested_model=%s, got=%s, attempt=%d/%d)",
                                why,
                                requested_model or "?",
                                got,
                                int(gateway_model_miss_tries + 1),
                                int(GATEWAY_MODEL_DETECTION_MAX_RETRIES),
                            )
                        except Exception:
                            pass
                        import time

                        time.sleep(
                            min(
                                float(TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS),
                                TRANSIENT_NETWORK_BACKOFF_SECONDS
                                * (2.0**gateway_model_miss_tries),
                            )
                        )
                        gateway_model_miss_tries += 1
                        continue
                    raise RuntimeError(
                        "Provider returned an invalid routed model name (missing or unexpected resp['model']). "
                        f"requested_model={requested_model!r}, gateway_model={gateway_model!r}. "
                        "This usually indicates an upstream gateway/proxy issue."
                    )

            break

        if cb.on_response_meta is not None:
            try:
                cb.on_response_meta(resp, int(_round))
            except Exception:
                # Meta callbacks must not break tool execution.
                pass

        # After a successful model call, drop any inline base64 images from tool
        # outputs that were included for this call. This keeps images ephemeral:
        # visible to the model once, then removed to avoid repeated prompt bloat.
        if ephemeral_inline_images and pending_inline_image_call_ids:
            try:
                _strip_inline_images_from_function_call_outputs(
                    in_items=in_items, call_ids=pending_inline_image_call_ids
                )
            except Exception:
                pass
            pending_inline_image_call_ids.clear()

        output_items = _coerce_output_items(resp)
        # Web search (provider built-in): log calls for UI/session history. This
        # does not require local dispatch and does not drive the tool loop.
        if cb.on_web_search_call is not None:
            try:
                for ws in _extract_web_search_calls(output_items):
                    try:
                        cb.on_web_search_call(ws, int(_round))
                    except Exception:
                        # Web-search callbacks must not break tool execution.
                        pass
            except Exception:
                pass
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
                if post_tool_output is not None:
                    try:
                        hook = post_tool_output(name, args_json, result_json)
                    except Exception:
                        hook = None
                else:
                    hook = None

                output_payload: ToolOutputPayload = result_json
                extra_items: list[dict[str, Any]] = []
                if isinstance(hook, dict):
                    maybe_output = hook.get("output")
                    if isinstance(maybe_output, str):
                        output_payload = maybe_output
                    elif isinstance(maybe_output, list):
                        # Best-effort validation: ensure it is a list of dict parts.
                        parts: list[dict[str, Any]] = []
                        for p in maybe_output:
                            if isinstance(p, dict):
                                parts.append(p)
                        if parts:
                            output_payload = parts
                    maybe_extra = hook.get("extra_input_items")
                    if isinstance(maybe_extra, list):
                        for it in maybe_extra:
                            if isinstance(it, dict):
                                extra_items.append(it)

                in_items.append(
                    {
                        "type": "function_call_output",
                        "call_id": call_id,
                        "output": output_payload,
                    }
                )
                if isinstance(output_payload, list):
                    if ephemeral_inline_images:
                        for p in output_payload:
                            if not isinstance(p, dict):
                                continue
                            if str(p.get("type") or "") != "input_image":
                                continue
                            if _looks_like_inline_image_url(p.get("image_url")):
                                pending_inline_image_call_ids.add(str(call_id))
                                break
                if extra_items:
                    in_items.extend(extra_items)
            continue

        # No tool calls: we consider this a final assistant output.
        stream_text = "".join(assistant_chunks).strip()
        # There may be multiple assistant message items; concatenate.
        parsed_text = "".join(
            _extract_assistant_text_from_output_item(it) for it in output_items
        ).strip()
        candidate = _choose_best_final_assistant_text(
            stream_text=stream_text, parsed_text=parsed_text
        )

        if candidate:
            final_assistant_text = _merge_continuation_text(
                prev=final_assistant_text, new=candidate
            )

        # Some providers return a final assistant message with status="incomplete" when they hit
        # max output tokens, even though there are no tool calls. Auto-continue a few times so
        # the user still gets a complete recap without having to type "continue".
        status = (
            str(resp.get("status") or "").strip().lower()
            if isinstance(resp, dict)
            else ""
        )
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
