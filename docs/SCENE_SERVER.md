# Atlas Scene RPC (GUI live preview)

## Overview

- Atlas hosts a gRPC service inside the GUI process so that an external agent (LLM) can load data, query the live scene, and set animation keyframes on the same rendering engine the user sees.
- This service is designed for **interactive authoring** (fast, low-latency writes with immediate preview), not long-running exports.

## Start (GUI)

- Launch Atlas normally (macOS/Windows/Linux).
- The gRPC server listens on `127.0.0.1:50051`.
- If `50051` is already owned by another Atlas instance or process, Atlas now keeps the GUI running, logs an RPC startup
  error, and leaves Scene RPC unavailable for that process instead of crashing during startup.

## ID Conventions

Most requests identify targets by `id`:
- `0`: camera (special; uses a typed `3DCamera` value)
- `1`: background
- `2`: axis
- `3`: global (global view settings; this is where “cuts” live)
- `>=4`: loaded objects

## Service: `atlas.rpc.Scene`

The Atlas GUI hosts the gRPC service `atlas.rpc.Scene` for live control.

### Health / Introspection

- `Ping() -> { ok }`
- `GetAppLocation() -> string` (protobuf `StringValue`)
  - Returns the installation root of the running Atlas instance so clients can locate bundled docs/schemas/protos.
- `GetAppVersion() -> string` (protobuf `StringValue`)
  - Returns a build/version identifier (git describe + build timestamp) for compatibility checks and session logs.
- `EngineReady() -> { ok }`
  - `ok=true` when the 3D rendering engine is initialized and ready.
- `Ensure3DWindow() -> { ok }`
  - Opens a 3D window/canvas when needed so the rendering engine can initialize.
- `Set3DCanvasSize({ logical_width, logical_height }) -> { ok, logical_width, logical_height, physical_width, physical_height, error? }`
  - Resizes the live 3D canvas in logical Qt pixels and returns the actual logical/physical size after layout.
  - This controls the on-screen render size of the 3D window.
  - On Retina/HiDPI displays, `physical_*` is typically `logical_* * device_pixel_ratio`.
  - Example: on a 2x Retina display, request `1000 x 750` logical to get about `2000 x 1500` physical rendering.
- `GetStatus({ ids?, include_all_objects? }) -> { ok, doc_ready, engine_ready, has_3d_window, objects[] }`
  - A structured readiness snapshot intended for deterministic agent orchestration.
  - `doc_ready` indicates the GUI document is registered with the RPC service.
  - `engine_ready` indicates the 3D rendering engine is registered (a 3D window exists and has initialized enough to accept engine-backed RPCs).
  - `has_3d_window` indicates a 3D window/canvas exists (does not create one).
  - Optional `objects[]` entries include an `ObjectLoadState` enum:
    - `DOC_NOT_READY`, `ENGINE_NOT_READY`, `VIEW_NOT_READY`, `READY`, etc.
  - Important: `READY` means the object has a bound 3D view/filter and is safe for engine-backed operations (bbox/params/camera). It is **not** a guarantee that all progressive data (e.g. Neuroglancer tiles) is fully loaded.
- `WaitForObjectsReady({ ids, timeout_ms?, poll_interval_ms? }) -> { ok, doc_ready, engine_ready, has_3d_window, objects[], error? }`
  - Blocks until all requested ids report `READY` (as defined above), or until `timeout_ms` elapses.
  - Intended for agent flows like: load → wait_ready → bbox/camera-fit, especially when object view creation is asynchronous.

### Files / Objects

- **Async load tasks (recommended for network datasets)**
  - `StartLoadTask({ sources, network_timeout_ms?, set_visible? }) -> { ok, task_id, error? }`
    - Starts a long-running load job and returns immediately.
    - Use this for Neuroglancer precomputed volumes/segmentations (`precomputed://`, `gs://`, `s3://`, `http(s)://`).
  - `GetTaskStatus({ id }) -> TaskStatus`
    - Poll task status (non-blocking).
  - `WaitTask({ task_id, timeout_ms?, poll_interval_ms? }) -> TaskStatus`
    - Wait for a task to reach a terminal state (`SUCCEEDED|FAILED|CANCELLED`) or until `timeout_ms` elapses.
    - If `timeout_ms=0`, returns the current snapshot without waiting.
  - `CancelTask({ id }) -> { ok }`
    - Best-effort cancellation (may not interrupt in-flight network I/O, but will try to prevent UI registration).
  - `DeleteTask({ id }) -> { ok }`
    - Forget a task and release stored results (best-effort cancels if still running).
  - Task completion semantics:
    - `SUCCEEDED` means the dataset(s) are registered in the current document and object ids are available via `TaskStatus.load.loaded_ids` (JSON: `load.loadedIds`).
    - `FAILED` may still include partial `TaskStatus.load` (some sources loaded/registered, some failed); check `error` and `warnings`.
    - For engine-backed operations after loading, use `WaitForObjectsReady` on the returned ids.

