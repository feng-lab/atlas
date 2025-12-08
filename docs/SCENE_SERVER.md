Atlas Scene RPC (GUI live preview)

Overview
- Atlas hosts a gRPC service inside the GUI process so that an external agent (LLM) can load data, query the live scene, and set animation keyframes on the same Z3DRenderingEngine the user sees. Changes are reflected immediately; users can hit Play.
- Headless export can be handled separately via the existing `--run_export_3d_animation` CLI; the RPC service focuses on building animations and keyframes, not long-running exports.

Start (GUI)
- Launch Atlas normally (macOS/Windows/Linux). The gRPC server listens on `0.0.0.0:50051`.

Service and Methods (initial)
- Health: `Ping() -> ok`
- Files/objects:
  - `LoadFiles(files) -> objects[]`
  - `ListObjects() -> objects[]` (id, type, name, path, visible)
- Geometry/capabilities:
  - `BBox(ids, after_clipping) -> bbox {min, max, size, center}`
- `Capabilities(ids?) -> { Background[], Axis[], Global[], Objects{Type: ParamList} }`
    - Parameter: `{ json_key, name, type, supports_interpolation, description? }`
      - `description` is an optional human‑readable string supplied by the engine to clarify semantics (e.g., what an option like "Mesh Color" means).

Scene (stateless) operations
- Use these when editing the scene/base state (no timeline/time/easing). They do not write keyframes.
  - `GetParamValues { id, json_keys? } -> { values: map<json_key, Value> }`
    - `id`: 0 returns camera scene value under `"Camera 3DCamera"`, 1 background, 2 axis, 3 global, ≥4 object ids. If `json_keys` omitted, returns all values for the id.
  - `ValidateSceneParams { set_params: [ { id, json_key, value } ] } -> { ok, results[] }`
    - Performs shape/range checks (and basic option checks) and returns `normalized_value` when clamping is applicable. For camera (id=0), accepts a `3DCamera` value object and returns a normalized camera value.
  - `ApplySceneParams { set_params: [...] } -> { ok }`
    - Applies the assignments atomically after validation. No time/easing. Marshalled to UI thread.
  - `SaveScene { path } -> { ok }`
    - Writes a `.scene` file, equivalent to the UI’s Save Scene (no timeline keys).

Scene vs Timeline semantics (important)
- `scene_apply` never creates or updates animation keys. It changes base scene state only.
- During playback (and preview at nonzero times), keyframed values take precedence over base scene values.
- If a parameter has timeline keys, changing the scene value will not alter the animated result; you must edit the keys.
- Time‑based requests (e.g., “at 3s, use Only Wireframe”) must be implemented with SetKey/ReplaceKey at t=3.0 (use easing=Switch for non‑interpolatable params).
- Timeline:
  - `EnsureAnimation() -> ok` (create/bind a default 3D animation with a baseline t=0 frame)
  - `SetDuration(seconds) -> ok`
  - `SetKey(id, json_key?, time, easing, value: google.protobuf.Value) -> ok`
    - `id`: `0` for camera or `>=4` for objects.
    - `value`: typed JSON value (protobuf Value). For camera, pass an object; for numeric/bool/vector parameters, pass number/bool/array accordingly; for options, pass a string.

Notes
- RPC requests marshal onto the UI and render threads (single-GL-context invariant). All updates are visible in the 3D window and timeline.
- Use the headless exporter (`--run_export_3d_animation`) for MP4 generation from completed `.animation3d` files; the Python app can run that CLI.

gRPC Scene Service (GUI)
- The Atlas GUI hosts a gRPC service `atlas.rpc.Scene` (listening on 0.0.0.0:50051) for live, low‑latency control from agents/tools. Use this when you want real‑time preview in the canvas.

New endpoints (additions)
- Camera helpers
  - `CameraFit` `{ ids?: [uint64], all?: bool, after_clipping?: bool, min_radius?: double }` → `{ values: [google.protobuf.Value] }`
    - Returns a typed camera value object that frames the targets. Feed into `SetKey` with `id=0`.
  - `CameraOrbitSuggest` `{ ids?: [uint64], axis: "x"|"y"|"z", degrees?: 360 }` → `{ values: [google.protobuf.Value, google.protobuf.Value] }`
    - Returns start/end typed camera value objects for a simple orbit.
  - `CameraDollySuggest` `{ ids?: [uint64], start_dist: double, end_dist: double }` → `{ values: [google.protobuf.Value, google.protobuf.Value] }`
    - Returns start/end typed camera value objects where the camera distance to center is set to the given distances.

  - Typed camera planning and validation (no guessing)
    - `FitCandidates` `{}` → `{ ids: [uint64] }`
      - Returns visual object ids suitable for camera fit/orbit (excludes `Animation3D`).
    - `CameraSolve` `{ mode: "FIT"|"ORBIT"|"DOLLY"|"STATIC", ids?: [uint64], t0: double, t1?: double, constraints?: { keep_visible?: bool, margin?: double, min_coverage?: double, fov_policy?: string }, params?: Struct }` → `{ keys: [{time, value}] }`
      - Computes typed camera key(s) for the targets. For `ORBIT`, `params={ axis: "x"|"y"|"z", degrees: double }` (degrees defaults to 360). `ORBIT` and `DOLLY` produce segmented keys from `t0..t1` when needed to avoid identical endpoints; `FIT`/`STATIC` produce a single key at `t0`.
      - The server excludes `Animation3D` when deriving target bbox. `constraints.margin` expands the bbox fractionally; `min_coverage` defaults to `0.95`.
    - `CameraValidate` `{ ids?: [uint64], times: [double], values: [Value], constraints?: {...}, policies?: { adjust_fov?: bool, adjust_distance?: bool, adjust_clipping?: bool } }` → `{ ok: bool, results: [{ time, within_frame, coverage, adjusted, adjusted_value?, reason }] }`
      - Evaluates coverage against the target bbox and optional margin. If policies allow, returns an `adjusted_value` with updated `fieldOfView` or eye/center distance. `ok=true` when all entries meet `min_coverage`.

