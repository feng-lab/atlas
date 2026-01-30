Atlas Agents Guide (Unified)

This guide includes:
- System overview and concepts
- Tooling stability contract
- Tooling reference (strict inputs)

Atlas Agents System (Scene + Animation)

Overview
- Tool-using agent that designs animations live in Atlas via gRPC, with preview in the GUI timeline.
- Single entry point: a chat interface. The agent saves .animation3d via RPC and, when asked, exports MP4 by invoking Atlas headless in the background (no extra CLI commands for the user).
 - Concepts:
   - Scene (.scene): current objects and their display parameters across 2D/3D. Saving a scene restores the view/state.
   - Animation (.animation2d/.animation3d): extends Scene with a timeline. Each display parameter (and camera) has keys with easing at specific times.
   - Playback rule: During playback, animation keys override scene values for affected parameters. To change what plays, write/replace timeline keys (not scene_apply).

Quickstart
- Run chat (only command):
  - `atlas-agent --model gpt-5.2` (console UI by default)
  - Optional plain REPL (no styling): `atlas-agent --plain --model gpt-5.2`

Docs Discovery (runtime)
- Atlas ships markdown docs inside the app bundle (same content as `docs/` in the repo).
- The agent can search and read these docs at runtime:
  - `docs_search` (find relevant sections)
  - `docs_read` (read a specific excerpt by line range)
  - `docs_list` (enumerate available docs)
- Key docs:
  - `SCENE_SERVER.md` (RPC semantics + contracts)
  - `AGENTS_GUIDE.md` (tooling contract + best practices)
  - `USER_GUIDE.md` (user-facing workflows)
  - `DEVELOPER_GUIDE.md` (engine architecture + threading rules)

Schema Discovery
- Preferred (default): the agent asks the running Atlas RPC server for its install location (authoritative).
- If the RPC server is not reachable (Atlas not open yet), the CLI will:
  1) detect Atlas in common install locations,
  2) launch Atlas,
  3) re-try RPC discovery until it succeeds (or times out).
- Common install locations (used for launch only):
  - macOS: `/Applications/fenglab/Atlas.app`, `/Applications/Atlas.app`
  - Windows: `C:\\Program Files\\fenglab\\Atlas`, `C:\\Program Files (x86)\\fenglab\\Atlas`
  - Linux: `/opt/fenglab/Atlas`, `/opt/fenglab/atlas`, `/usr/local/fenglab/Atlas`
- RPC protos are shipped with the running app at `Resources/protos/scene.proto`; the agent compiles Python stubs from that file so it always matches the running Atlas version (no monorepo fallback).

Headless Rendering
- When the agent exports video on your behalf, it invokes Atlas with the headless exporter (`--run_export_3d_animation`), adding `-platform offscreen` on Windows/Linux.

Notes
- Headless export uses the Atlas binary. The agent uses the running app's reported install location by default, and falls back to standard install locations when needed.
- The chat runtime uses the OpenAI Python SDK + the Responses API for streaming reasoning summaries + tool-calling.

Screenshots (optional)
- Visual verification can use `scene_screenshot` to render a single frame of the **current scene** to a temp image (preferred; does not create animations).
- When you specifically need to verify an **animation** at a particular time, the agent can use `animation_render_preview` (heavier; runs the animation exporter). This tool returns exactly one PNG file path.
- To maximize model/provider compatibility, the agent only uploads common image formats for visual inspection (prefer PNG; also accepts JPEG/WEBP/GIF when supported).
- On startup, the CLI asks once per session for consent to use preview screenshots for verification.
- You can toggle later in the REPL: `:screenshots on` / `:screenshots off`.
- If a requirement can't be verified from tools or screenshots, the agent will request a human-check step.
- Screenshots are primarily for model-based visual inspection. Do not ask the user to open temp screenshot files; if a human check is still needed, ask them to check in the Atlas UI.

Codegen Toggle
- Code generation helpers are disabled by default and gated behind a flag. Enable with `--enable-codegen` when invoking the chat agent. When disabled, the `python_write_and_run` tool is hidden and calls are rejected.

Web Search (optional)
- Atlas Agent can optionally expose the OpenAI Responses API built-in `web_search` tool (Codex-style) to let the model look things up.
- Enable with `--web-search cached` (cached content; no live internet) or `--web-search live` (allows live internet access; provider-controlled).
- What “cached” means: the model can issue search/browse actions, but the provider will serve cached results (no external browsing from your machine and no live outbound internet fetch). This is usually more deterministic and privacy-friendly than live browsing.
- Default is `--web-search off`.
- Requires the Responses API. If you force `--wire-api chat` (or the provider forces a fallback to Chat Completions), web search is not sent.

