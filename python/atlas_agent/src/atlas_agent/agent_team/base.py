import os
import json
import re
import time
import uuid
import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from openai import OpenAI  # type: ignore

from ..defaults import (
    DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR,
    DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR,
    GATEWAY_MODEL_DETECTION_MAX_RETRIES,
    TRANSIENT_NETWORK_BACKOFF_SECONDS,
    TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS,
)
from ..gateway_model import (
    gateway_model_matches_requested,
    openai_model_requires_gateway_model,
)
from ..provider_tool_schema import (
    convert_tools_to_responses_wire,
    normalize_tools_for_chat_completions_api,
    tighten_tools_schema_for_provider,
)

log = logging.getLogger(__name__)


@dataclass(frozen=True)
class ModelTokenBudgets:
    model: str
    total_context_window_tokens: int | None = None
    max_output_tokens: int | None = None
    effective_input_budget_tokens: int | None = None
    auto_compact_tokens: int | None = None


@dataclass
class LLMClient:
    api_key: str
    model: str
    base_url: str | None = None
    wire_api: str = "auto"  # "auto" | "responses" | "chat"
    _client: Any = field(init=False, default=None, repr=False)
    _responses_supports_temperature: bool | None = field(
        init=False, default=None, repr=False
    )
    _chat_supports_temperature: bool | None = field(
        init=False, default=None, repr=False
    )
    _wire_api_resolved: str | None = field(init=False, default=None, repr=False)
    _model_token_budgets_cache: dict[str, ModelTokenBudgets] = field(
        init=False, default_factory=dict, repr=False
    )
    _warned_files_api_unsupported: bool = field(init=False, default=False, repr=False)
    _files_api_disabled: bool = field(init=False, default=False, repr=False)
    _warned_responses_compact_unsupported: bool = field(
        init=False, default=False, repr=False
    )
    _responses_compact_disabled: bool = field(init=False, default=False, repr=False)

    def __post_init__(self):
        # Normalize base_url once so the rest of atlas_agent can rely on
        # client.base_url without re-reading environment variables.
        if not self.base_url:
            self.base_url = os.environ.get("OPENAI_BASE_URL") or None

    def upload_file_for_model_input(
        self,
        *,
        path: str | Path,
        purpose: str = "user_data",
        expires_after_sec: int | None = 3600,
    ) -> str | None:
        """Upload a local file and return a provider file id (Responses API compatible).

        This is intended for large, session-scoped artifacts (e.g. preview images)
        where inlining as base64 data URLs would bloat requests.

        Notes:
        - This is best-effort and provider-dependent. OpenAI supports `files.create`
          with `purpose="user_data"` and `expires_after`.
        - Some OpenAI-compatible gateways do not implement /files; callers must
          handle None and fall back to smaller previews or human checks.
        """

        if self._files_api_disabled:
            return None

        try:
            p = Path(path)
        except Exception:
            return None
        if not p.exists() or not p.is_file():
            return None

        client = self._ensure_client()
        files = getattr(client, "files", None)
        if files is None or not hasattr(files, "create"):
            self._disable_files_api("OpenAI client has no files.create")
            return None

        # expires_after is optional; some providers reject it even if they
        # implement /files. Try with expires_after, then fall back without.
        expires_after = None
        if expires_after_sec is not None:
            try:
                sec = int(expires_after_sec)
                if sec > 0:
                    expires_after = {"anchor": "created_at", "seconds": sec}
            except Exception:
                expires_after = None

        def _try_create(
            *, include_expires_after: bool
        ) -> tuple[str | None, Exception | None]:
            try:
                with open(p, "rb") as f:
                    kwargs: dict[str, Any] = {"file": f, "purpose": str(purpose)}
                    if include_expires_after and expires_after is not None:
                        kwargs["expires_after"] = expires_after
                    resp = files.create(**kwargs)
                data = self._to_plain_dict(resp)
                file_id = data.get("id") if isinstance(data, dict) else None
                if isinstance(file_id, str) and file_id.strip():
                    return (file_id.strip(), None)
            except Exception as e:
                return (None, e)
            return (None, None)

        out1, err1 = _try_create(include_expires_after=True)
        if out1 is not None:
            return out1

        # If the first attempt failed due to an unsupported endpoint, don't
        # retry with a modified payload; disable uploads for this session.
        if err1 is not None and self._is_files_unsupported_error(err1):
            self._disable_files_api(str(err1))
            return None

        out2, err2 = _try_create(include_expires_after=False)
        if out2 is not None:
            return out2

        if err2 is not None and self._is_files_unsupported_error(err2):
            self._disable_files_api(str(err2))
            return None

        # Some OpenAI-compatible gateways return a generic HTTP 400 for unsupported
        # endpoints (or for unknown/unsupported `purpose` values). If both payload
        # variants (with and without expires_after) produce HTTP 400, treat this as
        # a persistent incompatibility and disable further /files attempts for the
        # rest of this session to avoid repeated noisy requests.
        if (
            err1 is not None
            and err2 is not None
            and self._http_status_code(err1) == 400
            and self._http_status_code(err2) == 400
        ):
            self._disable_files_api(
                "Files API returned HTTP 400 for both upload attempts"
            )
        return None

    def _disable_files_api(self, reason: str) -> None:
        self._files_api_disabled = True
        self._warn_files_api_unsupported_once(reason)

    def _warn_files_api_unsupported_once(self, reason: str) -> None:
        if self._warned_files_api_unsupported:
            return
        self._warned_files_api_unsupported = True

        base = self.base_url or os.environ.get("OPENAI_BASE_URL") or ""
        base_msg = base.strip() or "<default OpenAI>"
        log.warning(
            "Provider does not appear to support the Files API (/v1/files). "
            "Preview images will fall back to inline base64 (small only) or text-only. "
            "base_url=%s reason=%s",
            base_msg,
            (reason or "").strip() or "<unknown>",
        )

    @classmethod
    def _is_files_unsupported_error(cls, err: Exception) -> bool:
        for e in cls._iter_exception_chain(err):
            name = type(e).__name__.lower()
            msg = str(e or "").lower()
            if "notfound" in name:
                return True
            if "files" in msg or "/files" in msg:
                if "404" in msg or "not found" in msg:
                    return True
                if "not implemented" in msg:
                    return True
                if "405" in msg and "method" in msg:
                    return True
                # Some gateways report unsupported endpoints as 400.
                if "400" in msg and (
                    "unknown endpoint" in msg
                    or "unrecognized endpoint" in msg
                    or "no such endpoint" in msg
                    or "unsupported" in msg
                    or "not supported" in msg
                ):
                    return True
            try:
                sc = int(getattr(e, "status_code", 0) or 0)
                if sc in {404, 405, 501}:
                    return True
            except Exception:
                pass
        return False

    @classmethod
    def _http_status_code(cls, err: Exception) -> int | None:
        for e in cls._iter_exception_chain(err):
            try:
                sc = int(getattr(e, "status_code", 0) or 0)
            except Exception:
                sc = 0
            if not sc:
                try:
                    # urllib.error.HTTPError uses `.code`.
                    sc = int(getattr(e, "code", 0) or 0)
                except Exception:
                    sc = 0
            if sc:
                return sc
        return None

    def _disable_responses_compact(self, reason: str) -> None:
        self._responses_compact_disabled = True
        self._warn_responses_compact_unsupported_once(reason)

    def _warn_responses_compact_unsupported_once(self, reason: str) -> None:
        if self._warned_responses_compact_unsupported:
            return
        self._warned_responses_compact_unsupported = True

        base = self.base_url or os.environ.get("OPENAI_BASE_URL") or ""
        base_msg = base.strip() or "<default OpenAI>"
        log.warning(
            "Provider does not appear to support the Responses API compaction endpoint "
            "(/v1/responses/compact). Falling back to atlas_agent checkpoint summarization. "
            "base_url=%s reason=%s",
            base_msg,
            (reason or "").strip() or "<unknown>",
        )

    @classmethod
    def _is_responses_compact_unsupported_error(cls, err: Exception) -> bool:
        # Best-effort: vendors/gateways that don't implement /v1/responses/compact
        # typically return 404. Only treat these as "endpoint unsupported" signals;
        # other 4xx errors are usually real request/validation issues.
        for e in cls._iter_exception_chain(err):
            msg = str(e or "").lower()
            sc = cls._http_status_code(e) or 0

            # If the request returned a hard "endpoint not found / not implemented"
            # status, treat it as unsupported even when the provider error message
            # is generic and does not include the full URL path.
            if sc in {404, 405, 501}:
                return True

            # Fast-path: endpoint mentioned and looks like a 404/405/501.
            if ("/responses/compact" in msg or "responses/compact" in msg) and sc in {
                404,
                405,
                501,
            }:
                return True

            # Some gateways report unknown endpoints as HTTP 400 with a generic message.
            if (
                ("/responses/compact" in msg or "responses/compact" in msg)
                and sc == 400
                and (
                    "unknown endpoint" in msg
                    or "unrecognized endpoint" in msg
                    or "no such endpoint" in msg
                    or "unsupported" in msg
                    or "not supported" in msg
                )
            ):
                return True

            # Heuristic: "not found" with both tokens.
            if ("responses" in msg and "compact" in msg) and (
                "404" in msg or "not found" in msg or "not implemented" in msg
            ):
                return True

        return False

    def compact_responses_input_items_with_response(
        self, *, input_items: list[dict[str, Any]]
    ) -> tuple[list[dict[str, Any]] | None, dict[str, Any] | None]:
        """Best-effort wrapper for the Responses API compaction endpoint.

        Uses the provider's `/v1/responses/compact` endpoint when available.
        Returns `(compacted_items, response_dict)` on success, otherwise `(None, None)`.

        This never mutates `input_items`.
        """

        if self._responses_compact_disabled:
            return (None, None)

        # Compaction items are only meaningful when we can keep using the Responses API.
        if self._wire_mode() == "chat":
            return (None, None)

        if not isinstance(input_items, list) or not input_items:
            return (None, None)

        safe_items = [it for it in input_items if isinstance(it, dict)]
        if not safe_items:
            return (None, None)

        client = self._ensure_client()
        responses = getattr(client, "responses", None)
        compact_fn = (
            getattr(responses, "compact", None) if responses is not None else None
        )

        # 1) Prefer SDK support when available.
        if callable(compact_fn):
            try:
                resp = compact_fn(model=self.model, input=safe_items)
                data = self._to_plain_dict(resp)
                out = data.get("output") if isinstance(data, dict) else None
                if isinstance(out, list):
                    return ([it for it in out if isinstance(it, dict)], data)
                return (None, data if isinstance(data, dict) else None)
            except Exception as e:
                if self._is_responses_compact_unsupported_error(e):
                    self._disable_responses_compact(str(e))
                return (None, None)

        # 2) SDK fallback: raw HTTP call so older `openai` versions can still use the endpoint.
        try:
            data = self._responses_compact_via_http(input_items=safe_items)
        except Exception as e:
            if self._is_responses_compact_unsupported_error(e):
                self._disable_responses_compact(str(e))
            return (None, None)

        out = data.get("output") if isinstance(data, dict) else None
        if isinstance(out, list):
            return ([it for it in out if isinstance(it, dict)], data)
        return (None, data if isinstance(data, dict) else None)

    def _responses_compact_via_http(
        self, *, input_items: list[dict[str, Any]]
    ) -> dict[str, Any]:
        import json as _json
        import urllib.error
        import urllib.request

        # Respect the configured base_url. The OpenAI SDK default is typically
        # https://api.openai.com/v1, but gateways may be different.
        base = ""
        try:
            base = str(getattr(self._ensure_client(), "base_url", "") or "")
        except Exception:
            base = ""
        if not base:
            base = str(self.base_url or os.environ.get("OPENAI_BASE_URL") or "").strip()
        if not base:
            base = "https://api.openai.com/v1"

        base = base.rstrip("/")
        if base.endswith("/v1"):
            url = base + "/responses/compact"
        else:
            url = base + "/v1/responses/compact"

        payload = {"model": str(self.model), "input": list(input_items or [])}
        body = _json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        req = urllib.request.Request(url, data=body, headers=headers, method="POST")

        try:
            with urllib.request.urlopen(req, timeout=30.0) as resp:
                raw = resp.read()
        except urllib.error.HTTPError:
            # Preserve the HTTPError so _http_status_code / message heuristics can detect support.
            raise

        data = _json.loads((raw or b"{}").decode("utf-8"))
        return data if isinstance(data, dict) else {}

    def _ensure_client(self):
        if self._client is None:
            # Respect explicit base_url if provided; otherwise read from env
            kwargs = {"api_key": self.api_key}
            base = self.base_url or os.environ.get("OPENAI_BASE_URL")
            if base:
                kwargs["base_url"] = base
            self._client = OpenAI(**kwargs)
        return self._client

    @staticmethod
    def _parse_token_count(value: Any) -> int | None:
        if isinstance(value, bool):
            return None
        if isinstance(value, int):
            return int(value) if int(value) > 0 else None
        if isinstance(value, float):
            if value.is_integer():
                n = int(value)
                return n if n > 0 else None
            return None
        if isinstance(value, str):
            s = value.strip().lower().replace(",", "").replace("_", "")
            if not s:
                return None
            m = re.fullmatch(r"(\d+(?:\.\d+)?)([km])?", s)
            if not m:
                return None
            try:
                base = float(m.group(1))
            except Exception:
                return None
            suffix = m.group(2) or ""
            mult = 1
            if suffix == "k":
                mult = 1_000
            elif suffix == "m":
                mult = 1_000_000
            n = int(base * mult)
            return n if n > 0 else None
        return None

    @classmethod
    def _find_first_token_limit_for_keys(
        cls, obj: Any, keys_lower: set[str]
    ) -> int | None:
        visited: set[int] = set()

        def _walk(cur: Any, depth: int) -> int | None:
            if depth > 10:
                return None
            try:
                oid = id(cur)
            except Exception:
                oid = 0
            if oid and oid in visited:
                return None
            if oid:
                visited.add(oid)

            if isinstance(cur, dict):
                for k, v in cur.items():
                    lk = str(k or "").strip().lower()
                    if lk in keys_lower:
                        n = cls._parse_token_count(v)
                        if n is not None:
                            return n
                for v in cur.values():
                    hit = _walk(v, depth + 1)
                    if hit is not None:
                        return hit
                return None

            if isinstance(cur, list):
                for v in cur:
                    hit = _walk(v, depth + 1)
                    if hit is not None:
                        return hit
                return None

            return None

        return _walk(obj, 0)

    def get_model_token_budgets(self, model: str | None = None) -> ModelTokenBudgets:
        """Best-effort fetch of model token budgets from the provider.

        Priority order:
        - Provider model metadata (when available) via models.retrieve().
        - Best-effort derivations (effective_input_budget = total - max_output,
          auto_compact = DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR/DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR).
        - Unknowns are returned as None.

        Important: providers differ in which fields they expose. We keep this tolerant
        and treat any missing fields as unknown.
        """

        model_id = str(model or self.model or "").strip()
        if not model_id:
            return ModelTokenBudgets(model="")
        cached = self._model_token_budgets_cache.get(model_id)
        if cached is not None:
            return cached

        total_context_window: int | None = None
        max_output_tokens: int | None = None
        effective_input_budget: int | None = None
        auto_compact: int | None = None

        try:
            client = self._ensure_client()
            models = getattr(client, "models", None)
            if models is not None and hasattr(models, "retrieve"):
                info = models.retrieve(model_id)
                data = self._to_plain_dict(info)
                total_context_window = self._find_first_token_limit_for_keys(
                    data,
                    {
                        "context_window",
                        "context_window_tokens",
                        "context_length",
                        "max_context_length",
                        "max_context_window",
                        "model_context_window",
                    },
                )
                max_output_tokens = self._find_first_token_limit_for_keys(
                    data,
                    {
                        "max_output_tokens",
                        "max_completion_tokens",
                        "max_output",
                        "max_output_length",
                        "max_completion_length",
                        "max_tokens_output",
                        "completion_tokens",
                        "output_tokens",
                    },
                )
                effective_input_budget = self._find_first_token_limit_for_keys(
                    data,
                    {
                        "max_input_tokens",
                        "max_prompt_tokens",
                        "max_input_length",
                        "max_prompt_length",
                        "max_prompt_size",
                        "max_input_size",
                        "prompt_tokens",
                        "input_tokens",
                    },
                )
                auto_compact = self._find_first_token_limit_for_keys(
                    data,
                    {
                        "auto_compact_token_limit",
                        "auto_compact_tokens",
                        "auto_compact_token_max",
                        "auto_compact_limit_tokens",
                    },
                )
        except Exception:
            # Best-effort: keep unknown and fall back to caller heuristics.
            total_context_window = None
            max_output_tokens = None
            effective_input_budget = None
            auto_compact = None

        if (
            effective_input_budget is None
            and total_context_window is not None
            and max_output_tokens is not None
        ):
            try:
                effective_input_budget = int(total_context_window) - int(
                    max_output_tokens
                )
                if effective_input_budget <= 0:
                    effective_input_budget = None
            except Exception:
                effective_input_budget = None

        if auto_compact is None and effective_input_budget is not None:
            try:
                auto_compact = max(
                    1,
                    (int(effective_input_budget) * DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR)
                    // DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR,
                )
            except Exception:
                auto_compact = None

        out = ModelTokenBudgets(
            model=model_id,
            total_context_window_tokens=total_context_window,
            max_output_tokens=max_output_tokens,
            effective_input_budget_tokens=effective_input_budget,
            auto_compact_tokens=auto_compact,
        )
        self._model_token_budgets_cache[model_id] = out
        return out

    def _wire_mode(self) -> str:
        mode = (self._wire_api_resolved or self.wire_api or "auto").strip().lower()
        return mode if mode in {"auto", "responses", "chat"} else "auto"

    @staticmethod
    def _is_unsupported_temperature_error(err: Exception) -> bool:
        msg = str(err or "")
        if not msg:
            return False
        m = msg.lower()
        return ("unsupported parameter" in m and "temperature" in m) or (
            "temperature" in m and "not supported" in m
        )

    @staticmethod
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

    @classmethod
    def _is_responses_unsupported_error(cls, err: Exception) -> bool:
        # Best-effort: vendors/gateways that don't implement /v1/responses typically
        # return 404. Only treat these as "wire unsupported" signals; other 4xx
        # errors are usually real request/validation issues.
        for e in cls._iter_exception_chain(err):
            msg = str(e or "").lower()
            if "404" in msg and "responses" in msg:
                return True
            if "not found" in msg and "responses" in msg:
                return True
            # OpenAI SDK error objects often carry status_code.
            try:
                sc = int(getattr(e, "status_code", 0) or 0)
                if sc == 404:
                    return True
            except Exception:
                pass
        return False

    def _model_supports_reasoning_summaries(self) -> bool:
        """Best-effort capability detection.

        We avoid hard failures by omitting unsupported request fields for models
        that are unlikely to implement them.
        """

        m = (self.model or "").strip().lower()
        return m.startswith("gpt-5") or m.startswith("o3") or m.startswith("o4-mini")

    def _model_supports_text_verbosity(self) -> bool:
        """Best-effort: text.verbosity is currently a GPT-5 family control."""

        m = (self.model or "").strip().lower()
        return m.startswith("gpt-5")

    @staticmethod
    def _normalize_tools_for_responses(
        raw_tools: Optional[List[Dict[str, Any]]],
    ) -> Optional[List[Dict[str, Any]]]:
        # Two-stage normalization:
        # 1) Wire adapter to Responses tool shape (so downstream code is uniform).
        # 2) Provider-border schema tightening for strict validators.
        tools_wire = convert_tools_to_responses_wire(raw_tools)
        return tighten_tools_schema_for_provider(tools_wire)

    @staticmethod
    def _to_plain_dict(obj: Any) -> Any:
        """Best-effort conversion to JSON-serializable Python structures.

        The OpenAI Python SDK returns Pydantic models for Responses API events and
        responses. We normalize to plain dicts/lists so the rest of atlas_agent
        can be provider-agnostic.
        """

        if obj is None:
            return None
        if isinstance(obj, (str, int, float, bool)):
            return obj
        if isinstance(obj, dict):
            return {str(k): LLMClient._to_plain_dict(v) for k, v in obj.items()}
        if isinstance(obj, list):
            return [LLMClient._to_plain_dict(v) for v in obj]
        # Pydantic v2
        md = getattr(obj, "model_dump", None)
        if callable(md):
            try:
                # Pydantic v2 can emit noisy "Pydantic serializer warnings" when
                # serializing union-heavy SDK models (e.g. OpenAI Responses output
                # items). For atlas_agent we only need a best-effort dict; silence
                # those warnings so users don't see a warning per streamed event.
                try:
                    return LLMClient._to_plain_dict(md(mode="json", warnings=False))
                except TypeError:
                    # Older Pydantic v2 versions may not support these kwargs.
                    return LLMClient._to_plain_dict(md())
            except Exception:
                pass
        # Pydantic v1
        dct = getattr(obj, "dict", None)
        if callable(dct):
            try:
                return LLMClient._to_plain_dict(dct())
            except Exception:
                pass
        # Fallback: last resort stringification
        try:
            return str(obj)
        except Exception:
            return "<unserializable>"

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
    ) -> Dict[str, Any]:
        """Stream a turn via an OpenAI-compatible wire API.

        - Emits SSE-derived events via on_event (already normalized to dict).
        - Returns the final response as a plain dict in the Responses API shape.

        When wire_api='auto', we prefer the Responses API and fall back to Chat
        Completions if the provider does not support `/v1/responses`.
        """

        def _responses_to_chat_messages(
            *, instructions_text: str, items: List[Dict[str, Any]]
        ) -> list[dict[str, Any]]:
            messages: list[dict[str, Any]] = []
            instructions_text = str(instructions_text or "").strip()
            if instructions_text:
                messages.append({"role": "system", "content": instructions_text})

            for it in items or []:
                if not isinstance(it, dict):
                    continue
                itype = str(it.get("type") or "")
                if itype == "message":
                    role = str(it.get("role") or "").strip() or "user"
                    content_in = it.get("content")
                    if isinstance(content_in, list):
                        parts: list[str] = []
                        for part in content_in:
                            if not isinstance(part, dict):
                                continue
                            txt = part.get("text")
                            if isinstance(txt, str) and txt:
                                parts.append(txt)
                        content = "".join(parts)
                    else:
                        content = str(content_in or "")
                    if content:
                        messages.append({"role": role, "content": content})
                    continue

                if itype == "function_call":
                    name = str(it.get("name") or "").strip()
                    call_id = str(it.get("call_id") or "").strip()
                    arguments = it.get("arguments")
                    if not isinstance(arguments, str):
                        try:
                            arguments = json.dumps(arguments, ensure_ascii=False)
                        except Exception:
                            arguments = str(arguments)
                    if name and call_id:
                        messages.append(
                            {
                                "role": "assistant",
                                "content": "",
                                "tool_calls": [
                                    {
                                        "id": call_id,
                                        "type": "function",
                                        "function": {
                                            "name": name,
                                            "arguments": arguments,
                                        },
                                    }
                                ],
                            }
                        )
                    continue

                if itype == "function_call_output":
                    call_id = str(it.get("call_id") or "").strip()
                    output = it.get("output")
                    if isinstance(output, list):
                        # Chat Completions tool messages are text-only. Avoid
                        # JSON-stringifying multimodal payloads (e.g. base64
                        # data URLs), which can explode prompt size and still
                        # won't let the model "see" the image. Keep only text
                        # parts and replace images with a stable placeholder.
                        parts: list[str] = []
                        for part in output:
                            if not isinstance(part, dict):
                                continue
                            ptype = str(part.get("type") or "")
                            if ptype in {"input_text", "output_text", "text"}:
                                txt = part.get("text")
                                if isinstance(txt, str) and txt:
                                    parts.append(txt)
                            elif ptype in {"input_image", "output_image"}:
                                parts.append(
                                    "[image omitted: chat tool outputs are text-only]"
                                )
                        output = "\n".join(parts).strip() or "(tool output omitted)"
                    elif not isinstance(output, str):
                        try:
                            output = json.dumps(output, ensure_ascii=False)
                        except Exception:
                            output = str(output)
                    if call_id:
                        messages.append(
                            {"role": "tool", "tool_call_id": call_id, "content": output}
                        )
                    continue

            return messages

        def _chat_response_to_responses_dict(chat_resp: Any) -> dict[str, Any]:
            data = self._to_plain_dict(chat_resp)
            out_items: list[dict[str, Any]] = []
            status = "completed"
            incomplete_details: dict[str, Any] | None = None
            try:
                choices = data.get("choices") if isinstance(data, dict) else None
                choice0 = choices[0] if isinstance(choices, list) and choices else {}
                if isinstance(choice0, dict):
                    fr = str(choice0.get("finish_reason") or "").strip().lower()
                    # Map Chat Completions finish reasons to a Responses-like status
                    # so the tool loop can handle truncation uniformly.
                    if fr == "length":
                        status = "incomplete"
                        incomplete_details = {"reason": "max_tokens"}
                    elif fr in {"content_filter"}:
                        status = "incomplete"
                        incomplete_details = {"reason": fr}
                msg = choice0.get("message") if isinstance(choice0, dict) else {}
                if not isinstance(msg, dict):
                    msg = {}
                content = msg.get("content")
                if isinstance(content, str) and content.strip():
                    out_items.append(
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": content}],
                        }
                    )
                tool_calls = msg.get("tool_calls")
                if isinstance(tool_calls, list):
                    for tc in tool_calls:
                        if not isinstance(tc, dict):
                            continue
                        call_id = str(tc.get("id") or "").strip()
                        fn = tc.get("function")
                        if not isinstance(fn, dict):
                            fn = {}
                        name = str(fn.get("name") or "").strip()
                        arguments = fn.get("arguments")
                        if not isinstance(arguments, str):
                            try:
                                arguments = json.dumps(arguments, ensure_ascii=False)
                            except Exception:
                                arguments = str(arguments)
                        if not call_id:
                            # Stable enough for a single tool loop round.
                            call_id = str(uuid.uuid4())
                        if name:
                            out_items.append(
                                {
                                    "type": "function_call",
                                    "call_id": call_id,
                                    "name": name,
                                    "arguments": arguments,
                                }
                            )
            except Exception:
                # Fall back to an empty output; tool loop will treat it as no-op.
                pass
            out: dict[str, Any] = {"output": out_items, "status": status}
            model = data.get("model") if isinstance(data, dict) else None
            if isinstance(model, str) and model.strip():
                out["model"] = model.strip()
            usage = data.get("usage") if isinstance(data, dict) else None
            if isinstance(usage, dict) and usage:
                out["usage"] = dict(usage)
            if incomplete_details is not None:
                out["incomplete_details"] = incomplete_details
            return out

        reasoning: dict[str, Any] | None = None
        if self._model_supports_reasoning_summaries():
            if reasoning_effort is not None or reasoning_summary is not None:
                reasoning = {}
                if reasoning_effort is not None:
                    reasoning["effort"] = reasoning_effort
                if (
                    reasoning_summary is not None
                    and str(reasoning_summary).lower() != "none"
                ):
                    reasoning["summary"] = reasoning_summary

        text: dict[str, Any] | None = None
        if self._model_supports_text_verbosity():
            if text_verbosity is not None:
                text = {"verbosity": text_verbosity}

        params: dict[str, Any] = {
            "model": self.model,
            "instructions": instructions,
            "input": input_items,
            "parallel_tool_calls": bool(parallel_tool_calls),
        }
        if temperature is not None:
            params["temperature"] = float(temperature)
        normalized_tools = self._normalize_tools_for_responses(tools)
        if normalized_tools:
            params["tools"] = normalized_tools
        if reasoning is not None:
            params["reasoning"] = reasoning
        if text is not None:
            params["text"] = text

        mode = self._wire_mode()
        if mode == "chat":
            client = self._ensure_client()
            chat_messages = _responses_to_chat_messages(
                instructions_text=instructions, items=input_items
            )
            # Normalize to Chat Completions wire shape + apply provider-border
            # schema tightening for strict validators.
            #
            # Important: The Chat Completions API does not reliably accept non-function
            # tool specs (e.g., Responses built-ins like {"type":"web_search"}). Filter
            # to function tools only to avoid provider errors when the agent is
            # configured to use (or falls back to) Chat Completions.
            chat_tools_in = [
                t
                for t in (tools or [])
                if isinstance(t, dict) and str(t.get("type") or "") == "function"
            ]
            chat_tools = normalize_tools_for_chat_completions_api(chat_tools_in)

            chat_params: dict[str, Any] = {
                "model": self.model,
                "messages": chat_messages,
                "tools": chat_tools or None,
            }
            if temperature is not None and self._chat_supports_temperature is not False:
                chat_params["temperature"] = float(temperature)
            try:
                resp = client.chat.completions.create(**chat_params)
                if "temperature" in chat_params:
                    self._chat_supports_temperature = True
            except Exception as e:
                if (
                    "temperature" in chat_params
                    and self._is_unsupported_temperature_error(e)
                ):
                    self._chat_supports_temperature = False
                    chat_params.pop("temperature", None)
                    resp = client.chat.completions.create(**chat_params)
                else:
                    raise
            return _chat_response_to_responses_dict(resp)

        client = self._ensure_client()
        if not hasattr(client, "responses"):
            if mode == "responses":
                raise RuntimeError("OpenAI client does not support the Responses API")
            # auto: fall back to chat
            self._wire_api_resolved = "chat"
            return self.responses_stream(
                instructions=instructions,
                input_items=input_items,
                tools=tools,
                temperature=temperature,
                parallel_tool_calls=parallel_tool_calls,
                reasoning_effort=reasoning_effort,
                reasoning_summary=reasoning_summary,
                text_verbosity=text_verbosity,
                on_event=on_event,
            )
        responses = getattr(client, "responses")
        if not hasattr(responses, "stream"):
            if mode == "responses":
                raise RuntimeError("OpenAI client does not support responses.stream")
            self._wire_api_resolved = "chat"
            return self.responses_stream(
                instructions=instructions,
                input_items=input_items,
                tools=tools,
                temperature=temperature,
                parallel_tool_calls=parallel_tool_calls,
                reasoning_effort=reasoning_effort,
                reasoning_summary=reasoning_summary,
                text_verbosity=text_verbosity,
                on_event=on_event,
            )

        # Some models reject optional sampling parameters (notably temperature).
        # We retry once without temperature and cache the result for this client.
        if self._responses_supports_temperature is False:
            params.pop("temperature", None)

        try:
            with responses.stream(**params) as stream:
                for ev in stream:
                    if on_event is None:
                        continue
                    try:
                        on_event(self._to_plain_dict(ev))
                    except Exception:
                        # Streaming callbacks must not break model execution.
                        continue
                try:
                    final = stream.get_final_response()
                except Exception:
                    # Some SDK versions may expose .response instead.
                    final = getattr(stream, "response", None)
            if "temperature" in params:
                self._responses_supports_temperature = True
            if mode == "auto" and self._wire_api_resolved is None:
                self._wire_api_resolved = "responses"
        except Exception as e:
            if mode == "auto" and self._is_responses_unsupported_error(e):
                self._wire_api_resolved = "chat"
                return self.responses_stream(
                    instructions=instructions,
                    input_items=input_items,
                    tools=tools,
                    temperature=temperature,
                    parallel_tool_calls=parallel_tool_calls,
                    reasoning_effort=reasoning_effort,
                    reasoning_summary=reasoning_summary,
                    text_verbosity=text_verbosity,
                    on_event=on_event,
                )
            if "temperature" in params and self._is_unsupported_temperature_error(e):
                self._responses_supports_temperature = False
                params.pop("temperature", None)
                with responses.stream(**params) as stream:
                    for ev in stream:
                        if on_event is None:
                            continue
                        try:
                            on_event(self._to_plain_dict(ev))
                        except Exception:
                            continue
                    try:
                        final = stream.get_final_response()
                    except Exception:
                        final = getattr(stream, "response", None)
                if mode == "auto" and self._wire_api_resolved is None:
                    self._wire_api_resolved = "responses"
            else:
                raise
        return self._to_plain_dict(final) if final is not None else {}

    def _responses_complete_text_with_response(
        self,
        *,
        system_prompt: str,
        user_text: str,
        temperature: float | None = None,
        max_tokens: int | None = None,
        text_verbosity: str | None = None,
    ) -> tuple[str, dict[str, Any]]:
        """Unary Responses API call that returns assistant text + full response dict.

        This is useful for internal sub-tasks where we still want access to usage
        statistics for budgeting / reporting.
        """

        client = self._ensure_client()
        if not hasattr(client, "responses"):
            raise RuntimeError("OpenAI client does not support the Responses API")
        responses = getattr(client, "responses")
        if not hasattr(responses, "create"):
            raise RuntimeError("OpenAI client does not support responses.create")

        input_items = [
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": str(user_text)}],
            }
        ]
        params: dict[str, Any] = {
            "model": self.model,
            "instructions": str(system_prompt),
            "input": input_items,
        }
        if temperature is not None:
            params["temperature"] = float(temperature)
        if max_tokens is not None:
            params["max_output_tokens"] = int(max_tokens)
        if text_verbosity is not None:
            params["text"] = {"verbosity": str(text_verbosity)}

        require_gateway_model = openai_model_requires_gateway_model(self.model)
        max_tries = max(1, int(GATEWAY_MODEL_DETECTION_MAX_RETRIES))
        last_gateway_model: str | None = None
        last_reason: str | None = None

        for attempt in range(max_tries):
            if self._responses_supports_temperature is False:
                params.pop("temperature", None)

            try:
                resp = responses.create(**params)
                if "temperature" in params:
                    self._responses_supports_temperature = True
            except Exception as e:
                if "temperature" in params and self._is_unsupported_temperature_error(
                    e
                ):
                    self._responses_supports_temperature = False
                    params.pop("temperature", None)
                    resp = responses.create(**params)
                else:
                    raise

            data = self._to_plain_dict(resp)

            # Gateway model validation (OpenAI models only): if the gateway fails to report a
            # routed model name, or reports an unexpected one, treat it as a transient
            # upstream issue and retry cleanly.
            gateway_model = data.get("model") if isinstance(data, dict) else None
            last_gateway_model = (
                gateway_model if isinstance(gateway_model, str) else None
            )
            if require_gateway_model:
                if not isinstance(gateway_model, str) or not gateway_model.strip():
                    last_reason = "missing_gateway_model"
                elif not gateway_model_matches_requested(self.model, gateway_model):
                    last_reason = "mismatched_gateway_model"
                else:
                    last_reason = None

                if last_reason is not None:
                    if attempt < max_tries - 1:
                        try:
                            log.warning(
                                "Gateway model %s in internal responses.create; retrying "
                                "(requested_model=%s, got=%s, attempt=%d/%d)",
                                "missing"
                                if last_gateway_model is None
                                else "mismatched",
                                str(self.model or "?"),
                                str(last_gateway_model or "?"),
                                int(attempt + 1),
                                int(max_tries),
                            )
                        except Exception:
                            pass
                        time.sleep(
                            min(
                                float(TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS),
                                TRANSIENT_NETWORK_BACKOFF_SECONDS * (2.0**attempt),
                            )
                        )
                        continue
                    raise RuntimeError(
                        "Provider returned an invalid routed model name from responses.create "
                        f"(reason={last_reason}, requested_model={self.model!r}, gateway_model={last_gateway_model!r})."
                    )

            # Extract assistant output text from Responses API output items.
            out = data.get("output") if isinstance(data, dict) else None
            if not isinstance(out, list):
                return ("", data if isinstance(data, dict) else {})
            chunks: list[str] = []
            for item in out:
                if not isinstance(item, dict):
                    continue
                if str(item.get("type") or "") != "message":
                    continue
                if str(item.get("role") or "") != "assistant":
                    continue
                content = item.get("content") or []
                if not isinstance(content, list):
                    continue
                for part in content:
                    if not isinstance(part, dict):
                        continue
                    if str(part.get("type") or "") == "output_text":
                        chunks.append(str(part.get("text") or ""))
            return ("".join(chunks).strip(), data if isinstance(data, dict) else {})

        raise RuntimeError("responses.create retry loop exited unexpectedly")

    def complete_text(
        self,
        *,
        system_prompt: str,
        user_text: str,
        temperature: float | None = None,
        max_tokens: int | None = None,
    ) -> str:
        text, _resp = self.complete_text_with_response(
            system_prompt=system_prompt,
            user_text=user_text,
            temperature=temperature,
            max_tokens=max_tokens,
        )
        return text

    def complete_text_with_response(
        self,
        *,
        system_prompt: str,
        user_text: str,
        temperature: float | None = None,
        max_tokens: int | None = None,
    ) -> tuple[str, dict[str, Any]]:
        """Text completion + response metadata (best-effort).

        Intended for internal agent sub-tasks where we want access to usage stats.
        """

        mode = self._wire_mode()
        if mode != "chat":
            client = self._ensure_client()
            supports_responses = bool(
                hasattr(client, "responses")
                and hasattr(getattr(client, "responses"), "create")
            )
            if supports_responses:
                try:
                    out = self._responses_complete_text_with_response(
                        system_prompt=system_prompt,
                        user_text=user_text,
                        temperature=temperature,
                        max_tokens=max_tokens,
                    )
                    if mode == "auto" and self._wire_api_resolved is None:
                        self._wire_api_resolved = "responses"
                    return out
                except Exception as e:
                    if mode == "responses":
                        raise
                    if mode == "auto" and self._is_responses_unsupported_error(e):
                        self._wire_api_resolved = "chat"
            else:
                if mode == "responses":
                    raise RuntimeError(
                        "OpenAI client does not support the Responses API"
                    )
                self._wire_api_resolved = "chat"

        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_text},
        ]
        params: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
        }
        if temperature is not None and self._chat_supports_temperature is not False:
            params["temperature"] = float(temperature)
        if max_tokens is not None:
            params["max_tokens"] = int(max_tokens)
        client = self._ensure_client()
        require_gateway_model = openai_model_requires_gateway_model(self.model)
        max_tries = max(1, int(GATEWAY_MODEL_DETECTION_MAX_RETRIES))
        last_gateway_model: str | None = None
        last_reason: str | None = None

        for attempt in range(max_tries):
            try:
                resp = client.chat.completions.create(**params)
                if "temperature" in params:
                    self._chat_supports_temperature = True
            except Exception as e:
                if "temperature" in params and self._is_unsupported_temperature_error(
                    e
                ):
                    self._chat_supports_temperature = False
                    params.pop("temperature", None)
                    resp = client.chat.completions.create(**params)
                else:
                    raise

            data = self._to_plain_dict(resp)

            gateway_model = data.get("model") if isinstance(data, dict) else None
            last_gateway_model = (
                gateway_model if isinstance(gateway_model, str) else None
            )
            if require_gateway_model:
                if not isinstance(gateway_model, str) or not gateway_model.strip():
                    last_reason = "missing_gateway_model"
                elif not gateway_model_matches_requested(self.model, gateway_model):
                    last_reason = "mismatched_gateway_model"
                else:
                    last_reason = None

                if last_reason is not None:
                    if attempt < max_tries - 1:
                        try:
                            log.warning(
                                "Gateway model %s in internal chat.completions.create; retrying "
                                "(requested_model=%s, got=%s, attempt=%d/%d)",
                                "missing"
                                if last_gateway_model is None
                                else "mismatched",
                                str(self.model or "?"),
                                str(last_gateway_model or "?"),
                                int(attempt + 1),
                                int(max_tries),
                            )
                        except Exception:
                            pass
                        time.sleep(
                            min(
                                float(TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS),
                                TRANSIENT_NETWORK_BACKOFF_SECONDS * (2.0**attempt),
                            )
                        )
                        continue
                    raise RuntimeError(
                        "Provider returned an invalid routed model name from chat.completions.create "
                        f"(reason={last_reason}, requested_model={self.model!r}, gateway_model={last_gateway_model!r})."
                    )

            text = ""
            try:
                text = (resp.choices[0].message.content or "").strip()
            except Exception:
                text = ""
            return (text, data if isinstance(data, dict) else {})

        raise RuntimeError("chat.completions.create retry loop exited unexpectedly")

    def complete_with_image(
        self,
        *,
        system_prompt: str,
        user_text: str,
        image_data_url: Optional[str] = None,
        temperature: float | None = None,
        max_tokens: int | None = None,
    ) -> str:
        """Multi‑modal completion with an optional inline image data URL (base64).

        Falls back to text‑only when image_data_url is None or the model/provider rejects image content.
        """
        # Compose user content as a list of parts when an image is provided
        if image_data_url:
            user_content: Any = [
                {"type": "text", "text": user_text},
                {"type": "image_url", "image_url": {"url": image_data_url}},
            ]
        else:
            user_content = user_text

        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_content},
        ]
        client = self._ensure_client()
        try:
            params: dict[str, Any] = {"model": self.model, "messages": messages}
            if temperature is not None:
                params["temperature"] = float(temperature)
            if max_tokens is not None:
                params["max_tokens"] = int(max_tokens)
            resp = client.chat.completions.create(**params)
            return (resp.choices[0].message.content or "").strip()
        except Exception:
            # Fallback to text‑only if multimodal fails
            return self.complete_text(
                system_prompt=system_prompt,
                user_text=user_text,
                temperature=temperature,
                max_tokens=max_tokens,
            )
