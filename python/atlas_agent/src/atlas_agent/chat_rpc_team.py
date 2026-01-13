from __future__ import annotations

import base64
import json
import logging
import mimetypes
import os
import re
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

from .agent_team.base import LLMClient
from .agent_team.intent_resolver import INTENT_RESOLVER_SYSTEM
from .agent_team.tools_agent import scene_tools_and_dispatcher
from .capabilities_prompt import build_atlas_agent_primer, build_capabilities_prompt
from .responses_tool_loop import ToolLoopCallbacks, run_responses_tool_loop
from .discovery import discover_schema_dir
from .scene_rpc import SceneClient
from .session_store import SessionStore


# Internal runtime policy (intentionally not user-configurable).
#
# Rationale: The agent needs stable behavior across sessions and machines; we
# avoid hidden env/flag tuning knobs. When these need changes, we update the
# implementation (and keep the on-disk session format stable).
SESSION_MEMORY_COMPACTION_MODE = "llm"  # "llm" | "heuristic" | "off"
SESSION_MAX_RECENT_MESSAGES = 24

AUTO_RETRIEVE_MODE = "auto"  # "off" | "auto" | "always"
AUTO_RETRIEVE_MAX_SNIPPETS = 6
AUTO_RETRIEVE_MAX_CHARS = 280
AUTO_RETRIEVE_RECENT_WRITE_EVENTS = 8


ATLAS_STATE_MUTATION_TOOLS: set[str] = {
    # Scene writes
    "scene_apply",
    "scene_camera_fit",
    "scene_camera_apply",
    "scene_load_files",
    "scene_ensure_loaded",
    "scene_smart_load",
    "scene_set_visibility",
    "scene_make_alias",
    "scene_cut_set_box",
    "scene_cut_clear",
    # Animation writes (timeline / playback / export)
    "animation_set_param_by_name",
    "animation_ensure_animation",
    "animation_set_duration",
    "animation_set_key_param",
    "animation_replace_key_param",
    "animation_remove_key_param_at_time",
    "animation_replace_key_param_at_times",
    "animation_remove_key",
    "animation_replace_key_camera",
    "animation_clear_keys",
    "animation_clear_keys_range",
    "animation_shift_keys_range",
    "animation_scale_keys_range",
    "animation_duplicate_keys_range",
    "animation_batch",
    "animation_camera_solve_and_apply",
    "animation_set_time",
}

ATLAS_OUTPUT_TOOLS: set[str] = {
    "scene_save_scene",
    "animation_save_animation",
    "animation_export_video",
    "animation_render_preview",
}

# Output tools that are allowed in verification (best-effort; may be disabled by env).
ATLAS_VERIFICATION_OUTPUT_TOOLS: set[str] = {
    "animation_render_preview",
}

SESSION_MUTATION_TOOLS: set[str] = {
    "update_plan",
    "verification_set_requirements",
    "verification_record",
}

CODEGEN_TOOLS: set[str] = {
    "python_write_and_run",
}

ATLAS_SHARED_SYSTEM_RULES = (
    "You are Atlas Agent, a tool-using assistant.\n"
    "You operate the Atlas desktop app through a local gRPC scene server.\n\n"
    "Core rules:\n"
    "- Use tools to inspect live state before writing; do not guess IDs, json_keys, or option strings.\n"
    "- Any mention of time/duration implies timeline edits via animation_* tools; otherwise prefer scene_* tools.\n"
    "- Minimal mutation policy: change ONLY what is required to fulfill the Task Brief.\n"
    "- Proceed-first: avoid confirmation questions. If defaults are needed, state assumptions and continue.\n"
    "- Keep plans/descriptions semantic; do not assert exact parameter json_keys or option label strings.\n"
    "- Camera: prefer typed intent (FIT/ORBIT/DOLLY/STATIC) via camera_* tools; do not invent raw coordinates.\n"
    "- File paths: treat user paths as hints; resolve/verify with fs_* tools before loading.\n"
    "- Maintain an explicit plan via the update_plan tool (4–7 short steps). Exactly one step may be in_progress.\n"
    "- Verify changes after applying: scene_get_values / animation_list_keys / animation_camera_validate, etc.\n"
    "- When blocked on missing user intent/input, call report_blocked and ask a single focused question.\n"
    "- For unclear workflow/semantics, consult docs via docs_search and docs_read (AGENTS_GUIDE.md, SCENE_SERVER.md).\n"
    "- Tool-call arguments must be STRICT JSON (double-quoted keys/strings; lowercase true/false/null; no trailing commas).\n\n"
    "Reasoning summary style:\n"
    "- The UI will show your reasoning summary live. Keep it high-level and safe (no hidden chain-of-thought).\n"
    "- Write in first person and future-looking (\"I will…\"), include risks/trade-offs and a verification strategy.\n\n"
    "Output expectations:\n"
    "- Final answer: concise, actionable, and evidence-based. State what you changed and how you verified.\n"
    "- If something cannot be verified via tools, say so and record it as a visual/human check (do not speculate).\n"
)

