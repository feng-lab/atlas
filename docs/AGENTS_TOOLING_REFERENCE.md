Atlas Agent Tooling Reference (Strict Inputs)

This sheet lists the agent tools, their required inputs, and a short description. All parameters listed under Required must be provided.

Camera
- fit_candidates — Required: none. Return ids suitable for fit/orbit.
- camera_focus — Required: ids(array<int>), after_clipping(bool), min_radius(number). Focus on targets.
- camera_point_to — Required: ids(array<int>), after_clipping(bool). Point center to targets.
- camera_rotate — Required: op(enum: AZIMUTH|ELEVATION|ROLL|YAW|PITCH|FLIP), degrees(number). Segmented ≤90°; chaining handled internally.
- camera_reset_view — Required: mode(enum: XY|XZ|YZ|RESET), ids(array<int>), after_clipping(bool), min_radius(number). Reset to preset view.
- camera_solve — Required: mode(enum), ids(array<int>), t0(number), t1(number), constraints(object), params(object). Plan typed camera keys.
- camera_validate — Required: ids(array<int>), times(array<number>), values(array<object>), constraints(object), policies(object). Validate coverage/visibility.

Scene (stateless)
- scene_list_objects — Required: none. List id/type/name/visible. Reserved ids unified across tools: 0=camera (virtual), 1=background, 2=axis, 3=global, ≥4=object ids.
- scene_bbox — Required: ids(array<int>), after_clipping(bool). Get bbox.
- scene_list_params — Required: id(int). List params for id (0=camera,1=background,2=axis,3=global,≥4=objects).
- scene_get_values — Required: id(int), json_keys(array<string>). Return values (empty array lists all). Camera (id=0) returns a single "Camera 3DCamera" value.
- scene_validate_apply — Required: set_params(array<object{id, json_key, value}>). Validate scene assignments (no time/easing).
- scene_apply — Required: set_params(array<object{id, json_key|string name, value}>). Apply scene edits atomically. If a display name is provided, the dispatcher resolves it to the canonical json_key using `scene_list_params` (case-insensitive, cached, light fuzzy).
- scene_save_scene — Required: path(string). Save .scene.

Timeline (animation)
- animation_set_key_camera — Required: time(number), value(object). Write camera key.
- animation_replace_key_camera — Required: time(number), value(object). Replace camera key near time.
- animation_set_key_param — Required: id(int), json_key(string)|name(string), time(number), value(JSON scalar|array). Write param key (json_key/name resolved via live params with caching).
- animation_replace_key_param — Required: id(int), json_key(string), time(number), value(JSON scalar|array). Replace param key near time.
- animation_replace_key_param_last — Required: id(int), json_key(string), value(JSON scalar|array). Replace most recent key.
- animation_replace_key_param_at_times — Required: id(int), json_key(string), times(array<number>), value(JSON scalar|array). Replace at times.
- animation_clear_keys — Required: id(int). Clear keys for id (id=0 clears camera keys).
- animation_remove_key — Required: id(int), json_key(string), time(number). Remove a key at time.
- animation_batch — Required: set_keys(array<object>), remove_keys(array<object>), commit(bool). Apply multiple operations atomically.
- animation_set_duration — Required: seconds(number). Set animation duration.
- animation_set_time — Required: seconds(number). Set current time.
- animation_play — Required: none. Start playback.
- animation_pause — Required: none. Pause playback.
- animation_save_animation — Required: path(string). Save .animation3d.

Filesystem / System
- system_info — Required: none. OS and common dirs.
- fs_expand_paths — Required: paths(array<string>). Expand ~ and env vars; normalize.
- fs_check_paths — Required: paths(array<string>). Return exists/missing.
- fs_resolve_path — Required: path(string). Heuristic resolution; optional: kind, base_dirs, max_candidates.
- repo_search — Required: name(string). Optional: type, max_depth, max_results.
- fs_glob — Required: dir(string). Optional: pattern, recursive.

Loading
- scene_load_files — Required: files(array<string>). Load into GUI scene.
- scene_ensure_loaded — Required: files(array<string>). Idempotent load.
- scene_smart_load — Required: names(array<string>). Optional: dir_hints, extensions, case_insensitive.
- fs_find_candidates — Required: dirs(array<string>), names(array<string>). Optional: extensions, case_insensitive.

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
