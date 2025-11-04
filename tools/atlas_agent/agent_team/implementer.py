from __future__ import annotations

from dataclasses import dataclass
import logging
from typing import List, Optional
from .base import LLMClient, AgentMessage
from .agents_sdk import act_with_agents_sdk
import os
from ..codegen_policy import allowed_imports_text, is_codegen_enabled
from ..scene_rpc import SceneClient


IMPLEMENTER_SYSTEM = (
    "You are the Implementer.\n"
    "Translate the selected design into precise tool calls that modify the scene or timeline; avoid guessing.\n\n"
    "Principles:\n"
    "- Do NOT self-detect intent. Use the Task Brief provided by the Supervisor (via shared context) to determine intent: scene, animation, playback, or save/export.\n"
    "- Prefer live discovery for keys/types/options (scene_list_params and scene_get_values); consult scene_animation_concepts, scene_capabilities_summary, or scene_params_handbook on demand when semantics are unclear.\n"
    "- Complete the requested intent in this run. If intent is scene, DO NOT create animations — resolve paths, ensure load, apply scene params, then verify. If intent is animation, ensure animation, set duration, write object keys, plan/write camera keys, then verify. Treat any soft 'confirmation' lines in the plan as non‑blocking assumptions and proceed unless information is truly missing.\n"
    "- Use canonical parameter names discovered from the scene; do not invent option strings.\n"
    "- Camera changes must use typed operators/planning, not raw coordinates. Prefer camera_solve_and_apply (or camera_solve + animation_replace_key_camera). Do not manually segment long motions; the solver returns appropriately sampled keys and we validate visibility.\n"
    "- Validate before applying; verify afterward with key/value listings (scene_get_values / animation_list_keys_* / camera_validate). Keep changes minimal and atomic.\n"
    "- Use the TODO list (checkbox section in context) as task reference for this turn.\n"
    "- Inspector updates TODO status based on verified outcomes; focus on executing the TODOs.\n"
    "- Success criteria (verify explicitly):\n"
    "  • Scene‑only: object(s) present/visible; scene params match the plan.\n"
    "  • Animation: object keys exist at intended times; camera keys span the intended window; camera_validate meets coverage constraints.\n"
    "  • If any check fails, diagnose (id/json_key/time/value) and retry within this run.\n"
)

