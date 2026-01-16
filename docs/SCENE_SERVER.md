# Atlas Scene RPC (GUI live preview)

## Overview

- Atlas hosts a gRPC service inside the GUI process so that an external agent (LLM) can load data, query the live scene, and set animation keyframes on the same rendering engine the user sees.
- This service is designed for **interactive authoring** (fast, low-latency writes with immediate preview), not long-running exports.

## Start (GUI)

- Launch Atlas normally (macOS/Windows/Linux).
- The gRPC server listens on `0.0.0.0:50051`.

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

### Files / Objects

- `LoadFiles(files) -> objects[]`
- `ListObjects() -> objects[]` (id, type, name, path, visible)

### Geometry / Capabilities

- `BBox(ids, after_clipping) -> bbox {min, max, size, center}`
- `Capabilities(ids?) -> { camera[], background[], axis[], global[], objects{type: ParamList} }`
  - Each Parameter includes `json_key`, `name`, `type`, `supports_interpolation`, and optionally:
    - `description` (human-readable semantics)
    - `value_schema` (canonical JSON Schema for the value shape)
- `ListParams(id) -> ParamList`
  - Enumerate parameters for a specific `id` (`0|1|2|3|>=4`).

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
- `SetVisibility { ids, on } -> { ok }`
- `MakeAlias { ids } -> { ok, aliases: [{src_id, alias_id}], error? }`

### Cuts (Global)

- `CutSet { box?:BBox | planes?:CutPlanes, refit_camera?: bool } -> { ok }`
- `CutClear {} -> { ok }`
- `CutSuggest { ids?, mode?:"box", margin?, after_clipping? } -> { box | planes }`

Note: cuts are global view settings (typically `id=3` in the parameter system), not per-object.

## Timeline (Animation) Operations

Keyframes override base scene values during playback/preview at nonzero times.

- `EnsureAnimation() -> { ok }` (create/bind a default 3D animation with a baseline t=0 frame)
- `SetDuration(seconds) -> { ok }`
- `SetTime { seconds, cancel_rendering? } -> { ok }`
- `SetKey { id, json_key?, time, easing, value } -> { ok }`
  - `id=0` for camera (json_key ignored), or `>=4` for objects.
  - `value` is typed JSON via protobuf `Value` (camera is an object; options are strings; vectors are arrays).
- `ClearKeys { id, json_key? } -> { ok }`
  - For `id=0`, clears all camera keys.
- `RemoveKey { id, json_key, time } -> { ok }`
- `Batch { set_keys, remove_keys, commit? } -> { ok }`
- `SaveAnimation { path } -> { ok }` (writes `.animation3d`)
  - `Save` is an alias of `SaveAnimation`.

## Camera Helpers (Typed Values; No Key Writes)

These return typed camera values/keys that clients can write via `SetKey(id=0, ...)`:

- `CameraGet() -> { values:[Value] }`
  - Returns the current engine camera as a typed value.
- `CameraFit(ids?, all?, after_clipping?, min_radius?) -> { values:[Value] }`
- `CameraOrbitSuggest(ids?, axis, degrees?) -> { values:[Value, Value] }`
- `CameraDollySuggest(ids?, start_dist, end_dist) -> { values:[Value, Value] }`

### UI-Parity Camera Operators (Deterministic)

- `CameraFocus(ids?, after_clipping?, min_radius?) -> { values:[Value] }`
- `CameraPointTo(ids?, after_clipping?) -> { values:[Value] }`
- `CameraRotate { op, degrees?, base_value? } -> { values:[Value] }`
- `CameraResetView { mode, ids?, after_clipping?, min_radius? } -> { values:[Value] }`

### Freeform Walkthrough Building Blocks

- `CameraMoveLocal { op:"FORWARD|BACK|RIGHT|LEFT|UP|DOWN", distance, distance_is_fraction_of_bbox_radius?, ids?, after_clipping?, move_center?, base_value? } -> { values:[Value] }`
  - Use this to build first-person walkthroughs without guessing world units (when `distance_is_fraction_of_bbox_radius=true`).
- `CameraLookAt { base_value?, target=(world_point|target_bbox_center|bbox_fraction_point), ids?, after_clipping? } -> { values:[Value] }`
  - Aim the camera (updates center; preserves eye).
- `CameraPathSolve { ids?, after_clipping?, base_value?, waypoints:[...] } -> { keys:[{time,value}...] }`
  - Produces typed camera keys from waypoint geometry (does not write keys).
  - Waypoints can specify eye/look-at in world coords or bbox fractions; missing fields are filled using the previous waypoint’s direction/distance.

## Camera Planning / Validation (Coverage-Based)

- `FitCandidates {} -> { ids:[uint64] }`
  - Returns visual object ids suitable for camera fit/orbit (excludes Animation3D).
- `CameraSolve { mode:"FIT|ORBIT|DOLLY|STATIC", ids, t0, t1, constraints?, params? } -> { keys:[{time,value}...] }`
- `CameraValidate { ids, times, values?, constraints?, policies? } -> { ok, results:[{time, within_frame, coverage, adjusted, adjusted_value?, reason}] }`
  - If `values` are omitted, the server may sample the current animation camera at those `times`.
  - Interior walkthroughs: set `constraints.keep_visible=false` to disable the coverage threshold (so the camera can go inside / let the bbox leave the frame).

## Camera Animation Settings (Timeline Evaluation)

- `SetCameraInterpolationMethod { method } -> { ok }`
- `GetCameraInterpolationMethod {} -> string` (protobuf `StringValue`)

Supported `method` strings:
- `Center`
- `Position Spline`
- `Position Rotation Spline` (recommended for waypoint spline fly-throughs and first-person walkthrough paths)

## Introspection

- `ListKeys { id, json_key?, include_values? } -> { keys:[{time,type,value_json?}] }`
  - For `id=0` (camera), `json_key` is ignored.
  - For non-camera tracks, provide the parameter `json_key`.
- `GetTime {} -> { seconds, duration }`

## Notes

- All requests are marshalled to the UI/rendering thread via `QMetaObject::invokeMethod` to respect single-GL-context threading rules.
- For MP4 export, use the existing headless CLI `--run_export_3d_animation` from Python once a `.animation3d` is authored; keep long-running export tasks out of the GUI RPC.
