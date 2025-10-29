from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional
import os


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
        # Normalize base_url once so other components (e.g., Agents SDK provider)
        # can rely on client.base_url without re-reading environment variables.
        if not self.base_url:
            self.base_url = os.environ.get("OPENAI_BASE_URL") or None

    def _ensure_client(self):
        if self._client is None:
            try:
                from openai import OpenAI  # type: ignore
            except Exception as e:
                raise RuntimeError(
                    "OpenAI Python package not installed. Please `pip install openai>=1.0.0`. "
                ) from e
            # Respect explicit base_url if provided; otherwise read from env
            kwargs = {"api_key": self.api_key}
            base = self.base_url or os.environ.get("OPENAI_BASE_URL")
            if base:
                kwargs["base_url"] = base
            self._client = OpenAI(**kwargs)
        return self._client

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

    def complete_text(
        self,
        *,
        system_prompt: str,
        user_text: str,
        temperature: float = 0.2,
        max_tokens: int | None = None,
    ) -> str:
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