@dataclass
class Implementer:
    client: LLMClient
    scene: SceneClient
    temperature: float = 0.2
    atlas_dir: str | None = None

    def run(self, *, user_text: str, selected_design: str, shared_context: Optional[str] = None) -> tuple[List[AgentMessage], list[dict]]:
        logger = logging.getLogger("atlas_agent.agents")
        logger.info("[Implementer] Start. user_text=%s", (user_text or ""))
        logger.info("[Implementer] Selected design:\n%s", (selected_design or ""))
        tools, dispatch = None, None
        from .tools_agent import scene_tools_and_dispatcher
        tools, dispatch = scene_tools_and_dispatcher(self.scene, atlas_dir=self.atlas_dir)
        # Wrap dispatch to capture a per-turn ledger of tool calls
        ledger: list[dict] = []
        import json as _json
        # Keep last planned camera value to enable implicit chaining when the model omits base_value.
        last_camera_value: dict | None = None

        def _dispatch_with_ledger(name: str, args_json: str) -> str:
            try:
                # Idempotency guard for key writes: skip if a key at the same time already exists
                import json as _json
                # Parse args to inspect id/time
                try:
                    args = _json.loads(args_json or "{}")
                except Exception:
                    args = {}
                nonlocal last_camera_value
                def _deny(reason: str) -> str:
                    _res = _json.dumps({"ok": False, "error": f"contract_violation: {reason}"})
                    entry = {"tool": name, "args": args, "result": _json.loads(_res)}
                    ledger.append(entry)
                    return _res
                precheck_note = None
                # Camera rotate policy: enforce chaining and segment large angles for stability
                if name == "camera_rotate":
                    try:
                        deg = float(args.get("degrees", 90.0))
                    except Exception:
                        deg = 90.0
                    # Ensure chaining when base_value omitted
                    try:
                        bv = args.get("base_value")
                        if (bv is None or bv == {}) and last_camera_value is not None:
                            args["base_value"] = last_camera_value
                            args_json = _json.dumps(args)
                    except Exception:
                        pass
                    # Segment rotations with magnitude > 120° into <= 90° steps for predictable interpolation
                    if abs(deg) > 120.0:
                        sign = 1.0 if deg >= 0 else -1.0
                        remaining = abs(deg)
                        segments = []
                        # Prefer 90° steps and a smaller remainder
                        while remaining > 90.0 + 1e-6:
                            segments.append(90.0 * sign)
                            remaining -= 90.0
                        if remaining > 1e-6:
                            segments.append(remaining * sign)
                        final_result = None
                        for i, sdeg in enumerate(segments):
                            step_args = dict(args)
                            step_args["degrees"] = float(sdeg)
                            # Always chain from the last_camera_value if available
                            try:
                                if last_camera_value is not None:
                                    step_args["base_value"] = last_camera_value
                            except Exception:
                                pass
                            step_json = _json.dumps(step_args)
                            step_result_raw = dispatch(name, step_json)
                            step_entry = {"tool": name, "args": step_args, "result": None}
                            try:
                                step_result = _json.loads(step_result_raw or "{}")
                            except Exception:
                                step_result = {"raw": step_result_raw}
                            step_entry["result"] = step_result
                            ledger.append(step_entry)
                            final_result = step_result_raw
                            # Update camera chaining base if the tool returned a typed value
                            try:
                                if isinstance(step_result, dict) and step_result.get("ok") and isinstance(step_result.get("value"), dict):
                                    last_camera_value = step_result.get("value")
                            except Exception:
                                pass
                        # Return the final segmented call result (already logged)
                        return final_result or _json.dumps({"ok": False, "error": "camera_rotate segmentation produced no result"})
                # Intercept animation_set_key_param
                if name == "animation_set_key_param":
                    idv = args.get("id")
                    json_key = args.get("json_key")
                    time_v = float(args.get("time", 0.0))
                    try:
                        if idv is not None and json_key:
                            lr = self.scene.list_keys(id=int(idv), json_key=str(json_key), include_values=False)
                            times = [k.time for k in getattr(lr, "keys", [])]
                            if any(abs(time_v - t) < 1e-6 for t in times):
                                _res = _json.dumps({"ok": True, "skipped": "duplicate_time"})
                                entry = {"tool": name, "args": args, "result": _json.loads(_res)}
                                ledger.append(entry)
                                return _res
                    except Exception:
                        pass
                # (removed camera direct set tool references)

                # Optional pre-verification notes (accumulate fields on a single note dict)
                if name == "animation_batch":
                    try:
                        set_keys = (args.get("set_keys") or []) if isinstance(args, dict) else []
                        cache: dict[int, set] = {}
                        verify: list[dict] = []
                        for sk in set_keys:
                            idv = int(sk.get("id", -1))
                            if idv == 0:
                                verify.append({"id": 0, "ok": True})
                                continue
                            jk = sk.get("json_key")
                            if idv not in cache:
                                try:
                                    pl = self.scene.list_params(id=idv)
                                    cache[idv] = {getattr(p, "json_key", "") for p in pl.params}
                                except Exception:
                                    cache[idv] = set()
                            exists = (str(jk) in cache[idv]) if jk else False
                            verify.append({"id": idv, "json_key": jk, "ok": exists})
                        if 'precheck_note' not in locals() or precheck_note is None:
                            precheck_note = {}
                        precheck_note["param_verification"] = verify
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
                if precheck_note is not None:
                    entry["note"] = precheck_note
                ledger.append(entry)
                # Capture last camera value from relevant tool results/calls
                try:
                    if name in ("camera_focus", "camera_point_to", "camera_rotate", "camera_reset_view"):
                        if isinstance(entry.get("result"), dict) and entry["result"].get("ok") and isinstance(entry["result"].get("value"), dict):
                            last_camera_value = entry["result"]["value"]
                    elif name in ("animation_replace_key_camera",):
                        # When writing a camera key with a value, prefer that as the new base
                        if isinstance(entry.get("args"), dict) and isinstance(entry["args"].get("value"), dict):
                            last_camera_value = entry["args"]["value"]
                except Exception:
                    pass
                return result
            except Exception as e:
                ledger.append({"tool": name, "args": args_json, "error": str(e)})
                raise
        # Compose shared context for this run without extra memory_texts injection
        # Keep selected design and reviewer feedback visible by appending to shared_context
        composed_ctx = (shared_context or "")
        if selected_design:
            composed_ctx += ("\n\nSelected design:\n" + selected_design)

        # Build system prompt dynamically; include codegen info only when enabled
        sys_prompt = IMPLEMENTER_SYSTEM + (
            "\n- Tool-call arguments must be STRICT JSON: double-quoted keys/strings, lowercase true/false, null, and no trailing commas.\n"
        )
        if is_codegen_enabled():
            allowed = allowed_imports_text()
            sys_prompt += (
                "Codegen is enabled. Allowed imports: " + allowed + "\n"
                "When helpful for complex calculations, small helper scripts may be used; prefer plan → validate → apply with verification.\n"
            )

        # Single SDK run with high max_turns; rely on SDK's internal tool-calling to complete the workflow.
        pre_len = len(ledger)
        msgs = act_with_agents_sdk(
            client=self.client,
            system_prompt=sys_prompt,
            user_text=user_text,
            shared_context=composed_ctx,
            tools=tools,
            dispatch=_dispatch_with_ledger,
            temperature=self.temperature,
            agent_name="Implementer",
            max_turns=1000,
        )

        # If the LLM provider failed, record it in the ledger for Supervisor loop control
        try:
            for m in msgs or []:
                if getattr(m, "role", "") == "assistant" and isinstance(getattr(m, "content", None), str):
                    c = m.content.strip()
                    if c.startswith("LLM provider error:"):
                        tool_names = []
                        try:
                            for t in tools:
                                fn = t.get("function", {}) if isinstance(t, dict) else {}
                                nm = fn.get("name")
                                if nm:
                                    tool_names.append(nm)
                        except Exception:
                            tool_names = []
                        entry = {
                            "tool": "_provider_error",
                            "error": c,
                            "agent_input": {
                                "user_text": (user_text or "")[:400],
                                "system_prompt_excerpt": (sys_prompt or "")[:400],
                                "shared_context_excerpt": (composed_ctx or "")[:400],
                                "tools": tool_names,
                            },
                        }
                        ledger.append(entry)
                        logger.error("[Implementer] Provider error: %s | agent_input=%s", c, entry.get("agent_input"))
                        break
        except Exception:
            pass
        logger.info("[Implementer] Completed with %d tool call(s)", len(ledger))
        if ledger:
            tool_names = [e.get("tool") for e in ledger]
            logger.info("[Implementer] Tools invoked: %s", ", ".join(tool_names))
        return msgs, ledger
