import json
import logging
import os
from dataclasses import dataclass
from typing import List, Optional

from ..scene_rpc import SceneClient
from .arbiter import Arbiter
from .base import AgentMessage, LLMClient
from .describer import Describer
from .designer import Designer
from .implementer import Implementer
from .inspector import Inspector
from .intent_resolver import IntentResolver
from .reviewer import Reviewer
from .tools_agent import scene_tools_and_dispatcher

SUPERVISOR_SYSTEM = (
    "You are the Supervisor (orchestrator) for an Atlas scene/animation multi‑agent team.\n"
    "Coordinate specialists; you do not call tools directly.\n\n"
    "Intent first:\n"
    "- Classify the request (file load, scene-only, animation, playback, save/export, help). Ask one clarifying question if ambiguous.\n\n"
    "Team protocol:\n"
    "- The Designer proposes 2–3 high‑level options; they should describe typed camera intent, not raw coordinates. The Designer must not assert exact parameter json_keys or option labels.\n"
    "- Before any writes, require a Plan Summary with two views (global timeline and per‑object). Keep it semantic (what changes when); the Implementer resolves canonical json_key names and option labels via live discovery.\n"
    "- Reviewers critique; Arbiter selects or blends into one plan; Implementer executes with verification.\n"
    "- Tool-call arguments must be strict JSON (double‑quoted keys/strings; lowercase true/false/null).\n"
    "- Inspector validates results; Describer summarizes verified facts.\n\n"
    "Success criteria: intended keys exist at the intended times and facts reflect only verified changes.\n"
)


