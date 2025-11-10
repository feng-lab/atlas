import json
import logging
from dataclasses import dataclass
from typing import Any, Dict

from .base import LLMClient

DESCRIBER_SYSTEM = (
    "You are the Describer.\n"
    "Given a facts snapshot (keys and times) and the user request, write a crisp, factual summary of what is now set.\n"
    "Do not claim anything not present in facts. If empty, say so and propose one next step."
)


@dataclass
class Describer:
    client: LLMClient
    temperature: float = 0.2

    def describe(self, *, user_text: str, facts: Dict[str, Any], conversation: str | None = None) -> str:
        logger = logging.getLogger("atlas_agent.agents")
        # Build a full facts text (no sampling) from scene_facts, including key values (excluding camera values)
        digest_text = ""
        if isinstance(facts, dict):
            try:
                lines: list[str] = []
                objs = (facts.get("objects_list") or [])
                lines.append(f"Objects ({len(objs)}):")
                # List all objects
                for o in objs:
                    try:
                        oid = o.get("id")
                        typ = o.get("type")
                        nm = o.get("name") or o.get("path")
                        vis = o.get("visible")
                        lines.append(f"- {oid}:{typ}:{nm} visible={bool(vis)}")
                    except Exception:
                        continue
                # Camera key times (values omitted by request)
                cam_times = []
                try:
                    cam_times = facts.get("keys", {}).get("camera") or []
                except Exception:
                    cam_times = []
                if cam_times:
                    lines.append("Camera key times: " + ", ".join(str(float(t)) for t in cam_times))
                # Per-object param keys with values when provided (exclude camera)
                per = facts.get("keys", {}).get("objects", {}) or {}
                if per:
                    lines.append("Object keys:")
                    for oid, mp in per.items():
                        for jk, entry in (mp.items() if isinstance(mp, dict) else []):
                            # entry is either [times...] or [{time,value}...]
                            if isinstance(entry, list) and entry and isinstance(entry[0], dict):
                                # include values
                                lines.append(f"- {oid}:{jk}:")
                                for it in entry:
                                    t = it.get("time")
                                    v = it.get("value", None)
                                    try:
                                        vtxt = json.dumps(v, ensure_ascii=False)
                                    except Exception:
                                        vtxt = str(v)
                                    lines.append(f"    - time={t} value={vtxt}")
                            else:
                                # times only
                                lines.append(f"- {oid}:{jk}: times={list(entry)}")
                # Scene values (current, all params)
                sv = facts.get("scene_values") or {}
                if sv:
                    lines.append("Scene values:")
                    for oid, mp in (sv.items() if isinstance(sv, dict) else []):
                        try:
                            lines.append(f"- {oid}:")
                            # Show all key/value pairs
                            for jk, v in (mp.items() if isinstance(mp, dict) else []):
                                try:
                                    vtxt = json.dumps(v, ensure_ascii=False)
                                except Exception:
                                    vtxt = str(v)
                                lines.append(f"    - {jk} = {vtxt}")
                        except Exception:
                            continue
                digest_text = "\n".join(lines)
            except Exception:
                digest_text = "(unavailable)"
        else:
            digest_text = "(unavailable)"
        prompt = (
            f"User request:\n{user_text}\n\n" +
            (f"Conversation history:\n{conversation}\n\n" if conversation else "") +
            f"Facts (full):\n{digest_text}\n\n" +
            "Write a short factual summary (2–5 bullets) strictly based on the facts."
        )
        text = self.client.complete_text(system_prompt=DESCRIBER_SYSTEM, user_text=prompt, temperature=self.temperature)
        logger.info("[Describer] Summary:\n%s", (text or ""))
        return text