Session Artifacts (File Writes)
- The agent can write helper files into a per-session artifacts folder (for intermediate outputs or hand-off files) using:
  - `artifact_write_text` (UTF-8 text)
  - `artifact_write_json` (JSON)
  - `artifact_write_bytes_base64` (binary)
- Safety: these tools only accept **relative paths** and always write under `<session>/artifacts` (they cannot write outside the session).
- Privacy: session logs redact file contents; tool-call args store only size + sha256 digests for debugging.
- Phase rule: artifacts writes are allowed in Executor; the Planner phase remains read-only.

No Other CLI Commands
- All actions happen through chat. Ask to load data, set keys, play/pause, save, or export and the agent will call the right tools under the hood.

Natural Language Examples
- Filesystem selection (auto-expands into Doc):
  - "Use swc files in /data/neuron_traces"
  - "Use image files in ./slices with name starts with 'slice__'"
  - "Add file soma.ply and fibers.obj"
- Camera motion:
  - "Orbit around mesh 2 for 12 seconds with radius 250 on y axis"
  - "Orbit around soma.ply for 8s, then dolly in"
- Per-object effects:
  - "Toggle all swc files off at time 10 seconds"
  - "Increase the opacity of file fibers.obj from 0 to 1 during time 10-12 seconds"
  - "Color of file soma.ply from red to blue during time 5-8 seconds"

Atlas Agent (live)
- Start: `atlas-agent --model gpt-5.2` (starts a console UI by default).
  - If Atlas is not running, the CLI attempts to launch it from a common install location and then waits for the RPC server.
- Long/complex tasks: set `--max-rounds 0` to allow the Executor phase to run an unbounded number of tool-loop rounds (no “did not converge after max_rounds” failures). Otherwise increase `--max-rounds` as needed.
- Runtime loop (streaming tool loop):
  - Phases (adaptive by default):
    - Planner: may run first to create/refresh the plan (read-only tools + `update_plan` only).
    - Executor: performs changes by calling tools.
    - Verifier: runs only if Executor made Atlas changes; verifies via read-only tools and updates the plan, then produces the final answer.
  - Uses the OpenAI Responses API with tool-calling to operate Atlas through the gRPC server.
  - Streams a first-person “Reasoning summary” as the model thinks, then executes tool calls step-by-step.
  - Stores deterministic evidence in the session log `session.jsonl` (tool calls + summarized results by default).
- Session memory (context-window resilience):
  - The runtime maintains a compact “Session Memory” summary so long conversations remain stable even when raw history exceeds the model context window.
  - This is built-in (not tuned via environment variables). Use the REPL command `:memory` to inspect what is stored.
- Context checkpoint compaction (within-turn resilience):
  - If a provider rejects a model call due to context window overflow, the runtime compacts older within-turn tool-loop history into a short “CONTEXT CHECKPOINT” summary and retries.
  - The runtime may also compact proactively when the estimated prompt budget is approaching the model’s effective input budget.
  - This compaction is only for prompt budgeting; the full append-only session log is still preserved on disk for deterministic resume/debug.
- Sessions (resume across restarts):
  - The chat runtime stores a single append-only session log (`session.jsonl`) containing domain events (plan/memory/verification/meta/consent), transcript entries, tool calls, and verification evidence. Durable state is reconstructed by replaying the log.
  - Flags:
    - `--session <id-or-path>`
    - `--session-dir <path>`
    - `--replay-reasoning-summary` (optional; include saved reasoning summaries in terminal replay when resuming)
  - REPL helpers: `:session`, `:plan`, `:memory`, `:budget`, `:screenshots on|off`
  - Default session location when `--session-dir` is omitted:
    - macOS/Linux: `$XDG_STATE_HOME/atlas_agent/sessions` if set, otherwise `~/.atlas_agent/sessions`
    - Windows: `%APPDATA%\\atlas_agent\\sessions`
- Auto-retrieval (context-window resilience):
  - When the user says “resume/continue/last time”, the runtime can inject a small, explicit “Auto-retrieved context” block derived from the session transcript + events log.
  - This is intentionally a small excerpt. For exact prior messages or more history, use session tools like `session_search_transcript` / `session_search_events`.
- Reasoning summary (first-person, streamed):
  - When the model supports reasoning summaries, the runtime streams them (Responses API `reasoning.summary`) so users can see the agent’s intent and verification plan before tool writes.
  - This is a high-level summary (what it plans to do and how it will verify), not chain-of-thought.

Inspector (legacy / optional)
- The current default runtime is a single tool-using agent that performs its own verification via tools.
- The Inspector role described below is legacy / optional design guidance for a verification-only sub-agent if reintroduced.
- No tools: The Inspector never calls tools or reads files. It judges only from the merged plan, scene context, verified Facts JSON, and an optional preview image.
- No out‑of‑band requests: It must not ask users to paste/upload logs or run shell commands. Filesystem/network access is out of scope for the Inspector.
- Facts‑first: Treat Facts as authoritative. If something is not present in Facts, do not assume it is missing in the scene.
- Pass when uncertain: Approve by default when evidence is insufficient. Only fail when Facts clearly contradict the core intent (e.g., required keys/values are missing or wrong, wrong objects changed, or camera_validation.ok=false).
- Feedback is advisory: Use the feedback field for non‑blocking notes (limitations, qualitative comments). Use TODO updates only as checkbox lines when helpful; do not block solely on naming or unverifiable external inputs.

