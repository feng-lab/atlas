from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping


LLM_USAGE_CALLS = "calls"
LLM_USAGE_CALLS_WITH_USAGE = "calls_with_usage"
LLM_USAGE_CALLS_WITH_CACHED_TOKENS = "calls_with_cached_tokens"
LLM_USAGE_INPUT_TOKENS = "input_tokens"
LLM_USAGE_OUTPUT_TOKENS = "output_tokens"
LLM_USAGE_CACHED_TOKENS = "cached_tokens"
LLM_USAGE_UNCACHED_INPUT_TOKENS = "uncached_input_tokens"


def _as_nonneg_int(v: Any) -> int | None:
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return int(v) if int(v) >= 0 else None
    if isinstance(v, float) and v.is_integer():
        n = int(v)
        return n if n >= 0 else None
    return None


@dataclass(frozen=True, slots=True)
class LlmUsageDelta:
    """Per-call, provider-reported usage counts (best-effort).

    Fields are aligned with typical billing:
    - input_tokens
    - output_tokens
    - cached_tokens (optional; when provider reports it)
    - uncached_input_tokens (optional; derived from input - cached when both are present)

    We intentionally do NOT model "total_tokens" here because most providers bill
    input/output separately, and "total" can be confusing in user-facing output.
    """

    input_tokens: int | None = None
    output_tokens: int | None = None
    cached_tokens: int | None = None
    uncached_input_tokens: int | None = None

    def has_any(self) -> bool:
        return any(
            v is not None
            for v in (
                self.input_tokens,
                self.output_tokens,
                self.cached_tokens,
                self.uncached_input_tokens,
            )
        )

    def to_dict(self) -> dict[str, int]:
        out: dict[str, int] = {}
        if isinstance(self.input_tokens, int):
            out[LLM_USAGE_INPUT_TOKENS] = int(self.input_tokens)
        if isinstance(self.output_tokens, int):
            out[LLM_USAGE_OUTPUT_TOKENS] = int(self.output_tokens)
        if isinstance(self.cached_tokens, int):
            out[LLM_USAGE_CACHED_TOKENS] = int(self.cached_tokens)
        if isinstance(self.uncached_input_tokens, int):
            out[LLM_USAGE_UNCACHED_INPUT_TOKENS] = int(self.uncached_input_tokens)
        return out


