from __future__ import annotations

import json
from dataclasses import dataclass
from typing import List, Optional

from .base import BaseAgent, LLMClient, AgentMessage
import logging
from .tools import scene_tools_and_dispatcher
from .planner import Planner
from .inspector import Inspector
from .designer import Designer
from .reviewer import Reviewer
from .implementer import Implementer
from .describer import Describer
from .agents_sdk import act_with_agents_sdk
from ..scene_rpc import SceneClient


SUPERVISOR_SYSTEM = (
    "You are the Supervisor (orchestrator) for an Atlas animation multi‑agent team.\n"
    "Your job is to coordinate specialists — you do not call tools directly.\n\n"
    "Team protocol:\n"
    "- Designer proposes 2–3 distinct high‑level designs based on the user request and scene context.\n"
    "- Reviewer A and Reviewer B critique the options (feasibility, clarity, alignment).\n"
    "- Arbiter selects the best option or blends two into a single merged plan.\n"
    "- Implementer uses typed tools to implement the merged plan (no guessing): enumerate params, write SetKey, verify with scene_list_keys.\n"
    "- Inspector validates the result; if gaps remain, feed feedback back to Implementer and iterate until satisfied.\n"
    "- Describer produces a concise, facts‑only summary using the verified keys/times.\n\n"
    "Success criteria:\n"
    "- Keys exist at the intended times for the intended targets (verified by scene_list_keys).\n"
    "- The timeline snapshot changes in the expected places.\n"
    "- The final description reflects only facts (do not claim anything not present).\n\n"
    "File loading policy (when requested by user):\n"
    "- If objects are already present (scene_list_objects), do not reload.\n"
    "- Otherwise, have Implementer/Tools resolve and validate paths (system_info, fs_expand_paths, fs_check_paths, scene_ensure_loaded).\n\n"
    "Constraints:\n"
    "- Keep exports out of RPC; use Save when requested.\n"
    "- Never summarize planned actions as completed until verification passes.\n"
)


