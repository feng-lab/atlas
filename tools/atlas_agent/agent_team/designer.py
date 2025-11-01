from __future__ import annotations

from dataclasses import dataclass
import logging
from .base import LLMClient


DESIGNER_SYSTEM = (
    "You are the Designer for Atlas scene/animation.\n"
    "Propose 2–3 distinct high‑level designs (no tool calls). Each design must follow these rules:\n"
    "- Intent guard: if the user is ONLY asking for file loading or static scene edits, DO NOT include animation/timelines.\n"
    "- Scene vs animation: clearly separate stateless scene edits (arrangement/styling, no time/easing) from timeline animation keys.\n"
    "- Stay on topic: do not propose unrelated global styling (background/axis/fonts) unless the user asked.\n"
    "- Do NOT invent camera numbers (eye/center/up/positions). Never propose raw coordinates.\n"
    "- For camera, specify typed planning only: mode=FIT|ORBIT|DOLLY|STATIC, targets (ids or 'primary mesh'), times, and constraints {keep_visible=true, margin ~0.0–0.1, min_coverage≥0.95}.\n"
    "- For camera, prefer UI‑parity operators in the description (focus → rotate → reset). E.g., 'Focus on Mesh(ID), then AZIMUTH 90° at 2.5s/5s/7.5s/10s'.\n"
    "- Non‑camera parameters: refer to canonical parameter names conceptually (e.g., Color Mode='Single Color'); avoid guessing option strings if unknown.\n"
    "- End each option with a short Plan Summary seed: a few key moments (times) and per‑object changes, using json_key names where known.\n"
    "Keep each option short (5–8 bullets)."
)


@dataclass
class Designer:
    client: LLMClient
    temperature: float = 0.3

    def propose(self, user_text: str, *, scene_context: str) -> list[str]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Designer] Proposing designs for request: %s", (user_text or "").strip()[:200])
        # No embedded examples; rely on the system prompt and scene context only
        examples_text = ""
        prompt = (
            f"User request:\n{user_text}\n\nScene context:\n{scene_context}\n\n"
            "Output 2–3 numbered design options; no prose, no tools." + examples_text
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
