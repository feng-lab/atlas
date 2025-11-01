Atlas Agents System (Scene + Animation)

Overview
- Multi‑agent system that designs animations live in Atlas via gRPC, with preview in the GUI timeline.
- Single entry point: a chat interface. The agent saves .animation3d via RPC and, when asked, exports MP4 by invoking Atlas headless in the background (no extra CLI commands for the user).

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
  - Execute with atomic key updates, adjust, and explain rationale concisely

Natural Language Contract (Summary‑first)
- Before any keys are written, require a concise Plan Summary with two synchronized views:
  - Global timeline view: a list of changes `{ time, target(camera|object|group), json_key?, value, easing? }`.
  - Per‑object view: for each object/group, list `{ json_key, time, value, easing? }`.
- Rules:
  - Use canonical `json_key` names discovered from `scene_list_params` (or `scene_capabilities`).
  - Camera steps must be typed via camera tools (Fit/Orbit/Dolly/Validate); do not invent raw camera numbers.
  - For scene (stateless) edits, use `scene_apply` (no time/easing); for animations, use `Batch`/`SetKey` with verification.
  - The Implementer translates the Plan Summary directly to tool calls and verifies with `scene_list_keys` and/or `scene_get_values`.

Grounding tools for summaries
- `scene_capabilities_summary(schema_dir?, max_lines?)` → NL overview of parameter catalogs (groups + object types).
- `scene_list_objects()` + `scene_list_params(scope)` + `scene_get_values(scope, json_keys?)` → facts snapshot used to enrich the Plan Summary.
 - `scene_params_handbook(schema_dir?, include_groups?, ...)` → generate a Markdown handbook of parameters from capabilities.json for quick reference (no code shortcuts).

Parameter discovery and docs
- Agents do not guess parameters. They must enumerate canonical json_keys via `scene_list_params` and consult `capabilities.json` / `animation3d.schema.json` when in doubt. 
- Typical arrangement flow (stateless):
  - `scene_list_objects` → pick targets
  - `scene_list_params(scope_object=ID)` → pick canonical transform json_key (correct type/shape)
  - `scene_bbox(ids)` → compute world‑space cell sizes (≥ extents × (1+margin))
  - Build `set_params` with correct value shapes; `scene_validate_apply` → `scene_apply`; verify with `scene_get_values`

Agents Architecture and Guidelines
- Two lanes, one plan:
  - Scene lane (stateless): scene_get_values, scene_validate_apply, scene_apply, scene_save_scene. No time/easing; atomic and idempotent.
  - Animation lane (timeline): EnsureAnimation, Batch/SetKey, ListKeys/SetTime, typed camera planning/validation.
- Summary‑first: require a Plan Summary (two views) before any writes. Use canonical json_key names; typed camera only.
- Intent guard: classify user requests (file load/import, static scene mgmt, animation, preview/playback, save/export). For file/static, never create animations or write keys.
- Strong Python Script core: compute layouts/paths in Python; use the Script API for plan building, validation, and reliable execution. Agents orchestrate planning/execution.
- Verification and safety: always verify keys/values after apply; keep changes atomic; avoid one‑off “arrange_xxx” RPCs — compute in Python and apply via general tools.

Agent Tooling vs Script API
- Agent Tooling (LLM function-calling)
  - Curated, safe, idempotent tools exposed to the Implementer (e.g., scene_apply, scene_validate_apply, scene_batch, camera_*). Tools return compact JSON and perform verification where applicable.
  - Located in `tools/atlas_agent/agent_team/tools_agent.py` (shim re-export of the stable entry point).
- Script API (typed Python for programmatic control)
  - Developer-friendly modules for building and executing plans in code. Prefer these for compute-heavy orchestration and reproducibility.
  - Modules:
    - `tools.atlas_agent.api.plan_types` — dataclasses for `Plan`, `SetParam`, `SetKey`, `RemoveKey`.
    - `tools.atlas_agent.api.fs` — filesystem helpers (`expand_paths`, `check_paths`, `glob_dir`, `resolve_path`, `repo_search`).
    - `tools.atlas_agent.api.scene` — typed wrappers around `SceneClient` (raise exceptions on failure).
    - `tools.atlas_agent.api.runner` — `run_plan()` helper (validate → apply → verify).
  - Philosophy: strict typing, exceptions on error, builders/utilities for larger workloads, fewer guardrails than the Agent Tooling.