Filesystem Tools (Best Practices)
- Exact paths: If the user provides an explicit absolute path (e.g., `/Users/...`, `~/...`, `C:\\...`), treat it as exact.
  - Use `fs_expand_paths` then `fs_check_paths` to verify it exists.
  - If it does not exist, use `fs_resolve_path` for typo correction (case/pluralization/prefix) rather than fuzzy-searching unrelated folders.
- Natural-language hints: If the user describes a location in words (“in my Documents/atlas_test folder”), use `fs_hint_resolve` with structured inputs (`expected_name` + `possible_dirs`) to turn the hint into candidate paths. Prefer this over guessing full paths.
  - Prefer passing structured inputs rather than relying on brittle hint parsing:
    - `expected_name`: the basename to score against (e.g. `test_st.stitching_log.txt`)
    - `possible_dirs`: likely search roots (e.g. `["~/Documents/atlas_test"]`)
    - If the user references a well-known folder name (“Documents”, “Downloads”, “Desktop”), use `system_info` to resolve it to an absolute path and build `possible_dirs` explicitly (e.g. `common_dirs.Documents + "/atlas_test"`).
  - `expected_name` is a scoring hint (not an exact-match contract). The tool returns `match:"exact"` when the best candidate basename matches `expected_name` case-insensitively; otherwise it returns `match:"best_candidate"` and includes a `hint` explaining that validation is required.
  - Always validate the selected path by reading/checking it before loading.
- Logs (simple, robust):
  - `fs_tail_lines`: last N lines (UTF‑8, BOM‑aware). Minimal params: `path`, `n`.
  - `fs_tail_bytes`: last K bytes (UTF‑8, BOM‑aware). Minimal params: `path`, `bytes`.
- Advanced reads (rare):
  - `fs_read_text` supports byte windows via `start`/`length` and line windows via `start_line`/`line_count` (regex optional). Semantics are symmetric:
    - `start` < 0 and `start_line` < 0 count from end.
    - Omitting `length` reads to end; omitting `line_count` reads to end. `line_count` requires `start_line` (use negative `start_line` to tail last N lines).
    - Provide both fields to read exact windows; no other special cases.
    - Avoid mixing byte and line windows in one call. If both are provided, fs_read_text reads the byte window first and then applies the line slice within that window (allowed but discouraged).
- Prefer these tools over custom file ops. Use `fs_hint_resolve` for user file hints. (Developer note: `repo_search` is dev-only and not exposed to the default LLM tool surface.)

Docs Tools (Best Practices)
- Prefer docs search over guessing semantics:
  - Use `docs_search(query, include_paths=["SCENE_SERVER.md"])` for RPC semantics and tool contracts.
  - Use `docs_search(query, include_paths=["USER_GUIDE.md"])` for user-facing workflows (GUI names/menus, export behavior).
  - For large result sets, page with `offset` + `max_results` (no silent truncation).
- Read excerpts narrowly:
  - Use `docs_read(doc_name="SCENE_SERVER.md", start_line=..., line_count=...)` rather than whole-file reads.

Natural Language Contract (Summary‑first)
- Before any keys are written, require a concise Plan Summary with two synchronized views:
- Global timeline view: a list of changes `{ time, target(id), json_key?, value, easing? }` where id=0 camera, ≥4 objects.
- Per‑target view: for each id, list `{ json_key, time, value, easing? }`.
- Rules:
- Use canonical `json_key` names discovered from `scene_list_params(id)` (or `scene_capabilities`). The dispatcher assists with resolution: when a tool call supplies a display name (e.g., "Coord Transform") or a non-canonical key that differs only by a type suffix (e.g., missing " 3DTransform"), it resolves to the canonical `json_key` using live `scene_list_params` metadata. The resolved mapping is cached per-id to make subsequent calls reliable.
- Unified id addressing: All tools accept `id` as a single field. Reserved ids mirror engine special ids:
  - 0=camera (exposed as a typed scene value via `"Camera 3DCamera"` and as timeline keys)
  - 1=background, 2=axis, 3=global
  - ≥4=object ids
  - Camera steps must be typed via camera tools (Fit/Orbit/Dolly/Validate); do not invent raw camera numbers.
  - Scene (stateless): use `scene_apply` (no time/easing). Animation (timeline): use `animation_*` to write/replace keys; during playback keys override scene values.
