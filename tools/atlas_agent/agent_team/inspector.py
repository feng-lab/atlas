from __future__ import annotations

from dataclasses import dataclass
from typing import Tuple
import json
from .base import LLMClient
from .agents_sdk import act_with_agents_sdk


INSPECTOR_SYSTEM = (
    "You are the Inspector for Atlas animation design.\n"
    "Critique the proposed plan against the scene state. Identify risks (occlusion, confusing colors),\n"
    "missing details (key times, parameter names), and ask clarifying questions where necessary.\n"
    "Suggest concrete adjustments (e.g., use CameraFit first; apply global cut with small margin; dim non-targets).\n"
    "Keep it brief and actionable."
)


@dataclass
class Inspector:
    client: LLMClient
    temperature: float = 0.2

    def review(self, user_text: str, *, scene_context: str, plan_text: str) -> str:
        combined = scene_context + "\n\nProposed Plan:\n" + plan_text
        msgs = act_with_agents_sdk(
            client=self.client,
            system_prompt=INSPECTOR_SYSTEM,
            user_text=user_text,
            shared_context=combined,
            tools=[],
            dispatch=lambda n, a: "{}",
            memory_texts=[],
            temperature=self.temperature,
            agent_name="Inspector",
        )
        return (msgs[0].content if msgs and msgs[0].content else "")

    def decide(self, *, user_text: str, scene_context: str, plan_text: str, facts: dict) -> Tuple[bool, str]:
        """Return (satisfied, feedback). Satisfied means no further changes are required.

        The decision is based on the current plan, scene context, and verified facts (keys/times).
        """
        SYSTEM = (
            "You are the Inspector making a go/no‑go decision.\n"
            "Given the user request, scene context, the plan text, and facts (objects list + verified keys/times), "
            "decide if the user request is fulfilled.\n"
            "Guidance: When the request is about loading files or listing objects, satisfaction is achieved if the requested file(s) appear in objects_list with expected names/paths; timeline keys are not required. For animation/keyframe requests, satisfaction requires the expected keys/times to exist.\n"
            "Respond with compact JSON only: {\n"
            "  \"satisfied\": true|false,\n"
            "  \"feedback\": \"short guidance if not satisfied (what to change next)\"\n"
            "}."
        )
        prompt = (
            f"User request:\n{user_text}\n\nScene context:\n{scene_context}\n\n"
            f"Plan:\n{plan_text}\n\nFacts (JSON):\n{json.dumps(facts)}\n\nRespond with JSON only."
        )
        text = self.client.complete_text(system_prompt=SYSTEM, user_text=prompt, temperature=min(0.3, self.temperature))
        try:
            data = json.loads(text or "{}")
            sat = bool(data.get("satisfied", False))
            fb = str(data.get("feedback", "")).strip()
            return sat, fb
        except Exception:
            return False, "Inspector could not parse decision; please re‑evaluate with tightened plan."
