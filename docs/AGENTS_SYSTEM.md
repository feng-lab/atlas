Atlas Agents System (Scene + Animation)

Overview
- Multi‑agent system that designs animations live in Atlas via gRPC, with preview in the GUI timeline.
- Single entry point: a chat interface. The agent saves .animation3d via RPC and, when asked, exports MP4 by invoking Atlas headless in the background (no extra CLI commands for the user).
 - Concepts:
   - Scene (.scene): current objects and their display parameters across 2D/3D. Saving a scene restores the view/state.
   - Animation (.animation2d/.animation3d): extends Scene with a timeline. Each display parameter (and camera) has keys with easing at specific times.
   - Playback rule: During playback, animation keys override scene values for affected parameters. To change what plays, write/replace timeline keys (not scene_apply).

Quickstart
- Run chat (only command):
  - `python -m tools.atlas_agent chat-rpc --address localhost:50051 --model o4`

Schema Discovery
- Preferred: pass `--atlas-dir` (installation root). The tool derives both the Atlas binary and the schema under it.
- If `--atlas-dir` is not provided, it searches common defaults and errors if not found:
  - macOS: `/Applications/fenglab/Atlas.app`, `/Applications/Atlas.app`
  - Windows: `C:\\Program Files\\fenglab\\Atlas`, `C:\\Program Files (x86)\\fenglab\\Atlas`
  - Linux: `/opt/fenglab/Atlas`, `/opt/fenglab/atlas`, `/usr/local/fenglab/Atlas`
- Alternatively, provide `--schema-dir` or set `ATLAS_SCHEMA_DIR`.

Headless Rendering
- When the agent exports video on your behalf, it invokes Atlas with the headless exporter (`--run_export_3d_animation`), adding `-platform offscreen` on Windows/Linux.

Notes
- Headless export uses the Atlas binary; set `--atlas-dir` or install Atlas to standard locations.
- Requires the Agents SDK (pip package name `agents`) for multi‑agent chat. Install it to enable session management and future MCP tool support.

Codegen Toggle
- Code generation helpers are disabled by default and gated behind a flag. Enable with `--enable-codegen` or `ATLAS_AGENT_ENABLE_CODEGEN=1` when invoking the chat agent. When disabled, the `python_write_and_run` tool is hidden and calls are rejected.

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

Multi‑Agent (live)
- Start: `python -m tools.atlas_agent chat-rpc --address localhost:50051 --model o4`
- The agents (Supervisor, Planner, Inspector) will:
  - Query scene state (objects, bbox, params) via tools
  - Propose ≥2 plans, critique, ask clarifying questions if inputs are ambiguous
  - Execute against a simple TODO list (see below): agents act on the listed tasks; adjust iteratively and explain rationale concisely

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
  Legacy scope addressing has been removed from the agent API in favor of id-only.
  - Camera steps must be typed via camera tools (Fit/Orbit/Dolly/Validate); do not invent raw camera numbers.
  - Scene (stateless): use `scene_apply` (no time/easing). Animation (timeline): use `animation_*` to write/replace keys; during playback keys override scene values.
- The Supervisor injects a Task Brief into the shared context. The Implementer must derive intent strictly from the Task Brief (do not reclassify).
- Arbiter appends a TODO section (checkboxes) to the merged plan with human‑readable, minimal steps. Implementer focuses on these tasks; avoid extra work.

TODO List (Plan → Execute)
- Minimal, human‑readable list of steps for the current turn using checkboxes (e.g., `- [ ] Load files`, `- [x] Fit camera`).
- Authored/updated by Arbiter; Implementer executes against it; Inspector verifies results and may suggest adjustments.
- Inspector updates TODO status after verification:
  - Implementer reads TODOs and executes; Inspector returns an updated checkbox list (if present), which Supervisor merges into the session ledger.

- Session TODO Ledger
- Supervisor maintains an explicit TODO ledger based on the TODO section (checkboxes), with statuses mapped to [ ] → pending, [x] → applied.
- The ledger is included in context each round and summarized in responses, providing continuity across rework rounds and user turns.
- Agents do not invent TODOs; items originate from the manifest or user instructions.

Grounding tools for summaries
- `scene_capabilities_summary()` → overview of parameter catalogs (background/axis/global + object types).
- `scene_list_objects()` + `scene_list_params(id)` + `scene_get_values(id,json_keys)` → facts snapshot used to enrich the Plan Summary.
 - `scene_params_handbook(schema_dir?, include_groups?, ...)` → generate a Markdown handbook of parameters from capabilities.json for quick reference (no code shortcuts).

Parameter discovery and docs
- Agents do not guess parameters. They must enumerate canonical json_keys via `scene_list_params` and consult `capabilities.json` / `animation3d.schema.json` when in doubt. 
- Typical arrangement flow (stateless):
  - `scene_list_objects` → pick targets
  - `scene_list_params(id=OBJECT_ID)` → pick canonical transform json_key (correct type/shape)
  - `scene_bbox(ids)` → compute world‑space cell sizes (≥ extents × (1+margin))
  - Build `set_params` with correct value shapes; `scene_validate_apply` → `scene_apply`; verify with `scene_get_values`. When in doubt about the key spelling, provide `name` instead of `json_key` and the dispatcher will resolve it.
  - Prefer id addressing (0/1/2/3/≥4) to avoid object/group ambiguity.

