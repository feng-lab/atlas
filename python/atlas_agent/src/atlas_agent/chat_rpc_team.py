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
from .agent_team.intent_resolver import INTENT_RESOLVER_SYSTEM, IntentResolver
from .agent_team.tools_agent import scene_tools_and_dispatcher
from .capabilities_prompt import build_atlas_agent_primer, build_capabilities_prompt
from .defaults import DEFAULT_EXECUTOR_MAX_ROUNDS
from .responses_tool_loop import (
    ToolLoopCallbacks,
    ToolLoopNonConverged,
    ToolLoopResult,
    run_responses_tool_loop,
)
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
SESSION_MEMORY_RECENT_WRITE_EVENTS = 12

AUTO_RETRIEVE_MODE = "auto"  # "off" | "auto" | "always"
AUTO_RETRIEVE_MAX_SNIPPETS = 6
AUTO_RETRIEVE_MAX_CHARS = 280
AUTO_RETRIEVE_RECENT_WRITE_EVENTS = 8

# Prompt-budget guardrail for the Supervisor Task Brief step.
#
# Rationale: The intent resolver prompt must remain small/stable so it can run
# reliably even in long sessions. We therefore include only a compact preview of
# the current object list in the scene snapshot; the full, authoritative list is
# always available via `scene_list_objects`.
INTENT_RESOLVER_SCENE_SNAPSHOT_MAX_CHARS = 2400


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
    "animation_camera_set_interpolation_method",
    "animation_camera_waypoint_spline_apply",
    "animation_camera_walkthrough_apply",
    "animation_set_time",
}

ATLAS_OUTPUT_TOOLS: set[str] = {
    "scene_save_scene",
    "scene_screenshot",
    "animation_save_animation",
    "animation_export_video",
    "animation_render_preview",
}

# Output tools that are allowed in verification (best-effort; may be disabled by env).
ATLAS_VERIFICATION_OUTPUT_TOOLS: set[str] = {
    "scene_screenshot",
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
    "- Camera: prefer typed operations. For orbits/dollies use animation_camera_solve_and_apply; for first-person walkthroughs use animation_camera_walkthrough_apply; for explicit waypoints use animation_camera_waypoint_spline_apply. Prefer bbox-fraction semantics over raw world coords.\n"
    "- Camera director rubric (routing): if the user provides explicit waypoints/points/beats, use animation_camera_waypoint_spline_apply; otherwise if the user describes motion verbs (fly/strafe/yaw/pitch/pause), use animation_camera_walkthrough_apply. Mixed prompts: do not drop waypoints/segments; add intermediate points or increase walkthrough step_seconds instead of truncating.\n"
    "- Camera director rubric (defaults): method='Position Rotation Spline' for walkthrough/waypoints; walkthrough constraints default keep_visible=false unless the user explicitly wants framing; step_seconds defaults: slow≈0.5, medium≈1.0, fast≈1.5–2.0.\n"
    "- Walkthrough planning: when inventing segments from words, you may use internal segment templates (template+amount/distance/degrees) and let the tool expand them; do not require the user to name templates.\n"
    "- File paths: classify input. Absolute/~/drive paths are exact (fs_expand_paths→fs_check_paths; if missing try fs_resolve_path). Natural-language hints use fs_hint_resolve with structured args (expected_name + possible_dirs). Always verify before loading.\n"
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
    "- You will be given a TASK BRIEF in the shared context. Treat it as authoritative; do not rewrite or reinterpret it.\n"
    "- Otherwise: update the plan via update_plan (4–7 short steps).\n\n"
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
            "- Derive intent strictly from the Task Brief in the shared context. If the brief conflicts with the latest user message, stop and ask one focused question.",
            "- Prefer live discovery for ids/json_keys/options: scene_list_objects → scene_list_params(id) → scene_get_values(id,json_keys). Do not guess.",
            "- For unclear semantics/workflows, consult docs via docs_search/docs_read (SCENE_SERVER.md, AGENTS_GUIDE.md, USER_GUIDE.md).",
            "- Long sessions: if the user references prior decisions, use session_get_memory/session_get_plan or session_search_transcript for exact quotes.",
            "- File paths (exact): when the user provides an absolute/~/drive path token (e.g. /Users/... , ~/... , C:\\...), treat it as exact. Use fs_expand_paths → fs_check_paths first; only if missing try fs_resolve_path (typo correction). Avoid fs_hint_resolve in this case.",
            "- File paths (hints): when the user describes location in words (e.g. “in my Documents/atlas_test”), use system_info to resolve common dirs and then call fs_hint_resolve with structured args: expected_name (basename to score) + possible_dirs (likely search roots). If match!='exact', validate by reading/checking the file before loading.",
            "- Scene vs timeline contract: any mention of time/duration implies animation_* tools. Never include time/easing in scene_apply.",
            "- Scene-only intent: do not write animation keys. Use scene_apply/scene_set_visibility/scene_camera_apply and verify with scene_get_values.",
            "- Animation intent: animation_ensure_animation → animation_set_duration → write keys (animation_batch / animation_set_key_param / animation_replace_key_param) and camera keys (animation_camera_solve_and_apply / animation_replace_key_camera).",
            "- Camera workflow: use camera_* producers (camera_focus / camera_point_to / camera_rotate / camera_reset_view) to compute typed camera values, then apply via scene_camera_apply or animation_* camera tools as appropriate.",
            "- Validate before committing where possible (scene_validate_params; animation_camera_validate). Verify after writing (animation_list_keys, animation_camera_validate, scene_get_values).",
            "- Large-file handling: prefer fs_tail_lines/fs_search_text; if you must scan, iterate fs_read_text in windows until EOF (no silent truncation).",
            "- Verification ledger: when a step has specific requirements, use verification_record(mode=tool|visual|human) to attach evidence and update_plan to advance statuses.",
            "- Visual checks: prefer scene_screenshot for current scene state; use animation_render_preview only when you need a specific animation time. Both require screenshots consent; otherwise mark as human-check in verification.",
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
    "- If something cannot be verified from tools alone, try a screenshot: prefer scene_screenshot for scene state; use animation_render_preview when verifying a specific animation time.\n"
    "- If the screenshot is unavailable or still ambiguous, keep the step pending and request a human-check.\n"
    "- Absence of evidence is not a failure: only mark a step failed when tool read-backs clearly contradict the plan.\n"
    "- Produce the final user-facing answer for this turn.\n\n"
    + ATLAS_SHARED_SYSTEM_RULES
)

