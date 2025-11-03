from __future__ import annotations

from dataclasses import dataclass
import logging
from typing import Any, Dict
from .base import LLMClient


DESCRIBER_SYSTEM = (
    "You are the Describer.\n"
    "Given a facts snapshot (keys and times) and the user request, write a crisp, factual summary of what is now set.\n"
    "Do not claim anything not present in facts. If empty, say so and propose one next step."
)


@dataclass
class Describer:
    client: LLMClient
    temperature: float = 0.2

    def describe(self, *, user_text: str, facts: Dict[str, Any], conversation: str | None = None) -> str:
        logger = logging.getLogger("atlas_agent.agents")
        import json
        prompt = (
            f"User request:\n{user_text}\n\n" +
            (f"Conversation history:\n{conversation}\n\n" if conversation else "") +
            f"Facts (JSON):\n{json.dumps(facts, indent=2)}\n\n" +
            "Write a short factual summary (2–5 bullets) strictly based on the facts."
        )
        text = self.client.complete_text(system_prompt=DESCRIBER_SYSTEM, user_text=prompt, temperature=self.temperature)
        logger.info("[Describer] Summary:\n%s", (text or "")[:1200])
        return text
