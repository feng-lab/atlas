from __future__ import annotations

import base64
import hashlib
import json
import logging
import re
import sys
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from .agent_team.base import LLMClient
from .agent_team.intent_resolver import INTENT_RESOLVER_SYSTEM, IntentResolver
from .agent_team.tools_agent import scene_tools_and_dispatcher
from .capabilities_prompt import build_atlas_agent_primer, build_capabilities_prompt
from .defaults import (
    AUTO_RETRIEVE_MAX_CHARS,
    AUTO_RETRIEVE_MAX_SNIPPETS,
    AUTO_RETRIEVE_MODE,
    AUTO_RETRIEVE_NEEDLE_MAX_TOKENS,
    AUTO_RETRIEVE_RECENT_WRITE_EVENTS,
    CONTEXT_COMPACTION_KEEP_TAIL_ITEMS,
    CONTEXT_COMPACTION_RECENT_TOOL_EVENTS,
    DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR,
    DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR,
    DEFAULT_EXECUTOR_MAX_ROUNDS,
    DEFAULT_PLANNER_MAX_ROUNDS,
    DEFAULT_WEB_SEARCH_MODE,
    INTENT_RESOLVER_SCENE_SNAPSHOT_MAX_CHARS,
    MAX_PREVIEW_IMAGE_INLINE_BYTES_FOR_MODEL,
    SESSION_MAX_RECENT_MESSAGES,
    SESSION_MEMORY_COMPACTION_MODE,
    SESSION_MEMORY_RECENT_WRITE_EVENTS,
)
from .discovery import discover_schema_dir
from .llm_usage import (
    LlmUsageDelta,
    LlmUsageTotals,
    extract_usage_delta_from_response,
)
from .responses_tool_loop import (
    ToolLoopCallbacks,
    ToolLoopNonConverged,
    ToolLoopResult,
    run_responses_tool_loop,
)
from .scene_rpc import SceneClient
from .session_store import SessionStore

# Internal runtime policy (intentionally not user-configurable).
#
# Rationale: The agent needs stable behavior across sessions and machines; we
# avoid hidden env/flag tuning knobs. When these need changes, update constants
# in `defaults.py` and keep on-disk session formats stable.


ATLAS_STATE_MUTATION_TOOLS: set[str] = {
    # Scene writes
    "scene_apply",
    "scene_camera_fit",
    "scene_camera_apply",
    "scene_load_sources",
    "scene_start_load_task",
    "scene_smart_load",
    "scene_set_visibility",
    "scene_remove_objects",
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
    # Declares the run blocked and short-circuits further execution; treat as output.
    # (Allowed in Executor, disallowed in Planner.)
    "report_blocked",
}

ARTIFACT_WRITE_TOOLS: set[str] = {
    "artifact_write_text",
    "artifact_write_json",
    "artifact_write_bytes_base64",
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
    "- Camera target selection: for establishing shots, ids may be the full relevant set (fit_candidates). For close-ups/highlight beats, choose ids as ONLY the highlighted objects (small subset) so framing can be tight; do not keep_visible-frame the whole scene by default.\n"
    "- Camera framing constraints: keep_visible=true enforces no cropping (target bbox stays fully within frame). For tight shots, set min_frame_coverage>0 (0..1 dominant-dimension fill). For interior/exploration, set keep_visible=false to skip framing validation. Keep margin small unless the user explicitly wants extra padding.\n"
    "- Relative dolly (no world-unit guessing): prefer animation_camera_walkthrough_apply with look_at_policy='bbox_center' and bbox-scaled move.forward/back (camera_move_local distance_is_fraction_of_bbox_radius=true) instead of DOLLY.\n"
    "- Timeline continuity: when chaining camera segments, do NOT overwrite the shared boundary time with a different pose using easing='Switch' unless you want a deliberate jump cut. For holds, use mode='STATIC' at the boundary; for reframes, create an explicit transition segment.\n"
    "- Camera director rubric (routing): if the user provides explicit waypoints/points/beats, use animation_camera_waypoint_spline_apply; otherwise if the user describes motion verbs (fly/strafe/yaw/pitch/pause), use animation_camera_walkthrough_apply. Mixed prompts: do not drop waypoints/segments; add intermediate points or increase walkthrough step_seconds instead of truncating.\n"
    "- Camera director rubric (defaults): walkthrough constraints default keep_visible=false unless the user explicitly wants framing; step_seconds defaults: slow≈0.5, medium≈1.0, fast≈1.5–2.0. For sparse waypoints, add intermediate waypoints instead of relying on interpolation modes.\n"
    "- Walkthrough planning: when inventing segments from words, you may use internal segment templates (template+amount/distance/degrees) and let the tool expand them; do not require the user to name templates.\n"
    "- File paths: classify input. Absolute/~/drive paths are exact (fs_check_paths expands ~ and env vars; if missing try fs_resolve_path). Natural-language hints use fs_hint_resolve with structured args (expected_name + possible_dirs). Always verify before loading.\n"
    "- Maintain an explicit plan via the update_plan tool. Choose a step breakdown that keeps execution manageable and easy to verify. Exactly one step may be in_progress.\n"
    "- Verify changes after applying: scene_get_values / animation_list_keys / animation_camera_validate, etc.\n"
    "- Avoid blocking on missing user intent/input; choose defaults and proceed. Use report_blocked only for technical/capability blocks (RPC down, tool missing, preconditions cannot be satisfied, etc.).\n"
    "- For unclear workflow/semantics, consult docs via docs_search and docs_read (AGENTS_GUIDE.md, SCENE_SERVER.md).\n"
    "- Tool-call arguments must be STRICT JSON (double-quoted keys/strings; lowercase true/false/null; no trailing commas).\n\n"
    '- Never embed tool calls or raw JSON blobs in assistant text (e.g., {"tool": ...} or {"plan": ...}). If you need to update the plan or verification, CALL THE TOOL.\n\n'
    "Personality:\n"
    "- Be calm, friendly, and direct.\n"
    "- Be transparent about uncertainty and verification; don’t bluff.\n"
    "- Optimize for the user’s outcome, not for a verbose report.\n"
    "- When aesthetics are underspecified (camera feel, pacing, framing), propose 1–2 tasteful options and pick one by default.\n\n"
    "Ambition vs precision:\n"
    "- If the user specifies exact constraints (duration, angles, times, filenames, paths), follow them exactly.\n"
    "- If the user leaves details unspecified, choose sensible defaults that produce a high-quality result.\n"
    "  - State those choices briefly as assumptions and continue (do not stall on questions).\n"
    "- For creative visualization/animation tasks, you may add small quality improvements that do not change intent\n"
    "  (e.g., smoother motion via higher key density, reasonable easing, better framing), and mention them briefly.\n\n"
    "User-facing responses:\n"
    "- Write like a helpful teammate: concise, direct, and friendly.\n"
    "- Scale detail with task complexity and the user’s request.\n"
    "- Default format: outcome → key details → how to view/next action → optional tweaks.\n"
    "- Avoid mechanical reports (e.g., “## 1) …”). Use headings only when they improve clarity or the user asked for a log.\n"
    "- Prefer user-facing descriptions of behavior over internal details (ids/json_keys/option labels).\n"
    "  - Include internal ids/paths only when they materially help reproduction/debug/tweaks, or when the user asked.\n"
    "- Verification: summarize evidence (tool read-back and/or preview). Do not paste long tool outputs.\n"
    "- If something cannot be verified via tools, say so and record it as a visual/human check (do not speculate).\n"
    "- Detailed report mode (only when asked for details/logs, e.g. “what happened”, “show details”, “show your work”, “full report/log”):\n"
    "  - Summary (brief)\n"
    "  - What changed (key actions; avoid dumping raw tool calls)\n"
    "  - How to view / where to find it (scene/animation names, how to play)\n"
    "  - Verification (what you checked, via tool/visual/human; call out any gaps)\n"
    "  - Assumptions + knobs (what you assumed, and the easiest tweaks the user can request)\n"
    "  - Technical details (ids/paths/json_keys) only if the user asked or it helps debugging\n"
)

ATLAS_PLANNER_SYSTEM_PROMPT = (
    "PHASE: Planner\n"
    "You must NOT change the Atlas scene/timeline in this phase.\n"
    "Allowed actions: read-only inspection tools, docs lookup, and update_plan.\n"
    "Important: The Planner phase tool list is intentionally LIMITED (read-only). It may not include file import or animation-key authoring tools.\n"
    "Do NOT treat missing write tools in this phase as a blocker; assume the Executor phase has the full write-capable toolset.\n"
    "Do NOT ask the user clarifying questions in this phase; instead, encode defaults as assumptions in your plan explanation.\n\n"
    "Planning rules for long animations:\n"
    "- If the Task Brief implies a long animation (roughly >20s) or multiple distinct beats, strongly prefer planning in timeline segments.\n"
    "- Segment size guideline: 5–12 seconds per segment (pick what best matches the pacing and number of beats).\n"
    '- Each segment plan step MUST include an explicit time range in the step text, e.g. "[0–12s] Establishing shot".\n'
    "- Keep each segment atomic: a single objective + the camera/visibility intent for that range.\n"
    "- For each segment, set verification requirements appropriate to the segment. Prefer tool read-backs; add visual or human checks when needed for framing/continuity.\n"
    '- Avoid a single step like "Create the whole animation"; this causes unmanageable key counts and hard-to-debug continuity issues.\n\n'
    "Format:\n"
    "- You will be given a TASK BRIEF in the shared context. Treat it as authoritative; do not rewrite or reinterpret it.\n"
    "- Otherwise: update the plan via update_plan. Choose a number of steps that makes the work easy to execute and verify; long animations may require more steps.\n\n"
    "Verification requirements:\n"
    "- After update_plan, call verification_set_requirements for each plan step_id.\n"
    "- Use policy.all_of groups to express multi-mode verification. Common patterns:\n"
    '  • Tool-only: all_of=[{any_of:["tool"], description:"..."}]\n'
    '  • Tool AND (Visual OR Human): all_of=[{any_of:["tool"]},{any_of:["visual","human"]}]\n'
    "- Prefer a single plan step with a multi-mode verification policy rather than splitting into multiple steps unless it improves clarity.\n\n"
    + ATLAS_SHARED_SYSTEM_RULES
    + "\n\nTask brief format (reference):\n"
    + INTENT_RESOLVER_SYSTEM
)

ATLAS_PLAN_REFINER_SYSTEM_PROMPT = (
    "PHASE: PlanRefiner\n"
    "You must NOT change the Atlas scene/timeline in this phase.\n"
    "Allowed actions: read-only inspection tools, docs lookup, and update_plan.\n\n"
    "Goal:\n"
    "- Ensure the current plan is executable without a single huge animation-authoring step.\n"
    "- Provide a conservative second opinion: when the TASK BRIEF leaves room, you may insert ONE short aesthetic beat segment.\n"
    "- If the plan is already good and no meaningful refinement is needed, do NOT call update_plan.\n\n"
    "Plan refinement rules:\n"
    "- Start by calling session_get_plan(include_explanation=true) to see the current durable plan.\n"
    "- If the plan contains a broad step that would require authoring an entire long animation in one go,\n"
    "  rewrite it into ~5–12s timeline segments with explicit time ranges in each step.\n"
    "- If the plan is already segmented and coherent, you MAY insert at most ONE additional short segment to improve\n"
    "  the overall aesthetic quality and viewing experience (pacing is one common reason, but not the only one), but ONLY when:\n"
    "  - it is consistent with the TASK BRIEF and does not contradict user constraints, and\n"
    "  - the task brief leaves room for creative sequencing/aesthetic choices.\n"
    "  If you insert a segment, adjust neighboring time ranges so the overall timeline remains coherent.\n"
    "- Preserve completed steps as completed when rewriting; keep at most one in_progress.\n"
    "- When you update the plan, immediately call verification_set_requirements for each step_id.\n"
    "  For animation segments, prefer tool verification; add visual or human verification when needed for framing/continuity.\n\n"
    "Guardrails:\n"
    "- Do NOT instruct the Executor how to implement steps (no tool choices, no parameter knobs, no preview timepoints).\n"
    "- Do NOT add requirements that force screenshots or additional permissions.\n"
    "- Only change the plan when it meaningfully improves executability or the overall aesthetic quality.\n\n"
    + ATLAS_SHARED_SYSTEM_RULES
)