@dataclass(slots=True)
class LlmUsageTotals:
    """Running totals across calls (per-turn or per-session)."""

    calls: int = 0
    calls_with_usage: int = 0
    calls_with_cached_tokens: int = 0
    input_tokens: int = 0
    output_tokens: int = 0
    cached_tokens: int = 0
    uncached_input_tokens: int = 0

    def apply_call(self, delta: LlmUsageDelta | None) -> None:
        """Accumulate one call into totals.

        This counts calls even when usage is unavailable.
        """

        self.calls = int(self.calls) + 1
        if delta is None or not isinstance(delta, LlmUsageDelta) or not delta.has_any():
            return

        self.calls_with_usage = int(self.calls_with_usage) + 1

        if isinstance(delta.input_tokens, int) and delta.input_tokens >= 0:
            self.input_tokens = int(self.input_tokens) + int(delta.input_tokens)
        if isinstance(delta.output_tokens, int) and delta.output_tokens >= 0:
            self.output_tokens = int(self.output_tokens) + int(delta.output_tokens)

        if isinstance(delta.cached_tokens, int) and delta.cached_tokens >= 0:
            self.calls_with_cached_tokens = int(self.calls_with_cached_tokens) + 1
            self.cached_tokens = int(self.cached_tokens) + int(delta.cached_tokens)

        if (
            isinstance(delta.uncached_input_tokens, int)
            and delta.uncached_input_tokens >= 0
        ):
            self.uncached_input_tokens = int(self.uncached_input_tokens) + int(
                delta.uncached_input_tokens
            )

    def to_dict(self) -> dict[str, int]:
        return {
            LLM_USAGE_CALLS: int(self.calls),
            LLM_USAGE_CALLS_WITH_USAGE: int(self.calls_with_usage),
            LLM_USAGE_CALLS_WITH_CACHED_TOKENS: int(self.calls_with_cached_tokens),
            LLM_USAGE_INPUT_TOKENS: int(self.input_tokens),
            LLM_USAGE_OUTPUT_TOKENS: int(self.output_tokens),
            LLM_USAGE_CACHED_TOKENS: int(self.cached_tokens),
            LLM_USAGE_UNCACHED_INPUT_TOKENS: int(self.uncached_input_tokens),
        }

    @classmethod
    def from_dict(cls, d: Mapping[str, Any] | None) -> LlmUsageTotals | None:
        if not isinstance(d, Mapping):
            return None
        calls = _as_nonneg_int(d.get(LLM_USAGE_CALLS))
        calls_with_usage = _as_nonneg_int(d.get(LLM_USAGE_CALLS_WITH_USAGE))
        calls_with_cached = _as_nonneg_int(d.get(LLM_USAGE_CALLS_WITH_CACHED_TOKENS))
        input_tokens = _as_nonneg_int(d.get(LLM_USAGE_INPUT_TOKENS))
        output_tokens = _as_nonneg_int(d.get(LLM_USAGE_OUTPUT_TOKENS))
        cached_tokens = _as_nonneg_int(d.get(LLM_USAGE_CACHED_TOKENS))
        uncached_input_tokens = _as_nonneg_int(d.get(LLM_USAGE_UNCACHED_INPUT_TOKENS))

        # If all are missing, treat as absent rather than a 0-filled record.
        if all(
            v is None
            for v in (
                calls,
                calls_with_usage,
                calls_with_cached,
                input_tokens,
                output_tokens,
                cached_tokens,
                uncached_input_tokens,
            )
        ):
            return None

        return cls(
            calls=int(calls or 0),
            calls_with_usage=int(calls_with_usage or 0),
            calls_with_cached_tokens=int(calls_with_cached or 0),
            input_tokens=int(input_tokens or 0),
            output_tokens=int(output_tokens or 0),
            cached_tokens=int(cached_tokens or 0),
            uncached_input_tokens=int(uncached_input_tokens or 0),
        )


def extract_usage_delta_from_response(resp: Any) -> LlmUsageDelta | None:
    """Extract provider-reported usage counts from a response dict (best-effort)."""

    if not isinstance(resp, dict):
        return None
    usage = resp.get("usage")
    if not isinstance(usage, dict):
        return None

    inp = _as_nonneg_int(usage.get(LLM_USAGE_INPUT_TOKENS))
    if inp is None:
        inp = _as_nonneg_int(usage.get("prompt_tokens"))
    out = _as_nonneg_int(usage.get(LLM_USAGE_OUTPUT_TOKENS))
    if out is None:
        out = _as_nonneg_int(usage.get("completion_tokens"))

    cached = None
    # Best-effort support for provider prompt caching.
    # Common shapes:
    # - Chat Completions: usage.prompt_tokens_details.cached_tokens
    # - Responses API: usage.input_tokens_details.cached_tokens
    for dk in ("input_tokens_details", "prompt_tokens_details"):
        d = usage.get(dk)
        if isinstance(d, dict):
            c = _as_nonneg_int(d.get(LLM_USAGE_CACHED_TOKENS))
            if c is not None:
                cached = c
                break
    if cached is None:
        cached = _as_nonneg_int(usage.get(LLM_USAGE_CACHED_TOKENS))

    uncached_in = None
    if inp is not None and cached is not None:
        uncached_in = max(0, int(inp) - int(cached))

    delta = LlmUsageDelta(
        input_tokens=inp,
        output_tokens=out,
        cached_tokens=cached,
        uncached_input_tokens=uncached_in,
    )
    return delta if delta.has_any() else None
