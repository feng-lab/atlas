import logging
from dataclasses import dataclass

from .base import LLMClient

REVIEWER_SYSTEM = (
    "You are a Reviewer for Atlas scene/animation designs.\n"
    "Evaluate options for clarity, feasibility, and alignment with the user’s request.\n"
    "- Prefer typed camera intent (FIT/ORBIT/DOLLY/STATIC) over raw coordinates.\n"
    "- Ensure scene edits (stateless) are separated from timeline animation.\n"
    "- Look for a concise Plan Summary seed (key moments and per‑object changes).\n"
    "- When a 'TASK BRIEF:' is present, check adherence and call out gaps.\n"
    "- Do NOT prescribe exact parameter json_keys or option label strings; leave parameter/option resolution to the Implementer via live discovery.\n"
    "- For camera motion, ensure the option states duration/targets/constraints clearly; do not prescribe step sizes — leave execution to Implementer.\n"
    "Provide the best option number, a short rationale, and 2–3 concrete improvements (e.g., clarify axis/margin/coverage constraints, or specify validation). Do not request user confirmations; suggest assumptions instead.\n"
    "When Intent=scene, flag any timeline/animation steps or animation verification as out of scope and suggest scene‑only validation (objects visible, non‑overlap, scene camera framing)."
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
            f"User request:\n{user_text}\n\nScene context + history:\n{scene_context}\n\n"
            f"Design options:\n{joined}\n\nRespond with: Best option #, why, and 2–3 improvements."
        )
        text = self.client.complete_text(system_prompt=REVIEWER_SYSTEM, user_text=prompt, temperature=self.temperature)
        logger.info("[%s] Feedback:\n%s", self.name, (text or ""))
        return text
