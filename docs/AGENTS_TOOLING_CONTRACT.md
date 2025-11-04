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