Agents Architecture and Guidelines
- Two lanes, one plan:
  - Scene lane (stateless): scene_get_values, scene_validate_apply, scene_apply, scene_save_scene. No time/easing; atomic and idempotent.
  - Animation lane (timeline): EnsureAnimation, Batch/SetKey, ListKeys/SetTime, typed camera planning/validation.
- Summary‑first: require a Plan Summary (two views) before any writes. Use canonical json_key names; typed camera only.
- Intent guard: classify user requests (file load/import, static scene mgmt, animation, preview/playback, save/export). For file/static, never create animations or write keys.
- Strong Python Script core: compute layouts/paths in Python; use the Script API for plan building, validation, and reliable execution. Agents orchestrate planning/execution.
- Verification and safety: always verify keys/values after apply; keep changes atomic; avoid one‑off “arrange_xxx” RPCs — compute in Python and apply via general tools.

Session Memory (ctx_with_history)
- Only the Intent Resolver consumes full chat history to produce a self‑contained Task Brief for the current turn.
- Downstream agents receive compact context: facts snapshot + Task Brief (no conversation history).
- Implementer and Inspector additionally receive only the merged plan text (and the TODO ledger snapshot), not the original options or reviewer feedback. This reduces redundancy and keeps execution grounded to the chosen plan.
 - Additionally, a compact TODO ledger snapshot is included to avoid repeating work across rework rounds.

Who Runs Python Codegen?
- When enabled, the Implementer may use code generation for complex calculations. In that mode it runs short scripts that import `tools.atlas_agent.api` via the `python_write_and_run` tool.
- Implementer follows a plan‑only → validate → apply → verify loop; scripts print compact JSON for machine parsing and are iterated on until success under guardrails.

Agent Tooling vs Script API
- Agent Tooling (LLM function-calling)
  - Curated, safe, idempotent tools exposed to the Implementer (stateless: scene_*; timeline: animation_*). Tools return compact JSON and perform verification where applicable.
  - Located in `tools/atlas_agent/agent_team/tools_agent.py` (shim re-export of the stable entry point).
- Script API (typed Python for programmatic control)
  - Developer-friendly modules for building and executing plans in code. Prefer these for compute-heavy orchestration and reproducibility.
  - Modules:
    - `tools.atlas_agent.api.plan_types` — dataclasses for `Plan`, `SetParam`, `SetKey`, `RemoveKey`.
    - `tools.atlas_agent.api.fs` — filesystem helpers (`expand_paths`, `check_paths`, `glob_dir`, `resolve_path`, `repo_search`).
    - `tools.atlas_agent.api.scene` — typed wrappers around `SceneClient` (raise exceptions on failure).
    - `tools.atlas_agent.api.runner` — `run_plan()` helper (validate → apply → verify).
  - Philosophy: strict typing, exceptions on error, builders/utilities for larger workloads, fewer guardrails than the Agent Tooling.

Task Brief (Intent Resolution)
- The Intent Resolver consumes full chat history and the latest user turn, and emits a minimal Task Brief that consolidates context so downstream agents do not need history.
- If the turn is ambiguous, it returns one concise clarifying question; otherwise it proceeds with defaults and lists them as Assumptions.
- The Task Brief is intentionally high‑level — it classifies intent and highlights inputs/constraints, leaving design details to the Designer:
  - Intent (scene | animation | mixed | playback | save | explain)
  - Targets (ids/names if known) and Inputs (files/patterns)
  - Assumptions (defaults due to ambiguity)
  - Signals (e.g., “update scene”, “update animation”) and Duration when explicitly provided
  - Verify (what success looks like), without prescribing steps or parameter names
This keeps roles clean: Resolver merges context and sets direction; Designer plans the how.

Camera Segmentation Rule (Chaining)
- For segmented camera motions (e.g., 4×90° AZIMUTH for a 360° orbit), always chain: each `camera_rotate` call must use the previous camera value as `base_value`. Do not rotate from the initial camera repeatedly.
- Typical pattern: `v0 = camera_focus(ids)` → write at `t0`; then `v1 = camera_rotate(op='AZIMUTH', degrees=Δ, base_value=v0)` at `t1`; `v2 = camera_rotate(..., base_value=v1)` at `t2`; … until `tn`.

Duration Handling
- When a user specifies a total duration, the Implementer must call `animation_set_duration(duration_seconds)` explicitly. Verify via `animation_get_time()`.

Example: 360° in 10s (segmented)
- ids = `fit_candidates()`
- `v0 = camera_focus(ids)`; `t = [0, 2.5, 5.0, 7.5, 10.0]`
- `v1 = camera_rotate('AZIMUTH', 90, base_value=v0)` at `t[1]`
- `v2 = camera_rotate('AZIMUTH', 90, base_value=v1)` at `t[2]`
- `v3 = camera_rotate('AZIMUTH', 90, base_value=v2)` at `t[3]`
- `v4 = camera_rotate('AZIMUTH', 90, base_value=v3)` at `t[4]`
- Validate with `camera_validate`; apply keys; then `animation_set_duration(10)` and verify times with `animation_list_keys_camera(json_key="")`.
