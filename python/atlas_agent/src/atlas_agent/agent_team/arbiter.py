import json
import logging
import re
from dataclasses import dataclass
from typing import List, Tuple

from .base import LLMClient

ARBITER_SYSTEM = (
    "You are the Arbiter.\n"
    "Select the best option (or blend two) based on the user request, scene context, and reviewer feedback.\n"
    "Prefer typed camera intent (FIT/ORBIT/DOLLY/STATIC) over raw coordinates. Do not prescribe implementation details (step sizes, chaining) — leave execution to the Implementer. Keep the merged plan concise and implementable.\n"
    "Apply a minimal mutation filter: remove any unrelated parameter edits not required by the Task Brief unless explicitly requested.\n"
    "Do NOT assert exact parameter json_keys or option label strings in the merged plan. Keep descriptions semantic (e.g., 'solid single blue', 'wireframe-only at 6.5s'). Parameter/option resolution is Implementer-only.\n"
    "Treat user-mentioned file/folder names as hints only; do not expand or guess exact paths. Leave path resolution/verification to the Implementer tools.\n"
    "Do not include any requests to confirm/ask the user in the plan. Convert such text into plain 'Assumptions' with reasonable defaults so the plan is immediately executable.\n"
    "End the merged plan with a TODO section using checkboxes (human-readable, minimal steps). Example:\n"
    "TODO:\n- [ ] Step A…\n- [ ] Step B…\n"
    "Respond with JSON only: {\"selected_index\": 1-based number, \"merged_plan\": string}."
)


@dataclass
class Arbiter:
    client: LLMClient
    temperature: float = 0.3

    def decide(self, *, user_text: str, scene_context: str, options: List[str], feedbacks: List[str]) -> Tuple[int, str]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Arbiter] Deciding among %d option(s)", len(options))
        joined_opts = "\n\n".join([f"Option {i+1}:\n{opt}" for i, opt in enumerate(options)])
        joined_fb = "\n\n".join([f"Feedback {i+1}:\n{fb}" for i, fb in enumerate(feedbacks) if fb])
        prompt = (
            f"User request:\n{user_text}\n\nScene context + history:\n{scene_context}\n\n"
            f"Design options:\n{joined_opts}\n\nReviewer feedback:\n{joined_fb}\n\n"
            "Respond with compact JSON only, no prose."
        )
        text = self.client.complete_text(system_prompt=ARBITER_SYSTEM, user_text=prompt, temperature=self.temperature)
        try:
            data = json.loads(text or "{}")
            idx = int(data.get("selected_index", 1))
            merged = str(data.get("merged_plan", ""))
            if not merged:
                merged = options[0] if options else ""
            logger.info("[Arbiter] selected_index=%d", idx)
            logger.info("[Arbiter] merged_plan:\n%s", merged)
            return idx, merged
        except Exception:
            # Robust fallback: infer index from reviewer feedbacks (look for 'Best Option: Option X' or 'Selected Option')
            idx_guess = None
            for fb in feedbacks:
                if not fb:
                    continue
                m = re.search(r"Option\s+(\d+)", fb, re.IGNORECASE)
                if m:
                    try:
                        idx_guess = int(m.group(1))
                        break
                    except Exception:
                        pass
            if idx_guess is None:
                logger.warning("[Arbiter] Failed to parse decision JSON; falling back to first option")
                idx_guess = 1
            merged = options[idx_guess - 1] if options and 1 <= idx_guess <= len(options) else (options[0] if options else "")
            return idx_guess, merged