@dataclass
class Supervisor:
    client: LLMClient
    scene: SceneClient
    temperature: float = 0.2
    atlas_dir: str | None = None

    def run_turn(self, user_text: str, *, shared_context: Optional[str] = None, max_steps: int = 24, recent_history: Optional[list[tuple[str, str]]] = None) -> List[AgentMessage]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Supervisor] Turn start. user_text=%s", (user_text or ""))
        # Ensure a session TODO ledger exists (persists across Implementer retries and turns via ChatTeam)
        if not hasattr(self, "_todo_ledger"):
            self._todo_ledger: list[dict] = []
        # 0) Build a small scene context to ground planning
        ctx_parts = []
        try:
            objs = self.scene.list_objects()
            brief = [f"{o.id}:{o.type}:{o.name}:{o.visible}" for o in objs.objects]
            ctx_parts.append("Objects: " + ", ".join(brief))
        except Exception:
            pass
        # Add a compact facts snapshot (camera key times and per‑object param key times)
        try:
            facts0 = self.scene.scene_facts()
            if isinstance(facts0, dict):
                cam_times = (facts0.get("keys", {}).get("camera") or [])
                if cam_times:
                    ctx_parts.append("Camera key times: " + ", ".join(str(float(t)) for t in sorted(cam_times)))
                obj_keys = (facts0.get("keys", {}).get("objects", {}))
                param_summaries: list[str] = []
                for oid, mp in (obj_keys.items() if isinstance(obj_keys, dict) else []):
                    for jk, times in (mp.items() if isinstance(mp, dict) else []):
                        if times:
                            param_summaries.append(f"id={oid} {jk}: times={sorted(times)}")
                if param_summaries:
                    # Avoid silent truncation; include full summary
                    s = " | ".join(param_summaries)
                    ctx_parts.append("Param keys: " + s)
        except Exception:
            pass
        # Keep context simple and factual; avoid hard-coded lane assumptions
        # Include a compact TODO ledger summary in context to keep agents grounded across rounds
        todo_lines: list[str] = []
        try:
            for i, item in enumerate(getattr(self, "_todo_ledger", []) or []):
                try:
                    st = item.get("status", "pending")
                    txt = item.get("text", "")
                    todo_lines.append(f"- [{'x' if st in ('applied','done','finished') else ' '}] {txt}")
                except Exception:
                    continue
        except Exception:
            pass
        todo_block = ("\nTODOs:\n" + "\n".join(todo_lines)) if todo_lines else ""
        ctx = (shared_context + "\n" if shared_context else "") + "\n".join(ctx_parts) + ("\n" + todo_block if todo_block else "")
        # Attach full conversation history to context
        history_text = ""
        try:
            if recent_history:
                history_text = "\n".join([f"{role}: {content}" for role, content in recent_history])
        except Exception:
            history_text = ""
        ctx_with_history = ctx + ("\n\nConversation history:\n" + history_text if history_text else "")

        # 1) Resolve intent into a Task Brief (or ask one clarifying question)
        resolver = IntentResolver(client=self.client, temperature=min(0.4, self.temperature + 0.1))
        # Only the resolver gets full conversation history; downstream agents will receive a compact context.
        brief = resolver.resolve(user_text, scene_context=ctx_with_history)
        try:
            logging.getLogger("atlas_agent.agents").info("[Supervisor] Resolver output:\n%s", (brief or "").strip())
        except Exception:
            pass
        if brief:
            b = brief.strip()
            lower = b.lower()
            if lower.startswith("clarify:"):
                # Defensive guard: require a substantive question; otherwise continue pipeline
                question = b[len("clarify:"):].strip()
                if question and question.endswith("?"):
                    return [AgentMessage(role="assistant", content="CLARIFY: " + question)]
            # else: not a valid clarify → continue

        # Build a compact context for downstream agents: facts + Task Brief (no conversation history)
        ctx_for_agents = ctx
        if brief and brief.strip().lower().startswith("task brief:"):
            ctx_for_agents = ctx_for_agents + "\n\n" + brief.strip()

        # 2) Supervisor requests 2–3 high‑level designs (aligned to Task Brief when present)
        designer = Designer(client=self.client, temperature=min(0.4, self.temperature + 0.1))
        options = designer.propose(user_text, scene_context=ctx_for_agents)
        logger.info("[Supervisor] Received %d design option(s)", len(options))
        # 3) Multiple reviewers critique the options
        rev1 = Reviewer(client=self.client, temperature=min(0.5, self.temperature + 0.2), name="Reviewer A")
        rev2 = Reviewer(client=self.client, temperature=min(0.6, self.temperature + 0.3), name="Reviewer B")
        fb1 = rev1.review(user_text, scene_context=ctx_for_agents, options=options)
        fb2 = rev2.review(user_text, scene_context=ctx_for_agents, options=options)
        logger.info("[Supervisor] Reviewers completed feedback")

        # 4) Arbiter: choose/blend options based on reviewer feedback
        arbiter = Arbiter(client=self.client, temperature=min(0.4, self.temperature + 0.1))
        idx, merged = arbiter.decide(user_text=user_text, scene_context=ctx_for_agents, options=options, feedbacks=[fb1, fb2])
        selected = merged or (options[idx - 1] if options else "")
        logger.info("[Supervisor] Arbiter selected option %d", idx)
        # Initialize/refresh TODO ledger from merged plan's TODO section (checkbox lines)
        try:
            def _parse_todos(text: str) -> list[dict]:
                out: list[dict] = []
                for ln in (text or "").splitlines():
                    s = ln.strip()
                    if s.startswith("- [") and "]" in s:
                        # Patterns: - [ ] Task or - [x] Task
                        mark = s[3]
                        rest = s[s.find("]")+1:].strip()
                        status = "applied" if mark.lower() == 'x' else "pending"
                        if rest:
                            out.append({"text": rest, "status": status})
                return out
            todos = _parse_todos(selected)
            if todos:
                # Merge without duplicating by text
                existing = { (it.get("text") or "").strip(): it for it in (getattr(self, "_todo_ledger", []) or []) }
                for t in todos:
                    key = (t.get("text") or "").strip()
                    if not key:
                        continue
                    if key in existing:
                        # Update status only if moving forward
                        if existing[key].get("status") != "applied" and t.get("status") == "applied":
                            existing[key]["status"] = "applied"
                    else:
                        existing[key] = {"text": key, "status": t.get("status", "pending")}
                self._todo_ledger = list(existing.values())
        except Exception:
            pass
        implementer = Implementer(client=self.client, scene=self.scene, temperature=self.temperature, atlas_dir=self.atlas_dir)
        inspector = Inspector(client=self.client, temperature=self.temperature)
        describer = Describer(client=self.client, temperature=self.temperature)

        # Iterate until Inspector is satisfied, or break after repeated no‑progress rounds
        messages: List[AgentMessage] = []
        loop = 0
        give_up_reason: str | None = None
        full_ledger: list[dict] = []
        no_change_rounds = 0
        while True:
            loop += 1
            logger.info("[Supervisor] Implementer loop iteration %d", loop)
            pre = self.scene.scene_facts(include_values=True, include_scene_values=True)
            # Downstream agents should not need reviewer prose; pass only the merged plan
            msgs, ledger = implementer.run(
                user_text=user_text,
                selected_design=selected,
                shared_context=ctx_for_agents,
            )
            messages.extend(msgs)
            full_ledger.extend(ledger)
            post = self.scene.scene_facts(include_values=True, include_scene_values=True)
            changed = (pre != post)
            logger.info("[Supervisor] Snapshot changed=%s", changed)
            # If Implementer reported a blocked reason, record it for the final outcome message
            blocked_text = None
            try:
                for e in ledger:
                    if e.get("tool") == "report_blocked":
                        try:
                            res = e.get("result") or {}
                            r = res.get("reason") or (e.get("args") or {}).get("reason")
                            d = res.get("details") or (e.get("args") or {}).get("details")
                            s = res.get("suggestion") or (e.get("args") or {}).get("suggestion")
                            msg = (r or "blocked")
                            if d:
                                msg += f": {d}"
                            if s:
                                msg += f" | suggestion: {s}"
                            blocked_text = msg
                        except Exception:
                            blocked_text = "blocked"
                        break
            except Exception:
                blocked_text = None
            # If camera keys exist, run typed validation to gate satisfaction
            camera_times = (post.get("keys", {}).get("camera") or []) if isinstance(post, dict) else []
            cam_validation = None
            if camera_times:
                try:
                    lr = self.scene.list_keys(id=0, include_values=True)
                    # Build times/values lists (ensure matching order to server's expected input)
                    times: list[float] = []
                    values: list[dict] = []
                    for k in getattr(lr, "keys", []):
                        try:
                            t = float(getattr(k, "time", 0.0))
                            v = getattr(k, "value_json", "")
                            val = json.loads(v) if v else {}
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
            # Optional single preview screenshot for qualitative checks
            preview_path = None
            try:
                allow = (os.environ.get("ATLAS_AGENT_ALLOW_SCREENSHOTS", "").strip().lower() in ("1", "true", "yes"))
                if allow:
                    # Choose a preview time: current timeline seconds if available; otherwise first camera key or 0
                    try:
                        ts = self.scene.get_time()
                        tsec = float(getattr(ts, "seconds", 0.0) or 0.0)
                    except Exception:
                        tsec = 0.0
                    cam_times = (post.get("keys", {}).get("camera") or []) if isinstance(post, dict) else []
                    if not tsec and cam_times:
                        tsec = float(cam_times[len(cam_times)//2])
                    # Invoke preview tool via dispatcher
                    _tools, _dispatch = scene_tools_and_dispatcher(self.scene, atlas_dir=self.atlas_dir)
                    res = _dispatch(
                        "animation_render_preview",
                        json.dumps({"time": tsec, "width": 512, "height": 512}),
                    )
                    try:
                        j = json.loads(res or "{}")
                        if j.get("ok") and j.get("path"):
                            preview_path = str(j.get("path"))
                    except Exception:
                        preview_path = None
            except Exception:
                preview_path = None

            # No digest logging; Inspector will use full facts below

            # Prefer read-only live facts over brittle digests for verification
            facts_for_inspector = post if isinstance(post, dict) else {}
            satisfied, fb, todo_update_text = inspector.decide(
                user_text=user_text,
                scene_context=ctx_for_agents,
                plan_text=selected,
                facts=facts_for_inspector,
                preview_image_path=preview_path,
            )
            # If something was blocked, avoid infinite retries: exit this loop after surfacing results once
            if blocked_text:
                logger.info("[Supervisor] Blocked info present; exiting after current iteration")
                satisfied = True
            if not satisfied:
                try:
                    logger.info(
                        "[Inspector] Not satisfied; dumping facts: %s",
                        json.dumps(facts_for_inspector),
                    )
                except Exception:
                    pass
            # Merge Inspector-provided TODO updates (checkbox lines) into session ledger
            try:
                if isinstance(todo_update_text, str) and todo_update_text.strip():
                    def _parse_todos(text: str) -> list[dict]:
                        out: list[dict] = []
                        for ln in (text or "").splitlines():
                            s = ln.strip()
                            if s.startswith("- [") and "]" in s:
                                mark = s[3]
                                rest = s[s.find("]")+1:].strip()
                                status = "applied" if mark.lower() == 'x' else "pending"
                                if rest:
                                    out.append({"text": rest, "status": status})
                        return out
                    updated = _parse_todos(todo_update_text)
                    if updated:
                        self._todo_ledger = updated
            except Exception:
                pass
            # Hard gate: if camera validation failed, do not allow satisfied
            if cam_validation and not bool(cam_validation.get("ok", False)):
                satisfied = False
            logger.info("[Supervisor] Inspector satisfied=%s", satisfied)
            # Exit only when satisfied; otherwise keep iterating while there is progress.
            if satisfied:
                break
            # Feed inspector feedback back into the loop
            selected = selected + "\n\nInspector feedback (rework):\n" + (fb or "")
            # If no observed timeline change and no effective write calls, stop to avoid infinite loops
            WRITE_TOOLS = {
                "scene_apply",
                "scene_ensure_loaded",
                "animation_set_key_param",
                "animation_replace_key_param",
                "animation_remove_key_param_at_time",
                "animation_replace_key_param_at_times",
                "animation_remove_key",
                "animation_replace_key_camera",
                "animation_clear_keys",
                "animation_set_duration",
                "animation_batch",
            }
            effective_calls = [
                e for e in ledger
                if e.get("tool") in WRITE_TOOLS and not (e.get("result", {}).get("skipped")) and bool(e.get("result", {}).get("ok", True))
            ]
            # Detect provider errors (do not count the round against progress; allow retry)
            provider_error = any(e.get("tool") == "_provider_error" for e in ledger)
            if (not changed) and (not effective_calls) and (not provider_error):
                no_change_rounds += 1
            else:
                no_change_rounds = 0
            if no_change_rounds >= 4:
                give_up_reason = (
                    "No effective timeline/scene changes after repeated attempts. "
                    "Likely missing capability, invalid parameter/option, or plan requires unavailable tools."
                )
                logger.info("[Supervisor] Breaking loop: no changes after %d rounds", no_change_rounds)
                break
            # No hard cap by default (can add external intervention if needed)

        # 4) Description using full scene facts so Describer can summarize comprehensively
        try:
            full_facts = self.scene.scene_facts(
                include_values=True, include_scene_values=True
            )
        except Exception:
            full_facts = {}
        # If we gave up due to no progress, surface a concise outcome message to the user
        if give_up_reason or blocked_text:
            try:
                # Extract top 2 error hints from ledger
                hints: list[str] = []
                for e in full_ledger[-30:]:
                    try:
                        if isinstance(e.get("result"), dict) and not e["result"].get("ok", True):
                            err = e["result"].get("error") or e["result"].get("reason")
                            if err:
                                hints.append(f"{e.get('tool')}: {err}")
                        elif e.get("error"):
                            hints.append(f"{e.get('tool')}: {e.get('error')}")
                    except Exception:
                        continue
                HMAX = int(os.environ.get("ATLAS_AGENT_OUTCOME_HINTS_MAX", "2"))
                more = 0
                if HMAX >= 0 and len(hints) > HMAX:
                    more = len(hints) - HMAX
                    hints = hints[:HMAX]
                reason_base = give_up_reason if give_up_reason else ""
                if blocked_text:
                    reason_base = (reason_base + ("; " if reason_base else "") + f"Blocked: {blocked_text}")
                tail = "; ".join(hints) + (f" (+{more} more)" if more else "") if hints else ""
                outcome = reason_base + (" Hints: " + tail if tail else "")
                messages.append(AgentMessage(role="assistant", content="Outcome: " + outcome))
            except Exception:
                pass

        desc = describer.describe(
            user_text=user_text, facts=full_facts, conversation=history_text
        )
        logger.info("[Supervisor] Turn complete. desc=%s", desc)

        messages.insert(0, AgentMessage(role="assistant", content="Design options:\n" + ("\n\n".join(options) or "(none)")))
        messages.insert(1, AgentMessage(role="assistant", content="Reviewer feedback A:\n" + (fb1 or "")))
        messages.insert(2, AgentMessage(role="assistant", content="Reviewer feedback B:\n" + (fb2 or "")))
        messages.append(AgentMessage(role="assistant", content="Description (facts‑based):\n" + (desc or "")))
        # Append TODO ledger snapshot for transparency and cross‑turn memory
        try:
            todo_lines: list[str] = []
            for it in getattr(self, "_todo_ledger", []) or []:
                st = it.get("status", "pending")
                mark = 'x' if st in ('applied','done','finished') else ' '
                txt = it.get("text", "")
                todo_lines.append(f"- [{mark}] {txt}")
            if todo_lines:
                messages.append(AgentMessage(role="assistant", content="TODO Ledger (session):\n" + "\n".join(todo_lines)))
        except Exception:
            pass
        # Log compact ledger for auditability (do not append into assistant chat content)
        try:
            flat_lines: list[str] = []
            for e in full_ledger:
                tool = e.get('tool')
                if 'result' in e:
                    line = f"- {tool}: args={json.dumps(e.get('args'))} result={json.dumps(e.get('result'))}"
                else:
                    parts = [f"- {tool}:"]
                    if e.get('args') is not None:
                        parts.append(f"args={json.dumps(e.get('args'))}")
                    if e.get('error') is not None:
                        parts.append(f"error={json.dumps(e.get('error'))}")
                    ai = e.get('agent_input') or {}
                    if isinstance(ai, dict) and ai:
                        try:
                            ui = (ai.get('user_text') or '')
                            sp = (ai.get('system_prompt_excerpt') or '')
                            sc = (ai.get('shared_context_excerpt') or '')
                            tools_list = ai.get('tools') or []
                            def trunc(s):
                                return s

                            parts.append(
                                f"agent_input={{user_text={json.dumps(trunc(ui))}, system_excerpt={json.dumps(trunc(sp))}, context_excerpt={json.dumps(trunc(sc))}, tools={json.dumps(tools_list)} }}"
                            )
                        except Exception:
                            parts.append(f"agent_input={json.dumps(ai)}")
                    line = " ".join(parts)
                flat_lines.append(line)
            if flat_lines:
                logger.info("[Supervisor] %s", "Ledger (tools invoked this turn):\n" + "\n".join(flat_lines))
        except Exception:
            pass
        # Expose the raw tool ledger for the chat layer to make informed
        # post-turn verification/guard decisions (e.g., only warn about
        # "no changes" when a write was actually attempted).
        try:
            self._last_ledger = list(full_ledger)
        except Exception:
            self._last_ledger = []
        return messages

    # Expose TODO ledger for the ChatTeam to persist across turns
    def get_todo_ledger(self) -> list[dict]:
        try:
            return list(getattr(self, "_todo_ledger", []) or [])
        except Exception:
            return []