- The Supervisor injects a Task Brief into the shared context. The Implementer must derive intent strictly from the Task Brief (do not reclassify).
- Arbiter appends a TODO section (checkboxes) to the merged plan with human‑readable, minimal steps. Implementer focuses on these tasks; avoid extra work.

---

Atlas Agent Tooling Contract (Stability Guidelines)

Purpose
- Provide a stable, curated tool surface for LLM function-calling. The Script API may evolve independently.

Stability rules
- Tool names are part of the public contract; avoid renames. Add new tools rather than changing existing names.
- Parameter shapes should remain backward compatible. Add optional fields; avoid removing or changing semantics.
- Return shapes should include stable fields (`ok`, and typed payloads). Additional optional fields are allowed.
- Prefer generic tools (batch/apply/validate) over one-offs to minimize surface churn.

Categories and current tools (non-exhaustive)
- File/FS: `system_info`, `fs_expand_paths`, `fs_check_paths`, `fs_glob`, `fs_resolve_path`
- Session: `session_info`, `session_get_plan`, `session_get_memory`, `session_search_transcript` (paging: `offset`/`max_results`, newest-first: `reverse=true`), `session_search_events` (same)
- Session (extras): `session_tail_events` (quick "recent events" without crafting a search query)
- Docs: `docs_list`, `docs_search`, `docs_read`
- Planning: `update_plan`
- Load: `scene_load_sources` (canonical; local + network), `scene_smart_load` (resolve-by-name for local files), task API: `scene_start_load_task`, `scene_wait_task`, `scene_cancel_task`, `scene_delete_task`
- Readiness: `scene_wait_objects_ready` (deterministic wait for 3D view/filter binding; not full progressive data completion)
- Scene (stateless): `scene_get_values(id,json_keys)`, `scene_validate_params`, `scene_apply`, `scene_save_scene`, `scene_screenshot`, `scene_set_visibility`, `scene_remove_objects`, `scene_make_alias`
- Discovery: `scene_list_objects`, `scene_list_params(id)`, `scene_capabilities`, `scene_schema`, `scene_capabilities_summary`, `scene_facts_summary`
- Timeline: `animation_ensure_animation(create_new,name)` (returns `animation_id`), then `animation_list_keys(animation_id,id,json_key,include_values)`, `animation_batch(animation_id,...)`, `animation_set_key_param(animation_id,...)`, `animation_replace_key_param(animation_id,...)`, `animation_set_duration(animation_id,seconds)`, `animation_set_time(animation_id,seconds)`, `animation_play`, `animation_pause`, `animation_save_animation(animation_id,path)`
- Camera: producers (typed values) — `fit_candidates`, `camera_get`, `camera_focus`, `camera_point_to`, `camera_rotate`, `camera_reset_view`, `camera_move_local`, `camera_look_at`, `camera_path_solve`; scene apply — `scene_camera_fit`, `scene_camera_apply`; animation (timeline) — `animation_camera_solve_and_apply(animation_id,...)`, `animation_replace_key_camera(animation_id,...)`, `animation_camera_validate(animation_id,...)`, `animation_camera_waypoint_spline_apply(animation_id,...)`, `animation_camera_walkthrough_apply(animation_id,...)`
- Geometry/Cuts: `scene_bbox`, `scene_cut_suggest`, `scene_cut_set`, `scene_cut_clear`

Notes
- The Script API (`atlas_agent.api.*`) may add helpers (plan builders, layouts) without changing the Agent Tooling.
- When the Script API adds a capability that belongs in Agent Tooling, introduce it via a new tool function and document it here.

Folder Loads (UI parity)
- `scene_load_sources` and `scene_start_load_task` accept local directory paths in addition to files/URLs.
  - A directory source is expanded **non-recursively** into the regular files directly inside the folder (symlinks skipped), matching the GUI drag-and-drop behavior.
  - If some files are unsupported/unreadable, the load continues for the rest; failures are surfaced via task warnings/errors (partial success is possible).
- Performance note: loading a whole folder can take a while and may create many objects. When possible, prefer loading a smaller subset (e.g., pre-filter with `fs_glob`) instead of pointing at a huge directory.

Scene Loads
- `scene_load_sources` can load Atlas scene files (`*.scene`) because the GUI load path treats `.scene` specially.
- A `.scene` load may re-use existing objects (no "new ids"), so `loaded_ids` can legitimately be empty even when the scene successfully applies view state.
  - For the authoritative post-load object list, use `task_status.load.objects`.
  - When `wait_ready=true`, `scene_load_sources` also returns `ready_ids` and `ready_status` to make subsequent bbox/camera/param calls deterministic.

