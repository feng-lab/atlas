import os
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

from openai import OpenAI  # type: ignore


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
    _client: Any = field(init=False, default=None, repr=False)

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
        """Convert Chat Completions-style tool specs to Responses API style.

        The OpenAI Python SDK accepts Responses-style tools like:
          {"type":"function","name":"...","description":"...","parameters":{...},"strict":true}

        It also *can* accept Chat Completions tools, but only when they were created via
        openai.pydantic_function_tool(), which we intentionally do not require.
        """

        if not raw_tools:
            return None

        out: List[Dict[str, Any]] = []
        for t in raw_tools:
            if not isinstance(t, dict):
                continue
            if str(t.get("type") or "") != "function":
                # Non-function tools (if introduced later) pass through.
                out.append(t)
                continue

            # Standard Responses tool shape: no nested "function" key.
            if "function" not in t:
                out.append(t)
                continue

            fn = t.get("function")
            if not isinstance(fn, dict):
                # SDK would error on this; drop it rather than breaking the whole request.
                continue

            name = str(fn.get("name") or "").strip()
            if not name:
                continue
            params = fn.get("parameters")
            if not isinstance(params, dict):
                params = {"type": "object", "properties": {}}

            converted: Dict[str, Any] = {
                "type": "function",
                "name": name,
                "parameters": params,
                # Default to strict JSON tool calling.
                "strict": bool(fn.get("strict", True)),
            }
            desc = fn.get("description")
            if isinstance(desc, str) and desc.strip():
                converted["description"] = desc.strip()
            out.append(converted)

        return out

    def chat(
        self,
        *,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]] = None,
        temperature: float = 0.2,
        stream: bool = False,
    ) -> Dict[str, Any]:
        client = self._ensure_client()
        return client.chat.completions.create(
            model=self.model,
            messages=messages,
            temperature=temperature,
            tools=tools or None,
        )

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
        temperature: float = 0.2,
        parallel_tool_calls: bool = False,
        reasoning_effort: str | None = "high",
        reasoning_summary: str | None = "detailed",
        text_verbosity: str | None = "high",
        on_event: Callable[[Dict[str, Any]], None] | None = None,
    ) -> Dict[str, Any]:
        """Stream a turn via the OpenAI Responses API.

        - Emits SSE-derived events via on_event (already normalized to dict).
        - Returns the final response as a plain dict.
        """

        client = self._ensure_client()
        if not hasattr(client, "responses"):
            raise RuntimeError("OpenAI client does not support the Responses API")
        responses = getattr(client, "responses")
        if not hasattr(responses, "stream"):
            raise RuntimeError("OpenAI client does not support responses.stream")

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
            "temperature": temperature,
            "parallel_tool_calls": bool(parallel_tool_calls),
        }
        normalized_tools = self._normalize_tools_for_responses(tools)
        if normalized_tools:
            params["tools"] = normalized_tools
        if reasoning is not None:
            params["reasoning"] = reasoning
        if text is not None:
            params["text"] = text

        # The SDK exposes a context-manager style stream object.
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
        return self._to_plain_dict(final) if final is not None else {}

    def responses_complete_text(
        self,
        *,
        system_prompt: str,
        user_text: str,
        temperature: float = 0.2,
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
            "temperature": temperature,
        }
        if max_output_tokens is not None:
            params["max_output_tokens"] = int(max_output_tokens)
        if text_verbosity is not None:
            params["text"] = {"verbosity": str(text_verbosity)}

        resp = responses.create(**params)
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
        temperature: float = 0.2,
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
        resp = client.chat.completions.create(
            model=self.model,
            messages=messages,
            temperature=temperature,
            **({"max_tokens": max_tokens} if max_tokens is not None else {}),
        )
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
        temperature: float = 0.2,
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
            resp = client.chat.completions.create(
                model=self.model,
                messages=messages,
                temperature=temperature,
                **({"max_tokens": max_tokens} if max_tokens is not None else {}),
            )
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