ATLAS_PLANNER_SYSTEM_PROMPT = (
    "PHASE: Planner\n"
    "You must NOT change the Atlas scene/timeline in this phase.\n"
    "Allowed actions: read-only inspection tools, docs lookup, and update_plan.\n\n"
    "Format:\n"
    "- If genuinely blocked by missing user intent, output exactly: CLARIFY: <one focused question>\n"
    "- Otherwise: update the plan via update_plan, then include a short TASK BRIEF so execution can proceed without extra history.\n\n"
    "Verification requirements:\n"
    "- After update_plan, call verification_set_requirements for each plan step_id.\n"
    "- Use policy.all_of groups to express multi-mode verification. Common patterns:\n"
    "  • Tool-only: all_of=[{any_of:[\"tool\"], description:\"...\"}]\n"
    "  • Tool AND (Visual OR Human): all_of=[{any_of:[\"tool\"]},{any_of:[\"visual\",\"human\"]}]\n"
    "- Prefer a single plan step with a multi-mode verification policy rather than splitting into multiple steps unless it improves clarity.\n\n"
    + ATLAS_SHARED_SYSTEM_RULES
    + "\n\nTask brief format (reference):\n"
    + INTENT_RESOLVER_SYSTEM
)

ATLAS_EXECUTOR_SYSTEM_PROMPT = (
    "PHASE: Executor\n"
    "Execute the plan by calling tools. Prefer discovery; do not guess.\n\n"
    + ATLAS_SHARED_SYSTEM_RULES
    + "\n\nExecutor playbook:\n"
    + "\n".join(
        [
            "- Treat the current plan as the source of truth; execute it step-by-step. If there is no plan yet, create one via update_plan.",
            "- Prefer live discovery for ids/json_keys/options: scene_list_objects → scene_list_params(id) → scene_get_values(id,json_keys). Do not guess.",
            "- For unclear semantics/workflows, consult docs via docs_search/docs_read (SCENE_SERVER.md, AGENTS_GUIDE.md, USER_GUIDE.md).",
            "- Long sessions: if the user references prior decisions, use session_get_memory/session_get_plan or session_search_transcript for exact quotes.",
            "- File paths: treat user paths as hints. Resolve with fs_hint_resolve/fs_resolve_path, verify with fs_check_paths, then load via scene_load_files/scene_ensure_loaded.",
            "- Scene vs timeline contract: any mention of time/duration implies animation_* tools. Never include time/easing in scene_apply.",
            "- Scene-only intent: do not write animation keys. Use scene_apply/scene_set_visibility/scene_camera_apply and verify with scene_get_values.",
            "- Animation intent: animation_ensure_animation → animation_set_duration → write keys (animation_batch / animation_set_key_param / animation_replace_key_param) and camera keys (animation_camera_solve_and_apply / animation_replace_key_camera).",
            "- Camera workflow: use camera_* producers (camera_focus / camera_point_to / camera_rotate / camera_reset_view) to compute typed camera values, then apply via scene_camera_apply or animation_* camera tools as appropriate.",
            "- Validate before committing where possible (scene_validate_params; animation_camera_validate). Verify after writing (animation_list_keys, animation_camera_validate, scene_get_values).",
            "- Large-file handling: prefer fs_tail_lines/fs_search_text; if you must scan, iterate fs_read_text in windows until EOF (no silent truncation).",
            "- Verification ledger: when a step has specific requirements, use verification_record(mode=tool|visual|human) to attach evidence and update_plan to advance statuses.",
            "- Visual checks: use animation_render_preview only when screenshots are permitted for this session; otherwise mark as human-check in verification.",
            "- If a validation/read-back check fails, diagnose precisely (id/json_key/time/value) and retry within this run before giving up.",
            "- If a requested step is not feasible with the available tools/capabilities, complete all feasible steps first, then call report_blocked once with a precise reason and suggestion.",
            "- When blocked on missing user intent/input, call report_blocked once with one focused question.",
        ]
    )
)

ATLAS_VERIFIER_SYSTEM_PROMPT = (
    "PHASE: Verifier\n"
    "You must NOT change the Atlas scene/timeline in this phase.\n"
    "Allowed actions: read-only inspection tools, docs lookup, session reads, and update_plan.\n\n"
    "Goals:\n"
    "- Start by calling verification_get (so you know what needs to be verified).\n"
    "- Verify the requested changes using read-back tools (scene_get_values, animation_list_keys, animation_camera_validate, etc.).\n"
    "- Record evidence with verification_record (mode=tool|visual|human).\n"
    "- Use verification_eval_plan to decide what can be marked completed, then update plan statuses via update_plan.\n"
    "- If something cannot be verified from tools alone, try a preview screenshot (animation_render_preview) when enabled.\n"
    "- If the screenshot is unavailable or still ambiguous, keep the step pending and request a human-check.\n"
    "- Absence of evidence is not a failure: only mark a step failed when tool read-backs clearly contradict the plan.\n"
    "- Produce the final user-facing answer for this turn.\n\n"
    + ATLAS_SHARED_SYSTEM_RULES
)

