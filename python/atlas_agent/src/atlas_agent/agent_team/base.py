import os
import json
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

from openai import OpenAI  # type: ignore

from ..provider_tool_schema import (
    convert_tools_to_responses_wire,
    normalize_tools_for_chat_completions_api,
    tighten_tools_schema_for_provider,
)


@dataclass
class AgentMessage:
    role: str
    content: Optional[str] = None
    name: Optional[str] = None
    tool_call_id: Optional[str] = None
    tool_calls: Optional[List[Dict[str, Any]]] = None


@dataclass
class LLMClient:
    api_key: str
    model: str
    base_url: str | None = None
    wire_api: str = "auto"  # "auto" | "responses" | "chat"
    _client: Any = field(init=False, default=None, repr=False)
    _responses_supports_temperature: bool | None = field(init=False, default=None, repr=False)
    _chat_supports_temperature: bool | None = field(init=False, default=None, repr=False)
    _wire_api_resolved: str | None = field(init=False, default=None, repr=False)

    def __post_init__(self):
        # Normalize base_url once so the rest of atlas_agent can rely on
        # client.base_url without re-reading environment variables.
        if not self.base_url:
            self.base_url = os.environ.get("OPENAI_BASE_URL") or None

    def _ensure_client(self):
        if self._client is None:
            # Respect explicit base_url if provided; otherwise read from env
            kwargs = {"api_key": self.api_key}
            base = self.base_url or os.environ.get("OPENAI_BASE_URL")
            if base:
                kwargs["base_url"] = base
            self._client = OpenAI(**kwargs)
        return self._client

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

    def chat(
        self,
        *,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]] = None,
        temperature: float | None = None,
        stream: bool = False,
    ) -> Dict[str, Any]:
        client = self._ensure_client()
        normalized_tools = normalize_tools_for_chat_completions_api(tools)
        params: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
            "tools": normalized_tools or None,
        }
        if temperature is not None:
            params["temperature"] = float(temperature)
        if bool(stream):
            params["stream"] = True
        return client.chat.completions.create(**params)

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
        tools: Optional[List[Dict[str, Any]]] = None,
        temperature: float | None = None,
        parallel_tool_calls: bool = False,
        reasoning_effort: str | None = "high",
        reasoning_summary: str | None = "detailed",
        text_verbosity: str | None = "high",
        on_event: Callable[[Dict[str, Any]], None] | None = None,
    ) -> Dict[str, Any]:
        """Stream a turn via an OpenAI-compatible wire API.

        - Emits SSE-derived events via on_event (already normalized to dict).
        - Returns the final response as a plain dict in the Responses API shape.

        When wire_api='auto', we prefer the Responses API and fall back to Chat
        Completions if the provider does not support `/v1/responses`.
        """

        def _wire_mode() -> str:
            mode = (self._wire_api_resolved or self.wire_api or "auto").strip().lower()
            return mode if mode in {"auto", "responses", "chat"} else "auto"

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

        def _is_responses_unsupported(err: Exception) -> bool:
            # Best-effort: vendors/gateways that don't implement /v1/responses typically
            # return 404. Only treat these as "wire unsupported" signals; other 4xx
            # errors are usually real request/validation issues.
            for e in _iter_exception_chain(err):
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
                                        "function": {"name": name, "arguments": arguments},
                                    }
                                ],
                            }
                        )
                    continue

                if itype == "function_call_output":
                    call_id = str(it.get("call_id") or "").strip()
                    output = it.get("output")
                    if not isinstance(output, str):
                        try:
                            output = json.dumps(output, ensure_ascii=False)
                        except Exception:
                            output = str(output)
                    if call_id:
                        messages.append({"role": "tool", "tool_call_id": call_id, "content": output})
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
            if incomplete_details is not None:
                out["incomplete_details"] = incomplete_details
            return out

        reasoning: dict[str, Any] | None = None
        if self._model_supports_reasoning_summaries():
            if reasoning_effort is not None or reasoning_summary is not None:
                reasoning = {}
                if reasoning_effort is not None:
                    reasoning["effort"] = reasoning_effort
                if reasoning_summary is not None and str(reasoning_summary).lower() != "none":
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

        mode = _wire_mode()
        if mode == "chat":
            client = self._ensure_client()
            chat_messages = _responses_to_chat_messages(
                instructions_text=instructions, items=input_items
            )
            # Normalize to Chat Completions wire shape + apply provider-border
            # schema tightening for strict validators.
            chat_tools = normalize_tools_for_chat_completions_api(tools)

            def _is_unsupported_temperature_error(err: Exception) -> bool:
                msg = str(err or "")
                if not msg:
                    return False
                m = msg.lower()
                return ("unsupported parameter" in m and "temperature" in m) or (
                    "temperature" in m and "not supported" in m
                )

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
                if "temperature" in chat_params and _is_unsupported_temperature_error(e):
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

        # The SDK exposes a context-manager style stream object.
        def _is_unsupported_temperature_error(err: Exception) -> bool:
            msg = str(err or "")
            if not msg:
                return False
            m = msg.lower()
            return ("unsupported parameter" in m and "temperature" in m) or (
                "temperature" in m and "not supported" in m
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
            if mode == "auto" and _is_responses_unsupported(e):
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
            if "temperature" in params and _is_unsupported_temperature_error(e):
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

    def responses_complete_text(
        self,
        *,
        system_prompt: str,
        user_text: str,
        temperature: float | None = None,
        max_output_tokens: int | None = None,
        text_verbosity: str | None = None,
    ) -> str:
        """Unary Responses API call that returns assistant text.

        This is useful for small internal sub-tasks (e.g., session memory compaction)
        while keeping the implementation aligned with a Responses-first approach.
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
        if max_output_tokens is not None:
            params["max_output_tokens"] = int(max_output_tokens)
        if text_verbosity is not None:
            params["text"] = {"verbosity": str(text_verbosity)}

        def _is_unsupported_temperature_error(err: Exception) -> bool:
            msg = str(err or "")
            if not msg:
                return False
            m = msg.lower()
            return ("unsupported parameter" in m and "temperature" in m) or (
                "temperature" in m and "not supported" in m
            )

        if self._responses_supports_temperature is False:
            params.pop("temperature", None)

        try:
            resp = responses.create(**params)
            if "temperature" in params:
                self._responses_supports_temperature = True
        except Exception as e:
            if "temperature" in params and _is_unsupported_temperature_error(e):
                self._responses_supports_temperature = False
                params.pop("temperature", None)
                resp = responses.create(**params)
            else:
                raise
        data = self._to_plain_dict(resp)

        # Extract assistant output text from Responses API output items.
        out = data.get("output") if isinstance(data, dict) else None
        if not isinstance(out, list):
            return ""
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
        return "".join(chunks).strip()

    def complete_text(
        self,
        *,
        system_prompt: str,
        user_text: str,
        temperature: float | None = None,
        max_tokens: int | None = None,
    ) -> str:
        # Prefer Responses API. Fall back to Chat Completions if the provider/model
        # does not support Responses.
        try:
            return self.responses_complete_text(
                system_prompt=system_prompt,
                user_text=user_text,
                temperature=temperature,
                max_output_tokens=max_tokens,
            )
        except Exception:
            pass

        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_text},
        ]
        client = self._ensure_client()
        params: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
        }
        if temperature is not None:
            params["temperature"] = float(temperature)
        if max_tokens is not None:
            params["max_tokens"] = int(max_tokens)
        resp = client.chat.completions.create(**params)
        try:
            return (resp.choices[0].message.content or "").strip()
        except Exception:
            return ""

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


@dataclass
class BaseAgent:
    name: str
    system_prompt: str
    client: LLMClient
    tools: List[Dict[str, Any]] = field(default_factory=list)
    temperature: float = 0.2
    memory: List[AgentMessage] = field(default_factory=list)

    def reset(self):
        self.memory.clear()

    def run(self, user_text: str, *, shared_context: Optional[str] = None) -> AgentMessage:
        msgs: List[Dict[str, Any]] = []
        sys_content = self.system_prompt + (f"\n\nShared context:\n{shared_context}" if shared_context else "")
        msgs.append({"role": "system", "content": sys_content})
        for m in self.memory:
            d: Dict[str, Any] = {"role": m.role}
            if m.content is not None:
                d["content"] = m.content
            if m.name:
                d["name"] = m.name
            if m.tool_call_id:
                d["tool_call_id"] = m.tool_call_id
            msgs.append(d)
        msgs.append({"role": "user", "content": user_text})

        resp = self.client.chat(messages=msgs, tools=self.tools, temperature=self.temperature)
        choice = resp.choices[0]
        msg = choice.message
        out = AgentMessage(
            role="assistant",
            content=getattr(msg, "content", None),
            tool_calls=[
                {"id": c.id, "function": {"name": c.function.name, "arguments": c.function.arguments}}
                for c in (getattr(msg, "tool_calls", []) or [])
            ] or None,
        )
        self.memory.append(out)
        return out
