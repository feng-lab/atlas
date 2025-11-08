Atlas Agents Guide (Unified)

This guide includes:
- System overview and concepts
- Tooling stability contract
- Tooling reference (strict inputs)

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

Inspector Behavior (Verification‑only)
- No tools: The Inspector never calls tools or reads files. It judges only from the merged plan, scene context, verified Facts JSON, and an optional preview image.
- No out‑of‑band requests: It must not ask users to paste/upload logs or run shell commands. Filesystem/network access is out of scope for the Inspector.
- Facts‑first: Treat Facts as authoritative. If something is not present in Facts, do not assume it is missing in the scene.
- Pass when uncertain: Approve by default when evidence is insufficient. Only fail when Facts clearly contradict the core intent (e.g., required keys/values are missing or wrong, wrong objects changed, or camera_validation.ok=false).
- Feedback is advisory: Use the feedback field for non‑blocking notes (limitations, qualitative comments). Use TODO updates only as checkbox lines when helpful; do not block solely on naming or unverifiable external inputs.

Filesystem Tools (Best Practices)
- Path resolution: Use `fs_hint_resolve` to turn natural language hints into candidate paths (anchors on ~/Documents, ~/Downloads, ~/Desktop, plus optional bases). Prefer this over guessing full paths.
- Logs (simple, robust):
  - `fs_tail_lines`: last N lines (UTF‑8, BOM‑aware). Minimal params: `path`, `n`.
  - `fs_tail_bytes`: last K bytes (UTF‑8, BOM‑aware). Minimal params: `path`, `bytes`.
- Advanced reads (rare):
  - `fs_read_text` supports byte windows via `start`/`length` and line windows via `start_line`/`line_count` (regex optional). Semantics are symmetric:
    - `start` < 0 and `start_line` < 0 count from end.
    - Omitting `length` reads to end; omitting `line_count` reads to end. `line_count` requires `start_line` (use negative `start_line` to tail last N lines).
    - Provide both fields to read exact windows; no other special cases.
    - Avoid mixing byte and line windows in one call. If both are provided, fs_read_text reads the byte window first and then applies the line slice within that window (allowed but discouraged).
- Prefer these tools over custom file ops. `repo_search` is not exposed to the LLM; `fs_hint_resolve` is the recommended entry.

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
- File/FS: `system_info`, `fs_expand_paths`, `fs_check_paths`, `fs_glob`, `fs_resolve_path`, `repo_search`
- Load: `scene_load_files`, `scene_ensure_loaded`, `scene_smart_load`
- Scene (stateless): `scene_get_values(id,json_keys)`, `scene_validate_apply`, `scene_apply`, `scene_save_scene`
- Discovery: `scene_list_objects`, `scene_list_params(id)`, `scene_capabilities`, `scene_schema`, `scene_capabilities_summary`, `scene_facts_summary`
- Timeline: `animation_list_keys(id,json_key,include_values)`, `animation_batch`, `animation_set_key_param`, `animation_replace_key_param`, `animation_set_duration`, `animation_set_time`, `animation_play`, `animation_pause`, `animation_save_animation`
- Camera: producers (typed values) — `fit_candidates`, `camera_focus`, `camera_point_to`, `camera_rotate`, `camera_reset_view`; scene apply — `scene_camera_fit`, `scene_camera_apply`; animation (timeline) — `animation_camera_solve_and_apply`, `animation_camera_validate`
- Geometry/Cuts: `scene_bbox`, `scene_cut_suggest`, `scene_cut_set`, `scene_cut_clear`

Notes
- The Script API (`tools.atlas_agent.api.*`) may add helpers (plan builders, layouts) without changing the Agent Tooling.
- When the Script API adds a capability that belongs in Agent Tooling, introduce it via a new tool function and document it here.

Scene vs Timeline contract (for LLMs)
- Scene (stateless): `scene_validate_apply` → `scene_apply` edits base scene only. It never writes keys and must not include times/easing.
- Timeline (animated): use `animation_set_key_param`/`animation_replace_key_param`/`animation_batch` (and camera equivalents) to write keys at times with easing. Keys override scene values at playback.
- Any mention of time (e.g., “at 6.5s”) implies timeline, not scene_apply. Use the `animation_*` tools.
- To change an existing animated moment, replace the key at that time (use easing=Switch for non‑interpolatable params like StringIntOption).

---

Agent Tools (Discovery — no duplication)

Source of truth
- Tools are defined in code and surfaced to LLMs at runtime via the tools parameter. Do not mirror schemas in docs.
- Location: `tools/atlas_agent/agent_team/tools_agent.py:scene_tools_and_dispatcher`.

Discover at runtime
- Print the live tool list (names + JSONSchemas) from a Python shell:
  ```bash
  python - <<'PY'
  from tools.atlas_agent.scene_rpc import SceneClient
  from tools.atlas_agent.agent_team.tools_agent import scene_tools_and_dispatcher
  import json
  sc = SceneClient(address='localhost:50051')
  tools, _ = scene_tools_and_dispatcher(sc)
  print(json.dumps(tools, indent=2, ensure_ascii=False))
  PY
  ```

Guidance
- Use `scene_list_params(id)` and `capabilities.json` for parameter metadata (types, option_names, descriptions); avoid hardcoding.
- Validate before applying: `scene_validate_apply` (scene) and `animation_camera_validate` (timeline camera). Handle soft errors (`type_mismatch`, `option_invalid`, etc.).

## Camera Usage (Produce → Apply)

Producers (no side effects)
- `camera_focus(ids?, after_clipping=true, min_radius=0.0)` → returns a typed camera value.
- `camera_point_to(ids?, after_clipping=true)` → returns a typed camera value.
- `camera_reset_view(mode, ids?, after_clipping=true, min_radius=0.0)` → returns a typed camera value.
- `camera_rotate(op, degrees, base_value?)` → returns a typed camera value.
 - Read current scene camera: `scene_get_values(id=0, json_keys=["Camera 3DCamera"])` (or pass empty `json_keys` to retrieve it among all values).

Scene-only (stateless) apply
- Apply a typed camera to the scene camera (no keys): `scene_camera_apply({value})` or `scene_apply([{id:0, json_key:"Camera 3DCamera", value}])`.
- One-shot fit and apply: `scene_camera_fit(ids?, after_clipping=true, min_radius=0.0)` (internally uses CameraFit and applies the result).

Animation (timeline) authoring
- Solve and write keys: `animation_camera_solve_and_apply(mode, ids, t0, t1, constraints?, params?, degrees?, …)`.
  - Tip: for ORBIT, use `degrees` (default 360). The agent maps this to the backend as needed. Use `params.axis` (default `"y"`).
- Validate camera key sequences: `animation_camera_validate(ids, times, values?, constraints?, policies?)` (values optional; when omitted, the server samples from the current animation at those times).
- Single-time explicit write: `animation_replace_key_camera(time, value, easing?)`.

Notes
- Prefer “produce → apply” for clarity: use `camera_*` producers to compute a typed camera, then choose scene vs. animation by the apply tool.
- The previous names `camera_solve_and_apply` and `camera_validate` are removed; use `animation_camera_*` for timeline operations. For scene‑only verification, do not call `animation_list_keys`; read the current camera via `scene_get_values(id=0, 'Camera 3DCamera')` or use `scene_camera_fit/scene_camera_apply`.
- Respect the Scene vs Timeline contract above; do not include time/easing in scene operations.
