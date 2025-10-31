from __future__ import annotations

from dataclasses import dataclass
import logging
from typing import List, Tuple
from .base import LLMClient


ARBITER_SYSTEM = (
    "You are the Arbiter.\n"
    "Never accept camera steps that specify raw coordinates; replace them with typed camera planning.\n"
    "Camera rules for merged_plan: only specify mode=FIT|ORBIT|DOLLY|STATIC, targets, times, and constraints (keep_visible=true, min_coverage≥0.95, margin 0.0–0.1). No numbers for eye/center/up.\n"
    "Given: user request, scene context, multiple design options, and reviewer feedback, pick the best option OR blend two into a single plan.\n"
    "Output strictly in JSON with fields: {\n"
    "  \"selected_index\": number (1-based),\n"
    "  \"merged_plan\": string (the final plan to implement)\n"
    "}. Keep merged_plan concise and implementable."
)


@dataclass
class Arbiter:
    client: LLMClient
    temperature: float = 0.3

    def decide(self, *, user_text: str, scene_context: str, options: List[str], feedbacks: List[str]) -> Tuple[int, str]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Arbiter] Deciding among %d option(s)", len(options))
        import json
        joined_opts = "\n\n".join([f"Option {i+1}:\n{opt}" for i, opt in enumerate(options)])
        joined_fb = "\n\n".join([f"Feedback {i+1}:\n{fb}" for i, fb in enumerate(feedbacks) if fb])
        prompt = (
            f"User request:\n{user_text}\n\nScene context:\n{scene_context}\n\n"
            f"Design options:\n{joined_opts}\n\nReviewer feedback:\n{joined_fb}\n\n"
            "Respond with compact JSON only, no prose."
        )
        text = self.client.complete_text(system_prompt=ARBITER_SYSTEM, user_text=prompt, temperature=self.temperature)
        try:
            data = json.loads(text or "{}")
            idx = int(data.get("selected_index", 1))
            merged = str(data.get("merged_plan", ""))
            if not merged:
                # fallback to first option if parsing failed
                merged = options[0] if options else ""
            logger.info("[Arbiter] selected_index=%d", idx)
            logger.info("[Arbiter] merged_plan:\n%s", merged[:1200])
            return idx, merged
        except Exception:
            logger.warning("[Arbiter] Failed to parse decision JSON; falling back to first option")
            return 1, (options[0] if options else "")