@dataclass
class Supervisor:
    client: LLMClient
    scene: SceneClient
    temperature: float = 0.2
    atlas_dir: str | None = None

    def run_turn(self, user_text: str, *, shared_context: Optional[str] = None, max_steps: int = 24, recent_history: Optional[list[tuple[str, str]]] = None) -> List[AgentMessage]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Supervisor] Turn start. user_text=%s", (user_text or "")[:200])
        # 0) Build a small scene context to ground planning
        ctx_parts = []
        try:
            objs = self.scene.list_objects()
            brief = [f"{o.id}:{o.type}:{o.name}:{o.visible}" for o in objs.objects]
            ctx_parts.append("Objects: " + ", ".join(brief))
        except Exception:
            pass
        ctx = (shared_context + "\n" if shared_context else "") + "\n".join(ctx_parts)

        # 1) Supervisor requests 2–3 high‑level designs
        designer = Designer(client=self.client, temperature=min(0.4, self.temperature + 0.1))
        options = designer.propose(user_text, scene_context=ctx)
        logger.info("[Supervisor] Received %d design option(s)", len(options))
        # 2) Multiple reviewers critique the options
        rev1 = Reviewer(client=self.client, temperature=min(0.5, self.temperature + 0.2), name="Reviewer A")
        rev2 = Reviewer(client=self.client, temperature=min(0.6, self.temperature + 0.3), name="Reviewer B")
        fb1 = rev1.review(user_text, scene_context=ctx, options=options)
        fb2 = rev2.review(user_text, scene_context=ctx, options=options)
        logger.info("[Supervisor] Reviewers completed feedback")

        # 3) Arbiter: choose/bend options based on reviewer feedback
        from .arbiter import Arbiter
        arbiter = Arbiter(client=self.client, temperature=min(0.4, self.temperature + 0.1))
        idx, merged = arbiter.decide(user_text=user_text, scene_context=ctx, options=options, feedbacks=[fb1, fb2])
        selected = merged or (options[idx - 1] if options else "")
        logger.info("[Supervisor] Arbiter selected option %d", idx)
        implementer = Implementer(client=self.client, scene=self.scene, temperature=self.temperature)
        inspector = Inspector(client=self.client, temperature=self.temperature)
        describer = Describer(client=self.client, temperature=self.temperature)

        # Keep iterating until there is a change in facts or inspector is satisfied
        messages: List[AgentMessage] = []
        loop = 0
        full_ledger: list[dict] = []
        no_change_rounds = 0
        while True:
            loop += 1
            logger.info("[Supervisor] Implementer loop iteration %d", loop)
            pre = self.scene.scene_facts()
            msgs, ledger = implementer.run(user_text=user_text, selected_design=selected, reviewer_feedback=(fb1 + "\n\n" + fb2), shared_context=ctx)
            messages.extend(msgs)
            full_ledger.extend(ledger)
            post = self.scene.scene_facts()
            changed = (pre != post)
            logger.info("[Supervisor] Snapshot changed=%s", changed)
            # If camera keys exist, run typed validation to gate satisfaction
            camera_times = (post.get("keys", {}).get("camera") or []) if isinstance(post, dict) else []
            cam_validation = None
            if camera_times:
                try:
                    lr = self.scene.list_keys(scope_camera=True, include_values=True)
                    # Build times/values lists (ensure matching order to server's expected input)
                    times: list[float] = []
                    values: list[dict] = []
                    for k in getattr(lr, "keys", []):
                        try:
                            t = float(getattr(k, "time", 0.0))
                            v = getattr(k, "value_json", "")
                            import json as _json
                            val = _json.loads(v) if v else {}
                            times.append(t)
                            values.append(val)
                        except Exception:
                            continue
                    ids = []
                    try:
                        ids = self.scene.fit_candidates()
                    except Exception:
                        ids = []
                    cam_validation = self.scene.camera_validate(ids=ids, times=times, values=values, constraints={"keep_visible": True, "min_coverage": 0.95}, policies={"adjust_fov": False, "adjust_distance": False, "adjust_clipping": False})
                    # Attach summary into facts for downstream describer
                    if isinstance(post, dict):
                        post = dict(post)
                        post["camera_validation"] = cam_validation
                except Exception:
                    pass
            # Ask Inspector for decision with facts
            satisfied, fb = inspector.decide(user_text=user_text, scene_context=ctx, plan_text=selected, facts=post)
            # Hard gate: if camera validation failed, do not allow satisfied
            if cam_validation and not bool(cam_validation.get("ok", False)):
                satisfied = False
            logger.info("[Supervisor] Inspector satisfied=%s", satisfied)
            if changed or satisfied:
                break
            # Feed inspector feedback back into the loop
            selected = selected + "\n\nInspector feedback (rework):\n" + (fb or "")
            # If no change and no tool calls (or only duplicates skipped), stop to avoid infinite loops
            effective_calls = [e for e in ledger if e.get("tool", "").startswith("scene_set_key") and not e.get("result", {}).get("skipped")]
            if not effective_calls:
                no_change_rounds += 1
            else:
                no_change_rounds = 0
            if no_change_rounds >= 2:
                logger.info("[Supervisor] Breaking loop: no changes after %d rounds", no_change_rounds)
                break
            # No hard cap by default (can add external intervention if needed)

        # 4) Description based on facts only
        facts = self.scene.scene_facts()
        desc = describer.describe(user_text=user_text, facts=facts)
        logger.info("[Supervisor] Turn complete. Facts camera=%s, objects=%d", facts.get("camera"), len(facts.get("objects", {})))

        messages.insert(0, AgentMessage(role="assistant", content="Design options:\n" + ("\n\n".join(options) or "(none)")))
        messages.insert(1, AgentMessage(role="assistant", content="Reviewer feedback A:\n" + (fb1 or "")))
        messages.insert(2, AgentMessage(role="assistant", content="Reviewer feedback B:\n" + (fb2 or "")))
        messages.append(AgentMessage(role="assistant", content="Description (facts‑based):\n" + (desc or "")))
        # Append compact ledger for auditability
        import json as _json
        try:
            flat = [
                f"- {e.get('tool')}: args={_json.dumps(e.get('args'))} result={_json.dumps(e.get('result'))}" if 'result' in e else f"- {e.get('tool')}: args={e.get('args')} error={e.get('error')}"
                for e in full_ledger
            ]
            if flat:
                messages.append(AgentMessage(role="assistant", content="Ledger (tools invoked this turn):\n" + "\n".join(flat)))
        except Exception:
            pass
        return messages
