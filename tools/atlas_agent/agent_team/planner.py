from __future__ import annotations

from dataclasses import dataclass
from .base import LLMClient
from .agents_sdk import act_with_agents_sdk


PLANNER_SYSTEM = (
    "You are the Planner for Atlas scene/animation design.\n"
    "Plan conservatively and precisely. Always enumerate parameters/options before proposing writes.\n"
    "Process:\n"
    "  1) Identify target objects (ids/types) via scene_list_objects.\n"
    "  2) For each target/group, call scene_list_params(...) to enumerate canonical json_keys, types, supports_interpolation, ranges, and option_names.\n"
    "  3) Separate stateless scene edits (no time/easing) from timeline keys.\n"
    "     - Scene: plan scene_validate_apply → scene_apply for base arrangement/styling.\n"
    "     - Animation: plan Batch(SetKey...) for timed changes.\n"
    "  4) Choose key times/easing (use 'Switch' for non‑interpolatable).\n"
    "  5) For camera, plan via typed modes (Fit/Orbit/Dolly/Static) only; add CameraValidate constraints.\n"
    "  6) Save animation path if requested.\n"
    "Output a concise, numbered plan plus a short Plan Summary draft (two views) that is translatable to tools.\n"
    "Intent guard: if the user request is ONLY file loading or static scene edits, do not include timelines or keys in the plan.\n"
)


@dataclass
class Planner:
    client: LLMClient
    temperature: float = 0.3

    def make(self, user_text: str, *, scene_context: str) -> str:
        msgs = act_with_agents_sdk(
            client=self.client,
            system_prompt=PLANNER_SYSTEM,
            user_text=user_text,
            shared_context=scene_context,
            tools=[],
            dispatch=lambda n, a: "{}",
            memory_texts=[],
            temperature=self.temperature,
            agent_name="Planner",
        )
        return (msgs[0].content if msgs and msgs[0].content else "")
