from __future__ import annotations

from dataclasses import dataclass
import logging
from .base import LLMClient


DESIGNER_SYSTEM = (
    "You are the Designer for Atlas animation.\n"
    "Propose 2–3 distinct high‑level designs (no tool calls). Each design should specify:\n"
    "- Targets (objects/groups) and rationale\n"
    "- Keyframes: which parameters/camera, their times, and intended values\n"
    "- Visual goals (emphasis, readability)\n"
    "Keep each option short (5–8 bullets)."
)


@dataclass
class Designer:
    client: LLMClient
    temperature: float = 0.3

    def propose(self, user_text: str, *, scene_context: str) -> list[str]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Designer] Proposing designs for request: %s", (user_text or "").strip()[:200])
        prompt = (
            f"User request:\n{user_text}\n\nScene context:\n{scene_context}\n\n"
            "Output 2–3 numbered design options; no prose, no tools."
        )
        text = self.client.complete_text(system_prompt=DESIGNER_SYSTEM, user_text=prompt, temperature=self.temperature)
        # Split on numbered headings as best‑effort
        options: list[str] = []
        cur: list[str] = []
        for line in (text or "").splitlines():
            if line.strip().startswith(("1.", "2.", "3.")):
                if cur:
                    options.append("\n".join(cur).strip())
                    cur = []
                cur.append(line)
            else:
                cur.append(line)
        if cur:
            options.append("\n".join(cur).strip())
        options = [o for o in options if o]
        logger.info("[Designer] Proposed %d option(s)", len(options))
        for i, opt in enumerate(options, 1):
            logger.info("[Designer] Option %d:\n%s", i, opt[:1200])
        return options