- **Agent cookbook: load (network) → wait → operate**
  - Minimal 3-call orchestration (recommended for Neuroglancer precomputed):
    1) `StartLoadTask(...)` to begin network metadata fetch off the UI thread
    2) `WaitTask(...)` to wait for completion and get `loaded_ids`
    3) `WaitForObjectsReady(...)` to ensure the returned ids are bound to a 3D view/filter (safe for bbox/camera/params)
  - Example:
    ```text
    # 1) Start async load
    start = Scene.StartLoadTask({
      sources: ["precomputed://..."],
      network_timeout_ms: 30000,
      set_visible: true,
    })
    task_id = start.task_id

    # 2) Wait for completion (terminal state: SUCCEEDED|FAILED|CANCELLED)
    status = Scene.WaitTask({
      task_id: task_id,
      timeout_ms: 120000,
      poll_interval_ms: 200,
    })

    # 3) If any ids loaded, wait for engine/view binding before engine-backed ops
    ids = status.load.loaded_ids
    ready = Scene.WaitForObjectsReady({ ids: ids, timeout_ms: 30000, poll_interval_ms: 50 })
    ```
  - Handling `FAILED` (partial success):
    - Do not assume `FAILED` means “nothing loaded”. A task can fail for one source but still succeed for others.
    - If `status.load.loaded_ids` is non-empty, you may proceed with those ids (after `WaitForObjectsReady`) and surface `status.error` / `status.load.warnings` for debugging.
    - If `loaded_ids` is empty, treat it as a hard failure and surface `status.error`.
  - Cleanup:
    - Optionally call `DeleteTask({id})` once you’ve consumed the result to release stored task data.

- Local paths are supported via the same task API (`StartLoadTask` / `WaitTask`) (recommended).
  - Task completion means the file(s) are loaded/registered in the document model and object ids are available.
  - It does **not** wait for:
    - Qt UI repaint (2D view refresh), or
    - 3D window creation / engine readiness, or
    - 3D object view/filter binding, or
    - progressive rendering completion.
    If you need deterministic engine-backed operations after loading:
    - Explicit barrier: call `Ensure3DWindow` (if needed) and then `WaitForObjectsReady` on the returned ids, or
    - Deadline-based: call engine-backed RPCs with a gRPC deadline/timeout (the server will auto-wait for readiness on `target_not_ready` / bbox-derived transient empties).
- `ListObjects() -> objects[]` (id, type, name, path, visible)

### Geometry / Capabilities

- Readiness / auto-wait (UI-parity):
  - Engine-backed RPCs such as `BBox`, `CutSuggest`, `ListParams`, `GetParamValues`, `ApplySceneParams`, and camera helpers (e.g., `CameraFit`) require objects to be bound into the live 3D engine (view/filter created).
  - If the client provided a gRPC deadline/timeout, the server will auto-wait and retry on transient readiness issues (e.g., `target_not_ready` or bbox-derived empties during view binding) until the deadline.
  - If the client did not provide a deadline, the server returns `FAILED_PRECONDITION` immediately (no unbounded waits on server threads).

- `BBox(ids, after_clipping) -> bbox {min, max, size, center}`
  - If `ids` is omitted/empty, the server uses all current **visual** objects (excludes `Animation3D`).
  - Invalid/unknown ids are rejected with `INVALID_ARGUMENT` (instead of returning an empty bbox).
- `Capabilities(ids?) -> { camera[], background[], axis[], global[], objects{type: ParamList} }`
  - Each Parameter includes `json_key`, `name`, `type`, `supports_interpolation`, and optionally:
    - `description` (human-readable semantics)
    - `value_schema` (canonical JSON Schema for the value shape)
- `ListParams(id) -> ParamList`
  - Enumerate parameters for a specific `id` (`0|1|2|3|>=4`).

### Parameter Types: `3DTransform` (Object Transform)

When a parameter’s `type` is `3DTransform` (for example, `Coord Transform 3DTransform`), its value is an object with canonical subfields:

- `Scale Vec3`: `[sx, sy, sz]`
- `Rotation Vec4`: `[angle_deg, axis_x, axis_y, axis_z]` (axis-angle; degrees)
- `Rotation Center Vec3`: `[cx, cy, cz]`
- `Translation Vec3`: `[tx, ty, tz]`

The engine composes these fields into a 4×4 transform matrix using GLM’s column-vector convention (rightmost term applies first):

`M = T(Translation + RotationCenter*Scale) * R(Rotation) * T(-RotationCenter*Scale) * S(Scale)`

Where `RotationCenter*Scale` is the **component-wise** product: `[cx*sx, cy*sy, cz*sz]`.

Interpretation:

- Scale about the object origin `(0,0,0)`
- Rotate about pivot `(RotationCenter*Scale)`
- Translate by `Translation` (applied last)

Equivalent step-by-step application for a local point `p` (homogeneous form implied):

- `p1 = S(Scale) * p`
- `p2 = T(-RotationCenter*Scale) * p1` (move the scaled rotation center to the origin)
- `p3 = R(Rotation) * p2`
- `p4 = T(Translation + RotationCenter*Scale) * p3`

Notes:

- `Rotation Vec4` is axis-angle. The axis is normalized; a zero-length axis produces an identity rotation.
- `Rotation Center` affects rotation only; scaling is always about the object origin. This is why scaling an object whose geometry is not centered at origin can look like it “moves” relative to world space.
- `Translation` is applied last and is not multiplied by `Scale`.
- Keeping the pivot fixed while scaling: because the effective rotation pivot is `(RotationCenter*Scale)`, changing `Scale` can change where the pivot lands in world space unless you also adjust `Translation`. To keep the pivot position constant when scaling from `s0` to `s1` (component-wise), use: `Translation_new = Translation_old + (s0 - s1) * RotationCenter`.

Partial updates and “do I need to provide all fields?”

- **Scene (stateless) apply is patch-style for composite objects**: when setting a `3DTransform` value via `ApplySceneParams`, you may provide **only the subfields you want to change**; omitted subfields are left unchanged.
  - Example (translation-only update): `{ "Translation Vec3": [tx, ty, tz] }`
  - The RPC server also accepts common aliases and normalizes them to canonical keys: `Scale`, `Translation`, `Rotation`, and `Center` (maps to `Rotation Center Vec3`).
- **Timeline (animation) keys are full values**: when writing a keyframe value (e.g., `SetKey`) for a `3DTransform`, the key stores a complete transform value. If you send only `{ "Translation Vec3": [...] }`, the missing subfields will remain at the parameter’s defaults for that key (not “inherit from the current scene”). If you intend to “change only translation but keep existing scale/rotation”, read the current value first (scene or sampled key) and write a full object.

## Scene (Stateless) Operations

Use these to edit base scene state (no time/easing; **does not** write keyframes):

- `GetParamValues { id, json_keys? } -> { values: map<json_key, Value> }`
  - If `json_keys` is omitted/empty, returns all values for the id.
  - For camera, the scene value lives under the json_key `"Camera 3DCamera"`.
- `ValidateSceneParams { set_params: [ { id, json_key, value } ] } -> { ok, results[] }`
  - Performs type/shape checks and returns `normalized_value` where applicable.
- `ApplySceneParams { set_params: [...] } -> { ok }`
  - Applies assignments atomically after validation. Marshalled to UI thread.
- `SaveScene { path } -> { ok }`
  - Writes a `.scene` file (equivalent to the GUI Save Scene).
- `TakeScreenshot3D { width, height, path?, overwrite? } -> { ok, path, error? }`
  - Renders a single image of the current **3D scene** state.
  - If `path` is omitted/empty, the server writes a `.png` into the OS temp directory.
  - If `path` is provided, the output format is inferred from the file extension (PNG recommended for LLM visual verification).
  - Does **not** create an animation or write keyframes (preferred for verification screenshots).
- `Set3DCanvasSize { logical_width, logical_height } -> { ok, logical_width, logical_height, physical_width, physical_height, error? }`
  - Resizes the live 3D window’s central `Z3DCanvas`.
  - Unlike `TakeScreenshot3D`, this affects interactive rendering, timer logs, and benchmarked live-window behavior.
  - The returned `physical_*` values let clients confirm the true render size on HiDPI screens.
- `SetVisibility { ids, on } -> { ok }`
- `RemoveObjects { ids, allow_unsaved? } -> { ok }`
  - By default, fails if any object has unsaved changes (no modal prompts in RPC).
  - Set `allow_unsaved=true` to discard unsaved changes without prompting.