Scene vs Timeline contract (for LLMs)
- Scene (stateless): `scene_validate_params` → `scene_apply` edits base scene only. Validation returns `{ok, results:[{json_key, ok, reason?, normalized_value?}]}` and performs no writes. It never writes keys and must not include times/easing.
- Timeline (animated): use `animation_set_key_param`/`animation_replace_key_param`/`animation_batch` (and camera equivalents) to write keys at times with easing. Keys override scene values at playback.
- Any mention of time (e.g., “at 6.5s”) implies timeline, not scene_apply. Use the `animation_*` tools.
- To change an existing animated moment, replace the key at that time (use easing=Switch for non‑interpolatable params like StringIntOption).

---

Agent Tools (Discovery — no duplication)

Source of truth
- Tools are defined in code and surfaced to LLMs at runtime via the tools parameter. Do not mirror schemas in docs.
- Location: `python/atlas_agent/src/atlas_agent/agent_team/tools_agent.py:scene_tools_and_dispatcher`.

Discover at runtime
- Print the live tool list (names + JSONSchemas) from a Python shell:
  ```bash
  python - <<'PY'
  from atlas_agent.scene_rpc import SceneClient
  from atlas_agent.agent_team.tools_agent import scene_tools_and_dispatcher
  import json
  sc = SceneClient(address='localhost:50051')
  tools, _ = scene_tools_and_dispatcher(sc)
  print(json.dumps(tools, indent=2, ensure_ascii=False))
  PY
  ```

Guidance
- Use `scene_list_params(id)` and `capabilities.json` for parameter metadata (types, option_names, descriptions); avoid hardcoding.
- Validate before applying: `scene_validate_params` for scene values and `animation_camera_validate` for timeline camera. Handle soft errors (`type_mismatch`, `option_invalid`, etc.).

## Camera Usage (Produce → Apply)

Producers (no side effects)
- `camera_get()` → returns the current engine camera as a typed camera value (useful as a base for scene operations, or for seeding the first camera key in a new animation).
- `camera_focus(ids?, after_clipping=true, min_radius=0.0)` → returns a typed camera value.
- `camera_point_to(ids?, after_clipping=true)` → returns a typed camera value.
- `camera_reset_view(mode, ids?, after_clipping=true, min_radius=0.0)` → returns a typed camera value.
- `camera_rotate(op, degrees, base_value)` → returns a typed camera value.
- `camera_move_local(op, distance, distance_is_fraction_of_bbox_radius=true, move_center=true, base_value)` → returns a typed camera value.
  - Use this for first-person walkthrough building blocks: `FORWARD/BACK/LEFT/RIGHT/UP/DOWN`.
- `camera_look_at(world_point | target_bbox_center | bbox_fraction_point, base_value)` → returns a typed camera value (sets camera center; keeps eye).
- `camera_path_solve(waypoints, ids?, base_value)` → returns `{time,value}` keys from waypoint geometry (does not write keys).
 - Read current scene camera: `scene_get_values(id=0, json_keys=["Camera 3DCamera"])` (or pass empty `json_keys` to retrieve it among all values).

Scene-only (stateless) apply
- Apply a typed camera to the scene camera (no keys): `scene_camera_apply(value=<typed_camera>)` or `scene_apply([{id:0, json_key:"Camera 3DCamera", value:<typed_camera>}])`.
- One-shot fit and apply: `scene_camera_fit(ids?, after_clipping=true, min_radius=0.0)` (internally uses CameraFit and applies the result).

Animation (timeline) authoring
- Full-state baseline (UI parity):
  - When you create a new Animation3D in the GUI, Atlas seeds the timeline with a full keyframe at `t=0`, capturing the current scene state for **all parameters** (camera + all objects + background/axis/global). This prevents playback from falling back to scene values.
  - In agent workflows, ensure the same determinism:
    - `animation_ensure_animation(..., create_new=true)` creates the animation and captures the default `t=0` keyframe.
    - If you load/add objects after creating the animation, call `animation_save_keyframe(animation_id, time=0)` to add baseline keys for the new objects.
- Full-scene keyframing (useful workflow to consider):
  - `animation_save_keyframe(animation_id, time=t)` is the RPC equivalent of editing the scene and clicking **Save Key Frame** in the GUI.
  - This is often the fastest way to author “beats” in an animation: pose the scene at a few key times (camera + object visibility/appearance/transforms), save keyframes, then rely on interpolation between beats.
  - You can also refine with per-parameter key tools (`animation_set_key_param`, `animation_replace_key_*`, `animation_batch`) when you need finer control than whole-scene snapshots provide.