MAX_PREVIEW_IMAGE_BYTES_FOR_MODEL = 3_000_000
MODEL_IMAGE_MIME_BY_SUFFIX: dict[str, str] = {
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".webp": "image/webp",
    ".gif": "image/gif",  # non-animated only (we don't generate animated gifs)
}


def _mime_for_model_image(path: Path) -> str | None:
    ext = str(path.suffix or "").strip().lower()
    return MODEL_IMAGE_MIME_BY_SUFFIX.get(ext)


def _message(*, role: str, text: str) -> dict[str, Any]:
    # The Responses API distinguishes between user input parts (input_text) and
    # assistant history parts (output_text). Some OpenAI-compatible providers
    # reject assistant messages that contain input_text parts.
    role_s = str(role)
    part_type = "output_text" if role_s == "assistant" else "input_text"
    return {
        "type": "message",
        "role": role_s,
        "content": [{"type": part_type, "text": str(text)}],
    }


@dataclass
class ChatTeam:
    address: str
    api_key: str
    model: str
    temperature: float = 0.2
    reasoning_effort: str | None = "high"
    max_rounds_executor: int = DEFAULT_EXECUTOR_MAX_ROUNDS
    atlas_dir: Optional[str] = None
    atlas_version: Optional[str] = None
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
        if not self.atlas_version:
            try:
                meta = self.session_store.get_meta()
                saved = meta.get("atlas_version")
                if isinstance(saved, str) and saved.strip():
                    self.atlas_version = saved.strip()
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

        # Best-effort: record the running app's build/version for compatibility logs.
        try:
            ver = self.scene.get_app_version()
            if isinstance(ver, str) and ver.strip():
                self.atlas_version = ver.strip()
        except Exception:
            pass

        self.llm = LLMClient(api_key=self.api_key, model=self.model)
        self.intent_resolver = IntentResolver(client=self.llm, temperature=self.temperature)

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
                reasoning_effort=self.reasoning_effort,
                max_rounds_executor=int(self.max_rounds_executor),
                atlas_dir=self.atlas_dir,
                atlas_version=self.atlas_version,
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

            # Include a small deterministic summary of recent state-changing tool calls so the
            # memory captures facts that may never have been stated in chat text (ids, paths).
            write_summaries: list[str] = []
            try:
                n_recent = max(0, int(SESSION_MEMORY_RECENT_WRITE_EVENTS))
                if n_recent > 0:
                    write_tools = set(ATLAS_STATE_MUTATION_TOOLS) | set(ATLAS_OUTPUT_TOOLS)
                    recent_tool_events = self.session_store.tail_events(
                        limit=max(1, n_recent * 3), event_type="tool_call"
                    )
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
            except Exception:
                write_summaries = []

            # Include the current plan (small) so memory retains outstanding work.
            plan_lines: list[str] = []
            try:
                cur_plan = self.session_store.get_plan() or []
                if cur_plan:
                    plan_lines.append("Current plan:")
                    for it in cur_plan:
                        if not isinstance(it, dict):
                            continue
                        step = str(it.get("step") or "").strip()
                        status = str(it.get("status") or "").strip()
                        if step:
                            plan_lines.append(f"- [{status}] {step}")
            except Exception:
                plan_lines = []

            prompt = "\n".join(
                [
                    "Existing memory (may be empty):",
                    self._memory_summary or "(none)",
                    "",
                    *(
                        ["Recent state changes (most recent first; summarized):", *write_summaries, ""]
                        if write_summaries
                        else []
                    ),
                    *(plan_lines + [""] if plan_lines else []),
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
                    hits = self.session_store.search_transcript(
                        query=needle,
                        max_results=2,
                        reverse=True,
                    )
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

        retrieved_context = _auto_retrieve_context(user_text)

        env_lines: list[str] = [
            "<environment_context>",
            f"  <session_id>{self.session_store.session_id()}</session_id>",
            f"  <address>{self.address}</address>",
            f"  <model>{self.model}</model>",
        ]
        if self.atlas_dir:
            env_lines.append(f"  <atlas_dir>{self.atlas_dir}</atlas_dir>")
        if self.atlas_version:
            env_lines.append(f"  <atlas_version>{self.atlas_version}</atlas_version>")
        env_lines.append("</environment_context>")
        env_text = "\n".join(env_lines)

        # Build a compact, factual scene snapshot (read-only) to ground intent resolution.
        # This is best-effort: if Atlas is busy/unavailable for some reason, we continue
        # without it rather than breaking the chat turn.
        scene_lines: list[str] = []
        try:
            objs = self.scene.list_objects()
            ol = list(getattr(objs, "objects", []) or [])
            scene_lines.append(f"Scene: {len(ol)} objects loaded")
            for o in ol:
                try:
                    oid = int(getattr(o, "id", 0))
                    typ = str(getattr(o, "type", "") or "")
                    name = str(getattr(o, "name", "") or "")
                    vis = bool(getattr(o, "visible", False))
                    candidate = f"- {oid}:{typ}:{name} visible={vis}"
                    # Keep this snapshot bounded for prompt stability; avoid silently truncating.
                    current = "\n".join(scene_lines)
                    if len(current) + 1 + len(candidate) > INTENT_RESOLVER_SCENE_SNAPSHOT_MAX_CHARS:
                        remaining = max(0, len(ol) - max(0, len(scene_lines) - 1))
                        scene_lines.append(
                            f"... (scene snapshot truncated for prompt budget; {remaining} more objects not shown; use scene_list_objects for full list)"
                        )
                        break
                    scene_lines.append(candidate)
                except Exception:
                    continue
        except Exception:
            pass
        try:
            ts = self.scene.get_time()
            tsec = float(getattr(ts, "seconds", 0.0) or 0.0)
            dur = float(getattr(ts, "duration", 0.0) or 0.0)
            scene_lines.append(f"Time: t={tsec:.3f}s / duration={dur:.3f}s")
        except Exception:
            pass
        scene_snapshot_text = "\n".join(scene_lines).strip()
        try:
            if scene_snapshot_text:
                self.session_store.append_event(
                    {
                        "type": "scene_snapshot",
                        "turn_id": turn_id,
                        "text": scene_snapshot_text,
                    }
                )
        except Exception:
            pass

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

        # Supervisor step: resolve a durable TASK BRIEF that downstream phases must follow.
        # This improves stability in long sessions and reduces accidental intent drift.
        brief_ctx_parts: list[str] = []
        if scene_snapshot_text:
            brief_ctx_parts.append("Scene snapshot:\n" + scene_snapshot_text)
        if self._memory_summary:
            brief_ctx_parts.append("Session memory (summary):\n" + self._memory_summary.strip())
        if retrieved_context:
            brief_ctx_parts.append(retrieved_context.strip())
        if plan:
            # Reuse the already-rendered plan block from context_blocks (keeps stable formatting).
            for b in context_blocks:
                if b.startswith("Current plan:\n"):
                    brief_ctx_parts.append(b)
                    break
        brief_ctx = "\n\n".join([p for p in brief_ctx_parts if p]).strip()

        task_brief = ""
        try:
            task_brief = (self.intent_resolver.resolve(user_text, scene_context=brief_ctx) or "").strip()
        except Exception:
            task_brief = ""

        # Persist the user input after auto-retrieval (so search doesn't match the current message),
        # but before any tool execution (so intent survives crashes mid-turn).
        try:
            self.session_store.append_transcript(role="user", content=user_text, turn_id=turn_id)
        except Exception:
            pass
        try:
            if task_brief:
                self.session_store.append_event(
                    {
                        "type": "task_brief",
                        "turn_id": turn_id,
                        "text": task_brief,
                    }
                )
        except Exception:
            pass

        # If the Supervisor needs clarification, stop early (no tool calls).
        if task_brief.lower().startswith("clarify:"):
            text = task_brief.strip()
            if user_text:
                self._history.append(("user", user_text))
            if text:
                self._history.append(("assistant", text))
            try:
                if text:
                    self.session_store.append_transcript(role="assistant", content=text, turn_id=turn_id)
            except Exception:
                pass
            _compress_history_if_needed()
            try:
                self.session_store.set_memory_summary(self._memory_summary)
                self.session_store.set_meta(
                    atlas_dir=self.atlas_dir,
                    atlas_version=self.atlas_version,
                    address=self.address,
                    model=self.model,
                    last_turn_id=turn_id,
                )
                self.session_store.save()
            except Exception:
                pass
            return text or "(no response)"

        # Make the Task Brief visible to downstream phases.
        if task_brief:
            context_blocks.append(task_brief)

        # Track successful Executor mutations so we can write a compact,
        # deterministic facts snapshot for long-context grounding and resume.
        #
        # Policy: This avoids enumerating every parameter. We snapshot only:
        # - touched scene params (id/json_key) via GetParamValues
        # - touched timeline keys (id/json_key) via ListKeys
        # - camera key times (id=0) when camera keys were edited
        # - object list only when tools likely changed objects/visibility
        touched_scene_values: dict[int, set[str]] = {}
        touched_key_targets: dict[int, set[str]] = {}
        touched_key_times: dict[int, dict[str, set[float]]] = {}
        touched_camera_key_times: set[float] = set()
        touched_object_ids: set[int] = set()
        touched_mutation_tools: set[str] = set()
        touched_duration_seconds: float | None = None
        touched_set_time_seconds: float | None = None
        include_objects_in_snapshot = False
        include_camera_key_times_in_snapshot = False
        include_camera_interpolation_method_in_snapshot = False
        touched_camera_interpolation_method: str | None = None

        def _touch_scene_value(*, id: int, json_key: str) -> None:
            jk = str(json_key or "").strip()
            if not jk:
                return
            try:
                touched_scene_values.setdefault(int(id), set()).add(jk)
            except Exception:
                return

        def _touch_key(*, id: int, json_key: str, time: float | None = None) -> None:
            jk = str(json_key or "").strip()
            if not jk:
                return
            try:
                touched_key_targets.setdefault(int(id), set()).add(jk)
                if time is not None:
                    touched_key_times.setdefault(int(id), {}).setdefault(jk, set()).add(float(time))
            except Exception:
                return

        def _touch_camera_key_time(*, time: float) -> None:
            try:
                touched_camera_key_times.add(float(time))
            except Exception:
                return

        def _record_touched_from_tool(*, name: str, args: dict[str, Any], result: Any) -> None:
            nonlocal include_objects_in_snapshot
            nonlocal include_camera_key_times_in_snapshot
            nonlocal include_camera_interpolation_method_in_snapshot
            nonlocal touched_camera_interpolation_method
            nonlocal touched_duration_seconds
            nonlocal touched_set_time_seconds

            tool = str(name or "")
            if not tool:
                return

            # Only capture successful writes (ok!=False).
            ok = True
            if isinstance(result, dict) and "ok" in result:
                try:
                    ok = bool(result.get("ok"))
                except Exception:
                    ok = True
            if ok is False:
                return

            touched_mutation_tools.add(tool)

            if tool in {"scene_load_files", "scene_ensure_loaded", "scene_smart_load"}:
                include_objects_in_snapshot = True
                try:
                    for o in (result.get("objects") or []):
                        if isinstance(o, dict):
                            touched_object_ids.add(int(o.get("id", 0)))
                except Exception:
                    pass
                return

            if tool == "scene_make_alias":
                include_objects_in_snapshot = True
                try:
                    for a in (result.get("aliases") or []):
                        if not isinstance(a, dict):
                            continue
                        touched_object_ids.add(int(a.get("src_id", 0)))
                        touched_object_ids.add(int(a.get("alias_id", 0)))
                except Exception:
                    pass
                return

            if tool == "scene_set_visibility":
                include_objects_in_snapshot = True
                try:
                    for i in (args.get("ids") or []):
                        touched_object_ids.add(int(i))
                except Exception:
                    pass
                return

            if tool in {"scene_camera_fit", "scene_camera_apply"}:
                # Scene camera value (stateless) is always id=0/"Camera 3DCamera".
                _touch_scene_value(id=0, json_key="Camera 3DCamera")
                return

            if tool == "scene_apply":
                # Prefer canonical mapping returned by the tool (name→json_key resolution).
                applied = result.get("applied_set_params") if isinstance(result, dict) else None
                if isinstance(applied, list):
                    for it in applied:
                        if not isinstance(it, dict):
                            continue
                        try:
                            _touch_scene_value(id=int(it.get("id")), json_key=str(it.get("json_key") or ""))
                        except Exception:
                            continue
                    return
                # Fallback: only record explicit json_keys (names are not canonical here).
                try:
                    for it in (args.get("set_params") or []):
                        if not isinstance(it, dict):
                            continue
                        jk = it.get("json_key")
                        if isinstance(jk, str) and jk.strip():
                            _touch_scene_value(id=int(it.get("id")), json_key=jk)
                except Exception:
                    pass
                return

            if tool in {"scene_cut_set_box", "scene_cut_clear"}:
                # Cuts are global view-setting parameters (typically id=3) and
                # may also refit the camera (id=0).
                touched = result.get("touched_scene_values") if isinstance(result, dict) else None
                if isinstance(touched, list):
                    for it in touched:
                        if not isinstance(it, dict):
                            continue
                        try:
                            _touch_scene_value(id=int(it.get("id")), json_key=str(it.get("json_key") or ""))
                        except Exception:
                            continue
                return

            if tool == "animation_ensure_animation":
                # May create baseline camera keys / default state.
                include_camera_key_times_in_snapshot = True
                return

            if tool == "animation_camera_set_interpolation_method":
                include_camera_interpolation_method_in_snapshot = True
                try:
                    touched_camera_interpolation_method = str(args.get("method") or "").strip() or None
                except Exception:
                    touched_camera_interpolation_method = None
                # No camera keys are written by this tool directly.
                return

            if tool == "animation_set_duration":
                try:
                    touched_duration_seconds = float(args.get("seconds", 0.0))
                except Exception:
                    touched_duration_seconds = None
                return

            if tool == "animation_set_time":
                try:
                    touched_set_time_seconds = float(args.get("seconds", 0.0))
                except Exception:
                    touched_set_time_seconds = None
                return

            if tool in {"animation_set_key_param", "animation_set_param_by_name"}:
                try:
                    kid = int(args.get("id"))
                except Exception:
                    return
                jk = None
                if isinstance(result, dict):
                    jk = result.get("json_key")
                if not jk:
                    jk = args.get("json_key")
                if isinstance(jk, str) and jk.strip():
                    try:
                        tm = float(args.get("time", 0.0))
                    except Exception:
                        tm = None
                    _touch_key(id=kid, json_key=str(jk), time=tm)
                return

            if tool in {
                "animation_replace_key_param",
                "animation_remove_key_param_at_time",
                "animation_replace_key_param_at_times",
                "animation_remove_key",
            }:
                try:
                    kid = int(args.get("id"))
                except Exception:
                    return
                jk = args.get("json_key")
                if isinstance(jk, str) and jk.strip():
                    if tool == "animation_replace_key_param_at_times":
                        try:
                            times = [float(t) for t in (args.get("times") or [])]
                        except Exception:
                            times = []
                        _touch_key(id=kid, json_key=jk)
                        for t in times:
                            _touch_key(id=kid, json_key=jk, time=t)
                    else:
                        try:
                            tm = float(args.get("time", 0.0))
                        except Exception:
                            tm = None
                        _touch_key(id=kid, json_key=jk, time=tm)
                return

            if tool in {
                "animation_clear_keys",
                "animation_clear_keys_range",
                "animation_shift_keys_range",
                "animation_scale_keys_range",
                "animation_duplicate_keys_range",
            }:
                try:
                    kid = int(args.get("id"))
                except Exception:
                    return
                if kid == 0:
                    include_camera_key_times_in_snapshot = True
                    # Preserve any explicit times/ranges for resume/debug.
                    if tool == "animation_clear_keys_range":
                        try:
                            _touch_camera_key_time(time=float(args.get("t0", 0.0)))
                            _touch_camera_key_time(time=float(args.get("t1", 0.0)))
                        except Exception:
                            pass
                    return
                jk = args.get("json_key")
                if isinstance(jk, str) and jk.strip():
                    _touch_key(id=kid, json_key=jk)
                return

            if tool in {"animation_batch"}:
                # Batch contains explicit id/json_key/time entries (non-camera only).
                try:
                    for sk in (args.get("set_keys") or []):
                        if not isinstance(sk, dict):
                            continue
                        kid = int(sk.get("id"))
                        jk = sk.get("json_key")
                        if isinstance(jk, str) and jk.strip():
                            try:
                                tm = float(sk.get("time", 0.0))
                            except Exception:
                                tm = None
                            _touch_key(id=kid, json_key=jk, time=tm)
                    for rk in (args.get("remove_keys") or []):
                        if not isinstance(rk, dict):
                            continue
                        kid = int(rk.get("id"))
                        jk = rk.get("json_key")
                        if isinstance(jk, str) and jk.strip():
                            try:
                                tm = float(rk.get("time", 0.0))
                            except Exception:
                                tm = None
                            _touch_key(id=kid, json_key=jk, time=tm)
                except Exception:
                    pass
                return

            if tool in {"animation_replace_key_camera", "animation_camera_solve_and_apply"}:
                include_camera_key_times_in_snapshot = True
                try:
                    # animation_replace_key_camera: explicit single-time.
                    if tool == "animation_replace_key_camera":
                        _touch_camera_key_time(time=float(args.get("time", 0.0)))
                    # animation_camera_solve_and_apply: may report applied times.
                    if isinstance(result, dict) and isinstance(result.get("applied"), list):
                        for t in result.get("applied") or []:
                            _touch_camera_key_time(time=float(t))
                except Exception:
                    pass
                return

            if tool == "animation_camera_waypoint_spline_apply":
                include_camera_key_times_in_snapshot = True
                include_camera_interpolation_method_in_snapshot = True
                try:
                    if isinstance(result, dict):
                        m = result.get("current_method") or result.get("method")
                        if isinstance(m, str) and m.strip():
                            touched_camera_interpolation_method = m.strip()
                    if touched_camera_interpolation_method is None:
                        m = args.get("method")
                        if isinstance(m, str) and m.strip():
                            touched_camera_interpolation_method = m.strip()
                except Exception:
                    pass
                try:
                    if isinstance(result, dict) and isinstance(result.get("applied"), list):
                        for t in result.get("applied") or []:
                            _touch_camera_key_time(time=float(t))
                except Exception:
                    pass
                return

            if tool == "animation_camera_walkthrough_apply":
                include_camera_key_times_in_snapshot = True
                include_camera_interpolation_method_in_snapshot = True
                try:
                    if isinstance(result, dict):
                        m = result.get("current_method") or result.get("method")
                        if isinstance(m, str) and m.strip():
                            touched_camera_interpolation_method = m.strip()
                    if touched_camera_interpolation_method is None:
                        m = args.get("method")
                        if isinstance(m, str) and m.strip():
                            touched_camera_interpolation_method = m.strip()
                except Exception:
                    pass
                try:
                    if isinstance(result, dict) and isinstance(result.get("applied"), list):
                        for t in result.get("applied") or []:
                            _touch_camera_key_time(time=float(t))
                except Exception:
                    pass
                return

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
            # Phase safety: nested dispatches must not bypass the phase tool allowlist.
            ph = str(current_phase or "")
            if ph in {"Planner", "Verifier"}:
                if name in ATLAS_STATE_MUTATION_TOOLS:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": f"tool '{name}' is not allowed in phase={ph}",
                        }
                    )
                if name in CODEGEN_TOOLS:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": f"tool '{name}' is not allowed in phase={ph}",
                        }
                    )
                if name in ATLAS_OUTPUT_TOOLS and (
                    ph != "Verifier" or name not in ATLAS_VERIFICATION_OUTPUT_TOOLS
                ):
                    return json.dumps(
                        {
                            "ok": False,
                            "error": f"tool '{name}' is not allowed in phase={ph}",
                        }
                    )

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

            # Track successful Executor-side mutations for a post-write snapshot.
            try:
                if str(current_phase or "") == "Executor" and name in ATLAS_STATE_MUTATION_TOOLS:
                    _record_touched_from_tool(name=name, args=args_obj, result=res_obj)
            except Exception:
                pass
            return result_json

        # Ensure tool modules that chain via ctx.dispatch also go through our
        # logging/touch-tracking wrapper (Codex-style determinism).
        try:
            runtime_state["dispatch_proxy"] = _dispatch_logged
        except Exception:
            pass

        def _append_executor_facts_snapshot() -> None:
            """Append a compact post-write scene facts snapshot for this turn.

            This is intended for:
            - deterministic resume across context windows
            - grounding future turns without re-enumerating everything
            """
            if not touched_mutation_tools:
                return

            key_targets: dict[int, list[str]] = {}
            for oid, keys in touched_key_targets.items():
                if not keys:
                    continue
                try:
                    key_targets[int(oid)] = sorted({str(k) for k in keys if str(k).strip()})
                except Exception:
                    continue

            scene_value_targets: dict[int, list[str]] = {}
            for oid, keys in touched_scene_values.items():
                if not keys:
                    continue
                try:
                    scene_value_targets[int(oid)] = sorted({str(k) for k in keys if str(k).strip()})
                except Exception:
                    continue

            time_status: dict[str, float] | None = None
            try:
                ts = self.scene.get_time()
                time_status = {
                    "seconds": float(getattr(ts, "seconds", 0.0) or 0.0),
                    "duration": float(getattr(ts, "duration", 0.0) or 0.0),
                }
            except Exception:
                time_status = None

            snapshot_ok = True
            snapshot_error = ""
            facts: dict[str, Any] = {}
            try:
                facts = self.scene.scene_facts_compact(
                    key_targets=key_targets or None,
                    include_key_values=False,
                    scene_value_targets=scene_value_targets or None,
                    include_objects=bool(include_objects_in_snapshot),
                    include_camera_key_times=bool(include_camera_key_times_in_snapshot),
                )
            except Exception as e:
                snapshot_ok = False
                snapshot_error = str(e)
                facts = {}

            if include_camera_interpolation_method_in_snapshot:
                try:
                    m = self.scene.get_camera_interpolation_method()
                    if isinstance(m, str) and m.strip():
                        facts["camera_interpolation_method"] = m.strip()
                except Exception:
                    pass

            touched_payload: dict[str, Any] = {
                "mutation_tools": sorted(touched_mutation_tools),
                "scene_value_targets": {
                    str(oid): sorted(list(keys))
                    for oid, keys in sorted(touched_scene_values.items(), key=lambda kv: int(kv[0]))
                    if keys
                },
                "key_targets": {
                    str(oid): sorted(list(keys))
                    for oid, keys in sorted(touched_key_targets.items(), key=lambda kv: int(kv[0]))
                    if keys
                },
                "key_times": {
                    str(oid): {
                        str(jk): sorted([float(t) for t in times])
                        for jk, times in sorted(per_key.items(), key=lambda kv: str(kv[0]))
                    }
                    for oid, per_key in sorted(touched_key_times.items(), key=lambda kv: int(kv[0]))
                    if isinstance(per_key, dict) and per_key
                },
            }
            if touched_camera_key_times:
                touched_payload["camera_key_times"] = sorted([float(t) for t in touched_camera_key_times])
            if touched_camera_interpolation_method is not None:
                touched_payload["camera_interpolation_method"] = touched_camera_interpolation_method
            if touched_object_ids:
                touched_payload["object_ids"] = sorted([int(i) for i in touched_object_ids])
            if touched_duration_seconds is not None:
                touched_payload["duration_seconds"] = float(touched_duration_seconds)
            if touched_set_time_seconds is not None:
                touched_payload["set_time_seconds"] = float(touched_set_time_seconds)

            ev: dict[str, Any] = {
                "type": "facts_snapshot",
                "turn_id": turn_id,
                "phase": "Executor",
                "ok": bool(snapshot_ok),
                "touched": touched_payload,
                "time_status": time_status,
                "facts": facts,
            }
            if snapshot_error:
                ev["error"] = snapshot_error
            try:
                self.session_store.append_event(ev)
            except Exception:
                pass

        # Shared per-phase grounding: keep this in instructions (not input items) so any
        # context trimming can drop old history/tool items without losing core policy.
        shared_instructions_parts: list[str] = []
        shared_instructions_parts.append(env_text)
        if context_blocks:
            shared_instructions_parts.append("Shared context:\n" + "\n\n".join(context_blocks))
        shared_instructions = "\n\n".join([p for p in shared_instructions_parts if p]).strip()

        # Build Responses API input items (local history; no server-side state).
        input_items: list[dict[str, Any]] = []
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

        phase_hit_round_budget_phase: str | None = None

        def _run_phase(
            *,
            phase: str,
            instructions: str,
            phase_tools: list[dict[str, Any]],
            phase_input_items: list[dict[str, Any]],
            max_rounds: int,
        ):
            nonlocal current_phase, phase_hit_round_budget_phase
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
                tool_name = str(name or "")
                if tool_name not in {"animation_render_preview", "scene_screenshot"}:
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

                mime = _mime_for_model_image(p)
                if not mime:
                    return [
                        _message(
                            role="user",
                            text=(
                                "Preview image format is not supported for model upload.\n"
                                f"- path: {path}\n"
                                f"- suffix: {p.suffix!r}\n"
                                "- allowed: .png, .jpg/.jpeg, .webp, .gif\n"
                                "Please re-render the screenshot as PNG (preferred) at a smaller width/height if needed."
                            ),
                        )
                    ]
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
                    f"- tool: {tool_name}\n"
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

            # Capture streamed reasoning deltas so we can persist them even when the
            # request fails mid-stream (e.g., provider network drops / 400s after some
            # output). The phase-level summaries returned by run_responses_tool_loop are
            # still the source of truth when available.
            streamed_summary_parts: dict[int, list[str]] = {}

            def _capture_reasoning_delta(delta: str, summary_index: int) -> None:
                if delta:
                    streamed_summary_parts.setdefault(int(summary_index or 0), []).append(delta)
                if callbacks is not None and callbacks.on_reasoning_summary_delta is not None:
                    callbacks.on_reasoning_summary_delta(delta, summary_index)

            def _capture_reasoning_part_added(summary_index: int) -> None:
                if callbacks is not None and callbacks.on_reasoning_summary_part_added is not None:
                    callbacks.on_reasoning_summary_part_added(summary_index)

            def _persist_reasoning_summary_complete(summary_text: str, call_index: int) -> None:
                """Persist reasoning summaries in chronological order.

                This is called by the tool loop after the Responses stream completes
                (so we have the full assembled reasoning summary), and BEFORE any
                tool calls from that response are executed. This preserves the same
                ordering as the terminal output: reasoning summary first, then tools.
                """

                txt = (summary_text or "").strip()
                if not txt:
                    return
                try:
                    self.session_store.append_event(
                        {
                            "type": "reasoning_summary",
                            "turn_id": turn_id,
                            "phase": phase,
                            "call_index": int(call_index),
                            "summaries": [txt],
                        }
                    )
                except Exception:
                    pass

            phase_callbacks = ToolLoopCallbacks(
                on_reasoning_summary_delta=_capture_reasoning_delta,
                on_reasoning_summary_part_added=_capture_reasoning_part_added,
                on_reasoning_summary_complete=_persist_reasoning_summary_complete,
                on_assistant_text_delta=(callbacks.on_assistant_text_delta if callbacks else None),
                on_tool_call=(callbacks.on_tool_call if callbacks else None),
                on_tool_result=(callbacks.on_tool_result if callbacks else None),
            )

            if callbacks is not None and callbacks.on_phase_start is not None:
                callbacks.on_phase_start(phase)
            try:
                result = run_responses_tool_loop(
                    llm=self.llm,
                    instructions=(instructions.rstrip() + "\n\n" + shared_instructions).strip(),
                    input_items=phase_input_items,
                    tools=phase_tools,
                    dispatch=_dispatch_logged,
                    post_tool_output=_post_tool_output,
                    callbacks=phase_callbacks,
                    temperature=self.temperature,
                    reasoning_effort=self.reasoning_effort,
                    max_rounds=max_rounds,
                )
            except ToolLoopNonConverged as e:
                phase_hit_round_budget_phase = str(phase)
                try:
                    self.session_store.append_event(
                        {
                            "type": "tool_loop_nonconverged",
                            "turn_id": turn_id,
                            "phase": phase,
                            "max_rounds": int(e.max_rounds),
                            "rounds_completed": int(e.rounds_completed),
                            "message": str(e),
                        }
                    )
                except Exception:
                    pass

                # Forced finalization: produce a user-facing progress update without tools.
                # This avoids surfacing a raw "Agent error" while preserving correctness:
                # users can say "continue" and the session log contains all prior tool work.
                try:
                    plan = self.session_store.get_plan() or []
                except Exception:
                    plan = []
                plan_lines: list[str] = []
                for it in plan:
                    if not isinstance(it, dict):
                        continue
                    st = str(it.get("status") or "").strip() or "pending"
                    step = str(it.get("step") or "").strip()
                    if not step:
                        continue
                    plan_lines.append(f"- {st}: {step}")
                plan_text = "\n".join(plan_lines) if plan_lines else "(no plan recorded)"

                # Keep the finalize prompt short and operational; do not ask for extra info.
                finalize_instructions = (
                    "You are Atlas Agent.\n"
                    "The previous tool-using loop reached its round budget while the model was still returning tool calls.\n"
                    "You must now STOP calling tools and produce a progress update to the user.\n\n"
                    "Requirements for your message:\n"
                    "- Summarize what has already been done (based on the tool outputs in the conversation).\n"
                    "- State what remains to be done, referencing the current plan.\n"
                    "- Provide one clear next step: tell the user to reply with 'continue' to resume execution.\n"
                    "- Do not call tools.\n"
                    "- Do not invent unknown ids/keys/values.\n\n"
                    + ATLAS_SHARED_SYSTEM_RULES
                )
                finalize_items = list(e.input_items or [])
                finalize_items.append(
                    _message(
                        role="user",
                        text=(
                            "INTERNAL: Tool-loop round budget reached; produce a progress update and stop.\n\n"
                            "Current plan:\n"
                            f"{plan_text}"
                        ),
                    )
                )

                final_summary_parts: dict[int, list[str]] = {}

                def _on_finalizer_event(ev: dict[str, Any]) -> None:
                    et = str(ev.get("type") or "")
                    if et == "response.reasoning_summary_text.delta":
                        delta = str(ev.get("delta") or "")
                        try:
                            summary_index = int(ev.get("summary_index", 0))
                        except Exception:
                            summary_index = 0
                        final_summary_parts.setdefault(summary_index, []).append(delta)
                        if callbacks is not None and callbacks.on_reasoning_summary_delta is not None:
                            callbacks.on_reasoning_summary_delta(delta, summary_index)
                        return
                    if et == "response.reasoning_summary_part.added":
                        try:
                            summary_index = int(ev.get("summary_index", 0))
                        except Exception:
                            summary_index = 0
                        if callbacks is not None and callbacks.on_reasoning_summary_part_added is not None:
                            callbacks.on_reasoning_summary_part_added(summary_index)
                        return

                resp = self.llm.responses_stream(
                    instructions=(finalize_instructions.rstrip() + "\n\n" + shared_instructions).strip(),
                    input_items=finalize_items,
                    tools=None,
                    temperature=self.temperature,
                    parallel_tool_calls=False,
                    reasoning_effort=self.reasoning_effort,
                    reasoning_summary="detailed",
                    text_verbosity="high",
                    on_event=_on_finalizer_event,
                )

                # Persist finalizer reasoning summary (after prior tool events).
                try:
                    merged = "\n\n".join(
                        "".join(final_summary_parts[k]) for k in sorted(final_summary_parts.keys())
                    ).strip()
                    if merged:
                        self.session_store.append_event(
                            {
                                "type": "reasoning_summary",
                                "turn_id": turn_id,
                                "phase": phase,
                                "call_index": int(e.rounds_completed),
                                "summaries": [merged],
                                "finalizer": True,
                            }
                        )
                except Exception:
                    pass

                # Extract assistant text from finalizer response.
                assistant_text_chunks: list[str] = []
                out_items = resp.get("output")
                if isinstance(out_items, list):
                    for it in out_items:
                        if not isinstance(it, dict):
                            continue
                        if str(it.get("type") or "") != "message":
                            continue
                        if str(it.get("role") or "") != "assistant":
                            continue
                        content = it.get("content") or []
                        if not isinstance(content, list):
                            continue
                        for part in content:
                            if not isinstance(part, dict):
                                continue
                            if str(part.get("type") or "") != "output_text":
                                continue
                            t = part.get("text")
                            if isinstance(t, str) and t:
                                assistant_text_chunks.append(t)
                final_text = "".join(assistant_text_chunks).strip() or "(no response)"

                # Return a synthetic ToolLoopResult so the caller can continue the turn.
                # We append the assistant message into the local input_items history so
                # subsequent phases (if any) see the finalizer output.
                final_items = list(e.input_items or [])
                final_items.append(_message(role="assistant", text=final_text))
                result = ToolLoopResult(
                    assistant_text=final_text,
                    reasoning_summaries=list(e.reasoning_summaries or []),
                    tool_calls=list(e.tool_calls or []),
                    input_items=final_items,
                )
            except Exception as e:
                # Persist any streamed reasoning summary we saw before the failure.
                merged_streamed = "\n\n".join(
                    "".join(streamed_summary_parts[k]) for k in sorted(streamed_summary_parts.keys())
                ).strip()
                if merged_streamed:
                    try:
                        self.session_store.append_event(
                            {
                                "type": "reasoning_summary",
                                "turn_id": turn_id,
                                "phase": phase,
                                "summaries": [merged_streamed],
                                "partial": True,
                                "error": str(e),
                            }
                        )
                    except Exception:
                        pass
                raise
            if callbacks is not None and callbacks.on_phase_end is not None:
                callbacks.on_phase_end(phase)
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
            if phase_hit_round_budget_phase == "Planner":
                final_result = planner_res
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
                max_rounds=int(self.max_rounds_executor),
            )
            # Deterministic post-write facts snapshot: store what changed in this turn
            # so future turns can ground without re-enumerating everything.
            _append_executor_facts_snapshot()
            phase_input = list(exec_res.input_items)
            if phase_hit_round_budget_phase == "Executor":
                # If we had to force-finalize due to tool-loop non-convergence, stop this
                # turn early (do not run Verifier). The session log contains all prior
                # tool work; the user can reply "continue" to resume with full context.
                final_result = exec_res
            else:
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
                atlas_version=self.atlas_version,
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
    reasoning_effort: str | None = "high",
    max_rounds: int = DEFAULT_EXECUTOR_MAX_ROUNDS,
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
        reasoning_effort=reasoning_effort,
        max_rounds_executor=int(max_rounds),
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