- `MakeAlias { ids } -> { ok, aliases: [{src_id, alias_id}], error? }`

### Cuts (Global)

- `CutSet { box?:BBox | planes?:CutPlanes, refit_camera?: bool } -> { ok }`
- `CutClear {} -> { ok }`
- `CutSuggest { ids?, mode?:"box", margin?, after_clipping? } -> { box | planes }`
  - `ids` omitted/empty means “all visual objects” (excludes `Animation3D`).
  - Returns `INVALID_ARGUMENT` when the resolved target set is empty (no visual objects).

Note: cuts are global view settings (typically `id=3` in the parameter system), not per-object.

## Timeline (Animation) Operations

Keyframes override base scene values during playback/preview at nonzero times.

- `EnsureAnimation { create_new?, name? } -> { ok, animation_id, created, error? }`
  - Creates or selects an `Animation3D` object and binds it for editing in the GUI.
  - All subsequent timeline operations require the returned `animation_id`.
  - UI parity: when this RPC creates a new animation (response `created=true`), Atlas captures a full-scene keyframe at `t=0` (camera + all objects + background/axis/global). This seeds a deterministic baseline so playback does not fall back to live scene values.
- `SetDuration { animation_id, seconds } -> { ok }`
- `SetTime { animation_id, seconds, cancel_rendering? } -> { ok }`
- `AddKeyFrame { animation_id, time, cancel_rendering? } -> { ok }`
  - UI “Save Key Frame” parity: captures the **entire current scene state** into the animation at the requested time (writes keys for all parameters, including camera).
  - This can be used as an animation authoring workflow (“pose the scene at beat time → save keyframe”), and also to re-seed baseline keys at `t=0` after loading/adding objects during authoring.
- `SetKey { animation_id, target_id, json_key?, time, easing, value, camera_tcb? } -> { ok }`
  - `target_id=0` for camera (json_key ignored), or `>=4` for objects.
  - `value` is typed JSON via protobuf `Value` (camera is an object; options are strings; vectors are arrays).
  - `camera_tcb` is only valid for `target_id=0`. It accepts `pos_tension`, `pos_continuity`, `pos_bias`, `rot_tension`, `rot_continuity`, and `rot_bias`; every value must be in `[-1, 1]`.
- `ClearKeys { animation_id, target_id, json_key? } -> { ok }`
  - For `target_id=0`, clears all camera keys.
- `RemoveKey { animation_id, target_id, json_key, time } -> { ok }`
- `Batch { animation_id, set_keys, remove_keys, commit? } -> { ok }`
- `SaveAnimation { animation_id, path } -> { ok }` (writes `.animation3d`)

## Camera Helpers (Typed Values; No Key Writes)

These return typed camera values/keys that clients can write via `SetKey(animation_id, target_id=0, ...)`:

- `CameraGet() -> { values:[Value] }`
  - Returns the current engine camera as a typed value.
- `CameraFit(ids?, all?, after_clipping?, min_radius?) -> { values:[Value] }`
- `CameraOrbitSuggest(ids?, axis, degrees?) -> { values:[Value, Value] }`
- `CameraDollySuggest(ids?, start_dist, end_dist) -> { values:[Value, Value] }`
  - For camera ops with `ids?`, an omitted/empty `ids` means “all visual objects” (excludes `Animation3D`).
  - When the resolved target set is empty, these return `INVALID_ARGUMENT` (no-op in the GUI; explicit error for deterministic agent control).
  - During initial load/view binding, bbox-derived camera ops may transiently fail with `FAILED_PRECONDITION` / “bbox empty”. With a gRPC deadline, the server auto-waits and retries.

### UI-Parity Camera Operators (Deterministic)

- `CameraFocus(ids?, after_clipping?, min_radius?) -> { values:[Value] }`
- `CameraPointTo(ids?, after_clipping?) -> { values:[Value] }`
- `CameraRotate { op, degrees?, base_value } -> { values:[Value] }`
- `CameraResetView { mode, ids?, after_clipping?, min_radius? } -> { values:[Value] }`

### Freeform Walkthrough Building Blocks

- `CameraMoveLocal { op:"FORWARD|BACK|RIGHT|LEFT|UP|DOWN", distance, distance_is_fraction_of_bbox_radius?, ids?, after_clipping?, move_center?, base_value } -> { values:[Value] }`
  - Use this to build first-person walkthroughs without guessing world units (when `distance_is_fraction_of_bbox_radius=true`).