- Solve and write keys: `animation_camera_solve_and_apply(animation_id, mode, ids, t0, t1, constraints?, params?, degrees?, …)`.
  - Continuity: this tool sets the engine timeline time to `t0` before solving so chained segments (e.g., ORBIT then STATIC) start from the timeline pose at the boundary instead of the current UI camera.
  - Tip: for ORBIT, use `degrees` (default 360) and optionally `max_step_degrees` to control key density (default 90; smaller → more keys/smoother). Use `params.axis` (default `"y"`).
  - FIT/STATIC semantics: the solver produces a single key at `t0` (it does **not** fill keys across the interval). A “hold” happens naturally because the timeline evaluates the last key until the next key. Avoid writing a second camera key at the same boundary time with `easing="Switch"` unless you intentionally want a jump cut.
  - If `clear_range=true` and `t1 > t0`, existing camera keys inside `[t0,t1]` may be removed (except solver key times) — set `t1=t0` (or `clear_range=false`) when you only want a one-shot fit.
  - Validate camera key sequences: `animation_camera_validate(animation_id, ids, times, values?, constraints?, policies?)` (values optional; when omitted, the server samples from `animation_id` at those times).
    - Aspect ratio note: validation assumes a 16:9 “planning viewport” (matches common export defaults), not the current UI window size.
- Sample the camera from the timeline (no validation, no key writes): `animation_camera_sample(animation_id, times)` → `samples:[{time,value}]`.
  - Use this to get a deterministic `base_value` for `camera_rotate/camera_move_local/camera_look_at` while editing an existing animation.
- Single-time explicit write: `animation_replace_key_camera(animation_id, time, value, easing?, allow_jump_cut?)`.
  - Note: `easing="Switch"` creates an instantaneous jump cut. By default the tool rejects large Switch jumps (prevents accidental boundary “resets”); pass `allow_jump_cut=true` only when a cut is intentional (or tune `max_switch_jump_fraction`).
- Guided waypoint spline (one-shot apply):
  - `animation_camera_waypoint_spline_apply(animation_id, t0, t1, base_value, waypoints=[...], constraints?, clear_range=true, easing="Linear")`
  - Tip: prefer `base_value = animation_camera_sample(animation_id, times=[t0]).samples[0].value` so the path is anchored to the timeline.
  - Optional: `look_at_policy="bbox_center"` fills missing `look_at` so the camera keeps tracking the bbox center; default preserves the previous view direction when `look_at` is omitted.
  - Note: this tool does **not** change the animation duration; call `animation_set_duration(animation_id, seconds)` separately if needed.
- First-person walkthrough (one-shot apply):
  - `animation_camera_walkthrough_apply(animation_id, t0, t1, base_value, segments=[...], step_seconds=1.0, constraints={keep_visible:false})`
  - Tip: prefer `base_value = animation_camera_sample(animation_id, times=[t0]).samples[0].value` so motion is anchored to the existing timeline.
  - Optional: `look_at_policy="bbox_center"` keeps aiming at the bbox center (third-person track) and interprets yaw/pitch as azimuth/elevation around the center; default preserves first-person yaw/pitch look control.
  - Note: this tool does **not** change the animation duration; call `animation_set_duration(animation_id, seconds)` separately if needed.

Notes
- Prefer “produce → apply” for clarity: use `camera_*` producers to compute a typed camera, then choose scene vs. animation by the apply tool.
- The previous names `camera_solve_and_apply` and `camera_validate` are removed; use `animation_camera_*` for timeline operations. For scene‑only verification, do not call `animation_list_keys`; read the current camera via `scene_get_values(id=0, 'Camera 3DCamera')` or use `scene_camera_fit/scene_camera_apply`.
- Respect the Scene vs Timeline contract above; do not include time/easing in scene operations.
- Camera timeline tools evaluate camera keys using a stable look-at + distance convention so authored keys evaluate deterministically.

## Camera Workflows (Walkthrough + Waypoints)

## Camera Director Rubric (Default Policy)

Atlas supports multiple camera authoring styles. Users may describe either style (or a mix), so the agent should **route** requests consistently and apply stable defaults.

Routing: pick the representation
- Prefer **guided waypoint spline** when the user gives explicit spatial beats:
  - “waypoint 1/2/3…”, “go to A then B then C”, explicit coordinates, or bbox-fraction points like `[0.5,0.5,0.8]`.
  - Tool: `animation_camera_waypoint_spline_apply(...)`
- Prefer **first-person walkthrough** when the user describes motion verbs rather than locations:
  - “fly forward”, “strafe right”, “ascend”, “yaw left”, “look around”, “pause”.
  - Tool: `animation_camera_walkthrough_apply(...)`
- Mixed prompts:
  - If the user provides 3–6 major beats AND describes motion between them, treat the beats as waypoints and add intermediate motion via additional waypoints or by using walkthrough segments for the in-between portion.
  - Never drop user-provided waypoints/segments. If the plan would generate too many keys, increase `step_seconds` (walkthrough) rather than truncating the path.

Default timing policy
- If the user gives a total duration only: use `t0=0` and `t1=duration`, and call `animation_set_duration(animation_id, duration)`.
- Waypoints:
  - If waypoint times are omitted, map them evenly across `[t0,t1]` using `u` in `[0,1]`.