MAX_PREVIEW_IMAGE_BYTES_FOR_MODEL = 3_000_000


def _message(*, role: str, text: str) -> dict[str, Any]:
    return {
        "type": "message",
        "role": str(role),
        "content": [{"type": "input_text", "text": str(text)}],
    }


@dataclass
class ChatTeam:
    address: str
    api_key: str
    model: str
    temperature: float = 0.2
    atlas_dir: Optional[str] = None
    session: Optional[str] = None
    session_dir: Optional[str] = None
    enable_codegen: bool = False

    def __post_init__(self):
        self.session_store = SessionStore.open(
            session=self.session,
            session_dir=self.session_dir,
        )
        try:
            self._memory_summary = self.session_store.get_memory_summary()
        except Exception:
            self._memory_summary = ""
        # Legacy (kept for compatibility with existing session state).
        try:
            self._todo_ledger = self.session_store.get_todo_ledger()
        except Exception:
            self._todo_ledger = []

        self._history: list[tuple[str, str]] = []

        # Use atlas_dir from the saved session meta when the caller did not specify one.
        if not self.atlas_dir:
            try:
                meta = self.session_store.get_meta()
                saved = meta.get("atlas_dir")
                if isinstance(saved, str) and saved.strip():
                    self.atlas_dir = saved.strip()
            except Exception:
                pass
        # Connect to the running Atlas instance.
        # The RPC server can report its install location, so callers typically
        # do not need to configure an install path.
        self.scene = SceneClient(address=self.address, atlas_dir=self.atlas_dir)
        try:
            guessed = getattr(self.scene, "atlas_dir", None)
            if isinstance(guessed, str) and guessed.strip():
                self.atlas_dir = guessed.strip()
        except Exception:
            pass
        if not self.atlas_dir:
            raise RuntimeError(
                "Atlas app location is unavailable. Ensure Atlas is running and the local RPC server is enabled."
            )

        self.llm = LLMClient(api_key=self.api_key, model=self.model)

        # Build capabilities context derived from atlas_dir or defaults.
        self._context: str = build_atlas_agent_primer()
        try:
            schema_dir, _ = discover_schema_dir(user_schema_dir=None, atlas_dir=self.atlas_dir)
            if schema_dir:
                self._context = build_capabilities_prompt(
                    schema_dir,
                    codegen_enabled=bool(self.enable_codegen),
                )
        except Exception:
            self._context = build_atlas_agent_primer()

        # Always include a short docs/tooling hint (docs are shipped inside the Atlas app bundle).
        self._context = (self._context or "").rstrip() + "\n\n" + "\n".join(
            [
                "Docs (runtime): use docs_search/docs_read to look up Atlas behavior and RPC/tool contracts.",
                "Key docs: AGENTS_GUIDE.md, SCENE_SERVER.md, USER_GUIDE.md, DEVELOPER_GUIDE.md.",
            ]
        )

        # Persist session meta for easy resume.
        try:
            self.session_store.set_meta(
                address=self.address,
                model=self.model,
                atlas_dir=self.atlas_dir,
            )
            self.session_store.save()
        except Exception:
            pass

    def turn(
        self,
        user_text: str,
        *,
        shared_context: Optional[str] = None,
        callbacks: ToolLoopCallbacks | None = None,
        emit_to_stdout: bool = True,
    ) -> str:
        """Execute one natural-language turn using a streaming Responses API tool loop."""

        ctx = shared_context or self._context or ""
        turn_id = uuid.uuid4().hex

        def _screenshots_allowed() -> bool:
            try:
                return self.session_store.get_consent("screenshots") is True
            except Exception:
                return False

        def _compress_history_if_needed() -> None:
            mode = str(SESSION_MEMORY_COMPACTION_MODE or "llm").strip().lower()
            if mode in {"0", "off", "false", "no"}:
                return
            max_recent = max(0, int(SESSION_MAX_RECENT_MESSAGES))
            if max_recent <= 0 or len(self._history) <= max_recent:
                return
            overflow = self._history[:-max_recent]
            self._history = self._history[-max_recent:]
            if not overflow:
                return

            if mode in {"heuristic", "simple"}:
                lines: list[str] = []
                for role, content in overflow:
                    c = (content or "").strip().replace("\n", " ")
                    if c:
                        lines.append(f"- {role}: {c}")
                if lines:
                    self._memory_summary = (
                        (self._memory_summary.rstrip() + "\n" + "\n".join(lines)).strip()
                        if self._memory_summary
                        else "\n".join(lines).strip()
                    )
                return

            prompt = "\n".join(
                [
                    "Existing memory (may be empty):",
                    self._memory_summary or "(none)",
                    "",
                    "Conversation to fold into memory (chronological):",
                    *[f"{r}: {c}" for (r, c) in overflow],
                    "",
                    "Update the memory to be durable and useful for future turns.",
                    "Include: user goals, loaded data hints, object ids/names, animation/scene decisions, and open plan items.",
                    "Do NOT include tool schemas or verbose logs.",
                ]
            )
            sys_prompt = (
                "You are the Session Memory for Atlas Agent.\n"
                "Write a compact memory summary (bullet list). Keep it factual and stable.\n"
                "Keep it short enough to fit into future prompts."
            )
            try:
                mem = self.llm.complete_text(
                    system_prompt=sys_prompt, user_text=prompt, temperature=0.0
                )
                if mem:
                    self._memory_summary = mem.strip()
            except Exception:
                pass

        def _should_retrieve(text: str) -> bool:
            mode = str(AUTO_RETRIEVE_MODE or "auto").strip().lower()
            if mode in {"0", "off", "false", "no"}:
                return False
            if mode in {"1", "on", "true", "yes", "always"}:
                return True
            t = (text or "").strip().lower()
            triggers = (
                "continue",
                "resume",
                "as before",
                "as we discussed",
                "last time",
                "previous",
                "earlier",
                "same as",
                "again",
            )
            return any(tok in t for tok in triggers)

        def _auto_retrieve_context(text: str) -> str:
            if not _should_retrieve(text):
                return ""

            max_snips = max(0, int(AUTO_RETRIEVE_MAX_SNIPPETS))
            max_chars = max(0, int(AUTO_RETRIEVE_MAX_CHARS))

            # Extract a small set of "needles" (quoted strings, file-ish tokens, ids).
            needles: list[str] = []
            for match in re.finditer(r"['\\\"]([^'\\\"]{3,})['\\\"]", text or ""):
                needles.append(match.group(1))
            for match in re.finditer(r"(?:/|~)[^\\s]+", text or ""):
                needles.append(match.group(0))
            for match in re.finditer(r"\\b\\d{3,}\\b", text or ""):
                needles.append(match.group(0))
            if not needles:
                words = re.findall(r"[A-Za-z0-9_./-]{4,}", text or "")
                seen: set[str] = set()
                for w in words:
                    wl = w.lower()
                    if wl in seen:
                        continue
                    seen.add(wl)
                    needles.append(w)
                    if len(needles) >= 4:
                        break

            snippets: list[str] = []

            # 1) Recent write tool calls (from events log) — deterministic "what changed last".
            n_recent = max(0, int(AUTO_RETRIEVE_RECENT_WRITE_EVENTS))
            if n_recent > 0:
                write_tools = set(ATLAS_STATE_MUTATION_TOOLS) | set(ATLAS_OUTPUT_TOOLS)
                recent_tool_events = self.session_store.tail_events(
                    limit=max(1, n_recent * 3), event_type="tool_call"
                )
                write_summaries: list[str] = []
                for ev in reversed(recent_tool_events):
                    try:
                        tool = str(ev.get("tool") or "")
                        if tool not in write_tools:
                            continue
                        rs = (
                            ev.get("result_summary")
                            if isinstance(ev.get("result_summary"), dict)
                            else {}
                        )
                        ok = rs.get("ok") if isinstance(rs, dict) else None
                        if ok is False:
                            continue
                        if isinstance(rs, dict) and rs.get("skipped"):
                            continue
                        args = ev.get("args")
                        short = f"- tool: {tool}"
                        if isinstance(args, dict):
                            keys: list[str] = []
                            for kk in (
                                "id",
                                "ids",
                                "json_key",
                                "name",
                                "time",
                                "t0",
                                "t1",
                                "path",
                                "files",
                            ):
                                if kk in args:
                                    keys.append(f"{kk}={args.get(kk)!r}")
                            if keys:
                                short += " (" + ", ".join(keys) + ")"
                        write_summaries.append(short)
                        if len(write_summaries) >= n_recent:
                            break
                    except Exception:
                        continue
                if write_summaries:
                    snippets.append("Recent verified writes (most recent first; summarized):")
                    snippets.extend(write_summaries)

            # 2) A few matching transcript messages (for qualitative recall).
            for needle in needles:
                if max_snips and len([s for s in snippets if s.startswith("- ")]) >= max_snips:
                    break
                try:
                    hits = self.session_store.search_transcript(query=needle, max_results=2)
                except Exception:
                    continue
                if not isinstance(hits, dict) or not hits.get("ok"):
                    continue
                for ent in hits.get("results", []) or []:
                    try:
                        role = str(ent.get("role") or "")
                        content = str(ent.get("content") or "")
                        if not content:
                            continue
                        one = content.replace("\n", " ").strip()
                        if max_chars and len(one) > max_chars:
                            one = (
                                one[:max_chars]
                                + "… (excerpt; use session_search_transcript for full)"
                            )
                        snippets.append(f"- {role}: {one}")
                    except Exception:
                        continue

            if not snippets:
                return ""
            return "Auto-retrieved context (session):\n" + "\n".join(snippets)

        # Keep history bounded before building the next prompt.
        _compress_history_if_needed()

        # Persist the user input early so intent survives crashes during tool execution.
        try:
            self.session_store.append_transcript(role="user", content=user_text, turn_id=turn_id)
        except Exception:
            pass

        retrieved_context = _auto_retrieve_context(user_text)

        env_lines: list[str] = [
            "<environment_context>",
            f"  <session_id>{self.session_store.session_id()}</session_id>",
            f"  <address>{self.address}</address>",
            f"  <model>{self.model}</model>",
        ]
        if self.atlas_dir:
            env_lines.append(f"  <atlas_dir>{self.atlas_dir}</atlas_dir>")
        env_lines.append("</environment_context>")

        context_blocks: list[str] = []
        if ctx:
            context_blocks.append(ctx.strip())
        if self._memory_summary:
            context_blocks.append("Session memory (summary):\n" + self._memory_summary.strip())
        if retrieved_context:
            context_blocks.append(retrieved_context.strip())
        try:
            plan = self.session_store.get_plan() or []
        except Exception:
            plan = []
        if plan:
            lines: list[str] = []
            for it in plan:
                if not isinstance(it, dict):
                    continue
                step = str(it.get("step") or "").strip()
                status = str(it.get("status") or "").strip()
                if step:
                    lines.append(f"- [{status}] {step}")
            if lines:
                context_blocks.append("Current plan:\n" + "\n".join(lines))

        # Tool list + dispatcher (writes go through this wrapper).
        runtime_state: dict[str, Any] = {
            "turn_id": turn_id,
            "phase": None,
            # Producer tools (camera_*) may chain using this per-turn cache.
            "last_camera_value": None,
        }
        tools, dispatch = scene_tools_and_dispatcher(
            self.scene,
            atlas_dir=self.atlas_dir,
            session_store=self.session_store,
            runtime_state=runtime_state,
            codegen_enabled=bool(self.enable_codegen),
        )

        full_log_tools = (
            set(ATLAS_STATE_MUTATION_TOOLS)
            | set(ATLAS_OUTPUT_TOOLS)
            | set(SESSION_MUTATION_TOOLS)
            | set(CODEGEN_TOOLS)
        )
        current_phase = "executor"

        def _dispatch_logged(name: str, args_json: str) -> str:
            result_json = dispatch(name, args_json or "{}")
            try:
                args_obj = json.loads(args_json or "{}")
            except Exception:
                args_obj = {"raw": args_json}
            try:
                res_obj = json.loads(result_json or "{}")
            except Exception:
                res_obj = {"raw": result_json}

            policy = "full" if name in full_log_tools else "summary"
            ok = True
            try:
                ok = bool(res_obj.get("ok", True)) if isinstance(res_obj, dict) else True
            except Exception:
                ok = True
            if (name not in full_log_tools) and (not ok):
                policy = "full"
            try:
                self.session_store.append_tool_event(
                    turn_id=turn_id,
                    phase=current_phase,
                    tool=name,
                    args=args_obj,
                    result=res_obj,
                    policy=policy,
                )
            except Exception:
                pass
            return result_json

        # Build Responses API input items (local history; no server-side state).
        input_items: list[dict[str, Any]] = []
        input_items.append(_message(role="user", text="\n".join(env_lines)))
        if context_blocks:
            input_items.append(
                _message(role="user", text="Shared context:\n" + "\n\n".join(context_blocks))
            )
        for role, content in self._history:
            if role in ("user", "assistant") and content:
                input_items.append(_message(role=role, text=content))
        input_items.append(_message(role="user", text=user_text))

        # Default streaming UX (plain terminal) unless the caller provides a UI callback sink.
        printed_reasoning = False
        if callbacks is None:

            def _phase_start(phase: str) -> None:
                nonlocal printed_reasoning
                if not emit_to_stdout:
                    return
                printed_reasoning = False
                sys.stdout.write(f"\n\n# {phase}\n")
                sys.stdout.flush()

            def _phase_end(_phase: str) -> None:
                nonlocal printed_reasoning
                if emit_to_stdout and printed_reasoning:
                    sys.stdout.write("\n\n")
                    sys.stdout.flush()
                printed_reasoning = False

            def _reasoning_delta(delta: str, _summary_index: int) -> None:
                nonlocal printed_reasoning
                if not emit_to_stdout or not delta:
                    return
                if not printed_reasoning:
                    sys.stdout.write("\nReasoning summary:\n")
                    printed_reasoning = True
                sys.stdout.write(delta)
                sys.stdout.flush()

            def _reasoning_part_added(_summary_index: int) -> None:
                if emit_to_stdout and printed_reasoning:
                    sys.stdout.write("\n\n")
                    sys.stdout.flush()

            def _tool_call(name: str, _args_json: str, _call_id: str) -> None:
                if not emit_to_stdout:
                    return
                sys.stdout.write(f"\n\n→ {name}\n")
                sys.stdout.flush()

            def _tool_result(name: str, _call_id: str, result_json: str) -> None:
                if not emit_to_stdout:
                    return
                ok = None
                err = ""
                try:
                    data = json.loads(result_json or "{}")
                    if isinstance(data, dict):
                        ok = data.get("ok")
                        err = str(data.get("error") or "")
                except Exception:
                    ok = None
                    err = ""
                if ok is True:
                    sys.stdout.write(f"← {name}: ok\n")
                elif ok is False:
                    sys.stdout.write(f"← {name}: fail {err}\n")
                else:
                    sys.stdout.write(f"← {name}: done\n")
                sys.stdout.flush()

            callbacks = ToolLoopCallbacks(
                on_phase_start=_phase_start,
                on_phase_end=_phase_end,
                on_reasoning_summary_delta=_reasoning_delta,
                on_reasoning_summary_part_added=_reasoning_part_added,
                on_tool_call=_tool_call,
                on_tool_result=_tool_result,
            )

        def _tool_name(tool_spec: dict[str, Any]) -> str:
            if not isinstance(tool_spec, dict):
                return ""
            if str(tool_spec.get("type") or "") != "function":
                return ""
            fn = tool_spec.get("function")
            if not isinstance(fn, dict):
                return ""
            return str(fn.get("name") or "")

        all_tool_names = {n for n in (_tool_name(t) for t in tools) if n}
        read_only_tool_names = (
            all_tool_names
            - set(ATLAS_STATE_MUTATION_TOOLS)
            - set(ATLAS_OUTPUT_TOOLS)
            - set(CODEGEN_TOOLS)
        )

        def _filter_tools(allowed_names: set[str]) -> list[dict[str, Any]]:
            return [t for t in tools if _tool_name(t) in allowed_names]

        def _should_run_planner() -> bool:
            # Adaptive planner: run when there is no plan yet, or the request looks multi-step/ambiguous.
            if not plan:
                return True
            t = (user_text or "").strip()
            tl = t.lower()
            if "\n" in t:
                return True
            if len(t) >= 220:
                return True
            if any(k in tl for k in ("plan", "steps", "todo", "design", "options")):
                return True
            # Heuristic: multi-clause requests benefit from an explicit plan.
            clause_hits = 0
            for tok in (" then ", " after ", " before ", " next ", " also "):
                if tok in tl:
                    clause_hits += 1
            return clause_hits >= 2

        def _run_phase(
            *,
            phase: str,
            instructions: str,
            phase_tools: list[dict[str, Any]],
            phase_input_items: list[dict[str, Any]],
            max_rounds: int,
        ):
            nonlocal current_phase
            current_phase = phase
            try:
                runtime_state["phase"] = phase
            except Exception:
                pass

            def _post_tool_output(name: str, args_json: str, result_json: str) -> list[dict[str, Any]]:
                # Allow the Verifier to actually *see* previews by attaching the
                # rendered image as an input_image in the next model call.
                if str(runtime_state.get("phase") or "") != "Verifier":
                    return []
                if str(name or "") != "animation_render_preview":
                    return []
                if not _screenshots_allowed():
                    return []
                try:
                    data = json.loads(result_json or "{}")
                except Exception:
                    data = {}
                if not isinstance(data, dict) or not data.get("ok"):
                    return []
                path = data.get("path")
                if not isinstance(path, str) or not path.strip():
                    return []

                p = Path(path)
                if not p.exists() or not p.is_file():
                    return [
                        _message(
                            role="user",
                            text=f"Preview render returned path={path!r}, but the file does not exist on disk.",
                        )
                    ]

                try:
                    raw = p.read_bytes()
                except Exception as e:
                    return [
                        _message(
                            role="user",
                            text=f"Preview image exists at {path!r} but could not be read: {e}",
                        )
                    ]

                if len(raw) > MAX_PREVIEW_IMAGE_BYTES_FOR_MODEL:
                    return [
                        _message(
                            role="user",
                            text=(
                                "Preview image is too large to attach for model-based visual checking.\n"
                                f"- path: {path}\n"
                                f"- bytes: {len(raw)}\n"
                                f"- limit_bytes: {MAX_PREVIEW_IMAGE_BYTES_FOR_MODEL}\n"
                                "Please re-render the preview at a smaller width/height."
                            ),
                        )
                    ]

                mime, _ = mimetypes.guess_type(str(p))
                mime = mime or "image/png"
                b64 = base64.b64encode(raw).decode("ascii")
                data_url = f"data:{mime};base64,{b64}"

                # Include a short text header so the model knows what this image represents.
                try:
                    args = json.loads(args_json or "{}")
                except Exception:
                    args = {}
                tsec = args.get("time") if isinstance(args, dict) else None
                w = args.get("width") if isinstance(args, dict) else None
                h = args.get("height") if isinstance(args, dict) else None
                header = (
                    "Preview image for visual verification.\n"
                    f"- tool: animation_render_preview\n"
                    f"- time_sec: {tsec!r}\n"
                    f"- size: {w!r}x{h!r}\n"
                    f"- path: {path}\n"
                    "Use this image to decide visual requirements (visual/human) for verification."
                )
                return [
                    {
                        "type": "message",
                        "role": "user",
                        "content": [
                            {"type": "input_text", "text": header},
                            {"type": "input_image", "image_url": data_url},
                        ],
                    }
                ]

            if callbacks is not None and callbacks.on_phase_start is not None:
                callbacks.on_phase_start(phase)
            result = run_responses_tool_loop(
                llm=self.llm,
                instructions=instructions,
                input_items=phase_input_items,
                tools=phase_tools,
                dispatch=_dispatch_logged,
                post_tool_output=_post_tool_output,
                callbacks=callbacks,
                temperature=self.temperature,
                max_rounds=max_rounds,
            )
            if callbacks is not None and callbacks.on_phase_end is not None:
                callbacks.on_phase_end(phase)
            # Persist reasoning summary blocks per phase (append-only) for resume/debug.
            try:
                if result.reasoning_summaries:
                    self.session_store.append_event(
                        {
                            "type": "reasoning_summary",
                            "turn_id": turn_id,
                            "phase": phase,
                            "summaries": list(result.reasoning_summaries),
                        }
                    )
            except Exception:
                pass
            try:
                if (result.assistant_text or "").strip():
                    self.session_store.append_event(
                        {
                            "type": "phase_output",
                            "turn_id": turn_id,
                            "phase": phase,
                            "assistant_text": (result.assistant_text or "").strip(),
                        }
                    )
            except Exception:
                pass
            return result

        # Phase runner (adaptive): Planner (optional) → Executor (always) → Verifier (if atlas writes occurred).
        phase_input = list(input_items)
        final_result = None

        if _should_run_planner():
            planner_tools = _filter_tools(read_only_tool_names | set(SESSION_MUTATION_TOOLS))
            planner_res = _run_phase(
                phase="Planner",
                instructions=ATLAS_PLANNER_SYSTEM_PROMPT,
                phase_tools=planner_tools,
                phase_input_items=phase_input,
                max_rounds=12,
            )
            phase_input = list(planner_res.input_items)
            # If the Planner asks a clarifying question, stop the turn here.
            pt = (planner_res.assistant_text or "").strip()
            if pt.lower().startswith("clarify:"):
                final_result = planner_res

        if final_result is None:
            exec_res = _run_phase(
                phase="Executor",
                instructions=ATLAS_EXECUTOR_SYSTEM_PROMPT,
                phase_tools=tools,
                phase_input_items=phase_input,
                max_rounds=24,
            )
            phase_input = list(exec_res.input_items)
            did_write_state = any(
                (call.get("name") in ATLAS_STATE_MUTATION_TOOLS)
                for call in (exec_res.tool_calls or [])
            )
            if did_write_state:
                verifier_tools = _filter_tools(
                    read_only_tool_names
                    | set(SESSION_MUTATION_TOOLS)
                    | set(ATLAS_VERIFICATION_OUTPUT_TOOLS)
                )
                ver_res = _run_phase(
                    phase="Verifier",
                    instructions=ATLAS_VERIFIER_SYSTEM_PROMPT,
                    phase_tools=verifier_tools,
                    phase_input_items=phase_input,
                    max_rounds=12,
                )
                final_result = ver_res
            else:
                final_result = exec_res

        text = (final_result.assistant_text or "").strip() or "(no response)"

        # Update local history + transcript.
        if user_text:
            self._history.append(("user", user_text))
            try:
                self.session_store.append_transcript(
                    role="user", content=user_text, turn_id=turn_id
                )
            except Exception:
                pass
        if text:
            self._history.append(("assistant", text))
            try:
                self.session_store.append_transcript(
                    role="assistant", content=text, turn_id=turn_id
                )
            except Exception:
                pass

        _compress_history_if_needed()

        try:
            self.session_store.set_memory_summary(self._memory_summary)
            # Legacy TODO ledger preserved as-is (primary ledger is update_plan).
            self.session_store.set_todo_ledger(self._todo_ledger)
            self.session_store.set_meta(
                atlas_dir=self.atlas_dir,
                address=self.address,
                model=self.model,
                last_turn_id=turn_id,
            )
            self.session_store.save()
        except Exception:
            pass

        return text


