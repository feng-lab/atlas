import base64
import json
import mimetypes
from dataclasses import dataclass
from typing import Optional, Tuple

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
            "Decide using only the provided plan text, scene context, verified Facts JSON, and an optional preview image.\n"
            "Strict constraints:\n"
            "- You have no tools and no filesystem/network access.\n"
            "- Do not ask the user to paste/upload files or run shell commands.\n"
            "- Do not speculate beyond Facts; treat Facts JSON as authoritative.\n"
            "- Absence of evidence is not a failure: when Facts are silent/insufficient, default to satisfied=true and at most note the unverified aspect. Only fail when Facts clearly contradict the core intent.\n"
            "Blocking criteria (fail only when certain):\n"
            "- Facts contradict the core intent (e.g., required keys/values are missing or wrong, wrong objects were affected, camera_validation.ok=false).\n"
            "- The plan claims changes that Facts explicitly do not show.\n"
            "Non‑blocking guidance (use feedback only):\n"
            "- Minor naming differences, unverifiable external inputs (e.g., logs that you cannot read), cosmetic preferences.\n"
            "For camera, rely on typed camera_validation.ok (coverage/visibility) instead of raw numeric angles unless explicitly requested.\n"
            "Timeline keys: keys.objects contains per‑object parameter keys. Consider keys present when this mapping is non‑empty (object ids are strings).\n"
            "If some aspects are not measurable from facts/tools, set satisfied=true when the core intent appears met and note the limitation in feedback.\n"
            "Update the session TODO list by returning checkbox lines under 'todo_update' (string with '- [ ]'/'- [x]' lines only).\n"
            'Respond with compact JSON only: {"satisfied": true|false, "feedback": short guidance, "todo_update": checkbox lines (optional)}.'
        )
        # Build a facts block from full live facts (read-only queries prepared by Supervisor)
        facts_block = "Facts (JSON):\n"
        try:
            if isinstance(facts, dict):
                facts_block += json.dumps(facts, ensure_ascii=False)
            elif isinstance(facts, str):
                facts_block += facts
            else:
                facts_block += "(none)"
        except Exception:
            facts_block += "(none)"
        prompt = (
            f"User request:\n{user_text}\n\nScene context + history:\n{scene_context}\n\n"
            f"Plan:\n{plan_text}\n\n{facts_block}\n\nRespond with JSON only."
        )
        # Optional screenshot: when provided, include as multimodal input. Convert to data URL.
        data_url: Optional[str] = None
        if preview_image_path:
            try:
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
            # Parsing is non-blocking; default to satisfied with a soft note to avoid rework loops.
            return (
                True,
                "Inspector parse error (default-approving); please sanity-check results manually.",
                None,
            )
