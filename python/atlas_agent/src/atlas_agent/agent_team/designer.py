import logging
from dataclasses import dataclass

from .base import LLMClient

DESIGNER_SYSTEM = (
    "You are the Designer for Atlas scene/animation.\n"
    "Propose 1–3 distinct high‑level designs (no tool calls). Focus on outcomes, not tool usage.\n"
    "- Separate scene styling/arrangement (no time) from timeline animation.\n"
    "- Do not invent camera coordinates; describe typed camera intent (e.g., FIT/ORBIT/DOLLY/STATIC) and targets. Include axis and duration window when relevant.\n"
    "- Do not specify implementation details such as step sizes or base_value chaining.\n"
    "- Do NOT assert concrete json_key or option label strings (e.g., avoid naming 'Color Mode StringIntOption', 'Mesh Color', etc.). Describe intended visual outcomes instead, such as 'solid single blue color until 6.5s; wireframe‑only after'.\n"
    "- Treat user-mentioned file/folder names as hints only; do not expand or guess exact paths. Leave path resolution/verification to Implementer tools.\n"
    "- Minimal mutation policy: propose ONLY the changes strictly necessary to fulfill the user’s intent. Do NOT add unrelated tweaks unless explicitly requested in the Task Brief.\n"
    "- Align with any 'TASK BRIEF:' provided in context.\n"
    "- Do not include confirmation questions; if defaults are needed, state them under 'Assumptions'.\n"
    "- End each option with a short Plan Summary seed (key moments and per‑object changes). Keep it concise and semantic; leave parameter resolution to the Implementer.\n"
    "- For Intent=scene, do NOT include timeline/animation steps or animation verification (e.g., do not mention listing keys).\n"
)


@dataclass
class Designer:
    client: LLMClient
    temperature: float = 0.3

    def propose(self, user_text: str, *, scene_context: str) -> list[str]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Designer] Proposing designs for request: %s", (user_text or "").strip())
        # No embedded examples; rely on the system prompt and scene context only
        examples_text = ""
        prompt = (
            f"User request:\n{user_text}\n\nScene context + history:\n{scene_context}\n\n"
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
            logger.info("[Designer] Option %d:\n%s", i, opt)
        return options