def run_repl(
    address: str,
    api_key: str,
    model: str,
    temperature: float = 0.2,
    *,
    session: Optional[str] = None,
    session_dir: Optional[str] = None,
    enable_codegen: bool = False,
) -> int:
    logger = logging.getLogger("atlas_agent.chat")
    if not logger.handlers:
        h = logging.StreamHandler()
        fmt = logging.Formatter(
            fmt="%(asctime)s [%(levelname)s] %(name)s: %(message)s", datefmt="%H:%M:%S"
        )
        h.setFormatter(fmt)
        logger.addHandler(h)
        logger.setLevel(logging.INFO)
        logger.propagate = False

    team = ChatTeam(
        address=address,
        api_key=api_key,
        model=model,
        temperature=temperature,
        session=session,
        session_dir=session_dir,
        enable_codegen=bool(enable_codegen),
    )
    logger.info(
        "Atlas Agent (RPC). Type :help for commands. Session=%s",
        team.session_store.session_id(),
    )
    logger.info("Atlas app: %s", team.atlas_dir)

    # One-time per-session consent prompt for screenshot-based visual verification.
    try:
        decided = team.session_store.get_consent("screenshots")
    except Exception:
        decided = None
    if decided is None:
        logger.info(
            "Privacy consent:\n"
            "- The agent can render a single-frame preview image for visual verification.\n"
            "- The image is generated locally (temporary file) and may be sent to the model for inspection.\n"
            "- If you deny, the agent will fall back to human-check steps for visual requirements.\n"
        )
        ans = input("Allow preview screenshots for this session? [Y/n] ").strip().lower()
        allow = ans in ("", "y", "yes")
        try:
            team.session_store.set_consent("screenshots", allow)
            team.session_store.save()
        except Exception:
            pass
        logger.info("Screenshots %s for this session.", "enabled" if allow else "disabled")

    while True:
        try:
            line = input(">> ").strip()
        except (EOFError, KeyboardInterrupt):
            logger.info("")
            return 0
        if not line:
            continue
        if line.startswith(":"):
            cmd, *rest = line[1:].split()
            if cmd in {"q", "quit", "exit"}:
                return 0
            if cmd in {"h", "help"}:
                logger.info(
                    "Commands:\n"
                    ":help                This help\n"
                    ":session             Show session paths\n"
                    ":screenshots on|off  Toggle preview screenshots for this session\n"
                    ":plan                Show current plan\n"
                    ":memory              Show session memory summary\n"
                    ":events [N]          Show recent events\n"
                    ":save <path>         Save current animation\n"
                    ":time <seconds>      Set current time\n"
                    ":objects             List objects"
                )
                continue
            if cmd == "session":
                try:
                    logger.info("session_id=%s", team.session_store.session_id())
                    logger.info("log=%s", str(team.session_store.log_path))
                    logger.info(
                        "consent.screenshots=%r",
                        team.session_store.get_consent("screenshots"),
                    )
                except Exception as e:
                    logger.info("fail: %s", e)
                continue
            if cmd == "screenshots":
                if not rest:
                    try:
                        c = team.session_store.get_consent("screenshots")
                    except Exception:
                        c = None
                    logger.info("consent.screenshots=%r", c)
                    logger.info("Usage: :screenshots on | off")
                    continue
                v = (rest[0] or "").strip().lower()
                if v in {"on", "1", "true", "yes", "y"}:
                    allow = True
                elif v in {"off", "0", "false", "no", "n"}:
                    allow = False
                else:
                    logger.info("Usage: :screenshots on | off")
                    continue
                try:
                    team.session_store.set_consent("screenshots", allow)
                    team.session_store.save()
                except Exception as e:
                    logger.info("fail: %s", e)
                    continue
                logger.info("ok")
                continue
            if cmd == "plan":
                try:
                    plan = team.session_store.get_plan()
                    if not plan:
                        logger.info("(no plan)")
                        continue
                    for it in plan:
                        step = (it.get("step") or "").strip() if isinstance(it, dict) else ""
                        status = (it.get("status") or "").strip() if isinstance(it, dict) else ""
                        if step:
                            logger.info("%s\t%s", status, step)
                except Exception as e:
                    logger.info("fail: %s", e)
                continue
            if cmd == "memory":
                try:
                    mem = team.session_store.get_memory_summary()
                    logger.info("%s", mem if mem else "(empty)")
                except Exception as e:
                    logger.info("fail: %s", e)
                continue
            if cmd == "events":
                try:
                    n = int(rest[0]) if rest else 20
                except Exception:
                    n = 20
                try:
                    evs = team.session_store.tail_events(limit=max(1, n))
                    if not evs:
                        logger.info("(no events)")
                        continue
                    for ev in evs:
                        try:
                            ts = ev.get("ts")
                            et = ev.get("type")
                            tool = ev.get("tool")
                            tid = ev.get("turn_id")
                            logger.info("%s\t%s\t%s\t%s", ts, et, tool, tid)
                        except Exception:
                            logger.info("%s", ev)
                except Exception as e:
                    logger.info("fail: %s", e)
                continue
            if cmd == "save" and rest:
                ok = team.scene.save_animation(rest[0])
                logger.info("%s", "ok" if ok else "fail")
                continue
            if cmd == "time" and rest:
                logger.info("%s", "ok" if team.scene.set_time(float(rest[0])) else "fail")
                continue
            if cmd == "objects":
                resp = team.scene.list_objects()
                for obj in resp.objects:
                    logger.info("%s\t%s\t%s\t%s", obj.id, obj.type, obj.name, obj.visible)
                continue
            logger.info("Unknown command; :help")
            continue

        # Natural language turn
        try:
            response_text = team.turn(line, shared_context=None)
            print(response_text)
        except Exception as e:
            logger.exception("Agent error: %s", e)
            continue
    return 0
