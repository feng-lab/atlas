from __future__ import annotations

from dataclasses import dataclass
from typing import Tuple, Optional
import json
from .base import LLMClient


@dataclass
class Inspector:
    client: LLMClient
    temperature: float = 0.2

    # Inspector has a single responsibility: decide()

    def decide(self, *, user_text: str, scene_context: str, plan_text: str, facts: dict, preview_image_path: Optional[str] = None) -> Tuple[bool, str, Optional[str]]:
        """Return (satisfied, feedback, todo_update_text). Satisfied means no further changes are required.

        The decision is based on the current plan, scene context, and verified facts (keys/times).
        """
        SYSTEM = (
            "You are the Inspector making a go/no‑go decision.\n"
            "Given the user request, scene context, the plan text, and verified facts, decide if the request is fulfilled.\n"
            "Prefer typed camera validation over raw coordinates when camera changes are involved. Favor validated coverage/visibility that matches the stated intent; avoid insisting on specific numeric angle proofs unless explicitly requested.\n"
            "If some verification aspects are not measurable from facts/tools, return satisfied=true when the core intent seems met and include feedback noting the limitation (do not block on non‑verifiable details).\n"
            "Facts JSON includes keys.objects (per‑object param key times). If keys.objects is non‑empty for the target object(s), acknowledge that parameter keys exist; do not request 'add keys' unless the mapping is truly empty.\n"
            "Also update the session TODO list if present in context by returning checkbox lines under 'todo_update' (string with '- [ ]'/'- [x]' lines only).\n"
            "Respond with compact JSON only: {\"satisfied\": true|false, \"feedback\": short guidance, \"todo_update\": checkbox lines (optional)}."
        )
        # Prefer compact digest text only; never fall back to large JSON to avoid prompt bloat
        digest_text = None
        try:
            if isinstance(facts, dict) and isinstance(facts.get("digest_text"), str):
                digest_text = facts.get("digest_text").strip()
        except Exception:
            digest_text = None
        facts_block = "Facts (compact):\n" + (digest_text or "(none)")
        prompt = (
            f"User request:\n{user_text}\n\nScene context + history:\n{scene_context}\n\n"
            f"Plan:\n{plan_text}\n\n{facts_block}\n\nRespond with JSON only."
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
            tu = data.get("todo_update")
            if isinstance(tu, str):
                tu = tu.strip()
            else:
                tu = None
            return sat, fb, tu
        except Exception:
            return False, "Inspector could not parse decision; please re‑evaluate with tightened plan.", None