- Scene state
  - `SetVisibility` `{ ids: [uint64], on: bool }` → `{ ok: true }`
  - `MakeAlias` `{ ids: [uint64] }` → `{ ok: bool, aliases: [{src_id, alias_id}], error?: string }`
    - Creates alias objects for each valid id that supports aliasing. Aliases share backing data with the source object but have independent view/display parameters, matching the UI’s “Make Alias” behavior.

- Parameter enumeration
  - `ListParams` `{ id }` → `ParamList`
    - Mirrors `scene.capabilities` but for a specific `id` (`0|1|2|3|≥4`).

- Timeline/keyframe ops
  - `ClearKeys` `{ id, json_key? }` → `{ ok }` (for `id=0`, clears camera keys; otherwise clears keys for the specific parameter)
  - `RemoveKey` `{ id, json_key, time }` → `{ ok }`
  - `Batch` `{ set_keys: [SetKeyRequest], remove_keys: [RemoveKeyRequest], commit?: bool }` → `{ ok }`
    - Verification: use `ListKeys` to confirm that each requested key time exists after a batch. When `commit=true`, time 0 keys are applied immediately, but verification works regardless of `commit`.
  - When `commit=true`, any keys at `time=0` are applied immediately (engine evaluates t=0) so the initial state is visible without a separate `SetTime` call. Non‑zero keys do not move the playhead; use `SetTime` to preview later moments.

- Playback controls
  - `SetTime` `{ seconds: double, cancel_rendering?: bool }` → `{ ok }`
  - `Play` `{ fps?: double, loop?: bool }` → `{ ok }` (lightweight timer‑driven playback affecting canvas)
  - `Pause` `{}` → `{ ok }`

- Save/export
  - `Save` `{ path: string }` → `{ ok }` (writes `.animation3d`)
  - `SaveAnimation` `{ path: string }` → `{ ok }` (alias of `Save`)

- Introspection
  - `ListKeys` `{ id, json_key?, include_values? }` → `{ keys: [{time, type, value_json?}] }`
    - For `id=0` (camera), `json_key` is ignored. For others, provide the parameter’s `json_key`.
    - Note: for efficiency and compatibility, `ListKeys` returns stringified key values today. Use it for verification. `SetKey` uses typed `google.protobuf.Value`.
  - `GetTime` `{}` → `{ seconds, duration }`

Cuts (global)
- `CutSet` `{ box?: {min,max}, planes?: [{a,b,c,d}], refit_camera?: bool }` → `{ ok }`
  - Current implementation supports axis‑aligned box or axis‑aligned planes. Non‑axis‑aligned planes return an error.
- `CutClear` `{}` → `{ ok }` (clears global cuts)
- `CutSuggest` `{ ids?: [uint64], mode?: "box", margin?: double, after_clipping?: bool }` → `{ box | planes }`
  - Returns a suggested cut (currently a box) derived from the selection or all objects.

  - UI-parity camera operators (deterministic)
    - `CameraFocus` `{ ids: [uint64], after_clipping?: bool, min_radius?: double }` → `{ values: [google.protobuf.Value] }`
      - Returns a typed camera value that focuses on the targets (preserves current view vector). Excludes `Animation3D`.
    - `CameraPointTo` `{ ids: [uint64], after_clipping?: bool }` → `{ values: [google.protobuf.Value] }`
      - Returns a typed camera value where the camera center points to the target bbox center; eye is unchanged.
    - `CameraRotate` `{ op: "AZIMUTH"|"ELEVATION"|"ROLL"|"YAW"|"PITCH"|"FLIP", degrees?: double, base_value?: Value }` → `{ values: [Value] }`
      - Applies the operator to `base_value` (or current camera if omitted) and returns the resulting typed camera value.
    - `CameraResetView` `{ mode: "XY"|"XZ"|"YZ"|"RESET", ids?: [uint64], after_clipping?: bool, min_radius?: double }` → `{ values: [Value] }`
      - Resets camera orientation using scene (or provided ids) bbox and returns a typed camera value.

Usage notes
- All requests are marshalled to the UI/rendering thread via `QMetaObject::invokeMethod` to respect single GL context and threading rules.
- For camera suggestions, returned strings are the camera value JSON objects (not full key objects). Pass them to `SetKey` with `id=0`, your chosen `time`, and `easing`.
- For camera planning, prefer `FitCandidates` + `CameraSolve` to obtain typed values, and confirm with `CameraValidate` before writing to the timeline. Do not invent camera numbers in clients.
- Operator workflows for animation (example: 360° orbit in 10s): Focus → Rotate(azimuth 90°) at 2.5s/5s/7.5s/10s. Use `SetKey`/`Batch` with `id=0` for each step and validate with `CameraValidate`.
- `Play/Pause` are minimal and independent of the UI’s `QTimeLine`; they drive live preview by stepping `setCurrentTime`.
- For MP4 export, run the existing headless CLI `--run_export_3d_animation` from your Python app; keep long-running export tasks out of the GUI RPC.