ATLAS_EXECUTOR_SYSTEM_PROMPT = (
    "PHASE: Executor\n"
    "Execute the plan by calling tools. Prefer discovery; do not guess.\n\n"
    + ATLAS_SHARED_SYSTEM_RULES
    + "\n\nExecutor playbook:\n"
    + "\n".join(
        [
            "- Treat the current plan as the source of truth; execute it step-by-step. If there is no plan yet, create one via update_plan.",
            "- Persistence: keep going until the Task Brief is fulfilled and you have verified what you can within this run. Do not stop at partial progress unless blocked.",
            "- If you create a new plan (or materially rewrite it), immediately call verification_set_requirements for each step_id so verification is explicit.",
            "- At the start of execution (or before verifying a step), call verification_get(include_plan=true) so you know the current verification requirements and statuses.",
            "- Derive intent strictly from the Task Brief in the shared context. If the brief conflicts with the latest user message, stop and ask one focused question.",
            "- Prefer live discovery for ids/json_keys/options: scene_list_objects → scene_list_params(id) → scene_get_values(id,json_keys). Do not guess.",
            "- For unclear semantics/workflows, consult docs via docs_search/docs_read (SCENE_SERVER.md, AGENTS_GUIDE.md, USER_GUIDE.md).",
            "- Long sessions: if the user references prior decisions, use session_get_memory/session_get_plan or session_search_transcript for exact quotes.",
            "- File paths (exact): when the user provides an absolute/~/drive path token (e.g. /Users/... , ~/... , C:\\...), treat it as exact. Use fs_check_paths (expands ~ and env vars) first; only if missing try fs_resolve_path (typo correction). Avoid fs_hint_resolve in this case.",
            "- File paths (hints): when the user describes location in words (e.g. “in my Documents/atlas_test”), use system_info to resolve common dirs and then call fs_hint_resolve with structured args: expected_name (basename to score) + possible_dirs (likely search roots). If match!='exact', validate by reading/checking the file before loading.",
            "- Scene vs timeline contract: any mention of time/duration implies animation_* tools. Never include time/easing in scene_apply.",
            "- Scene-only intent: do not write animation keys. Use scene_apply/scene_set_visibility and camera ops: scene_camera_fit (fit+apply) or scene_camera_apply(value=...) (apply a typed camera from camera_*). Verify with scene_get_values.",
            "- Animation intent: animation_ensure_animation → animation_set_duration → write keys (animation_batch / animation_set_key_param / animation_replace_key_param) and camera keys (animation_camera_solve_and_apply / animation_replace_key_camera).",
            "- Animation determinism: animations should define the FULL state. Ensure a baseline keyframe exists at t=0; if you load/add objects after creating the animation, call animation_save_keyframe(animation_id, time=0) so those new objects do not fall back to scene values during playback.",
            "- Animation authoring (UI parity): for many tasks, it can be faster and more predictable to author beats by editing the scene state at time t and calling animation_save_keyframe(animation_id, time=t) than by writing dozens of individual keys; then refine the parameters that need extra tuning.",
            "- Camera workflow: use camera_* producers (camera_focus / camera_point_to / camera_rotate / camera_reset_view) to compute typed camera values, then apply via scene_camera_apply(value=...) for scene-only or animation_* camera tools for timeline work.",
            "- Validate before committing where possible (scene_validate_params; animation_camera_validate). Verify after writing (animation_list_keys, animation_camera_validate, scene_get_values).",
            "- Large-file handling: prefer fs_tail_lines/fs_search_text; if you must scan, iterate fs_read_text in windows until EOF (no silent truncation).",
            "- Verification ledger: satisfy the current step’s verification policy using read-back tools and/or screenshots; record outcomes via verification_record(mode=tool|visual|human).",
            "- Plan bookkeeping: after completing a step, update_plan to mark it completed and advance the next step to in_progress. Near the end, call verification_eval_plan and do a final update_plan pass to ensure statuses are consistent.",
            "- Long animations: never author an entire long animation in a single plan step. If a step is vague or spans too much time, STOP and call update_plan to split it into ~5–12s timeline segments with explicit [t0–t1] ranges before writing keys.",
            "- Segment workflow: for each [t0–t1] segment, author keys only within that range, then validate framing/continuity using representative checks (tool read-backs and, when appropriate, a small number of preview frames). Fix within the segment before moving on.",
            "- Visual checks: prefer scene_screenshot for current scene state; use animation_render_preview only when you need a specific animation time. If screenshots are allowed for this session, do NOT ask the user again—just call the tool and inspect the image. Never ask the user to open temp screenshot files; if a human check is needed, ask them to check in the Atlas UI.",
            "- If a plan step cannot be verified from tools alone and screenshots are unavailable/ambiguous, keep the step pending and request a human-check (in the Atlas UI). Do not mark fail without concrete contradictory evidence.",
            "- If a validation/read-back check fails, diagnose precisely (id/json_key/time/value) and retry within this run before giving up.",
            "- If a requested step is not feasible with the available tools/capabilities, complete all feasible steps first, then call report_blocked once with a precise reason and suggestion.",
            "- Completion contract: do not stop while there are plan steps that can be executed/verified with available tools. When you do stop, produce a user-facing recap. Default to a concise recap, but include more explanation when the task is complex or the user asked for details. Include what you did, how to view it, what you verified (tool/screenshot/human), any assumptions/trade-offs, and what remains (if anything).",
        ]
    )
)

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


def _guess_model_effective_input_budget_tokens(model: str) -> int | None:
    """Best-effort effective *input* token budget guess from model name.

    This is only used for proactive prompt-budget compaction when the provider
    does not expose model token limits (total context window and/or max output).

    If the provider later returns an explicit "maximum context length is N tokens"
    error, we persist that value and prefer it over this guess.
    """

    m = (model or "").strip().lower()
    if not m:
        return None
    # Very old / small-window models
    if m.startswith("gpt-3.5"):
        return 16_384
    # Modern OpenAI-style naming families (best-effort; update as needed)
    if m.startswith(("gpt-4o", "gpt-4.1", "gpt-4")):
        return 128_000
    if m.startswith("gpt-5"):
        # GPT-5 family defaults to a larger context window than GPT-4o.
        return 272_000
    if m.startswith(("o3", "o4")):
        return 128_000
    # Unknown provider/model: fall back to a reasonable modern default.
    return 128_000


