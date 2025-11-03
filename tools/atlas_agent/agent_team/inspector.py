from __future__ import annotations

from dataclasses import dataclass
from typing import Tuple, Optional
import json
from .base import LLMClient
from .agents_sdk import act_with_agents_sdk


INSPECTOR_SYSTEM = (
    "You are the Inspector for Atlas scene/animation design.\n"
    "Review the proposed plan against the scene state and provide brief, actionable feedback.\n"
    "- Prefer typed camera intent over raw coordinates.\n"
    "- Expect a Plan Summary with two views (global timeline + per‑object) using canonical json_keys and separating scene vs timeline.\n"
    "- For camera changes, look for validation coverage/visibility to match intent (e.g., keep_visible true, coverage≥requested). Do not require specific step sizes; focus on outcomes.\n"
    "- Call out missing details or risks, and ask one clarifying question if needed.\n"
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
            temperature=self.temperature,
            agent_name="Inspector",
        )
        return (msgs[0].content if msgs and msgs[0].content else "")

    def decide(self, *, user_text: str, scene_context: str, plan_text: str, facts: dict, preview_image_path: Optional[str] = None) -> Tuple[bool, str]:
        """Return (satisfied, feedback). Satisfied means no further changes are required.

        The decision is based on the current plan, scene context, and verified facts (keys/times).
        """
        SYSTEM = (
            "You are the Inspector making a go/no‑go decision.\n"
            "Given the user request, scene context, the plan text, and verified facts, decide if the request is fulfilled.\n"
            "Prefer typed camera validation over raw coordinates when camera changes are involved. For 360°, require a validated continuous orbit over the stated duration (avoid trivial 2‑key tricks).\n"
            "Respond with compact JSON only: {\"satisfied\": true|false, \"feedback\": short guidance if not satisfied}."
        )
        prompt = (
            f"User request:\n{user_text}\n\nScene context + history:\n{scene_context}\n\n"
            f"Plan:\n{plan_text}\n\nFacts (JSON):\n{json.dumps(facts)}\n\nRespond with JSON only."
        )
        # Optional screenshot: when provided, include as multimodal input. Convert to data URL.
        data_url: Optional[str] = None
        if preview_image_path:
            try:
                import base64, mimetypes
                mime, _ = mimetypes.guess_type(preview_image_path)
                mime = mime or "image/png"
                with open(preview_image_path, "rb") as f:
                    b64 = base64.b64encode(f.read()).decode("ascii")
                data_url = f"data:{mime};base64,{b64}"
            except Exception:
                data_url = None
        text = self.client.complete_with_image(
            system_prompt=SYSTEM,
            user_text=prompt,
            image_data_url=data_url,
            temperature=min(0.3, self.temperature),
        )
        try:
            data = json.loads(text or "{}")
            sat = bool(data.get("satisfied", False))
            fb = str(data.get("feedback", "")).strip()
            return sat, fb
        except Exception:
            return False, "Inspector could not parse decision; please re‑evaluate with tightened plan."
