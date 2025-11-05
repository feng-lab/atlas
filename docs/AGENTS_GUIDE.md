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
- Camera: `fit_candidates`, `camera_solve_and_apply`, `camera_validate`, `camera_focus`, `camera_point_to`, `camera_rotate`, `camera_reset_view`
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

Atlas Agent Tooling Reference (Strict Inputs)

This sheet lists the agent tools, their required inputs, and a short description. All parameters listed under Required must be provided.

Camera
- fit_candidates — Required: none. Return ids suitable for fit/orbit.
- camera_focus — Required: ids(`array<int>`), after_clipping(bool), min_radius(number). Focus on targets.
- camera_point_to — Required: ids(`array<int>`), after_clipping(bool). Point center to targets.
- camera_rotate — Required: op(enum: AZIMUTH|ELEVATION|ROLL|YAW|PITCH|FLIP), degrees(number). Segmented ≤90°; chaining handled internally.
- camera_reset_view — Required: mode(enum: XY|XZ|YZ|RESET), ids(`array<int>`), after_clipping(bool), min_radius(number). Reset to preset view.
- camera_validate — Required: ids(`array<int>`), times(`array<number>`), values(`array<object>`), constraints(object), policies(object). Validate coverage/visibility.
- camera_solve_and_apply — Required: mode(enum), ids(`array<int>`), t0(number), t1(number). Solve keys and apply with validation; clears existing keys in [t0,t1] by default. Do not wrap in `animation_batch` and do not follow with `animation_replace_key_camera` — this tool already commits the keys.

Scene (stateless)
- scene_list_objects — Required: none. List id/type/name/visible. Reserved ids unified across tools: 0=camera (virtual), 1=background, 2=axis, 3=global, ≥4=object ids.
- scene_bbox — Required: ids(`array<int>`), after_clipping(bool). Get bbox.
- scene_list_params — Required: id(int). List params for id (0=camera,1=background,2=axis,3=global,≥4=objects).
- scene_get_values — Required: id(int), json_keys(`array<string>`). Return values (empty array lists all). Camera (id=0) returns a single "Camera 3DCamera" value.
- scene_validate_apply — Required: set_params(`array<object{id, json_key, value}>`). Validate scene assignments (no time/easing).
- scene_apply — Required: set_params(`array<object{id, json_key|string name, value}>`). Apply scene edits atomically. If a display name is provided, the dispatcher resolves it to the canonical json_key using `scene_list_params` (case-insensitive, cached, light fuzzy).
- scene_save_scene — Required: path(string). Save .scene.

Timeline (animation)
- camera_solve_and_apply — Required: mode(enum), ids(`array<int>`), t0(number), t1(number). Solve keys and apply with validation.
- animation_replace_key_camera — Required: time(number), value(object). Replace camera key near time.
- animation_set_key_param — Required: id(int), json_key(string)|name(string), time(number), value(`JSON scalar|array`). Write param key (json_key/name resolved via live params with caching).
- animation_replace_key_param — Required: id(int), json_key(string), time(number), value(`JSON scalar|array`). Replace param key near time.
- animation_replace_key_param_at_times — Required: id(int), json_key(string), times(`array<number>`), value(`JSON scalar|array`). Replace at times.
- animation_clear_keys — Required: id(int). Clear keys for id (id=0 clears camera keys).
- animation_remove_key — Required: id(int), json_key(string), time(number). Remove a key at time.
- animation_batch — Required: set_keys(`array<object>`), remove_keys(`array<object>`), commit(bool). Apply multiple operations atomically.
- animation_set_duration — Required: seconds(number). Set animation duration.
- animation_set_time — Required: seconds(number). Set current time.
- animation_play — Required: none. Start playback.
- animation_pause — Required: none. Pause playback.
- animation_save_animation — Required: path(string). Save .animation3d.

Filesystem / System
- system_info — Required: none. OS and common dirs.
- fs_expand_paths — Required: paths(`array<string>`). Expand ~ and env vars; normalize.
- fs_check_paths — Required: paths(`array<string>`). Return exists/missing.
- fs_resolve_path — Required: path(string). Heuristic resolution; optional: kind, base_dirs, max_candidates.
- repo_search — Required: name(string). Optional: type, max_depth, max_results.
- fs_glob — Required: dir(string). Optional: pattern, recursive.

Loading
- scene_load_files — Required: files(`array<string>`). Load into GUI scene.
- scene_ensure_loaded — Required: files(`array<string>`). Idempotent load.
- scene_smart_load — Required: names(`array<string>`). Optional: dir_hints, extensions, case_insensitive.
- fs_find_candidates — Required: dirs(`array<string>`), names(`array<string>`). Optional: extensions, case_insensitive.

Docs / Info
- scene_capabilities — Required: none. Return full capabilities.
- scene_schema — Required: none. Return JSON schema.
- scene_capabilities_summary — Required: none. Condensed overview.
- scene_facts_summary — Required: none. Concise facts (objects/keys/time).
- animation_describe_file — Required: path(string). Summarize .animation3d.

Export & Preview
- atlas_export_video — Required: animation(string), out(string), fps(number), start(int), end(int), width(int), height(int), overwrite(bool), use_gpu_devices(string).
- animation_render_preview — Required: time(number), fps(number), width(int), height(int). Render a single frame.

Notes
- All scene_* tools are stateless (no time/easing). Playback uses animation_* keys.
- Keys override scene values at playback. To change what plays, write/replace keys.
- Prefer camera_solve_and_apply (or camera_solve + animation_replace_key_camera) for camera motion.
