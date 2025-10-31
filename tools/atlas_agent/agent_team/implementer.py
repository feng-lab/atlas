from __future__ import annotations

from dataclasses import dataclass
import logging
from typing import List, Optional
from .base import LLMClient, AgentMessage
from .agents_sdk import act_with_agents_sdk
from ..scene_rpc import SceneClient


IMPLEMENTER_SYSTEM = (
    "You are the Implementer.\n"
    "Use the provided tools to implement the selected design precisely (no guessing).\n\n"
    "Low‑level tool rules (mandatory):\n"
    "- Enumerate params per target (scene_list_params) and use canonical json_keys and option_names.\n"
    "- Validate candidate values with scene_validate_param_value before writing non‑camera params.\n"
    "- For option parameters, the value MUST be one of option_names.\n"
    "- For numeric/Vec arrays, use correct shapes (e.g., Vec4=[r,g,b,a]).\n"
    "- Use 'Switch' easing for non‑interpolatable parameters.\n"
    "- Use typed writes only: scene_set_param_by_name/scene_set_key_param and scene_set_key_camera.\n"
    "- For any camera work: never guess values. Use fit_candidates + camera_solve for planning, and camera_validate to adjust/verify before and after writing.\n"
    "- If a design mentions camera numbers (eye/center/up/positions), IGNORE them and compute via camera_solve using targets and constraints instead.\n"
    "- Use scene_batch only when you have concrete SetKey entries (non‑empty).\n"
    "- After each write (or batch), call scene_list_keys to verify the expected times exist.\n"
    "- If verification fails, diagnose (wrong json_key? wrong scope? type mismatch?) and retry.\n"
    "- Never claim success until verification passes; keep the textual status minimal.\n"
    "- Ask a concise clarifying question if a required id/param is missing.\n"
    "- For file‑loading requests, prefer scene_ensure_loaded (after resolving paths with fs_* tools if needed). Consider success when the requested file(s) appear in scene_list_objects; timeline keys are not required.\n"
    "- For deletions, use scene_remove_key_param_at_time (with tolerance) or scene_clear_keys.\n"
    "- For replacements, prefer scene_replace_key_param/scene_replace_key_camera which remove nearby keys and set the desired value at time.\n"
)


@dataclass
class Implementer:
    client: LLMClient
    scene: SceneClient
    temperature: float = 0.2
    atlas_dir: str | None = None

    def run(self, *, user_text: str, selected_design: str, reviewer_feedback: str, shared_context: Optional[str] = None, memory_texts: Optional[List[str]] = None) -> tuple[List[AgentMessage], list[dict]]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Implementer] Start. user_text=%s", (user_text or "")[:200])
        logger.info("[Implementer] Selected design:\n%s", (selected_design or "")[:600])
        logger.info("[Implementer] Reviewer feedback:\n%s", (reviewer_feedback or "")[:600])
        tools, dispatch = None, None
        from .tools import scene_tools_and_dispatcher
        tools, dispatch = scene_tools_and_dispatcher(self.scene, atlas_dir=self.atlas_dir)
        # Wrap dispatch to capture a per-turn ledger of tool calls
        ledger: list[dict] = []
        import json as _json
        def _dispatch_with_ledger(name: str, args_json: str) -> str:
            try:
                # Idempotency guard for key writes: skip if a key at the same time already exists
                import json as _json
                # Parse args to inspect scope/time
                try:
                    args = _json.loads(args_json or "{}")
                except Exception:
                    args = {}
                # Intercept scene_set_key_param
                if name == "scene_set_key_param":
                    scope = args.get("scope_object"), args.get("scope_group")
                    json_key = args.get("json_key")
                    time_v = float(args.get("time", 0.0))
                    try:
                        if scope[0] is not None:
                            lr = self.scene.list_keys(scope_object=int(scope[0]), json_key=json_key, include_values=False)
                        elif scope[1] is not None:
                            lr = self.scene.list_keys(scope_group=str(scope[1]), json_key=json_key, include_values=False)
                        else:
                            lr = None
                        if lr is not None:
                            times = [k.time for k in getattr(lr, "keys", [])]
                            if any(abs(time_v - t) < 1e-6 for t in times):
                                # Duplicate; return a synthetic ok result and record ledger
                                _res = _json.dumps({"ok": True, "skipped": "duplicate_time"})
                                entry = {"tool": name, "args": args, "result": _json.loads(_res)}
                                ledger.append(entry)
                                return _res
                    except Exception:
                        pass
                # Intercept scene_set_key_camera
                if name == "scene_set_key_camera":
                    time_v = float(args.get("time", 0.0))
                    try:
                        lr = self.scene.list_keys(scope_camera=True, include_values=False)
                        times = [k.time for k in getattr(lr, "keys", [])]
                        if any(abs(time_v - t) < 1e-6 for t in times):
                            _res = _json.dumps({"ok": True, "skipped": "duplicate_time"})
                            entry = {"tool": name, "args": args, "result": _json.loads(_res)}
                            ledger.append(entry)
                            return _res
                    except Exception:
                        pass

                result = dispatch(name, args_json)
                entry = {
                    "tool": name,
                    "args": None,
                    "result": None,
                }
                entry["args"] = args if isinstance(args, dict) else {"raw": args_json}
                try: entry["result"] = _json.loads(result or "{}")
                except Exception: entry["result"] = {"raw": result}
                ledger.append(entry)
                return result
            except Exception as e:
                ledger.append({"tool": name, "args": args_json, "error": str(e)})
                raise
        memo = memory_texts or []
        # Load compact few-shot examples to ground tool usage (short, plan+facts)
        try:
            from .examples_loader import load_compact_examples
            ex = load_compact_examples(max_count=2)
            if ex:
                memo.append("Few-shot examples:\n" + "\n\n".join(ex))
        except Exception:
            pass
        memo = [
            "Selected design:\n" + (selected_design or "(none)"),
            "Reviewer feedback:\n" + (reviewer_feedback or "(none)"),
        ] + memo
        msgs = act_with_agents_sdk(
            client=self.client,
            system_prompt=IMPLEMENTER_SYSTEM,
            user_text=user_text,
            shared_context=shared_context,
            tools=tools,
            dispatch=_dispatch_with_ledger,
            memory_texts=memo,
            temperature=self.temperature,
            agent_name="Implementer",
            max_turns=1000,
        )
        logger.info("[Implementer] Completed with %d tool call(s)", len(ledger))
        if ledger:
            tool_names = [e.get("tool") for e in ledger]
            logger.info("[Implementer] Tools invoked: %s", ", ".join(tool_names))
        return msgs, ledger