@dataclass
class ChatTeam:
    address: str
    api_key: str
    model: str
    wire_api: str = "auto"
    web_search_mode: str = DEFAULT_WEB_SEARCH_MODE
    temperature: float | None = None
    max_rounds_planner: int = DEFAULT_PLANNER_MAX_ROUNDS
    max_rounds_executor: int = DEFAULT_EXECUTOR_MAX_ROUNDS
    ephemeral_inline_images: bool = False
    atlas_dir: Optional[str] = None
    atlas_version: Optional[str] = None
    session: Optional[str] = None
    session_dir: Optional[str] = None
    enable_codegen: bool = False
    reasoning_effort: str | None = field(kw_only=True)
    reasoning_summary: str | None = field(kw_only=True)
    text_verbosity: str | None = field(kw_only=True)

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

        # Rolling (in-memory) token usage totals for this session. We persist a copy
        # into session meta so the CLI can show totals without scanning session.jsonl.
        self._llm_session_usage = LlmUsageTotals()
        # Per-turn/per-call live stats for terminal display (initialized here so the
        # console can treat these as hard-contract attributes).
        self._llm_turn_usage_current = LlmUsageTotals()
        self._llm_last_call_usage: LlmUsageDelta | None = None
        self._llm_last_call_index: int | None = None
        self._llm_last_call_phase: str | None = None
        self._llm_last_call_gateway_model: str | None = None
        try:
            meta = self.session_store.get_meta()
            loaded = LlmUsageTotals.from_dict(meta.get("llm_session_usage"))
            if loaded is not None:
                self._llm_session_usage = loaded
        except Exception:
            pass

        self._history: list[tuple[str, str]] = []
        # Rolling (in-memory) marker to avoid re-running PlanRefiner on every turn
        # when only plan statuses change. This is not persisted on disk.
        self._plan_refiner_last_steps_sig: str | None = None

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

        self.llm = LLMClient(
            api_key=self.api_key, model=self.model, wire_api=self.wire_api
        )
        self.intent_resolver = IntentResolver(
            client=self.llm, temperature=self.temperature
        )
        # Live (in-memory) pointer to the currently edited Animation3D object.
        # Note: ids are not stable across Atlas runs; this is used only within
        # a single running app instance for convenience and deterministic tool calls.
        self._current_animation_id: int | None = None

        # Build capabilities context derived from atlas_dir or defaults.
        self._context: str = build_atlas_agent_primer()
        try:
            schema_dir, _ = discover_schema_dir(
                user_schema_dir=None, atlas_dir=self.atlas_dir
            )
            if schema_dir:
                self._context = build_capabilities_prompt(
                    schema_dir,
                    codegen_enabled=bool(self.enable_codegen),
                )
        except Exception:
            self._context = build_atlas_agent_primer()

        # Always include a short docs/tooling hint (docs are shipped inside the Atlas app bundle).
        self._context = (
            (self._context or "").rstrip()
            + "\n\n"
            + "\n".join(
                [
                    "Docs (runtime): use docs_search/docs_read to look up Atlas behavior and RPC/tool contracts.",
                    "Key docs: AGENTS_GUIDE.md, SCENE_SERVER.md, USER_GUIDE.md, DEVELOPER_GUIDE.md.",
                ]
            )
        )

        # Persist session meta for easy resume.
        try:
            self.session_store.set_meta(
                address=self.address,
                model=self.model,
                wire_api=self.wire_api,
                web_search_mode=str(self.web_search_mode or DEFAULT_WEB_SEARCH_MODE),
                temperature=self.temperature,
                reasoning_effort=self.reasoning_effort,
                reasoning_summary=self.reasoning_summary,
                text_verbosity=self.text_verbosity,
                max_rounds_planner=int(self.max_rounds_planner),
                max_rounds_executor=int(self.max_rounds_executor),
                atlas_dir=self.atlas_dir,
                atlas_version=self.atlas_version,
            )
            self.session_store.save()
        except Exception:
            pass

        # Track the actual model name returned by the OpenAI-compatible gateway
        # (some gateways silently reroute requests).
        self._gateway_model_last: str | None = None
        try:
            meta = self.session_store.get_meta()
            gm = meta.get("gateway_model_last")
            if isinstance(gm, str) and gm.strip():
                self._gateway_model_last = gm.strip()
        except Exception:
            self._gateway_model_last = None

        # Best-effort: restore the session's internal animation autosave.
        #
        # The Animation3D object id is not stable across Atlas runs, so we recover
        # it by loading the autosaved .animation3d file and then mapping it back
        # to the newly created in-app object id.
        try:
            autosave_path = self._animation_autosave_path()
            if autosave_path.exists():
                # Load the autosaved animation into the live Atlas doc, then recover
                # the new in-app object id by matching on the absolute path.
                #
                # Notes:
                # - This uses the unified load_sources path (task API) for both local
                #   and network sources.
                # - We do not wait for "object ready" here: Animation3D is not a
                #   visual object and does not need a bound 3D view/filter.
                self.scene.load_sources(
                    [str(autosave_path)], set_visible=False, wait_ready=False
                )
                objs = self.scene.list_objects()
                for o in getattr(objs, "objects", []):
                    try:
                        if str(getattr(o, "type", "") or "") != "Animation3D":
                            continue
                        op = str(getattr(o, "path", "") or "").strip()
                        if not op:
                            continue
                        if Path(op).expanduser().resolve() == autosave_path.resolve():
                            self._current_animation_id = int(getattr(o, "id", 0) or 0)
                            break
                    except Exception:
                        continue
        except Exception:
            pass

    def _animation_autosave_path(self) -> Path:
        """Per-session internal animation autosave location."""
        return self.session_store.root / "artifacts" / "animation_autosave.animation3d"

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

        # Best-effort token usage stats (per call + per turn). These are:
        # - persisted per call as llm_call_stats events (for replay/debug)
        # - summarized into meta at end-of-turn (for quick CLI display/resume)
        turn_llm_usage = LlmUsageTotals()
        # Exposed for the terminal UI to render mid-turn usage progress.
        self._llm_turn_usage_current = turn_llm_usage
        self._llm_last_call_usage: LlmUsageDelta | None = None
        self._llm_last_call_index: int | None = None
        self._llm_last_call_phase: str | None = None
        self._llm_last_call_gateway_model: str | None = None
        last_llm_usage_delta: LlmUsageDelta | None = None
        last_llm_gateway_model: str | None = None
        last_llm_phase: str | None = None
        internal_llm_call_seq = 0

        def _persist_internal_llm_call_stats(
            *,
            resp: dict[str, Any],
            phase: str | None,
            kind: str,
        ) -> None:
            """Persist LLM call stats for internal helper calls (best-effort)."""

            nonlocal internal_llm_call_seq

            if not isinstance(resp, dict):
                return

            internal_llm_call_seq += 1
            call_index = -int(internal_llm_call_seq)

            usage_delta = extract_usage_delta_from_response(resp)

            # Turn-level summary counters: include internal calls so totals align
            # with session-wide aggregates.
            try:
                self._llm_session_usage.apply_call(usage_delta)
                turn_llm_usage.apply_call(usage_delta)
            except Exception:
                pass

            try:
                model_name = resp.get("model")
                detected = isinstance(model_name, str) and bool(model_name.strip())
                model_name = str(model_name or "").strip() if detected else ""
                if not model_name:
                    model_name = "can not detect gateway model"
            except Exception:
                model_name = "can not detect gateway model"
                detected = False

            # Persist the rolling totals into meta so they survive crashes mid-turn.
            try:
                self.session_store.set_meta(
                    gateway_model_last=str(model_name),
                    llm_session_usage=self._llm_session_usage.to_dict(),
                    llm_last_turn_usage=turn_llm_usage.to_dict(),
                    llm_last_call={
                        "phase": str(phase or "") if phase is not None else None,
                        "call_index": int(call_index),
                        "gateway_model": str(model_name),
                        "usage": (
                            usage_delta.to_dict() if usage_delta is not None else None
                        ),
                    },
                )
            except Exception:
                pass

            try:
                self.session_store.append_event(
                    {
                        "type": "llm_call_stats",
                        "turn_id": turn_id,
                        "phase": phase,
                        "call_index": int(call_index),
                        "requested_model": str(self.model),
                        "gateway_model": str(model_name),
                        "gateway_model_detected": bool(detected),
                        "status": None,
                        "incomplete_details": None,
                        "usage": usage_delta.to_dict()
                        if usage_delta is not None
                        else None,
                    }
                )
            except Exception:
                pass

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
                        (
                            self._memory_summary.rstrip() + "\n" + "\n".join(lines)
                        ).strip()
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
                    write_tools = (
                        set(ATLAS_STATE_MUTATION_TOOLS)
                        | set(ATLAS_OUTPUT_TOOLS)
                        | set(ARTIFACT_WRITE_TOOLS)
                    )
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
                        [
                            "Recent state changes (most recent first; summarized):",
                            *write_summaries,
                            "",
                        ]
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
                mem, mem_resp = self.llm.complete_text_with_response(
                    system_prompt=sys_prompt,
                    user_text=prompt,
                    temperature=0.0,
                )
                if mem:
                    self._memory_summary = mem.strip()
                _persist_internal_llm_call_stats(
                    resp=mem_resp,
                    phase="Internal",
                    kind="session_memory_compaction",
                )
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
                    if len(needles) >= int(AUTO_RETRIEVE_NEEDLE_MAX_TOKENS):
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
                    snippets.append(
                        "Recent verified writes (most recent first; summarized):"
                    )
                    snippets.extend(write_summaries)

            # 2) A few matching transcript messages (for qualitative recall).
            for needle in needles:
                if (
                    max_snips
                    and len([s for s in snippets if s.startswith("- ")]) >= max_snips
                ):
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
                    if (
                        len(current) + 1 + len(candidate)
                        > INTENT_RESOLVER_SCENE_SNAPSHOT_MAX_CHARS
                    ):
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
            if (
                self._current_animation_id is not None
                and int(self._current_animation_id) > 0
            ):
                ts = self.scene.get_time(animation_id=int(self._current_animation_id))
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
        # Make session-level capability toggles explicit to the model so it
        # doesn't waste rounds asking the user for things that are already
        # allowed (e.g., screenshot consent).
        try:
            context_blocks.append(
                "Session settings:\n"
                f"- screenshots_allowed: {bool(_screenshots_allowed())}\n"
                f"- codegen_enabled: {bool(self.enable_codegen)}"
            )
        except Exception:
            pass
        if self._memory_summary:
            context_blocks.append(
                "Session memory (summary):\n" + self._memory_summary.strip()
            )
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
            brief_ctx_parts.append(
                "Session memory (summary):\n" + self._memory_summary.strip()
            )
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
        task_brief_initial = ""
        suppressed_clarify: str | None = None
        try:
            task_brief_initial, task_brief_resp = (
                self.intent_resolver.resolve_with_response(
                    user_text, scene_context=brief_ctx
                )
            )
            task_brief_initial = (task_brief_initial or "").strip()
            _persist_internal_llm_call_stats(
                resp=task_brief_resp,
                phase="Internal",
                kind="intent_resolver",
            )
        except Exception:
            task_brief_initial = ""

        task_brief = task_brief_initial
        if task_brief_initial.lower().startswith("clarify:"):
            # Some providers/models over-use CLARIFY even when the request is
            # actionable with reasonable defaults. Prefer proceed-first: re-run
            # the supervisor with a "TASK BRIEF only" constraint and continue.
            forced = ""
            try:
                forced, forced_resp = (
                    self.intent_resolver.resolve_force_task_brief_with_response(
                        user_text, scene_context=brief_ctx
                    )
                )
                forced = (forced or "").strip()
                _persist_internal_llm_call_stats(
                    resp=forced_resp,
                    phase="Internal",
                    kind="intent_resolver_force_task_brief",
                )
            except Exception:
                forced = ""
            if forced and not forced.lower().startswith("clarify:"):
                suppressed_clarify = task_brief_initial.strip()
                task_brief = forced

        if task_brief.lower().startswith("clarify:"):
            # Last-resort fallback: the supervisor must not block the run.
            # Encode the "clarify" as an assumption and proceed with a minimal,
            # actionable brief that preserves the raw user request.
            suppressed_clarify = suppressed_clarify or task_brief.strip()
            # Avoid language-specific heuristics. Downstream phases already have
            # rules for choosing scene_* vs animation_* tools from the Task Brief
            # and user request.
            intent = "unknown"
            task_brief = (
                "TASK BRIEF:\n"
                f"- Intent: {intent}\n"
                f"- Targets/Inputs: {user_text.strip()}\n"
                "- Assumptions: Proceed with reasonable defaults and fill any gaps without asking; resolve file/location hints via fs_* tools; do not modify unrelated settings.\n"
                "- Signals: Apply the user-requested scene/animation changes.\n"
                "- Verify: Use tool read-backs (and screenshots if allowed) to confirm the requested outcome.\n"
            ).strip()

        # Persist the user input after auto-retrieval (so search doesn't match the current message),
        # but before any tool execution (so intent survives crashes mid-turn).
        try:
            self.session_store.append_transcript(
                role="user", content=user_text, turn_id=turn_id
            )
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
            if suppressed_clarify:
                self.session_store.append_event(
                    {
                        "type": "task_brief_clarify_suppressed",
                        "turn_id": turn_id,
                        "question": suppressed_clarify,
                    }
                )
        except Exception:
            pass

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
                    touched_key_times.setdefault(int(id), {}).setdefault(jk, set()).add(
                        float(time)
                    )
            except Exception:
                return

        def _touch_camera_key_time(*, time: float) -> None:
            try:
                touched_camera_key_times.add(float(time))
            except Exception:
                return

        def _record_touched_from_tool(
            *, name: str, args: dict[str, Any], result: Any
        ) -> None:
            nonlocal include_objects_in_snapshot
            nonlocal include_camera_key_times_in_snapshot
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

            # Track the current animation id for future deterministic calls.
            # This is intentionally in-memory only (ids are not stable across app runs).
            try:
                aid_in = args.get("animation_id")
                if isinstance(aid_in, (int, float)) and int(aid_in) > 0:
                    self._current_animation_id = int(aid_in)
            except Exception:
                pass
            try:
                if isinstance(result, dict):
                    aid_out = result.get("animation_id")
                    if isinstance(aid_out, (int, float)) and int(aid_out) > 0:
                        self._current_animation_id = int(aid_out)
            except Exception:
                pass

            if tool in {"scene_load_sources", "scene_smart_load"}:
                include_objects_in_snapshot = True
                try:
                    if isinstance(result, dict):
                        for i in result.get("loaded_ids") or []:
                            touched_object_ids.add(int(i))
                except Exception:
                    pass
                return

            if tool == "scene_make_alias":
                include_objects_in_snapshot = True
                try:
                    for a in result.get("aliases") or []:
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
                    for i in args.get("ids") or []:
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
                applied = (
                    result.get("applied_set_params")
                    if isinstance(result, dict)
                    else None
                )
                if isinstance(applied, list):
                    for it in applied:
                        if not isinstance(it, dict):
                            continue
                        try:
                            _touch_scene_value(
                                id=int(it.get("id")),
                                json_key=str(it.get("json_key") or ""),
                            )
                        except Exception:
                            continue
                    return
                # Fallback: only record explicit json_keys (names are not canonical here).
                try:
                    for it in args.get("set_params") or []:
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
                touched = (
                    result.get("touched_scene_values")
                    if isinstance(result, dict)
                    else None
                )
                if isinstance(touched, list):
                    for it in touched:
                        if not isinstance(it, dict):
                            continue
                        try:
                            _touch_scene_value(
                                id=int(it.get("id")),
                                json_key=str(it.get("json_key") or ""),
                            )
                        except Exception:
                            continue
                return

            if tool == "animation_ensure_animation":
                # May create baseline camera keys / default state.
                include_camera_key_times_in_snapshot = True
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
                    for sk in args.get("set_keys") or []:
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
                    for rk in args.get("remove_keys") or []:
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

            if tool in {
                "animation_replace_key_camera",
                "animation_camera_solve_and_apply",
            }:
                include_camera_key_times_in_snapshot = True
                try:
                    # animation_replace_key_camera: explicit single-time.
                    if tool == "animation_replace_key_camera":
                        _touch_camera_key_time(time=float(args.get("time", 0.0)))
                    # animation_camera_solve_and_apply: may report applied times.
                    if isinstance(result, dict) and isinstance(
                        result.get("applied"), list
                    ):
                        for t in result.get("applied") or []:
                            _touch_camera_key_time(time=float(t))
                except Exception:
                    pass
                return

            if tool == "animation_camera_waypoint_spline_apply":
                include_camera_key_times_in_snapshot = True
                try:
                    if isinstance(result, dict) and isinstance(
                        result.get("applied"), list
                    ):
                        for t in result.get("applied") or []:
                            _touch_camera_key_time(time=float(t))
                except Exception:
                    pass
                return

            if tool == "animation_camera_walkthrough_apply":
                include_camera_key_times_in_snapshot = True
                try:
                    if isinstance(result, dict) and isinstance(
                        result.get("applied"), list
                    ):
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
            "current_animation_id": int(self._current_animation_id or 0),
        }
        tools, dispatch = scene_tools_and_dispatcher(
            self.scene,
            atlas_dir=self.atlas_dir,
            session_store=self.session_store,
            runtime_state=runtime_state,
            codegen_enabled=bool(self.enable_codegen),
        )
        # Optional: Responses API built-in web search tool.
        #
        # Note: this is a non-function tool (handled by the provider), so it does not
        # participate in our dispatcher allowlist. We gate it by an explicit CLI flag.
        web_search_tool: dict[str, Any] | None = None
        try:
            wsm = str(self.web_search_mode or DEFAULT_WEB_SEARCH_MODE).strip().lower()
        except Exception:
            wsm = str(DEFAULT_WEB_SEARCH_MODE).strip().lower()
        if wsm in {"cached"}:
            web_search_tool = {"type": "web_search", "external_web_access": False}
        elif wsm in {"live"}:
            web_search_tool = {"type": "web_search", "external_web_access": True}
        elif wsm in {"off", "disabled", "0", "false", "no", ""}:
            web_search_tool = None
        else:
            # Fail safe: unknown mode disables web search.
            web_search_tool = None

        if web_search_tool is not None:
            tools = list(tools or []) + [web_search_tool]

        full_log_tools = (
            set(ATLAS_STATE_MUTATION_TOOLS)
            | set(ATLAS_OUTPUT_TOOLS)
            | set(SESSION_MUTATION_TOOLS)
            | set(CODEGEN_TOOLS)
            # Camera validation results are small but high-value for debugging.
            # Store full results even when ok=true so users can inspect per-time
            # within_frame/frame_coverage and avoid “it said ok” confusion.
            | {"animation_camera_validate"}
        )
        current_phase = "executor"

        def _dispatch_logged(name: str, args_json: str) -> str:
            # Phase safety: nested dispatches must not bypass the phase tool allowlist.
            ph = str(current_phase or "")
            if ph in {"Planner", "PlanRefiner"}:
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
                if name in ATLAS_OUTPUT_TOOLS:
                    return json.dumps(
                        {
                            "ok": False,
                            "error": f"tool '{name}' is not allowed in phase={ph}",
                        }
                    )
                if name in ARTIFACT_WRITE_TOOLS:
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

            # Avoid persisting large/sensitive payloads (e.g., file contents) into the
            # append-only session.jsonl tool args. Keep a stable digest instead.
            try:
                if name in ARTIFACT_WRITE_TOOLS and isinstance(args_obj, dict):
                    redacted = dict(args_obj)
                    if name == "artifact_write_text":
                        text = redacted.pop("text", None)
                        if isinstance(text, str):
                            data = text.encode("utf-8", errors="replace")
                            redacted["text_bytes"] = int(len(data))
                            redacted["text_sha256"] = hashlib.sha256(data).hexdigest()
                        elif text is not None:
                            redacted["text_kind"] = type(text).__name__
                    elif name == "artifact_write_json":
                        data_obj = redacted.pop("data", None)
                        try:
                            stable = json.dumps(
                                data_obj, ensure_ascii=False, sort_keys=True
                            ).encode("utf-8", errors="replace")
                            redacted["data_bytes"] = int(len(stable))
                            redacted["data_sha256"] = hashlib.sha256(stable).hexdigest()
                        except Exception:
                            redacted["data_kind"] = type(data_obj).__name__
                    elif name == "artifact_write_bytes_base64":
                        b64 = redacted.pop("data_base64", None)
                        if isinstance(b64, str):
                            raw = b64.encode("ascii", errors="ignore")
                            redacted["data_base64_bytes"] = int(len(raw))
                            redacted["data_base64_sha256"] = hashlib.sha256(
                                raw
                            ).hexdigest()
                        elif b64 is not None:
                            redacted["data_base64_kind"] = type(b64).__name__
                    args_obj = redacted
            except Exception:
                pass

            policy = "full" if name in full_log_tools else "summary"
            ok = True
            try:
                ok = (
                    bool(res_obj.get("ok", True)) if isinstance(res_obj, dict) else True
                )
            except Exception:
                ok = True
            if (name not in full_log_tools) and (not ok):
                policy = "full"

            # Circuit breaker: if a tool observes an RPC connection failure, mark this
            # turn as RPC-unavailable so we can avoid background RPC calls (e.g.
            # post-write facts snapshots) that would otherwise spam errors and waste time.
            try:
                if isinstance(res_obj, dict) and not ok:
                    err = str(res_obj.get("error") or "").lower()
                    if (
                        "statuscode.unavailable" in err
                        or "connection refused" in err
                        or "failed to connect to all addresses" in err
                        or "socket closed" in err
                    ):
                        runtime_state["rpc_unavailable"] = {
                            "tool": str(name),
                            "error": str(res_obj.get("error") or ""),
                        }
            except Exception:
                pass
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
                if (
                    str(current_phase or "") == "Executor"
                    and name in ATLAS_STATE_MUTATION_TOOLS
                ):
                    _record_touched_from_tool(name=name, args=args_obj, result=res_obj)
            except Exception:
                pass
            return result_json

        # Ensure tool modules that chain via ctx.dispatch also go through our
        # logging/touch-tracking wrapper (deterministic + auditable).
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
            # If the RPC server is unavailable, do not attempt any snapshotting.
            try:
                if isinstance(runtime_state.get("rpc_unavailable"), dict):
                    return
            except Exception:
                pass

            key_targets: dict[int, list[str]] = {}
            for oid, keys in touched_key_targets.items():
                if not keys:
                    continue
                try:
                    key_targets[int(oid)] = sorted(
                        {str(k) for k in keys if str(k).strip()}
                    )
                except Exception:
                    continue

            scene_value_targets: dict[int, list[str]] = {}
            for oid, keys in touched_scene_values.items():
                if not keys:
                    continue
                try:
                    scene_value_targets[int(oid)] = sorted(
                        {str(k) for k in keys if str(k).strip()}
                    )
                except Exception:
                    continue

            time_status: dict[str, float] | None = None
            try:
                if (
                    self._current_animation_id is not None
                    and int(self._current_animation_id) > 0
                ):
                    ts = self.scene.get_time(
                        animation_id=int(self._current_animation_id)
                    )
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
                    animation_id=(
                        int(self._current_animation_id)
                        if (
                            self._current_animation_id is not None
                            and int(self._current_animation_id) > 0
                        )
                        else None
                    ),
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

            # Best-effort: autosave the current animation after successful animation mutations.
            #
            # This is intentionally separate from user-facing saves/exports so the agent can
            # resume deterministically across context windows without relying on the user to
            # remember to "save". The autosave path is per-session under the session store.
            try:
                did_animation_mutate = any(
                    str(t).startswith("animation_") for t in touched_mutation_tools
                )
                if (
                    did_animation_mutate
                    and self._current_animation_id is not None
                    and int(self._current_animation_id) > 0
                ):
                    p = self._animation_autosave_path()
                    p.parent.mkdir(parents=True, exist_ok=True)
                    ok = bool(
                        self.scene.save_animation(
                            animation_id=int(self._current_animation_id), path=p
                        )
                    )
                    if ok:
                        try:
                            self.session_store.set_meta(animation_autosave_path=str(p))
                        except Exception:
                            pass
            except Exception:
                pass

            touched_payload: dict[str, Any] = {
                "mutation_tools": sorted(touched_mutation_tools),
                "scene_value_targets": {
                    str(oid): sorted(list(keys))
                    for oid, keys in sorted(
                        touched_scene_values.items(), key=lambda kv: int(kv[0])
                    )
                    if keys
                },
                "key_targets": {
                    str(oid): sorted(list(keys))
                    for oid, keys in sorted(
                        touched_key_targets.items(), key=lambda kv: int(kv[0])
                    )
                    if keys
                },
                "key_times": {
                    str(oid): {
                        str(jk): sorted([float(t) for t in times])
                        for jk, times in sorted(
                            per_key.items(), key=lambda kv: str(kv[0])
                        )
                    }
                    for oid, per_key in sorted(
                        touched_key_times.items(), key=lambda kv: int(kv[0])
                    )
                    if isinstance(per_key, dict) and per_key
                },
            }
            if touched_camera_key_times:
                touched_payload["camera_key_times"] = sorted(
                    [float(t) for t in touched_camera_key_times]
                )
            if touched_object_ids:
                touched_payload["object_ids"] = sorted(
                    [int(i) for i in touched_object_ids]
                )
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
            shared_instructions_parts.append(
                "Shared context:\n" + "\n\n".join(context_blocks)
            )
        shared_instructions = "\n\n".join(
            [p for p in shared_instructions_parts if p]
        ).strip()

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
            # When the current plan is fully completed, treat the next user message as a new task
            # and always refresh the plan. This avoids relying on language-specific heuristics.
            all_completed = True
            for it in plan:
                if not isinstance(it, dict):
                    continue
                st = str(it.get("status") or "").strip()
                if st != "completed":
                    all_completed = False
                    break
            if all_completed:
                return True
            t = (user_text or "").strip()
            if "\n" in t:
                return True
            if len(t) >= 220:
                return True
            # Avoid keyword heuristics (may not be English). If there is already
            # an in-progress plan and the user message is short/single-line,
            # proceed directly to the Executor.
            return False

        def _plan_has_incomplete_steps(current_plan: list[object]) -> bool:
            for it in current_plan:
                if not isinstance(it, dict):
                    continue
                st = str(it.get("status") or "").strip()
                if st != "completed":
                    return True
            return False

        def _plan_steps_signature(current_plan: list[object]) -> str:
            # Signature used to avoid re-running PlanRefiner on every turn when
            # only statuses change (e.g., marking steps completed). This is
            # language-agnostic and based solely on the ordered step texts.
            steps: list[str] = []
            for it in current_plan:
                if not isinstance(it, dict):
                    continue
                step = str(it.get("step") or "").strip()
                steps.append(step)
            payload = "\n".join(steps).encode("utf-8")
            return hashlib.sha1(payload).hexdigest()

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

            # Best-effort cache: avoid re-uploading the same preview image repeatedly
            # when using file_id based attachments.
            uploaded_preview_file_ids_by_path: dict[str, str] = {}

            def _post_tool_output(
                name: str, args_json: str, result_json: str
            ) -> dict[str, Any] | None:
                """Optionally augment tool outputs before feeding them back to the model.

                For screenshot-like tools, we attach the rendered image to the *tool output*
                (function_call_output) rather than injecting a fake user message. This keeps
                conversation semantics clean and matches the Responses API pattern where
                function_call_output.output can contain multimodal content parts.
                """

                # Only attach images for Executor. Planner is read-only and should avoid
                # expensive/visual operations unless strictly necessary.
                if str(runtime_state.get("phase") or "") != "Executor":
                    return None
                tool_name = str(name or "")
                if tool_name not in {"animation_render_preview", "scene_screenshot"}:
                    return None
                if not _screenshots_allowed():
                    return None

                try:
                    data = json.loads(result_json or "{}")
                except Exception:
                    data = {}
                if not isinstance(data, dict) or not data.get("ok"):
                    return None
                path = data.get("path")
                if not isinstance(path, str) or not path.strip():
                    return None

                # Include a short text header so the model knows what this image represents.
                try:
                    args = json.loads(args_json or "{}")
                except Exception:
                    args = {}
                tsec = args.get("time") if isinstance(args, dict) else None
                w = args.get("width") if isinstance(args, dict) else None
                h = args.get("height") if isinstance(args, dict) else None
                header = (
                    "INTERNAL TOOL OUTPUT (not a user request): preview image for model-based visual verification.\n"
                    f"- tool: {tool_name}\n"
                    f"- time_sec: {tsec!r}\n"
                    f"- size: {w!r}x{h!r}\n"
                    f"- path: {path}\n"
                    "This image was generated by an Atlas tool call during execution.\n"
                    "Do NOT ask the user to open the temp file.\n"
                    "If you still need a human check, ask the user to look in the Atlas UI (not the file path)."
                )

                def _try_get_preview_file_id() -> str | None:
                    """Best-effort upload of a preview image to get a reusable file_id.

                    This keeps tool-loop payloads small and avoids repeating base64 bytes
                    in subsequent tool-loop rounds.
                    """

                    if self.llm._wire_mode() == "chat":
                        return None
                    cache_key = ""
                    try:
                        cache_key = str(p.resolve())
                    except Exception:
                        cache_key = str(p)
                    file_id = uploaded_preview_file_ids_by_path.get(cache_key)
                    if not file_id:
                        try:
                            file_id = self.llm.upload_file_for_model_input(
                                path=p,
                                purpose="user_data",
                                # Keep this as session-ish: auto-expire after 1 hour when supported.
                                expires_after_sec=3600,
                            )
                        except Exception:
                            file_id = None
                        if isinstance(file_id, str) and file_id.strip():
                            uploaded_preview_file_ids_by_path[cache_key] = (
                                file_id.strip()
                            )
                            file_id = file_id.strip()
                        else:
                            file_id = None
                    return file_id

                p = Path(path)
                if not p.exists() or not p.is_file():
                    return {
                        "output": [
                            {"type": "input_text", "text": header},
                            {
                                "type": "input_text",
                                "text": f"Preview render returned path={path!r}, but the file does not exist on disk.",
                            },
                            {
                                "type": "input_text",
                                "text": f"Tool JSON result:\n{result_json}",
                            },
                        ]
                    }

                # Fast-path size check: avoid reading huge images into memory just
                # to learn they can't be inlined.
                file_bytes: int | None = None
                try:
                    file_bytes = int(p.stat().st_size)
                except Exception:
                    file_bytes = None

                mime = _mime_for_model_image(p)
                if not mime:
                    return {
                        "output": [
                            {"type": "input_text", "text": header},
                            {
                                "type": "input_text",
                                "text": (
                                    "Preview image format is not supported for model upload.\n"
                                    f"- path: {path}\n"
                                    f"- suffix: {p.suffix!r}\n"
                                    "- allowed: .png, .jpg/.jpeg, .webp, .gif\n"
                                    "Re-render the screenshot as PNG (preferred) at a smaller width/height if needed."
                                ),
                            },
                            {
                                "type": "input_text",
                                "text": f"Tool JSON result:\n{result_json}",
                            },
                        ]
                    }

                # Preferred: attach as a provider file_id (keeps tool-loop payloads small).
                # This is best-effort and provider-dependent; if upload fails we fall back
                # to inline base64 for small images.
                file_id = _try_get_preview_file_id()
                if isinstance(file_id, str) and file_id.strip():
                    return {
                        "output": [
                            {"type": "input_text", "text": header},
                            {
                                "type": "input_text",
                                "text": f"Tool JSON result:\n{result_json}",
                            },
                            {
                                "type": "input_image",
                                "file_id": file_id.strip(),
                                "detail": "auto",
                            },
                        ]
                    }

                try:
                    raw = p.read_bytes()
                except Exception as e:
                    return {
                        "output": [
                            {"type": "input_text", "text": header},
                            {
                                "type": "input_text",
                                "text": f"Preview image exists at {path!r} but could not be read: {e}",
                            },
                            {
                                "type": "input_text",
                                "text": f"Tool JSON result:\n{result_json}",
                            },
                        ]
                    }

                if len(raw) > MAX_PREVIEW_IMAGE_INLINE_BYTES_FOR_MODEL:
                    # Either the upload failed, or the provider doesn't support Files.
                    # Don't inline large base64 payloads; request a smaller preview instead.
                    return {
                        "output": [
                            {"type": "input_text", "text": header},
                            {
                                "type": "input_text",
                                "text": (
                                    "Preview image is too large to inline as base64 and could not be attached as file_id.\n"
                                    f"- path: {path}\n"
                                    f"- bytes: {len(raw)}\n"
                                    f"- inline_limit_bytes: {MAX_PREVIEW_IMAGE_INLINE_BYTES_FOR_MODEL}\n"
                                    "Re-render the preview at a smaller width/height, or request a human check in the Atlas UI."
                                ),
                            },
                            {
                                "type": "input_text",
                                "text": f"Tool JSON result:\n{result_json}",
                            },
                        ]
                    }

                b64 = base64.b64encode(raw).decode("ascii")
                data_url = f"data:{mime};base64,{b64}"
                return {
                    "output": [
                        {"type": "input_text", "text": header},
                        {
                            "type": "input_text",
                            "text": f"Tool JSON result:\n{result_json}",
                        },
                        {
                            "type": "input_image",
                            "image_url": data_url,
                            "detail": "auto",
                        },
                    ]
                }

            # Capture streamed reasoning deltas so we can persist them even when the
            # request fails mid-stream (e.g., provider network drops / 400s after some
            # output). The phase-level summaries returned by run_responses_tool_loop are
            # still the source of truth when available.
            streamed_summary_parts: dict[int, list[str]] = {}

            def _capture_reasoning_delta(delta: str, summary_index: int) -> None:
                if delta:
                    streamed_summary_parts.setdefault(
                        int(summary_index or 0), []
                    ).append(delta)
                if (
                    callbacks is not None
                    and callbacks.on_reasoning_summary_delta is not None
                ):
                    callbacks.on_reasoning_summary_delta(delta, summary_index)

            def _capture_reasoning_part_added(summary_index: int) -> None:
                if (
                    callbacks is not None
                    and callbacks.on_reasoning_summary_part_added is not None
                ):
                    callbacks.on_reasoning_summary_part_added(summary_index)

            def _persist_reasoning_summary_complete(
                summary_text: str, call_index: int
            ) -> None:
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

            last_gateway_model_logged: str | None = None

            def _persist_llm_call_stats(resp: dict[str, Any], call_index: int) -> None:
                """Persist per-call context usage/budget stats (best-effort)."""

                nonlocal last_llm_usage_delta, last_llm_gateway_model, last_llm_phase

                if not isinstance(resp, dict):
                    return
                model_name = resp.get("model")
                detected = isinstance(model_name, str) and bool(model_name.strip())
                model_name = str(model_name or "").strip() if detected else ""
                if not model_name:
                    model_name = "can not detect gateway model"
                usage_delta = extract_usage_delta_from_response(resp)
                status = resp.get("status")
                incomplete = resp.get("incomplete_details")

                last_llm_phase = str(phase or "") if phase is not None else None
                last_llm_gateway_model = model_name
                last_llm_usage_delta = usage_delta

                try:
                    self._llm_session_usage.apply_call(usage_delta)
                    turn_llm_usage.apply_call(usage_delta)
                except Exception:
                    pass

                # Expose last-call details for the terminal UI.
                try:
                    self._llm_last_call_usage = usage_delta
                    self._llm_last_call_index = int(call_index)
                    self._llm_last_call_phase = last_llm_phase
                    self._llm_last_call_gateway_model = model_name
                except Exception:
                    pass

                try:
                    self.session_store.append_event(
                        {
                            "type": "llm_call_stats",
                            "turn_id": turn_id,
                            "phase": phase,
                            "call_index": int(call_index),
                            "requested_model": str(self.model),
                            "gateway_model": model_name,
                            "gateway_model_detected": bool(detected),
                            "status": str(status or "") if status is not None else None,
                            "incomplete_details": incomplete
                            if isinstance(incomplete, dict)
                            else None,
                            "usage": (
                                usage_delta.to_dict()
                                if usage_delta is not None
                                else None
                            ),
                        }
                    )
                except Exception:
                    pass

            def _persist_response_meta(resp: dict[str, Any], call_index: int) -> None:
                nonlocal last_gateway_model_logged
                if not isinstance(resp, dict):
                    return
                model_name = resp.get("model")
                detected = isinstance(model_name, str) and bool(model_name.strip())
                if not detected:
                    model_name = "can not detect gateway model"
                model_name = str(model_name).strip()
                if model_name == last_gateway_model_logged:
                    return
                last_gateway_model_logged = model_name
                try:
                    self._gateway_model_last = model_name
                except Exception:
                    pass
                if emit_to_stdout:
                    try:
                        msg = f"[gateway model: {model_name}]"
                        if str(self.model) and model_name != str(self.model):
                            msg = f"[gateway model: {model_name} (requested {self.model})]"
                        sys.stdout.write(msg + "\n")
                        sys.stdout.flush()
                    except Exception:
                        pass
                try:
                    self.session_store.append_event(
                        {
                            "type": "llm_response_meta",
                            "turn_id": turn_id,
                            "phase": phase,
                            "call_index": int(call_index),
                            "requested_model": str(self.model),
                            "gateway_model": model_name,
                            "gateway_model_detected": bool(detected),
                        }
                    )
                except Exception:
                    pass

            def _persist_response_meta_and_forward(
                resp: dict[str, Any], call_index: int
            ) -> None:
                _persist_llm_call_stats(resp, call_index)
                _persist_response_meta(resp, call_index)
                # Persist rolling LLM usage into meta so it survives crashes mid-turn and
                # is visible to the terminal UI without log scanning.
                try:
                    model_name = None
                    if isinstance(resp, dict):
                        model_name = resp.get("model")
                    if not isinstance(model_name, str) or not model_name.strip():
                        model_name = "can not detect gateway model"
                    model_name = model_name.strip()
                    self.session_store.set_meta(
                        gateway_model_last=model_name,
                        llm_session_usage=self._llm_session_usage.to_dict(),
                        llm_last_turn_usage=turn_llm_usage.to_dict(),
                        llm_last_call={
                            "phase": str(phase or "") if phase is not None else None,
                            "call_index": int(call_index),
                            "gateway_model": model_name,
                            "usage": (
                                self._llm_last_call_usage.to_dict()
                                if self._llm_last_call_usage is not None
                                else None
                            ),
                        },
                    )
                except Exception:
                    pass
                if callbacks is not None and callbacks.on_response_meta is not None:
                    try:
                        callbacks.on_response_meta(resp, call_index)
                    except Exception:
                        # UI callbacks must not break tool execution.
                        pass

            def _persist_web_search_call_and_forward(
                call: dict[str, Any], call_index: int
            ) -> None:
                try:
                    action = call.get("action") if isinstance(call, dict) else None
                    entry: dict[str, Any] = {
                        "type": "web_search",
                        "turn_id": turn_id,
                        "phase": phase,
                        "call_index": int(call_index),
                        "mode": str(self.web_search_mode or DEFAULT_WEB_SEARCH_MODE),
                        "call": call if isinstance(call, dict) else {"raw": str(call)},
                    }
                    if isinstance(action, dict):
                        at = str(action.get("type") or "").strip()
                        if at:
                            entry["action_type"] = at
                        for k in ("query", "url", "pattern"):
                            v = action.get(k)
                            if isinstance(v, str) and v.strip():
                                entry[k] = v.strip()
                    self.session_store.append_event(entry)
                except Exception:
                    pass
                if callbacks is not None and callbacks.on_web_search_call is not None:
                    try:
                        callbacks.on_web_search_call(call, call_index)
                    except Exception:
                        # UI callbacks must not break tool execution.
                        pass

            phase_callbacks = ToolLoopCallbacks(
                on_reasoning_summary_delta=_capture_reasoning_delta,
                on_reasoning_summary_part_added=_capture_reasoning_part_added,
                on_reasoning_summary_complete=_persist_reasoning_summary_complete,
                on_response_meta=_persist_response_meta_and_forward,
                on_web_search_call=_persist_web_search_call_and_forward,
                on_assistant_text_delta=(
                    callbacks.on_assistant_text_delta if callbacks else None
                ),
                on_tool_call=(callbacks.on_tool_call if callbacks else None),
                on_tool_result=(callbacks.on_tool_result if callbacks else None),
            )

            if callbacks is not None and callbacks.on_phase_start is not None:
                callbacks.on_phase_start(phase)
            try:

                def _looks_like_embedded_tool_json(text: str) -> bool:
                    """Heuristic: model printed tool/plan JSON into assistant text.

                    Some providers/models occasionally fail to emit tool calls and instead
                    print JSON blobs like {"tool": "..."} or {"plan": [...]} into the message.
                    That output is not actionable and should be retried with explicit instructions.
                    """

                    t = (text or "").strip()
                    if not t:
                        return False
                    # Common failure patterns we have seen in session logs.
                    if '{"tool"' in t or "{'tool'" in t:
                        return True
                    if '{"plan"' in t or "{'plan'" in t:
                        return True
                    if '"requirements"' in t and '"step_id"' in t:
                        return True
                    return False

                def _tool_json_repair_prompt(*, phase_name: str) -> str:
                    ph = str(phase_name or "")
                    if ph == "Planner":
                        return (
                            "INTERNAL: Your previous reply incorrectly embedded tool/plan JSON into assistant text.\n"
                            "That is INVALID. You must CALL tools instead of printing JSON.\n\n"
                            "Do this now:\n"
                            "1) Call update_plan with a clear, scannable plan (choose a manageable number of steps; exactly one in_progress).\n"
                            "2) Call verification_set_requirements for each step_id.\n"
                            "3) Do NOT output any JSON in your assistant text.\n"
                        )
                    return (
                        "INTERNAL: Your previous reply incorrectly embedded tool/plan JSON into assistant text.\n"
                        "That is INVALID. You must CALL tools instead of printing JSON.\n\n"
                        "Do this now:\n"
                        "- If there is no plan yet, call update_plan with a clear, scannable plan and then verification_set_requirements.\n"
                        "- Otherwise, proceed to execute the plan via tools.\n"
                        "- Do NOT output any JSON in your assistant text.\n"
                    )

                # Best-effort repair: retry a small number of times when the model
                # prints tool JSON instead of emitting tool calls.
                tool_json_repair_attempts = 0
                executor_no_tool_calls_repair_attempts = 0
                planner_no_plan_repair_attempts = 0
                unexpected_refusal_repair_attempts = 0
                phase_input_items_local = list(phase_input_items)
                retry_extra_instructions = ""

                CONTEXT_CHECKPOINT_PREFIX = (
                    "CONTEXT CHECKPOINT (atlas_agent)\n"
                    "This is an automatically generated summary of earlier within-turn context.\n"
                    "It exists to keep the conversation within the model context window.\n"
                    "When you need exact details beyond this summary, use session_search_events/session_search_transcript.\n"
                    "\n"
                )

                model_token_limits_validated: set[str] = set()

                def _log_effective_input_budget_confirmed_once(
                    *, model_name: str, tokens: int, method: str
                ) -> None:
                    """Log the effective input prompt budget once per session+model.

                    This is the single number that drives proactive compaction decisions.
                    We log it once to avoid spamming session.jsonl, and we record a coarse
                    method (one of 4) so debugging doesn't require reproducing compaction:
                    - session_meta
                    - provider_models_retrieve
                    - context_length_error
                    - name_guess
                    """

                    mn = str(model_name or "").strip()
                    m = str(method or "").strip()
                    if not mn or not m:
                        return
                    if m not in {
                        "session_meta",
                        "provider_models_retrieve",
                        "context_length_error",
                        "name_guess",
                    }:
                        return
                    try:
                        tok = int(tokens)
                    except Exception:
                        return
                    if tok <= 0:
                        return

                    # Dedupe across resumes by storing a small marker in session meta.
                    try:
                        meta_now = self.session_store.get_meta() or {}
                    except Exception:
                        meta_now = {}
                    logged = (
                        meta_now.get(
                            "model_effective_input_budget_confirmed_logged_by_model"
                        )
                        if isinstance(meta_now, dict)
                        else None
                    )
                    if not isinstance(logged, dict):
                        logged = {}
                    marker = f"{m}:{int(tok)}"
                    existing = logged.get(mn)
                    if isinstance(existing, str) and existing.strip() == marker:
                        return
                    logged[mn] = marker
                    try:
                        self.session_store.set_meta(
                            model_effective_input_budget_confirmed_logged_by_model=logged
                        )
                    except Exception:
                        pass

                    try:
                        self.session_store.append_event(
                            {
                                "type": "model_effective_input_budget_confirmed",
                                "turn_id": turn_id,
                                "phase": phase,
                                "model_name": mn,
                                "effective_input_budget_tokens": int(tok),
                                "method": m,
                            }
                        )
                    except Exception:
                        pass

                def _model_prompt_budgets_for_current_model() -> tuple[
                    int | None, int | None
                ]:
                    requested_model = str(self.model or "").strip()
                    gateway_model = str(
                        getattr(self, "_gateway_model_last", None) or ""
                    ).strip()
                    gateway_model_l = gateway_model.lower()

                    # Avoid using placeholder strings as model IDs for lookups.
                    model_name = (
                        gateway_model
                        if (
                            gateway_model
                            and "detect gateway model" not in gateway_model_l
                        )
                        else requested_model
                    ).strip()
                    if not model_name:
                        return (None, None)

                    total_context_window_tokens: int | None = None
                    max_output_tokens: int | None = None
                    effective_input_budget_tokens: int | None = None
                    auto_compact_tokens: int | None = None
                    effective_method: str | None = None

                    try:
                        meta = self.session_store.get_meta()
                    except Exception:
                        meta = {}
                    if isinstance(meta, dict):
                        # Total context window (provider-reported or error-derived).
                        total_by_model = meta.get(
                            "model_total_context_window_tokens_by_model"
                        )
                        if isinstance(total_by_model, dict):
                            try:
                                v = total_by_model.get(model_name)
                                if v is not None:
                                    n = int(v)
                                    if n > 0:
                                        total_context_window_tokens = n
                            except Exception:
                                pass

                        # Optional provider-reported max output tokens.
                        max_out_by_model = meta.get("model_max_output_tokens_by_model")
                        if isinstance(max_out_by_model, dict):
                            try:
                                v = max_out_by_model.get(model_name)
                                if v is not None:
                                    n = int(v)
                                    if n > 0:
                                        max_output_tokens = n
                            except Exception:
                                pass

                        # Optional provider-reported effective input budget (prompt tokens).
                        eff_in_by_model = meta.get(
                            "model_effective_input_budget_tokens_by_model"
                        )
                        if isinstance(eff_in_by_model, dict):
                            try:
                                v = eff_in_by_model.get(model_name)
                                if v is not None:
                                    n = int(v)
                                    if n > 0:
                                        effective_input_budget_tokens = n
                                        eff_in_src_by_model = meta.get(
                                            "model_effective_input_budget_tokens_source_by_model"
                                        )
                                        src = None
                                        if isinstance(eff_in_src_by_model, dict):
                                            s = eff_in_src_by_model.get(model_name)
                                            if isinstance(s, str) and s.strip():
                                                src = s.strip()
                                        if src in {
                                            "name_guess",
                                            "provider_models_retrieve",
                                            "context_length_error",
                                        }:
                                            effective_method = src
                                        else:
                                            effective_method = "session_meta"
                            except Exception:
                                pass

                    # Provider-first lookup (best-effort): fetch model token limits once per model.
                    if model_name not in model_token_limits_validated:
                        model_token_limits_validated.add(model_name)
                        try:
                            budgets = self.llm.get_model_token_budgets(model_name)
                        except Exception:
                            budgets = None

                        if budgets is not None:
                            provider_total: int | None = None
                            provider_max_out: int | None = None
                            # Persist provider-reported token limits so resumes have stable budgeting.
                            if (
                                budgets.total_context_window_tokens is not None
                                and int(budgets.total_context_window_tokens) > 0
                            ):
                                total_context_window_tokens = int(
                                    budgets.total_context_window_tokens
                                )
                                provider_total = int(total_context_window_tokens)
                                try:
                                    meta_now = self.session_store.get_meta() or {}
                                except Exception:
                                    meta_now = {}
                                total_now = (
                                    meta_now.get(
                                        "model_total_context_window_tokens_by_model"
                                    )
                                    if isinstance(meta_now, dict)
                                    else None
                                )
                                if not isinstance(total_now, dict):
                                    total_now = {}
                                total_now[model_name] = int(total_context_window_tokens)
                                try:
                                    self.session_store.set_meta(
                                        model_total_context_window_tokens_by_model=total_now
                                    )
                                except Exception:
                                    pass

                            if (
                                budgets.max_output_tokens is not None
                                and int(budgets.max_output_tokens) > 0
                            ):
                                max_output_tokens = int(budgets.max_output_tokens)
                                provider_max_out = int(max_output_tokens)
                                try:
                                    meta_now = self.session_store.get_meta() or {}
                                except Exception:
                                    meta_now = {}
                                max_out_now = (
                                    meta_now.get("model_max_output_tokens_by_model")
                                    if isinstance(meta_now, dict)
                                    else None
                                )
                                if not isinstance(max_out_now, dict):
                                    max_out_now = {}
                                max_out_now[model_name] = int(max_output_tokens)
                                try:
                                    self.session_store.set_meta(
                                        model_max_output_tokens_by_model=max_out_now
                                    )
                                except Exception:
                                    pass

                            if (
                                budgets.effective_input_budget_tokens is not None
                                and int(budgets.effective_input_budget_tokens) > 0
                            ):
                                effective_input_budget_tokens = int(
                                    budgets.effective_input_budget_tokens
                                )
                                try:
                                    meta_now = self.session_store.get_meta() or {}
                                except Exception:
                                    meta_now = {}
                                eff_in_now = (
                                    meta_now.get(
                                        "model_effective_input_budget_tokens_by_model"
                                    )
                                    if isinstance(meta_now, dict)
                                    else None
                                )
                                if not isinstance(eff_in_now, dict):
                                    eff_in_now = {}
                                eff_in_now[model_name] = int(
                                    effective_input_budget_tokens
                                )
                                eff_in_src_now = (
                                    meta_now.get(
                                        "model_effective_input_budget_tokens_source_by_model"
                                    )
                                    if isinstance(meta_now, dict)
                                    else None
                                )
                                if not isinstance(eff_in_src_now, dict):
                                    eff_in_src_now = {}
                                eff_in_src_now[model_name] = "provider_models_retrieve"
                                try:
                                    self.session_store.set_meta(
                                        model_effective_input_budget_tokens_by_model=eff_in_now,
                                        model_effective_input_budget_tokens_source_by_model=eff_in_src_now,
                                    )
                                except Exception:
                                    pass
                                effective_method = "provider_models_retrieve"

                            if (
                                budgets.auto_compact_tokens is not None
                                and int(budgets.auto_compact_tokens) > 0
                            ):
                                auto_compact_tokens = int(budgets.auto_compact_tokens)

                            # If provider exposed total+max_output, we can derive an effective
                            # input budget even when it doesn't provide one explicitly.
                            if (
                                effective_input_budget_tokens is None
                                and provider_total is not None
                                and provider_max_out is not None
                            ):
                                try:
                                    eff = int(provider_total) - int(provider_max_out)
                                    if eff > 0:
                                        effective_input_budget_tokens = int(eff)
                                        effective_method = "provider_models_retrieve"
                                except Exception:
                                    pass

                    # Compute effective input budget when total+max_output are known.
                    if (
                        effective_input_budget_tokens is None
                        and total_context_window_tokens is not None
                        and max_output_tokens is not None
                    ):
                        try:
                            eff = int(total_context_window_tokens) - int(
                                max_output_tokens
                            )
                            effective_input_budget_tokens = eff if eff > 0 else None
                            if effective_input_budget_tokens is not None:
                                effective_method = effective_method or "session_meta"
                        except Exception:
                            effective_input_budget_tokens = None

                    # Fall back to a conservative name-based effective-input guess only when needed.
                    if effective_input_budget_tokens is None:
                        guessed_eff = _guess_model_effective_input_budget_tokens(
                            model_name
                        )
                        if (
                            guessed_eff is not None
                            and total_context_window_tokens is not None
                        ):
                            effective_input_budget_tokens = min(
                                int(total_context_window_tokens), int(guessed_eff)
                            )
                            effective_method = "name_guess"
                        elif guessed_eff is not None:
                            effective_input_budget_tokens = int(guessed_eff)
                            effective_method = "name_guess"
                        elif total_context_window_tokens is not None:
                            # Last resort: treat total window as input budget (optimistic).
                            effective_input_budget_tokens = int(
                                total_context_window_tokens
                            )
                            effective_method = effective_method or "session_meta"

                    # Default auto-compaction threshold when not explicitly provided.
                    if (
                        auto_compact_tokens is None
                        and effective_input_budget_tokens is not None
                    ):
                        try:
                            auto_compact_tokens = max(
                                1,
                                (
                                    int(effective_input_budget_tokens)
                                    * DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR
                                )
                                // DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR,
                            )
                        except Exception:
                            auto_compact_tokens = None

                    # If we didn't have a persisted effective input budget, persist a
                    # best-effort value for this session so future turns/resumes can
                    # avoid re-guessing. This is safe: if later we learn a more precise
                    # budget (provider metadata or context error), that path overwrites it.
                    if (
                        effective_input_budget_tokens is not None
                        and effective_method == "name_guess"
                    ):
                        try:
                            meta_now = self.session_store.get_meta() or {}
                        except Exception:
                            meta_now = {}
                        eff_in_now = (
                            meta_now.get("model_effective_input_budget_tokens_by_model")
                            if isinstance(meta_now, dict)
                            else None
                        )
                        if not isinstance(eff_in_now, dict):
                            eff_in_now = {}
                        eff_in_src_now = (
                            meta_now.get(
                                "model_effective_input_budget_tokens_source_by_model"
                            )
                            if isinstance(meta_now, dict)
                            else None
                        )
                        if not isinstance(eff_in_src_now, dict):
                            eff_in_src_now = {}
                        if model_name not in eff_in_src_now:
                            eff_in_now[model_name] = int(effective_input_budget_tokens)
                            eff_in_src_now[model_name] = "name_guess"
                            try:
                                self.session_store.set_meta(
                                    model_effective_input_budget_tokens_by_model=eff_in_now,
                                    model_effective_input_budget_tokens_source_by_model=eff_in_src_now,
                                )
                            except Exception:
                                pass

                    if (
                        effective_input_budget_tokens is not None
                        and effective_method is not None
                    ):
                        _log_effective_input_budget_confirmed_once(
                            model_name=model_name,
                            tokens=int(effective_input_budget_tokens),
                            method=effective_method,
                        )
                    return (effective_input_budget_tokens, auto_compact_tokens)

                def _extract_message_text(item: dict[str, Any]) -> str:
                    if not isinstance(item, dict):
                        return ""
                    if str(item.get("type") or "") != "message":
                        return ""
                    content = item.get("content")
                    if not isinstance(content, list):
                        return ""
                    parts: list[str] = []
                    for p in content:
                        if not isinstance(p, dict):
                            continue
                        txt = p.get("text")
                        if isinstance(txt, str) and txt:
                            parts.append(txt)
                    return "".join(parts).strip()

                def _is_context_checkpoint_message(item: dict[str, Any]) -> bool:
                    if not isinstance(item, dict):
                        return False
                    if str(item.get("type") or "") != "message":
                        return False
                    txt = _extract_message_text(item)
                    return txt.startswith("CONTEXT CHECKPOINT (atlas_agent)")

                context_compaction_attempts = 0

                def _context_overflow_compact(
                    in_items: list[dict[str, Any]], exc: BaseException
                ) -> bool:
                    """Checkpoint compaction for long tool loops.

                    When a provider rejects a request due to context overflow, we prefer to
                    replace older within-turn history with a compact checkpoint summary (so
                    the model can keep working), rather than blindly dropping old items.
                    """

                    nonlocal context_compaction_attempts
                    context_compaction_attempts += 1

                    keep_tail = max(0, int(CONTEXT_COMPACTION_KEEP_TAIL_ITEMS))
                    if keep_tail <= 0:
                        return False
                    if not isinstance(in_items, list) or len(in_items) <= 1:
                        return False
                    orig_len = len(in_items)

                    exc_name = type(exc).__name__
                    exc_msg = str(exc or "")
                    exc_msg_l = exc_msg.lower()
                    is_proactive_budget = (
                        exc_name == "PromptBudgetExceeded"
                        or "prompt budget exceeded" in exc_msg_l
                        or "proactive" in exc_msg_l
                    )
                    reason_key = (
                        "prompt_budget"
                        if is_proactive_budget
                        else "context_length_error"
                    )
                    reason_label = (
                        "proactive prompt-budget compaction"
                        if is_proactive_budget
                        else "context window overflow"
                    )

                    # Best-effort: if the provider error message includes an explicit maximum
                    # context window token count, persist it for future proactive estimation.
                    #
                    # Common formats:
                    # - "maximum context length is 128000 tokens"
                    # - "This model's maximum context length is 8192 tokens"
                    try:
                        m = re.search(
                            r"maximum context length is\s*(\d{4,})\s*tokens", exc_msg_l
                        )
                        if not m:
                            m = re.search(
                                r"max(?:imum)?\s*context(?:\s*length)?\s*is\s*(\d{4,})\s*tokens",
                                exc_msg_l,
                            )
                        if not m:
                            m = re.search(r"maximum\s*(\d{4,})\s*tokens", exc_msg_l)
                        if m:
                            n = int(m.group(1))
                            if n > 0:
                                try:
                                    meta = self.session_store.get_meta() or {}
                                except Exception:
                                    meta = {}
                                by_model = (
                                    meta.get(
                                        "model_total_context_window_tokens_by_model"
                                    )
                                    if isinstance(meta, dict)
                                    else None
                                )
                                if not isinstance(by_model, dict):
                                    by_model = {}
                                # Prefer the routed model name when available, but avoid
                                # persisting under placeholder strings when a gateway does not
                                # report the model (so the value remains useful on resume).
                                requested_model = str(self.model or "").strip()
                                gateway_model = str(
                                    getattr(self, "_gateway_model_last", None) or ""
                                ).strip()
                                gateway_model_l = gateway_model.lower()
                                model_key = (
                                    gateway_model
                                    if (
                                        gateway_model
                                        and "detect gateway model"
                                        not in gateway_model_l
                                    )
                                    else requested_model
                                ).strip()
                                if model_key:
                                    by_model[model_key] = int(n)
                                    meta_updates: dict[str, Any] = {
                                        "model_total_context_window_tokens_by_model": by_model,
                                    }

                                    # If the provider gives us a maximum context length, we can
                                    # use it as a best-effort *effective input* budget fallback
                                    # for proactive compaction (until we learn more precise limits).
                                    eff_in_by_model = (
                                        meta.get(
                                            "model_effective_input_budget_tokens_by_model"
                                        )
                                        if isinstance(meta, dict)
                                        else None
                                    )
                                    if not isinstance(eff_in_by_model, dict):
                                        eff_in_by_model = {}
                                    eff_in_src_by_model = (
                                        meta.get(
                                            "model_effective_input_budget_tokens_source_by_model"
                                        )
                                        if isinstance(meta, dict)
                                        else None
                                    )
                                    if not isinstance(eff_in_src_by_model, dict):
                                        eff_in_src_by_model = {}
                                    existing_src = eff_in_src_by_model.get(model_key)
                                    if not isinstance(existing_src, str):
                                        existing_src = ""

                                    # Only overwrite an existing value if it was a name-based guess.
                                    should_set_eff_in = (
                                        model_key not in eff_in_by_model
                                    ) or (existing_src.strip() == "name_guess")
                                    if should_set_eff_in:
                                        eff_in_by_model[model_key] = int(n)
                                        eff_in_src_by_model[model_key] = (
                                            "context_length_error"
                                        )
                                        meta_updates[
                                            "model_effective_input_budget_tokens_by_model"
                                        ] = eff_in_by_model
                                        meta_updates[
                                            "model_effective_input_budget_tokens_source_by_model"
                                        ] = eff_in_src_by_model

                                    self.session_store.set_meta(**meta_updates)
                                    if should_set_eff_in:
                                        _log_effective_input_budget_confirmed_once(
                                            model_name=str(model_key),
                                            tokens=int(n),
                                            method="context_length_error",
                                        )
                    except Exception:
                        pass

                    # Identify the most recent user message; we keep it when compacting so the
                    # "current ask" is still explicitly present in the prompt.
                    last_user_idx: int | None = None
                    for i in range(len(in_items) - 1, -1, -1):
                        it = in_items[i]
                        if (
                            isinstance(it, dict)
                            and str(it.get("type") or "") == "message"
                            and str(it.get("role") or "") == "user"
                        ):
                            last_user_idx = i
                            break
                    if last_user_idx is None:
                        return False

                    # If we overflow repeatedly in the same phase, fall back to a hard compaction
                    # (keep only the current user message + an updated checkpoint). This avoids
                    # getting stuck when a single recent tool output is too large to fit.
                    force_hard_mode = context_compaction_attempts >= 2

                    suffix_start = max(0, len(in_items) - keep_tail)
                    suffix_start0 = suffix_start
                    suffix_start = min(suffix_start, int(last_user_idx))

                    # Avoid splitting function_call/function_call_output pairs across the boundary:
                    # if the kept suffix references any call_id, keep the matching items too.
                    call_ids: set[str] = set()
                    for it in in_items[suffix_start:]:
                        if not isinstance(it, dict):
                            continue
                        cid = it.get("call_id")
                        if isinstance(cid, str) and cid.strip():
                            call_ids.add(cid.strip())
                    if call_ids:
                        earliest = suffix_start
                        for i in range(suffix_start):
                            it = in_items[i]
                            if not isinstance(it, dict):
                                continue
                            if str(it.get("type") or "") not in {
                                "function_call",
                                "function_call_output",
                            }:
                                continue
                            cid = it.get("call_id")
                            if isinstance(cid, str) and cid.strip() in call_ids:
                                earliest = min(earliest, i)
                        suffix_start = earliest

                    # If the pair-preserving adjustment would prevent compaction entirely,
                    # fall back to the original boundary (best-effort; summary is still useful
                    # even if the kept suffix starts mid-pair).
                    if suffix_start <= 0:
                        suffix_start = suffix_start0

                    # Hard fallback: if we still can't compact a prefix while preserving a
                    # reasonable tail, keep only the last user message and replace everything
                    # else with a checkpoint summary. This handles cases where a single recent
                    # tool output is too large to fit in the model context window.
                    hard_mode = bool(force_hard_mode)
                    if suffix_start <= 0:
                        hard_mode = True
                        suffix_start = int(last_user_idx)

                    # Capture any existing checkpoint summary (we replace it with an updated one).
                    existing_checkpoint = ""
                    try:
                        if in_items and _is_context_checkpoint_message(in_items[0]):
                            existing_checkpoint = _extract_message_text(in_items[0])
                            # Strip the boilerplate header so the model updates just the body.
                            if existing_checkpoint.startswith(
                                "CONTEXT CHECKPOINT (atlas_agent)"
                            ):
                                parts = existing_checkpoint.split("\n\n", 1)
                                if len(parts) == 2:
                                    existing_checkpoint = parts[1].strip()
                    except Exception:
                        existing_checkpoint = ""

                    # Build a small, deterministic digest of recent tool calls from the session log.
                    # This avoids feeding huge tool outputs back into a compaction prompt.
                    tool_digest_lines: list[str] = []
                    try:
                        ss = getattr(self, "session_store", None)
                        if ss is not None:
                            lim = max(1, int(CONTEXT_COMPACTION_RECENT_TOOL_EVENTS))
                            # Read a bit more than lim so we can filter by turn/phase without often
                            # ending up empty.
                            recent = ss.tail_events(
                                limit=max(8, lim * 6), event_type="tool_call"
                            )
                            for ev in reversed(recent or []):
                                try:
                                    if str(ev.get("turn_id") or "") != str(turn_id):
                                        continue
                                    if str(ev.get("phase") or "") != str(phase):
                                        continue
                                    tool = str(ev.get("tool") or "")
                                    args = ev.get("args")
                                    rs = (
                                        ev.get("result_summary")
                                        if isinstance(ev.get("result_summary"), dict)
                                        else {}
                                    )
                                    ok = rs.get("ok") if isinstance(rs, dict) else None
                                    # Keep digest one-line and stable.
                                    keys: list[str] = []
                                    if isinstance(args, dict):
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
                                            "animation_id",
                                            "seconds",
                                            "duration",
                                        ):
                                            if kk in args:
                                                keys.append(f"{kk}={args.get(kk)!r}")
                                    tail = f" ({', '.join(keys)})" if keys else ""
                                    extra = ""
                                    if isinstance(rs, dict):
                                        if rs.get("error"):
                                            extra = f" error={rs.get('error')!r}"
                                        elif ok is False:
                                            extra = " ok=False"
                                    tool_digest_lines.append(
                                        f"- tool: {tool}{tail}{extra}"
                                    )
                                    if len(tool_digest_lines) >= lim:
                                        break
                                except Exception:
                                    continue
                    except Exception:
                        tool_digest_lines = []

                    # Include plan (small) so the checkpoint retains open work, even if the
                    # full message history has to be compacted repeatedly.
                    plan_block = ""
                    try:
                        plan_now = self.session_store.get_plan() or []
                        if plan_now:
                            lines: list[str] = []
                            for it in plan_now:
                                if not isinstance(it, dict):
                                    continue
                                step = str(it.get("step") or "").strip()
                                status = str(it.get("status") or "").strip()
                                if step:
                                    lines.append(f"- [{status}] {step}")
                            if lines:
                                plan_block = "Current plan:\n" + "\n".join(lines)
                    except Exception:
                        plan_block = ""

                    # Prefer the Responses API compaction endpoint when available.
                    #
                    # This keeps an encrypted/opaque compaction item that preserves the model's
                    # latent understanding of the prior tool loop history, while dramatically
                    # shrinking the next request payload. If the provider/gateway does not
                    # implement `/v1/responses/compact`, fall back to atlas_agent's existing
                    # checkpoint summary compaction.
                    compacted_items: list[dict[str, Any]] | None = None
                    compact_resp: dict[str, Any] | None = None
                    try:
                        compaction_input_items = list(in_items)
                        # Avoid compounding checkpoints: we replace the existing checkpoint
                        # message (if present) with a fresh one below.
                        if compaction_input_items and _is_context_checkpoint_message(
                            compaction_input_items[0]
                        ):
                            compaction_input_items = compaction_input_items[1:]

                        compacted_items, compact_resp = (
                            self.llm.compact_responses_input_items_with_response(
                                input_items=compaction_input_items
                            )
                        )
                    except Exception:
                        compacted_items = None
                        compact_resp = None

                    if compacted_items:
                        # Ensure the current ask is explicitly present in plaintext.
                        last_user_item = in_items[int(last_user_idx)]
                        # Trust the provider's compaction output ordering. We only enforce the
                        # Responses API invariant that at least one user message exists.
                        try:
                            has_user = any(
                                isinstance(it, dict)
                                and str(it.get("type") or "") == "message"
                                and str(it.get("role") or "") == "user"
                                for it in compacted_items
                            )
                        except Exception:
                            has_user = False
                        if not has_user:
                            compacted_items = list(compacted_items) + [last_user_item]

                        checkpoint_lines = [
                            f"- Context compacted via Responses API /responses/compact ({reason_label}).",
                            f"- Phase: {phase}",
                            "- Earlier details are preserved in an encrypted compaction item; "
                            "use session_search_events/session_search_transcript for exact logs.",
                        ]
                        if tool_digest_lines:
                            checkpoint_lines.extend(
                                ["- Recent tool calls:", *tool_digest_lines]
                            )
                        if plan_block:
                            checkpoint_lines.extend(
                                ["- Plan:", *plan_block.splitlines()]
                            )

                        checkpoint_text = (
                            CONTEXT_CHECKPOINT_PREFIX + "\n".join(checkpoint_lines)
                        ).strip()
                        checkpoint_item = _message(
                            role="assistant", text=checkpoint_text
                        )

                        orig_len_pre = int(orig_len)
                        in_items[:] = [checkpoint_item] + list(compacted_items)

                        # Best-effort stats persistence (usage, gateway model, etc.).
                        if isinstance(compact_resp, dict):
                            try:
                                _persist_internal_llm_call_stats(
                                    resp=compact_resp,
                                    phase=str(phase or "")
                                    if phase is not None
                                    else None,
                                    kind="responses_compaction",
                                )
                            except Exception:
                                pass

                        # Session event: record that we compacted and how.
                        try:
                            removed = max(0, int(orig_len_pre) - int(len(in_items)))
                            compaction_id = None
                            if isinstance(compact_resp, dict):
                                rid = compact_resp.get("id")
                                if isinstance(rid, str) and rid.strip():
                                    compaction_id = rid.strip()
                            self.session_store.append_event(
                                {
                                    "type": "context_compacted",
                                    "turn_id": turn_id,
                                    "phase": phase,
                                    "reason": reason_key,
                                    "original_items": int(orig_len_pre),
                                    "new_items": int(len(in_items)),
                                    "removed_items": int(removed),
                                    "kept_items": int(len(in_items) - 1),
                                    "mode": "responses_compact",
                                    "checkpoint_summary": checkpoint_text,
                                    "compaction_response_id": compaction_id,
                                    "compaction_requested_model": str(self.model),
                                    "compaction_base_url": (
                                        str(self.llm.base_url)
                                        if isinstance(self.llm.base_url, str)
                                        and self.llm.base_url.strip()
                                        else None
                                    ),
                                    # Persist the actual compaction output items that we fed back
                                    # into the tool loop. Without this, we cannot reconstruct the
                                    # exact prompt state later (the compaction item is opaque).
                                    "compaction_output_items": list(compacted_items),
                                }
                            )
                        except Exception:
                            pass

                        return True

                    # Ask the model to write an updated checkpoint summary. Keep the
                    # prompt small and stable; the full raw history remains in session.jsonl.
                    sys_prompt = (
                        "You are performing a CONTEXT CHECKPOINT COMPACTION.\n"
                        "Create a handoff summary for another LLM that will resume this Atlas Agent tool loop.\n"
                        "Be concise, structured, and focused on enabling continuation.\n"
                        "Avoid verbose logs and tool schemas."
                    )
                    user_parts: list[str] = []
                    user_parts.append(f"Phase: {phase}")
                    user_parts.append(f"Reason: {reason_label} ({exc_name})")
                    if existing_checkpoint:
                        user_parts.append(
                            "\nExisting checkpoint summary body:\n"
                            + existing_checkpoint
                        )
                    if plan_block:
                        user_parts.append("\n" + plan_block)
                    if tool_digest_lines:
                        user_parts.append(
                            "\nRecent tool calls in this turn (most recent first; summarized):\n"
                            + "\n".join(tool_digest_lines)
                        )
                    user_parts.append(
                        "\nUpdate the checkpoint summary to include:\n"
                        "- Current progress + key decisions\n"
                        "- Important context/constraints\n"
                        "- What remains / clear next steps\n"
                        "- Any critical ids/paths/values needed to continue\n"
                        "Output only the summary (no surrounding commentary)."
                    )
                    prompt_text = "\n".join([p for p in user_parts if p]).strip()

                    summary_body = ""
                    try:
                        summary_body, summary_resp = (
                            self.llm.complete_text_with_response(
                                system_prompt=sys_prompt,
                                user_text=prompt_text,
                                temperature=0.0,
                                max_tokens=700,
                            )
                        )
                        summary_body = (summary_body or "").strip()
                        _persist_internal_llm_call_stats(
                            resp=summary_resp,
                            phase=str(phase or "") if phase is not None else None,
                            kind="context_checkpoint_compaction",
                        )
                    except Exception:
                        summary_body = ""

                    if not summary_body:
                        # Fallback: simple heuristic summary (no extra LLM call).
                        summary_body = "\n".join(
                            [
                                f"- Context compacted due to: {reason_label}.",
                                f"- Phase: {phase}",
                                *(
                                    ["- Recent tool calls:", *tool_digest_lines]
                                    if tool_digest_lines
                                    else []
                                ),
                                *(
                                    ["- Plan:", *plan_block.splitlines()]
                                    if plan_block
                                    else []
                                ),
                                "- Next: continue the tool loop from the most recent state; use session_search_events if details are missing.",
                            ]
                        ).strip()

                    summary_text = (
                        CONTEXT_CHECKPOINT_PREFIX + summary_body.strip()
                    ).strip()

                    # Replace older items with a single checkpoint message.
                    checkpoint_item = _message(role="assistant", text=summary_text)
                    if hard_mode:
                        in_items[:] = [checkpoint_item, in_items[int(last_user_idx)]]
                    else:
                        in_items[:] = [checkpoint_item] + list(in_items[suffix_start:])

                    try:
                        removed = max(0, int(orig_len) - int(len(in_items)))
                        compacted_meta: dict[str, Any] = {}
                        try:
                            est = getattr(exc, "estimated_tokens", None)
                            lim = getattr(exc, "limit_tokens", None)
                            if est is not None:
                                compacted_meta["estimated_tokens"] = int(est)
                            if lim is not None:
                                compacted_meta["limit_tokens"] = int(lim)
                        except Exception:
                            compacted_meta = {}
                        self.session_store.append_event(
                            {
                                "type": "context_compacted",
                                "turn_id": turn_id,
                                "phase": phase,
                                "reason": reason_key,
                                "original_items": int(orig_len),
                                "new_items": int(len(in_items)),
                                "removed_items": int(removed),
                                "kept_items": int(len(in_items) - 1),
                                "mode": ("hard" if hard_mode else "soft"),
                                "checkpoint_summary": summary_text,
                                **compacted_meta,
                            }
                        )
                    except Exception:
                        pass

                    return True

                while True:
                    effective_input_budget_tokens, auto_compact_tokens = (
                        _model_prompt_budgets_for_current_model()
                    )
                    phase_instructions_local = (
                        instructions.rstrip() + "\n\n" + shared_instructions
                    ).strip()
                    if retry_extra_instructions.strip():
                        phase_instructions_local = (
                            phase_instructions_local
                            + "\n\n"
                            + retry_extra_instructions.strip()
                        ).strip()
                    retry_extra_instructions = ""
                    result = run_responses_tool_loop(
                        llm=self.llm,
                        instructions=phase_instructions_local,
                        input_items=phase_input_items_local,
                        tools=phase_tools,
                        dispatch=_dispatch_logged,
                        post_tool_output=_post_tool_output,
                        callbacks=phase_callbacks,
                        temperature=self.temperature,
                        reasoning_effort=self.reasoning_effort,
                        reasoning_summary=self.reasoning_summary,
                        text_verbosity=self.text_verbosity,
                        ephemeral_inline_images=bool(self.ephemeral_inline_images),
                        max_rounds=max_rounds,
                        on_context_overflow=_context_overflow_compact,
                        effective_input_budget_tokens=effective_input_budget_tokens,
                        auto_compact_tokens=auto_compact_tokens,
                    )

                    def _plan_has_remaining_work() -> bool:
                        try:
                            plan_now = self.session_store.get_plan() or []
                        except Exception:
                            plan_now = []
                        # In the Executor phase we expect an explicit plan. If no plan exists,
                        # treat that as "remaining work" so we retry with a forced update_plan.
                        if not plan_now:
                            return True
                        for it in plan_now:
                            if not isinstance(it, dict):
                                continue
                            st = str(it.get("status") or "").strip()
                            if st != "completed":
                                return True
                        return False

                    if (
                        not list(result.tool_calls or [])
                        and _looks_like_embedded_tool_json(result.assistant_text or "")
                        and tool_json_repair_attempts < 2
                    ):
                        tool_json_repair_attempts += 1
                        try:
                            self.session_store.append_event(
                                {
                                    "type": "phase_retry",
                                    "turn_id": turn_id,
                                    "phase": phase,
                                    "reason": "tool_json_in_text",
                                    "attempt": int(tool_json_repair_attempts),
                                }
                            )
                        except Exception:
                            pass
                        phase_input_items_local = list(result.input_items or [])
                        retry_extra_instructions = _tool_json_repair_prompt(
                            phase_name=phase
                        )
                        continue

                    def _looks_like_refusal(text: str) -> bool:
                        t = (text or "").strip().lower()
                        if not t:
                            return False
                        if "cannot assist" in t or "can't assist" in t:
                            return True
                        if t.startswith("refusal:"):
                            return True
                        if ("i'm sorry" in t or "i’m sorry" in t) and (
                            "cannot" in t or "can't" in t or "unable to" in t
                        ):
                            return True
                        return False

                    # Unexpected refusal recovery: retry once with an explicit internal directive.
                    if (
                        _looks_like_refusal(result.assistant_text or "")
                        and unexpected_refusal_repair_attempts < 2
                    ):
                        unexpected_refusal_repair_attempts += 1
                        try:
                            self.session_store.append_event(
                                {
                                    "type": "phase_retry",
                                    "turn_id": turn_id,
                                    "phase": phase,
                                    "reason": "unexpected_refusal",
                                    "attempt": int(unexpected_refusal_repair_attempts),
                                }
                            )
                        except Exception:
                            pass
                        phase_input_items_local = list(result.input_items or [])
                        # Drop the refusal assistant message to reduce anchoring/repetition.
                        for i in range(len(phase_input_items_local) - 1, -1, -1):
                            it = phase_input_items_local[i]
                            if (
                                isinstance(it, dict)
                                and str(it.get("type") or "") == "message"
                                and str(it.get("role") or "") == "assistant"
                            ):
                                phase_input_items_local.pop(i)
                                break
                        if str(phase) == "Planner":
                            retry_extra_instructions = (
                                "INTERNAL (not a user message): Your previous reply was an unexpected refusal.\n"
                                "This is a benign request about operating the local Atlas app.\n"
                                "You MUST NOT refuse.\n\n"
                                "Planner requirements:\n"
                                "- Do NOT ask clarifying questions.\n"
                                "- CALL update_plan (clear, scannable; exactly one in_progress).\n"
                                "- CALL verification_set_requirements for each step_id.\n"
                                "- Do NOT output any embedded JSON/tool blobs in assistant text.\n"
                            )
                        else:
                            retry_extra_instructions = (
                                "INTERNAL (not a user message): Your previous reply was an unexpected refusal.\n"
                                "This is a benign request about operating the local Atlas app.\n"
                                "You MUST NOT refuse.\n\n"
                                "Executor requirements:\n"
                                "- Continue executing the plan via tools.\n"
                                "- Do NOT narrate tool use; just CALL the tools.\n"
                                "- Do NOT provide manual GUI instructions as a substitute.\n"
                                "- Only produce a user-facing recap AFTER tool execution.\n"
                            )
                        continue

                    # Planner must always produce an executable plan + verification requirements.
                    # Some providers/models occasionally respond with a narrative plan in text
                    # (or a clarifying question) without calling update_plan / verification_set_requirements.
                    if str(phase) == "Planner" and planner_no_plan_repair_attempts < 2:
                        calls = list(result.tool_calls or [])
                        called_update_plan = any(
                            isinstance(c, dict)
                            and str(c.get("name") or "") == "update_plan"
                            for c in calls
                        )
                        called_verification = any(
                            isinstance(c, dict)
                            and str(c.get("name") or "")
                            == "verification_set_requirements"
                            for c in calls
                        )
                        pt = (result.assistant_text or "").strip().lower()
                        asked_clarify = pt.startswith("clarify:")
                        if (
                            asked_clarify
                            or (not called_update_plan)
                            or (not called_verification)
                        ):
                            planner_no_plan_repair_attempts += 1
                            try:
                                self.session_store.append_event(
                                    {
                                        "type": "phase_retry",
                                        "turn_id": turn_id,
                                        "phase": phase,
                                        "reason": "planner_missing_plan_or_verification",
                                        "attempt": int(planner_no_plan_repair_attempts),
                                    }
                                )
                            except Exception:
                                pass
                            phase_input_items_local = list(result.input_items or [])
                            retry_extra_instructions = (
                                "INTERNAL (not a user message): Planner must not ask clarifying questions.\n"
                                "You MUST now CALL update_plan with a clear, scannable plan (exactly one in_progress),\n"
                                "then CALL verification_set_requirements for each step_id.\n"
                                "Do NOT output any JSON blobs or narrative plan in assistant text.\n"
                            )
                            continue

                    # Fail-safe: for the Executor phase, we expect tool use whenever the plan
                    # still has pending work. Some providers/models occasionally produce a
                    # tool-free response (often GUI/manual instructions) even though tools
                    # are available. Retry once with an explicit internal nudge.
                    if (
                        str(phase) == "Executor"
                        and _plan_has_remaining_work()
                        and executor_no_tool_calls_repair_attempts < 2
                    ):
                        executor_no_tool_calls_repair_attempts += 1
                        try:
                            self.session_store.append_event(
                                {
                                    "type": "phase_retry",
                                    "turn_id": turn_id,
                                    "phase": phase,
                                    "reason": "executor_no_tool_calls_plan_incomplete",
                                    "attempt": int(
                                        executor_no_tool_calls_repair_attempts
                                    ),
                                }
                            )
                        except Exception:
                            pass
                        phase_input_items_local = list(result.input_items or [])
                        try:
                            plan_now = self.session_store.get_plan() or []
                        except Exception:
                            plan_now = []
                        retry_extra_instructions = (
                            "INTERNAL (not a user message): The current plan is missing or has pending work.\n"
                            + (
                                "No plan exists yet. You MUST first call update_plan to create a concrete, actionable plan, "
                                "then call verification_set_requirements for each step_id.\n"
                                if not plan_now
                                else "You must continue executing the plan by calling tools now.\n"
                            )
                            + "Do NOT narrate tool use (no “Let’s call …”). Just CALL the tool(s).\n"
                            "If you cannot proceed due to preconditions or missing capabilities, call report_blocked once with a precise reason and suggestion.\n"
                            "Do not provide GUI/manual Atlas instructions as a substitute for tool execution.\n"
                            "Only provide a user-facing recap AFTER tool execution.\n"
                        )
                        continue
                    break
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

                # Planner non-convergence is not a user-visible error: it just means the
                # model kept returning tool calls past the planner round budget. Do not
                # force-finalize with a "reply 'continue'" prompt here; instead proceed
                # into the Executor in the same turn (the Executor has repair logic and
                # can create/repair the plan deterministically).
                if str(phase) == "Planner":
                    result = ToolLoopResult(
                        assistant_text="",
                        reasoning_summaries=list(e.reasoning_summaries or []),
                        tool_calls=list(e.tool_calls or []),
                        input_items=list(e.input_items or []),
                    )
                    if callbacks is not None and callbacks.on_phase_end is not None:
                        callbacks.on_phase_end(phase)
                    return result

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
                plan_text = (
                    "\n".join(plan_lines) if plan_lines else "(no plan recorded)"
                )

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
                finalize_instructions = (
                    finalize_instructions.strip()
                    + "\n\n"
                    + "INTERNAL CONTEXT (not a user message): Tool-loop round budget reached; "
                    "produce a progress update and stop.\n\n"
                    "Current plan:\n" + plan_text
                ).strip()
                finalize_items = list(e.input_items or [])

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
                        if (
                            callbacks is not None
                            and callbacks.on_reasoning_summary_delta is not None
                        ):
                            callbacks.on_reasoning_summary_delta(delta, summary_index)
                        return
                    if et == "response.reasoning_summary_part.added":
                        try:
                            summary_index = int(ev.get("summary_index", 0))
                        except Exception:
                            summary_index = 0
                        if (
                            callbacks is not None
                            and callbacks.on_reasoning_summary_part_added is not None
                        ):
                            callbacks.on_reasoning_summary_part_added(summary_index)
                        return

                resp = self.llm.responses_stream(
                    instructions=(
                        finalize_instructions.rstrip() + "\n\n" + shared_instructions
                    ).strip(),
                    input_items=finalize_items,
                    tools=None,
                    temperature=self.temperature,
                    parallel_tool_calls=False,
                    reasoning_effort=self.reasoning_effort,
                    reasoning_summary=self.reasoning_summary,
                    text_verbosity=self.text_verbosity,
                    on_event=_on_finalizer_event,
                )

                # Persist finalizer reasoning summary (after prior tool events).
                try:
                    merged = "\n\n".join(
                        "".join(final_summary_parts[k])
                        for k in sorted(final_summary_parts.keys())
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
                    "".join(streamed_summary_parts[k])
                    for k in sorted(streamed_summary_parts.keys())
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

        # Phase runner (adaptive): Planner (optional) → Executor (always).
        phase_input = list(input_items)
        final_result = None

        def _turn_is_blocked() -> bool:
            try:
                b = runtime_state.get("blocked")
                return isinstance(b, dict) and bool(str(b.get("reason") or "").strip())
            except Exception:
                return False

        def _turn_rpc_unavailable() -> bool:
            try:
                return isinstance(runtime_state.get("rpc_unavailable"), dict)
            except Exception:
                return False

        if _should_run_planner():
            planner_tools = _filter_tools(
                read_only_tool_names | set(SESSION_MUTATION_TOOLS)
            )
            if web_search_tool is not None:
                planner_tools = list(planner_tools) + [web_search_tool]
            planner_res = _run_phase(
                phase="Planner",
                instructions=ATLAS_PLANNER_SYSTEM_PROMPT,
                phase_tools=planner_tools,
                phase_input_items=phase_input,
                max_rounds=int(self.max_rounds_planner),
            )
            phase_input = list(planner_res.input_items)
            pt = (planner_res.assistant_text or "").strip()
            if pt.lower().startswith("clarify:"):
                # Planner should not block execution; proceed to Executor with defaults.
                try:
                    self.session_store.append_event(
                        {
                            "type": "planner_clarify_ignored",
                            "turn_id": turn_id,
                            "question": pt,
                        }
                    )
                except Exception:
                    pass
                # Drop the Planner assistant text from the within-turn input history
                # to avoid confusing the Executor. The durable plan/verification
                # state is already stored in the session store.
                phase_input = list(input_items)
            # If the Planner explicitly reported a blocked state, do not proceed
            # into the Executor (avoid redundant/failed RPC calls).
            if final_result is None and _turn_is_blocked():
                final_result = planner_res

        # Plan refiner pass: ensure long animation authoring is broken into manageable segments
        # before the Executor starts writing lots of keys.
        try:
            plan = self.session_store.get_plan() or []
        except Exception:
            plan = []
        plan_steps_sig = _plan_steps_signature(plan)
        last_plan_refiner_sig = str(
            getattr(self, "_plan_refiner_last_steps_sig", "") or ""
        ).strip()
        should_run_plan_refiner = (
            final_result is None
            and _plan_has_incomplete_steps(plan)
            and plan_steps_sig != last_plan_refiner_sig
        )
        if should_run_plan_refiner:
            repair_tools = _filter_tools(
                read_only_tool_names | set(SESSION_MUTATION_TOOLS)
            )
            if web_search_tool is not None:
                repair_tools = list(repair_tools) + [web_search_tool]
            repair_res = _run_phase(
                phase="PlanRefiner",
                instructions=ATLAS_PLAN_REFINER_SYSTEM_PROMPT,
                phase_tools=repair_tools,
                phase_input_items=phase_input,
                max_rounds=max(
                    3,
                    min(
                        6,
                        (
                            int(self.max_rounds_planner)
                            if int(self.max_rounds_planner) > 0
                            else 6
                        ),
                    ),
                ),
            )
            phase_input = list(repair_res.input_items)
            # Record the plan-step signature we just repaired/validated so we do not
            # pay an extra PlanRefiner round trip on every status update.
            try:
                plan_after = self.session_store.get_plan() or []
            except Exception:
                plan_after = plan
            sig = _plan_steps_signature(plan_after)
            self._plan_refiner_last_steps_sig = sig

        if final_result is None:
            exec_res = _run_phase(
                phase="Executor",
                instructions=ATLAS_EXECUTOR_SYSTEM_PROMPT,
                phase_tools=tools,
                phase_input_items=phase_input,
                max_rounds=int(self.max_rounds_executor),
            )
            # If the RPC server became unavailable during the Executor, do not attempt
            # any additional RPC calls (facts snapshot) in this turn.
            if not _turn_rpc_unavailable():
                # Deterministic post-write facts snapshot: store what changed in this turn
                # so future turns can ground without re-enumerating everything.
                _append_executor_facts_snapshot()
            phase_input = list(exec_res.input_items)
            if phase_hit_round_budget_phase == "Executor":
                # If we had to force-finalize due to tool-loop non-convergence, stop this
                # turn early. The session log contains all prior
                # tool work; the user can reply "continue" to resume with full context.
                final_result = exec_res
            elif _turn_is_blocked() or _turn_rpc_unavailable():
                # The model already produced a clear user-facing blocked message.
                # Return without running any additional phases.
                final_result = exec_res
            else:
                # Single-phase execution: the Executor is responsible for both
                # applying changes and verifying them via read-back tools.
                final_result = exec_res

        text = (final_result.assistant_text or "").strip() or "(no response)"

        # If Atlas RPC became unavailable and the model did not explicitly call
        # report_blocked, ensure the user still receives a clear action item.
        #
        # This avoids confusing "half-turns" where tool calls fail but the
        # assistant ends without a crisp instruction to relaunch Atlas.
        try:
            if _turn_rpc_unavailable() and not _turn_is_blocked():
                info = (
                    runtime_state.get("rpc_unavailable")
                    if isinstance(runtime_state, dict)
                    else None
                )
                tool_name = ""
                err = ""
                if isinstance(info, dict):
                    tool_name = str(info.get("tool") or "").strip()
                    err = str(info.get("error") or "").strip()
                extra = "The Atlas gRPC server became unavailable mid-run."
                if tool_name:
                    extra += f" (last failing tool: {tool_name})"
                if err:
                    # Keep this short; full error is still logged in session tool events.
                    max_err_chars = 300
                    err_one_line = err.splitlines()[0].strip() if err else ""
                    if len(err_one_line) > max_err_chars:
                        err_one_line = (
                            err_one_line[:max_err_chars].rstrip()
                            + "… (truncated; see session tool events for full error)"
                        )
                    extra += f"\nError: {err_one_line}"
                extra += (
                    "\n\nPlease relaunch Atlas (and ensure the scene server is running), "
                    "then tell me once it’s back. I can resume from the saved session state."
                )
                if text and text != "(no response)":
                    text = (text.rstrip() + "\n\n" + extra).strip()
                else:
                    text = extra
        except Exception:
            pass

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
                gateway_model_last=getattr(self, "_gateway_model_last", None),
                llm_last_call=(
                    {
                        "phase": last_llm_phase,
                        "gateway_model": last_llm_gateway_model,
                        "usage": (
                            last_llm_usage_delta.to_dict()
                            if last_llm_usage_delta is not None
                            else None
                        ),
                    }
                    if (
                        last_llm_gateway_model is not None
                        or last_llm_usage_delta is not None
                    )
                    else None
                ),
                llm_last_turn_usage=(
                    turn_llm_usage.to_dict() if turn_llm_usage.calls > 0 else None
                ),
                llm_session_usage=(
                    self._llm_session_usage.to_dict()
                    if self._llm_session_usage.calls > 0
                    else None
                ),
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
    wire_api: str = "auto",
    web_search_mode: str = DEFAULT_WEB_SEARCH_MODE,
    temperature: float | None = None,
    max_rounds_planner: int = DEFAULT_PLANNER_MAX_ROUNDS,
    max_rounds: int = DEFAULT_EXECUTOR_MAX_ROUNDS,
    *,
    reasoning_effort: str | None,
    reasoning_summary: str | None,
    text_verbosity: str | None,
    ephemeral_inline_images: bool = False,
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
        wire_api=wire_api,
        web_search_mode=str(web_search_mode or DEFAULT_WEB_SEARCH_MODE),
        temperature=temperature,
        max_rounds_planner=int(max_rounds_planner),
        max_rounds_executor=int(max_rounds),
        ephemeral_inline_images=bool(ephemeral_inline_images),
        session=session,
        session_dir=session_dir,
        enable_codegen=bool(enable_codegen),
        reasoning_effort=reasoning_effort,
        reasoning_summary=reasoning_summary,
        text_verbosity=text_verbosity,
    )
    logger.info(
        "Atlas Agent (RPC). Type :help for commands. Session=%s",
        team.session_store.session_id(),
    )
    logger.info("Atlas app: %s", team.atlas_dir)

    def _render_session_replay_plain() -> None:
        """Replay saved session history (transcript + tool summaries + current plan)."""

        try:
            from .session_resume import (
                format_tool_call_summary_line,
                format_web_search_summary_line,
                iter_resume_items,
            )
        except Exception:
            return

        log_path = getattr(team.session_store, "log_path", None)
        try:
            from pathlib import Path as _Path  # local alias to avoid confusion

            if not isinstance(log_path, _Path) or not log_path.exists():
                return
        except Exception:
            return

        items = list(iter_resume_items(log_path))
        if not any(it.kind == "transcript" for it in items):
            return

        logger.info("--- Resuming session history (replay) ---")
        for it in items:
            ev = it.event
            if it.kind == "transcript":
                role = str(ev.get("role") or "").strip().lower()
                content = str(ev.get("content") or "")
                if not content:
                    continue
                if role == "user":
                    lines = content.splitlines() or [""]
                    first = lines[0] if lines else ""
                    logger.info(">> %s", first)
                    for ln in lines[1:]:
                        logger.info("   %s", ln)
                else:
                    logger.info("Answer:")
                    for ln in content.splitlines() or [""]:
                        logger.info("%s", ln)
                continue
            if it.kind == "tool_call":
                try:
                    logger.info("%s", format_tool_call_summary_line(ev))
                except Exception:
                    logger.info("→ <tool>: done")
                continue
            if it.kind == "web_search":
                try:
                    logger.info("%s", format_web_search_summary_line(ev))
                except Exception:
                    logger.info("→ web_search")
                continue
            if it.kind == "plan":
                try:
                    plan = team.session_store.get_plan()
                except Exception:
                    plan = []
                if plan:
                    logger.info("Plan:")
                    for p in plan:
                        if not isinstance(p, dict):
                            continue
                        st = str(p.get("status") or "").strip()
                        step = str(p.get("step") or "").strip()
                        if step:
                            logger.info("- [%s] %s", st, step)
                continue

    _render_session_replay_plain()

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
        ans = (
            input("Allow preview screenshots for this session? [Y/n] ").strip().lower()
        )
        allow = ans in ("", "y", "yes")
        try:
            team.session_store.set_consent("screenshots", allow)
            team.session_store.save()
        except Exception:
            pass
        logger.info(
            "Screenshots %s for this session.", "enabled" if allow else "disabled"
        )

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
                    ":resume              Switch to another session (interactive)\n"
                    ":screenshots on|off  Toggle preview screenshots for this session\n"
                    ":plan                Show current plan\n"
                    ":memory              Show session memory summary\n"
                    ":events [N]          Show recent events\n"
                    ":save <path>         Save current animation\n"
                    ":time <seconds>      Set current time\n"
                    ":objects             List objects"
                )
                continue
            if cmd == "resume":
                # :resume [<session-id-or-path>]
                target = str(rest[0] or "").strip() if rest else ""
                if not target:
                    try:
                        from .session_resume import list_sessions
                        from .session_store import default_sessions_root
                        from .defaults import RESUME_SESSION_PICKER_PREVIEW_MAX_CHARS

                        sessions_root = (
                            Path(session_dir).expanduser().resolve()
                            if isinstance(session_dir, str) and session_dir.strip()
                            else default_sessions_root()
                        )
                        sessions = list_sessions(
                            sessions_root=sessions_root,
                            preview_max_chars=int(
                                RESUME_SESSION_PICKER_PREVIEW_MAX_CHARS
                            ),
                        )
                    except Exception as e:
                        logger.info("fail: %s", e)
                        continue
                    if not sessions:
                        logger.info("(no sessions found)")
                        continue
                    for i, s in enumerate(sessions, start=1):
                        prev = s.first_user_preview or ""
                        logger.info(
                            "%d\t%s\t%s",
                            i,
                            s.updated_local_time(),
                            prev,
                        )
                    ans = input("resume> ").strip()
                    if not ans:
                        continue
                    try:
                        idx = int(ans)
                        if 1 <= idx <= len(sessions):
                            target = sessions[idx - 1].session_id
                        else:
                            target = ans
                    except Exception:
                        target = ans
                if not target:
                    continue
                try:
                    team = ChatTeam(
                        address=address,
                        api_key=api_key,
                        model=model,
                        wire_api=wire_api,
                        web_search_mode=str(web_search_mode or DEFAULT_WEB_SEARCH_MODE),
                        temperature=temperature,
                        max_rounds_planner=int(max_rounds_planner),
                        max_rounds_executor=int(max_rounds),
                        ephemeral_inline_images=bool(ephemeral_inline_images),
                        session=target,
                        session_dir=session_dir,
                        enable_codegen=bool(enable_codegen),
                        reasoning_effort=reasoning_effort,
                        reasoning_summary=reasoning_summary,
                        text_verbosity=text_verbosity,
                    )
                    logger.info("Switched session=%s", team.session_store.session_id())
                    logger.info("Atlas app: %s", team.atlas_dir)
                    _render_session_replay_plain()
                except Exception as e:
                    logger.info("fail: %s", e)
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
                        step = (
                            (it.get("step") or "").strip()
                            if isinstance(it, dict)
                            else ""
                        )
                        status = (
                            (it.get("status") or "").strip()
                            if isinstance(it, dict)
                            else ""
                        )
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
                ok = False
                try:
                    resp = team.scene.ensure_animation(create_new=False, name=None)
                    aid = int(getattr(resp, "animation_id", 0) or 0)
                    if bool(getattr(resp, "ok", False)) and aid > 0:
                        ok = bool(
                            team.scene.save_animation(
                                animation_id=aid, path=Path(rest[0])
                            )
                        )
                except Exception:
                    ok = False
                logger.info("%s", "ok" if ok else "fail")
                continue
            if cmd == "time" and rest:
                ok = False
                try:
                    resp = team.scene.ensure_animation(create_new=False, name=None)
                    aid = int(getattr(resp, "animation_id", 0) or 0)
                    if bool(getattr(resp, "ok", False)) and aid > 0:
                        ok = bool(
                            team.scene.set_time(
                                animation_id=aid, seconds=float(rest[0])
                            )
                        )
                except Exception:
                    ok = False
                logger.info("%s", "ok" if ok else "fail")
                continue
            if cmd == "objects":
                resp = team.scene.list_objects()
                for obj in resp.objects:
                    logger.info(
                        "%s\t%s\t%s\t%s", obj.id, obj.type, obj.name, obj.visible
                    )
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