- Walkthrough segments:
  - If segments have explicit `duration`, normalize durations to fill `[t0,t1]`.
  - Otherwise, split `[t0,t1]` evenly across segments.

Default smoothing policy
- For walkthroughs and waypoint fly-throughs, prefer **denser sampling** (`step_seconds` smaller) and/or add **intermediate waypoints** to smooth motion.

Default “speed” mapping (heuristics)
- If the user says **slow / cinematic / gentle**:
  - Use smaller motion per segment and denser sampling (e.g., `step_seconds` around `0.5`–`1.0`).
- If the user says **fast / quick**:
  - Use coarser sampling (e.g., `step_seconds` around `1.5`–`2.0`) to avoid generating excessive keys.
- If no speed is specified:
  - Use `step_seconds=1.0` and keep segment count modest (but increase when motion needs curvature/precision).

Default motion magnitude mapping (bbox-scaled)
- Walkthrough move distances are interpreted as **fractions of bbox radius** (so the same script works across datasets).
- If the user gives no numeric distances:
  - “slightly / a bit” ≈ `0.05`–`0.15`
  - “forward into it / enter” ≈ `0.3`–`0.7`
  - “deep inside” ≈ `0.7`–`1.2` (may require multiple segments)
- Rotation defaults when no degrees are specified:
  - “slight turn” ≈ `15°`
  - “turn” ≈ `45°`
  - “turn right/left” (strong) ≈ `90°`
  - “look around” ≈ a sequence of `yaw/pitch` moves rather than a single 180° snap.

Default look/aim policy
- Walkthrough: default to “look forward” (preserve view direction). Only lock `look_at` when the user explicitly requests it.
- Waypoints: default to preserving direction (when `look_at` is omitted, the solver keeps the previous view direction + center distance). Use `look_at_policy="bbox_center"` (or explicit waypoint `look_at`) when the intent is “keep the object centered / orbit around it”.

### Camera Policy Matrix (Safe Combinations)

This is the agent-facing “policy matrix” for camera authoring tools. The goal is to keep the model choosing **intent knobs** (key density, aim behavior, visibility constraints) rather than low-level interpolation representations that can silently change semantics.

Terminology (avoid ambiguity)
- **Key easing**: per-key transition type (Qt/QEasingCurve names, e.g., `Linear`, `InOutQuad` (ease-in-out), `Switch`). This is what `easing=` controls in agent tools.
- **Camera interpolation**: Atlas evaluates camera keys using a stable look-at + distance convention (interpolates look-at target + view distance + orientation).

Hard rule (representation safety)
- Camera interpolation is fixed to a stable look-at + distance convention so authored keys mean what the tools intend (orbit/dolly/track).
- For different motion “feel”, use intent knobs (key density, aim policy, visibility constraints) rather than trying to change camera interpolation.

Tool-level matrix (what to use, and what knobs are safe)

| Tool | Best for | Default framing | Default aim behavior | Primary “smoothness” knob | Notes / common pitfalls |
|---|---|---|---|---|---|
| `animation_camera_solve_and_apply(mode="ORBIT")` | Exterior orbit / rotate-around-object shots | `constraints.keep_visible=true` (presentation framing) | Implicitly targets bbox center (orbit is around the target bbox center) | `max_step_degrees` (smaller → more keys → smoother) | If the orbit feels “wrong”, it is almost always key density or target selection (ids). Prefer tuning `max_step_degrees` over inventing new representations. |
| `animation_camera_solve_and_apply(mode="DOLLY")` | Zoom/dolly in/out while keeping the subject framed | `constraints.keep_visible=true` | Implicitly targets bbox center (center distance changes) | Split into multiple DOLLY windows (more keys) and/or use easing | DOLLY requires explicit `params.start_dist/end_dist` (>0) to create motion. If you don’t know absolute distances, use `animation_camera_walkthrough_apply` with `look_at_policy="bbox_center"` and a bbox-scaled `move.forward/back` segment. If you want an “arc” (move+rotate), DOLLY alone is not enough—use waypoints or walkthrough. |
| `animation_camera_waypoint_spline_apply(...)` | Camera path through explicit spatial beats (A→B→C) | `constraints.keep_visible=true` | `look_at_policy="preserve_direction"` by default; optionally lock to bbox center | Add intermediate waypoints (spatial key density) | For “orbit around object”, either use ORBIT solve or set `look_at_policy="bbox_center"` / explicit `look_at`. Leaving `look_at` omitted will preserve direction and can look like a drift rather than a track. |
| `animation_camera_walkthrough_apply(...)` | First-person “drone” flythroughs and interior exploration | `constraints.keep_visible=false` (exploration framing) | `look_at_policy="preserve_direction"` by default; optionally track bbox center | `step_seconds` (smaller → more keys → smoother curved motion) | For third-person track/orbit behavior, set `look_at_policy="bbox_center"` (switches yaw/pitch to azimuth/elevation and uses `move_center=false`). Don’t try to approximate ORBIT with first-person yaw unless the intent is truly “look around while moving”. |

