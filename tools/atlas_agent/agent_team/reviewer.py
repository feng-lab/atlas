from __future__ import annotations

from dataclasses import dataclass
import logging
from .base import LLMClient


REVIEWER_SYSTEM = (
    "You are a Reviewer for Atlas animation designs.\n"
    "Given multiple design options, critique clarity, feasibility, and user alignment.\n"
    "Identify the best option and suggest small improvements (bullet points). Keep it concise."
)


@dataclass
class Reviewer:
    client: LLMClient
    temperature: float = 0.3
    name: str = "Reviewer"

    def review(self, user_text: str, *, scene_context: str, options: list[str]) -> str:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[%s] Reviewing %d option(s)", self.name, len(options))
        joined = "\n\n".join([f"Option {i+1}:\n{opt}" for i, opt in enumerate(options)])
        prompt = (
            f"User request:\n{user_text}\n\nScene context:\n{scene_context}\n\n"
            f"Design options:\n{joined}\n\nRespond with: Best option #, why, and 2–3 improvements."
        )
        text = self.client.complete_text(system_prompt=REVIEWER_SYSTEM, user_text=prompt, temperature=self.temperature)
        logger.info("[%s] Feedback:\n%s", self.name, (text or "")[:1200])
        return text