- `CameraLookAt { base_value, target=(world_point|target_bbox_center|bbox_fraction_point), ids?, after_clipping? } -> { values:[Value] }`
  - Aim the camera (updates center; preserves eye).
- `CameraPathSolve { ids?, after_clipping?, base_value, waypoints:[...] } -> { keys:[{time,value}...] }`
  - Produces typed camera keys from waypoint geometry (does not write keys).
  - Waypoints can specify eye/look-at in world coords or bbox fractions; missing fields are filled using the previous waypoint’s direction/distance.

## Camera Planning / Validation (Coverage-Based)

- `FitCandidates {} -> { ids:[uint64] }`
  - Returns visual object ids suitable for camera fit/orbit (excludes Animation3D).
- `CameraSolve { mode:"FIT|ORBIT|DOLLY|STATIC", ids, t0, t1, constraints?, params? } -> { keys:[{time,value}...] }`
  - Mode params (selected):
    - `ORBIT`: `params.axis` (`"x"|"y"|"z"`), `params.degrees` (double), optional `params.max_step_degrees` (double; controls key density; default 90).
    - `DOLLY`: `params.start_dist` (double), `params.end_dist` (double).
- `CameraValidate { ids, times, values?, constraints?, policies?, animation_id? } -> { ok, results:[{time, within_frame, frame_coverage, adjusted, adjusted_value?, reason}] }`
  - If `values` are omitted (or shorter than `times`), `animation_id` is required and the server samples the animation camera at those `times`.
  - Interior walkthroughs: set `constraints.keep_visible=false` to disable framing validation (so the camera can go inside / let the bbox leave the frame).
  - Semantics note:
    - `frame_coverage` is a **screen-space framing** metric (0..1 dominant-dimension bbox fill). Higher values mean the target appears larger on screen (tighter framing).
    - Higher `constraints.min_frame_coverage` pushes toward tighter shots (when `keep_visible=true`); set it to `0.0` to disable.
    - If the camera looks “too wide” (targets feel small), it is usually because you are validating/solving against **too many ids** (e.g., the entire scene) and/or using a large `margin`. For close-ups, pass only the target ids and raise `min_frame_coverage`.
- `CameraSample { animation_id, times } -> { samples:[{time, value}] }`
  - Samples the evaluated animation camera at the requested `times` without changing engine time or writing keys.
  - Use this to get a deterministic `base_value` for camera operators while authoring an animation.

## Camera Animation Settings (Timeline Evaluation)

- `SetCameraInterpolationMethod { animation_id, method } -> { ok }`
- `GetCameraInterpolationMethod { animation_id } -> string` (protobuf `StringValue`)

Supported `method` strings:
- `Center`
- `Position Spline`
- `Position Rotation Spline`

Notes:
- Method matching is case-insensitive and ignores spaces, underscores, and hyphens.
- `Center` is the default and is strongly advised for almost all agent-authored camera animation. It matches Atlas' trackball/orbit camera model by interpolating the look-at center, center distance, and orientation, so object-centric views usually remain stable and predictable.
- Use `Position Spline` or `Position Rotation Spline` only when there is a specific, intentional need for a free-camera path whose eye position follows a shaped spline. Agents should not choose spline modes as a general smoothing default; they frequently produce unexpected orbit, framing, or rotation results for normal scene presentations.
- `Position Spline` evaluates the camera eye position with a Kochanek-Bartels TCB spline; `Position Rotation Spline` evaluates both eye position and rotation with TCB splines.
- `SetKey` and `Batch.set_keys[]` can provide per-key `camera_tcb` values for camera keys. Omit `camera_tcb` for neutral TCB values (`0`) and for all non-camera tracks.

## Introspection

- `ListKeys { animation_id, target_id, json_key?, include_values? } -> { keys:[{time,type,value_json?}] }`
  - For `target_id=0` (camera), `json_key` is ignored.
  - For non-camera tracks, provide the parameter `json_key`.
- `GetTime { animation_id } -> { seconds, duration }`

## Notes

- Most requests are marshalled to the UI/rendering thread via `QMetaObject::invokeMethod` to respect single-GL-context threading rules.
- Network-backed dataset loads use the task API so metadata fetch (e.g. Neuroglancer `.../info`) runs off the UI thread, then registration happens on the UI thread.
- For MP4 export, use the existing headless CLI `--run_export_3d_animation` from Python once a `.animation3d` is authored; keep long-running export tasks out of the GUI RPC.