Aim policy semantics (explicit)
- `look_at_policy="preserve_direction"`:
  - Interprets `yaw/pitch` as first-person camera look (YAW/PITCH).
  - Interprets local moves as “fly” (translate eye + center together; maintains view direction).
- `look_at_policy="bbox_center"`:
  - Locks camera center to the target bbox center (third-person tracking).
  - Interprets `yaw/pitch` as AZIMUTH/ELEVATION around the target (orbit-like).
  - Interprets local moves as “boom/dolly” (translate eye only; center stays on target).

Default validation constraints
- Walkthrough/interior: default `constraints.keep_visible=false` (user intent is exploration, not framing the whole bbox).
- Exterior presentation/orbit: default `constraints.keep_visible=true` with `min_frame_coverage=0.0` (no minimum size; only enforce no-cropping).
  - `min_frame_coverage` is a **screen-space** framing metric (0..1 dominant-dimension bbox fill). Higher values push toward tighter framing (larger subjects).
  - For close-ups, validate/solve against a smaller set of `ids` and raise `min_frame_coverage` (and keep `margin` small). For intentional cropping / interior exploration, use `keep_visible=false`.

### First-person walkthrough (freeform)

Use this when you want to **enter** a volume/mesh and navigate “like a drone” (not an orbit).

Typical tool pattern (preferred: one-shot apply):
1) `animation_ensure_animation` → capture `animation_id`, then `animation_set_duration(animation_id, seconds)`
2) `base_value = animation_camera_sample(animation_id, times=[t0]).samples[0].value` (use `camera_focus(...)` or `camera_get()` only when seeding a new timeline with no camera keys yet)
3) `animation_camera_walkthrough_apply(animation_id, t0, t1, base_value, segments=[...], step_seconds=1.0, constraints={keep_visible:false})`
4) Verify:
   - `animation_list_keys(animation_id, id=0)` and optionally `animation_camera_validate(animation_id, ids, times, values?, constraints={keep_visible:false})`

Low-level pattern (when you need exact control over each pose):
1) `animation_ensure_animation` → capture `animation_id`, then `animation_set_duration(animation_id, seconds)`
2) Build a sequence of camera values by chaining:
   - `animation_camera_sample(animation_id, times=[t0])` → base_value (preferred when editing an existing timeline)
   - `camera_get()` → base_value (useful for seeding a new timeline before any camera keys exist)
   - `camera_move_local(FORWARD/RIGHT/UP, distance_is_fraction_of_bbox_radius=true, base_value=...)`
   - `camera_rotate(YAW/PITCH, degrees, base_value=...)`
   - `camera_look_at(...)` (optional, for guided aiming; requires base_value)
3) For each produced camera value, write a key:
   - `animation_replace_key_camera(animation_id, time=..., value=..., constraints={keep_visible:false})` for interior shots
4) Verify:
   - `animation_list_keys(animation_id, id=0)` and optionally `animation_camera_validate(animation_id, ids, times, values?, constraints={keep_visible:false})`

Prompting guidance (user-facing):
- “Create a 12s first-person walkthrough: start outside the volume, fly forward into it, then slowly yaw right while ascending; keep it smooth and avoid snap turns.”
- “Do a slow interior fly-through; it’s OK if the object goes out of frame (keep_visible=false).”

### Guided waypoint spline (bbox/world waypoints)

Use this when you want a **controlled camera path** through specific locations (waypoints). For smoother motion between sparse waypoints, add intermediate waypoints.

Typical tool pattern:
1) `animation_ensure_animation` → capture `animation_id`, then `animation_set_duration(animation_id, t1)`
2) `base_value = animation_camera_sample(animation_id, times=[t0]).samples[0].value` (use `camera_focus(...)` or `camera_get()` only when seeding a new timeline with no camera keys yet)
3) `animation_camera_waypoint_spline_apply(animation_id, t0, t1, base_value, waypoints=[...])`
   - Waypoints can use bbox fractions to avoid guessing world units:
     - `eye: {bbox_fraction:[fx,fy,fz]}`
     - `look_at: {bbox_center:true}` or `look_at: {bbox_fraction:[fx,fy,fz]}`
4) Verify:
   - `animation_list_keys(animation_id, id=0)` and optionally `animation_camera_validate(animation_id, ids, times, values?, constraints={keep_visible:false})`

Prompting guidance (user-facing):
- “Make a 10s guided fly-through: waypoint 1 outside the front face, waypoint 2 inside the center, waypoint 3 near the top-right corner; look at the bbox center throughout.”
- “Use bbox fractions for waypoints so it works across datasets.”
