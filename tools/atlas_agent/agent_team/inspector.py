from __future__ import annotations

from dataclasses import dataclass
from typing import Tuple, Optional
import json
from .base import LLMClient
from .agents_sdk import act_with_agents_sdk


INSPECTOR_SYSTEM = (
    "You are the Inspector for Atlas animation design.\n"
    "Strict camera rule: plans must not propose raw camera coordinates. Flag and request typed camera planning instead (mode, targets, constraints).\n"
    "Critique the proposed plan against the scene state. Identify risks (occlusion, confusing colors), missing details (key times, parameter names),\n"
    "and ask clarifying questions where necessary. Suggest concrete adjustments (e.g., use CameraSolve with keep_visible=true; margin≈0.05; validate coverage≥0.95; dim non‑targets).\n"
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

    def decide(self, *, user_text: str, scene_context: str, plan_text: str, facts: dict, preview_image_path: Optional[str] = None) -> Tuple[bool, str]:
        """Return (satisfied, feedback). Satisfied means no further changes are required.

        The decision is based on the current plan, scene context, and verified facts (keys/times).
        """
        SYSTEM = (
            "You are the Inspector making a go/no‑go decision.\n"
            "Given the user request, scene context, the plan text, and facts (objects list + verified keys/times), decide if the user request is fulfilled.\n"
            "Camera policy: if any camera changes are part of the plan, satisfaction requires typed validation (camera_validate ok=true with coverage≥min_coverage) and no reliance on raw coordinates in the plan.\n"
            "Guidance: For file‑loading requests, satisfaction is achieved if requested file(s) appear in objects_list. For animation/keyframe requests, satisfaction requires the expected keys/times to exist (and camera validation if applicable).\n"
            "Respond with compact JSON only: {\n"
            "  \"satisfied\": true|false,\n"
            "  \"feedback\": \"short guidance if not satisfied (what to change next)\"\n"
            "}."
        )
        prompt = (
            f"User request:\n{user_text}\n\nScene context:\n{scene_context}\n\n"
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
