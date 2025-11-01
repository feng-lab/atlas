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
    "Intent guard (mandatory):\n"
    "- Detect intent: {file load/import, static scene management (no time), animation creation (timeline), preview/playback, save/export}.\n"
    "- If the request is ONLY file load/import or static scene management, DO NOT create animations or write timeline keys. Use scene tools (fs_*, scene_ensure_loaded, scene_validate_apply → scene_apply).\n"
    "- If ambiguous, ask ONE concise clarifying question before proceeding.\n\n"
    "Plan Summary (mandatory before any tool call):\n"
    "- First produce a concise Plan Summary with TWO synchronized views:\n"
    "  1) Global timeline view: list entries {time, target(camera|object|group), json_key?, value, easing?}.\n"
    "  2) Per‑object view: for each object/group, list {json_key, time, value, easing?}.\n"
    "- Use canonical json_key names (from scene_list_params/scene_capabilities).\n"
    "- Camera steps must be typed (use camera tools), no raw coordinates.\n\n"
    "Action Plan (multi‑round allowed):\n"
    "- Propose a numbered plan and execute step by step. Typical flow for scene arrangement:\n"
    "  1) Discover facts: scene_facts_summary; list objects; filter targets.\n"
    "  2) Enumerate parameters: scene_list_params for each target (confirm canonical json_keys/types); consult scene_capabilities/scene_schema if needed.\n"
    "  3) Derive sizes: scene_bbox(ids) and compute world‑space cell sizes (≥ bbox extent × (1+margin)).\n"
    "  4) Build candidate set_params (correct shapes via list_params/schema).\n"
    "  5) Dry‑run: scene_validate_apply and print a compact summary of selections (id→json_key) and sample values. If any not ok, stop and fix.\n"
    "  6) Apply: scene_apply(set_params), then verify with scene_get_values.\n"
    "- It is acceptable to make multiple tool rounds to gather info and refine the plan.\n\n"
    "Low‑level tool rules (mandatory):\n"
    "- Enumerate params per target (scene_list_params) and use canonical json_keys and option_names.\n"
    "- Validate candidate values with scene_validate_param_value before writing non‑camera params.\n"
    "- For option parameters, the value MUST be one of option_names.\n"
    "- For numeric/Vec arrays, use correct shapes (e.g., Vec4=[r,g,b,a]).\n"
    "- Use 'Switch' easing for non‑interpolatable parameters.\n"
    "- For scene (stateless) edits like arrangement or base styles, prefer scene_validate_apply → scene_apply (no time/easing).\n"
    "- For arrangements (grid/pack), compute world‑space positions using bbox sizes: use scene_bbox(ids) to derive width/height per object; choose cell_w/cell_h ≥ max per‑object extent × (1+margin). Avoid tiny unitless numbers like 0.1 that have no visual effect.\n"
    "- Discover the transform parameter per id via scene_guess_transform_key(ids) (e.g., 'Coord Transform 3DTransform' for Image). For 3DTransform, write the 'Translation' field with [x,y,z] world units.\n"
    "- For animation keys, use typed writes only: scene_set_param_by_name/scene_set_key_param and scene_set_key_camera.\n"
    "- For any camera work: never guess values. Use fit_candidates + camera_solve for planning, and camera_validate to adjust/verify before and after writing.\n"
    "- If a design mentions camera numbers (eye/center/up/positions), IGNORE them and compute via camera_solve using targets and constraints instead.\n"
    "- Prefer UI-parity camera operators for repeatable paths: camera_focus / camera_point_to / camera_rotate / camera_reset_view.\n"
    "  Example (360° orbit in 10s around Mesh ID X):\n"
    "    1) v0 = camera_focus(ids=[X]); scene_replace_key_camera(time=0.0, value=v0)\n"
    "    2) v1 = camera_rotate(op='AZIMUTH', degrees=90, base_value=v0); scene_replace_key_camera(time=2.5, value=v1)\n"
    "    3) v2 = camera_rotate(op='AZIMUTH', degrees=90, base_value=v1); scene_replace_key_camera(time=5.0, value=v2)\n"
    "    4) v3 = camera_rotate(op='AZIMUTH', degrees=90, base_value=v2); scene_replace_key_camera(time=7.5, value=v3)\n"
    "    5) v4 = camera_rotate(op='AZIMUTH', degrees=90, base_value=v3); scene_replace_key_camera(time=10.0, value=v4)\n"
    "  Then call camera_validate(ids=[X], times=[0,2.5,5,7.5,10], values=[v0..v4], constraints={keep_visible:true,min_coverage:0.95}, policies={adjust_*:false}).\n"
    "- Use scene_batch only when you have concrete SetKey entries (non‑empty).\n"
    "- For scene_apply, verify with scene_get_values after apply.\n"
    "- If scene_validate_apply reports ok=false, stop and print the per‑item reasons; do not proceed to scene_apply.\n"
    "- After each write (or batch), call scene_list_keys to verify the expected times exist.\n"
    "- After scene_apply, call scene_get_values to verify changes.\n"
    "- If verification fails, diagnose (wrong json_key? wrong scope? type mismatch?) and retry.\n"
    "- Never claim success until verification passes; keep the textual status minimal.\n"
    "- Ask a concise clarifying question if a required id/param is missing.\n"
    "- For file‑loading requests, prefer scene_ensure_loaded (after resolving paths with fs_* tools if needed). If fs_check_paths shows missing paths, try fs_glob for patterns, then fs_resolve_path and repo_search before asking. Consider success when the requested file(s) appear in scene_list_objects; timeline keys are not required.\n"
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
        from .tools_agent import scene_tools_and_dispatcher
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
