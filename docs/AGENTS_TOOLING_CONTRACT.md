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
- Scene (stateless): `scene_get_values`, `scene_validate_apply`, `scene_apply`, `scene_save_scene`
- Discovery: `scene_list_objects`, `scene_list_params`, `scene_capabilities`, `scene_schema`, `scene_capabilities_summary`, `scene_facts_summary`
- Timeline: `scene_list_keys`, `scene_batch`, `scene_replace_key_param`, `scene_replace_key_camera`, `scene_set_time`, `scene_play`, `scene_pause`, `scene_save_animation`
- Camera: `fit_candidates`, `camera_solve`, `camera_validate`, `camera_focus`, `camera_point_to`, `camera_rotate`, `camera_reset_view`
- Geometry/Cuts: `scene_bbox`, `scene_cut_suggest`, `scene_cut_set`, `scene_cut_clear`

Notes
- The Script API (`tools.atlas_agent.api.*`) may add helpers (plan builders, layouts) without changing the Agent Tooling.
- When the Script API adds a capability that belongs in Agent Tooling, introduce it via a new tool function and document it here.

