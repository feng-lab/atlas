Atlas Developer Guide

Build, Run, and Layout

- Build instructions: see `readme.md` (macOS/Linux/Windows, Qt 6.9.x, Intel oneAPI, Vulkan SDK 1.3+, Ninja, Conda recipe for `zimg`).
  - Minimum Vulkan runtime/driver: 1.3. The engine assumes 1.3 core features (dynamic rendering, synchronization2) and no longer enables 1.2 KHR fallbacks.
- Source layout (selected):
  - `src/atlas/` — application code (UI, engine, docs, filters, views)
  - `src/img/` — image I/O/processing utilities
  - `docs/` — documentation
  - `util/` — build scripts

Key References

- Image Paging & Progressive Rendering: [Atlas_Image_Paging_and_Progressive_Rendering.md](Atlas_Image_Paging_and_Progressive_Rendering.md)
- Agents Overview and Tools: [AGENTS_GUIDE.md](AGENTS_GUIDE.md)

Neuroglancer Precomputed (HTTP)

- Atlas can load Neuroglancer “precomputed” volumes over HTTP (both unsharded and sharded storage via `"sharding": {"@type":"neuroglancer_uint64_sharded_v1", ...}`) with these chunk encodings:
  - `encoding: "raw"` — little-endian raw voxel bytes (Fortran order).
  - `encoding: "jpeg"` — requires `data_type: "uint8"` and `num_channels` ∈ {1, 3}.
  - `encoding: "png"` — requires an unsigned `data_type` with 1 or 2 bytes/voxel and `num_channels` ∈ {1, 2, 3, 4}. Output is decoded to planar channel order.
  - `encoding: "compresso"` — requires an unsigned `data_type` with 1/2/4/8 bytes/voxel and `num_channels = 1` (segmentation-style labels).
  - `encoding: "compressed_segmentation"` — requires `data_type` ∈ {`"uint32"`, `"uint64"`} and `compressed_segmentation_block_size`.
  - Sharded volumes require HTTP `Range` support; Atlas supports `minishard_index_encoding` and `data_encoding` of `raw` or `gzip` as specified in Neuroglancer’s sharded format. (Sharding `data_encoding` is applied first, then the per-scale chunk `encoding` is decoded.)
  - Networking is implemented with proxygen/folly: `src/atlas/zproxygenhttpclient.h` and `src/atlas/zproxygenhttpclient.cpp`.
    - For non-`Range` requests, Atlas advertises `Accept-Encoding: br, gzip, zstd` and transparently decodes `Content-Encoding` (`br`/`gzip`/`deflate`/`zstd`) before returning bytes to callers.
    - For `Range` requests, Atlas forces `Accept-Encoding: identity` and rejects encoded responses to preserve byte-exact range semantics (required by sharded Neuroglancer formats).
    - For direct connections (no proxy), Atlas resolves hostnames via the OS system resolver and passes the resolved IP to Proxygen (bypassing Proxygen’s c-ares coroutine DNS path). This is a stability workaround for reproducible SIGSEGV crashes observed under heavy timeout churn with some unstable servers.
  - Optional persistent HTTP disk cache (SQLite-backed, cross-OS) is implemented in `src/atlas/zhttpdiskcache.h`, `src/atlas/zhttpdiskcache.cpp`, and `src/atlas/zsqlitelrucache.h` / `src/atlas/zsqlitelrucache.cpp` and is integrated into `ZProxygenHttpClient::getBytesOnEventBase()` (cache lookup before network; store after successful 200/206).
    - Enable with `--atlas_disk_cache_http_max_bytes=<N>` (default 10 GiB; set to 0 to disable).
    - Async write queue: `--atlas_disk_cache_http_async_max_pending_bytes=<N>` bounds queued SQLite writes (touch/put/erase). Values smaller than 256 MiB are clamped to 256 MiB. When the queue is full, disk writes are dropped (best-effort cache semantics).
    - Read path: synchronous point lookups using per-thread read-only SQLite connections (so concurrent readers do not block each other); LRU touches happen asynchronously.
    - Optional location override: `--atlas_disk_cache_dir=<path>` (otherwise uses the Atlas cache/config directories).
    - The cache is keyed by `(URL + Range)` and stores the already-decoded bytes returned to callers (after HTTP-level `Content-Encoding` handling).
    - Multi-process RW: multiple Atlas processes may concurrently read/write the same cache DB. SQLite contention yields best-effort behavior (writes may be dropped; reads degrade to cache misses).
    - On disk, the cache lives at `<root>/atlas_disk_cache_v1/http.sqlite` (WAL mode).
  - Additional persistent cache buckets can live under the same `<root>/atlas_disk_cache_v1/` directory (each bucket is its own SQLite DB).
  - Optional persistent image-region disk cache (SQLite-backed) can be enabled behind `ZImgRegionCache` to persist computed `ZImg` region blocks for file-backed datasets:
    - Integrated behind the in-memory cache (call sites do not contain disk-cache logic): `src/img/zconcurrentlrucache.h` + `src/atlas/zimgregioncache.h` / `src/atlas/zimgregioncache.cpp`.
    - Enable with `--atlas_disk_cache_imgregion_max_bytes=<N>` (default 20 GiB; set to 0 to disable).
    - Async write queue: `--atlas_disk_cache_imgregion_async_max_pending_bytes=<N>` bounds queued SQLite writes (touch/put/erase). Values smaller than 256 MiB are clamped to 256 MiB. When the queue is full, disk writes are dropped (best-effort cache semantics).
    - Read path: synchronous point lookups using per-thread read-only SQLite connections (so concurrent readers do not block each other); LRU touches happen asynchronously.
    - Payloads are stored compressed (zstd via Folly) to reduce disk usage. Compression runs on the async writer thread; reads transparently decompress on cache hit.
    - Stored at `<root>/atlas_disk_cache_v1/imgregion.sqlite` (single bucket DB).
    - Current scope: file-backed sources (`ZImgRegionCacheSourceKind::File`); Neuroglancer region caching is still memory-only.
  - Optional persistent image-preview disk cache (SQLite-backed) can be enabled to persist the downsampled *raw* 3D preview volume built by `Z3DImg::readVolumes()` for file-backed datasets:
    - Integrated into `ZImgPack::resizedImgCached(...)` and consumed by `Z3DImg::readVolumes()` (fast-path volume construction).
    - Enable with `--atlas_disk_cache_imgpreview_max_bytes=<N>` (default 5 GiB; set to 0 to disable).
    - Async write queue: `--atlas_disk_cache_imgpreview_async_max_pending_bytes=<N>` bounds queued SQLite writes (touch/put/erase). Values smaller than 256 MiB are clamped to 256 MiB. When the queue is full, disk writes are dropped (best-effort cache semantics).
    - Read path: synchronous point lookups using per-thread read-only SQLite connections (so concurrent readers do not block each other); LRU touches happen asynchronously.
    - Payloads are stored compressed (zstd via Folly) to reduce disk usage.
    - Stored at `<root>/atlas_disk_cache_v1/imgpreview.sqlite` (single bucket DB).
- Dataset parsing + chunk addressing lives in `src/atlas/zneuroglancerprecomputed.h` and `src/atlas/zneuroglancerprecomputed.cpp` (reads `.../info`, then fetches chunks on demand).
  - Chunk decode helpers live in `src/atlas/zneuroglancerprecomputedchunkdecoder.h` and `src/atlas/zneuroglancerprecomputedchunkdecoder.cpp`.
  - Sharded-format helpers are in `src/atlas/zneuroglanceruint64sharding.h` and `src/atlas/zneuroglanceruint64sharding.cpp`.
- Neuroglancer “viewer state” import (Option A):
  - Parser: `src/atlas/zneuroglancerstate.h` and `src/atlas/zneuroglancerstate.cpp` extracts only supported precomputed volume layers (image + segmentation) and records warnings for skipped/unsupported layer types.
  - UI entry point: `ZImgDoc::loadNeuroglancerState()` opens those layers and can also apply per-dataset source overrides (mesh/skeleton/annotations) discovered from the state (configuration only; unsupported layers are not created as objects in Atlas yet).
- Caches (per opened Neuroglancer volume; these are budgets, not preallocated):
  - Chunk cache size is controlled by `--atlas_ng_precomputed_chunk_cache_memory_proportion` (default `0.3`, valid range `[0, 1]`).
  - Sharded minishard-index cache size is controlled by `--atlas_ng_precomputed_minishard_index_cache_memory_proportion` (default `0.05`, valid range `[0, 1]`).
  - Higher values can significantly improve 2D/3D interaction (less network I/O and fewer recomputes), but opening multiple Neuroglancer layers simultaneously multiplies the memory budget.
- Integration point is `ZImgPack`: 2D uses on-demand chunk reads for the visible viewport; 3D paging uses `ZImgPack::readRegionToImgAsync()` to populate the GPU page cache. Synchronous “hot-path” queries (`displayValue()`/`value()`) are cache-only to avoid blocking UI/render threads on network I/O.
  - 2D Neuroglancer rendering is progressive: cache-only (best-effort from cached chunks) → preview (fetch coarsest XY to fill holes) → final (fetch best-for-scale after debounce). Preview is skipped if coarsest XY coverage is already cached.
- Neuroglancer segmentation extras:
  - Segment properties (`segment_properties/`) are supported via `ZNeuroglancerPrecomputedSegmentProperties` (`src/atlas/zneuroglancerprecomputedsegmentproperties.*`) and are loaded on demand by mesh/tooling paths (no explicit per-object button required).
  - Precomputed meshes (`mesh/`) are supported via `ZNeuroglancerPrecomputedMeshSource` (`src/atlas/zneuroglancerprecomputedmesh.*`). Mesh import is initiated from the 2D right-click context menu and adds a normal `ZMesh` object to `ZMeshDoc`. Atlas loads a coarse LOD first, then refines to the finest available LOD by replacing mesh geometry in-place (`ZMeshDoc::replaceMeshGeometry` + `meshChanged` signal).
  - Precomputed skeletons (`skeletons/`) are supported via `ZNeuroglancerPrecomputedSkeletonSource` (`src/atlas/zneuroglancerprecomputedskeleton.*`) and are imported into `ZSkeletonDoc` for SWC-like rendering.
  - Precomputed annotations collections are supported via `ZNeuroglancerPrecomputedAnnotationsSource` (`src/atlas/zneuroglancerprecomputedannotations.*`):
    - Relationship index loads (segment/object id → annotations) are used for “Load Neuroglancer Annotations for Segment …” actions.
    - Spatial index loads (voxel AABB → annotations) are used for “Load Neuroglancer Annotations in View (spatial index)…”.
    - POINT/ELLIPSOID annotations are imported as `ZPuncta` and rendered in 3D via `Z3DPunctaFilter`; ellipsoids preserve anisotropic radii (see `src/img/zpunctum.*` and `src/atlas/z3dpunctafilter.*`).
    - LINE/POLYLINE annotations are imported as `ZSkeleton`.
  - Mesh/skeleton source resolution:
    - If the segmentation `info` declares `mesh`/`skeletons` keys, Atlas uses those directory URLs.
    - Otherwise, users can configure per-dataset overrides on the `ZImgPack` (UI: Object View Setting → “Neuroglancer Sources”). These overrides are serialized in `.scene` files and used by the right-click import actions (Atlas does not prompt for source URLs in the context menu).

Testing (Linking Atlas Code)

- Atlas is still a Qt GUI app. Common code now builds as three static libraries to keep Windows COFF archives well under 4GB with LTCG enabled, while maintaining clean backend boundaries:
  - `atlas_core` (STATIC) — non‑graphics/core code (UI, data, utilities). Carries all include dirs/defs/link deps used across Atlas.
  - `atlas_z3d` (STATIC) — shared/OpenGL 3D code (`Z3D*`). Links to `atlas_core` and inherits its usage requirements.
  - `atlas_vulkan` (STATIC) — Vulkan‑only code (`ZVulkan*`). Links to `atlas_core` and inherits its usage requirements.
  - `atlas_lib` (INTERFACE) — umbrella target that links all of the above, preserving a single consumer dependency.
- Prefer linking tests against the smallest Atlas library set that satisfies the test:
  - Core-only tests should link `atlas_core`.
  - Z3D/OpenGL tests should link `atlas_z3d` + `atlas_core`.
  - Vulkan-only tests should link `atlas_vulkan` + `atlas_core`.
  - Use `atlas_lib` when a single consumer target is preferred or when a test truly needs both render backends.
- Usage in CMake (already wired in `test/test.cmake`):
  - `add_atlas_core_gtest_executable(name)` links `GTest::gtest_main` and `atlas_core`.
  - `add_atlas_z3d_gtest_executable(name)` links `GTest::gtest_main`, `atlas_z3d`, and `atlas_core`.
  - `add_atlas_vulkan_gtest_executable(name)` links `GTest::gtest_main`, `atlas_vulkan`, and `atlas_core`.
  - `add_atlas_gtest_executable(name)` links `GTest::gtest_main` and `atlas_lib` (full umbrella).
  - For headless Qt runs, the tests default to `QT_QPA_PLATFORM=minimal`.
- Runtime resources (shaders/assets) remain app-packaged; unit tests around Vulkan/RAII pipeline contracts do not depend on runtime discovery.
- GPU/UI-heavy tests should be gated/opt-in and prefer offscreen surfaces or SwiftShader where available.
- Neuroglancer precomputed E2E tests:
  - `test/zneuroglancerprecomputede2etest.cpp` is a networked smoke test (public GCS URLs) gated by `ATLAS_ENABLE_NETWORK_TESTS=1`.
- Developer-only tooling:
  - `ATLAS_ENABLE_CUSTOM_COMMAND` controls whether Atlas includes the **Help → Run Custom Command** menu item (`ZCustomCommand`). Deployed builds should set this OFF.

Agents: Preview Screenshots

- The Python agent tools expose a headless preview renderer for 3D animation verification.
- Tool: `animation_render_preview` (saves current animation to a temp file and renders exactly one PNG frame via the Atlas binary in offscreen mode).
- Privacy/consent: The CLI prompts once per session for consent to use preview screenshots for verification (default allow). This decision is stored in the session state; you can toggle at runtime via `:screenshots on` / `:screenshots off`.
- Binary resolution: The tool uses `--atlas-dir` if provided to the agent CLI, or searches default install locations.
- Typical usage:
  - `python -m atlas_agent --atlas-dir /Applications/fenglab/Atlas.app` then accept the startup consent prompt.

Agents: Camera Planning & Validation

- Essential tools (LLM function-calling):
  - `fit_candidates` → choose ids to frame (excludes Animation3D).
  - `camera_focus`, `camera_point_to`, `camera_rotate`, `camera_reset_view` → deterministic operators (UI parity) for stateless camera value generation.
  - `animation_camera_solve_and_apply` (modes FIT | ORBIT | DOLLY | STATIC) → solves and writes validated camera keys; clears existing keys in the time range by default. The tool sets the engine timeline time to `t0` before solving so adjacent segments start from the timeline pose (prevents boundary “camera resets”).
  - `animation_camera_validate` → dry‑run with constraints/policies; prefer strict first, then allow adjustments if needed.
  - Aspect ratio: camera planning/validation currently assumes a 16:9 “planning viewport” (matches common export defaults), not the interactive UI window size.
  - `animation_batch` → write non‑camera parameter keys atomically. Camera keys are written by `animation_camera_solve_and_apply` (do not wrap in batch).

- Deprecated/removed: all camera "recipe" tools. Compose motions with the general tools above; do not rely on hardcoded recipes.

Agents: Camera Planning & Validation (Example)

- Example (“rotate around the mesh 360° in 10 seconds”):
  1) Pick targets: `fit_candidates` or `scene_list_objects` → ids
  2) Solve + write: `animation_camera_solve_and_apply(mode='ORBIT', ids=ids, t0=0, t1=10, degrees=360, params={axis:'y'}, constraints={keep_visible:true, min_frame_coverage:0.0})`. This clears existing keys in [0,10] and writes validated keys.
  3) Set duration: `animation_set_duration(10)`.
  4) Optional validate: `animation_camera_validate(ids, times, values, constraints={keep_visible:true,min_frame_coverage:0.0}, policies={adjust_distance:false})` using the written keys.
  5) Preview (optional): `animation_render_preview(time=5)` for a mid‑orbit frame.
  - Note: `min_frame_coverage` is a **screen-space** framing metric (dominant-dimension bbox fill). Higher values push toward tighter framing (larger subjects). For close-ups, validate/solve against a smaller set of target ids and raise `min_frame_coverage` (and keep `margin` small).

Agents: Codegen Mode

- When tasks require loops, randomness, or file parsing (e.g., “for each SWC, randomly translate within ranges”, or “read transforms from file and apply”), prefer codegen over raw tool chaining.
- Workflow (multi‑step, iterative):
 - Discover with tools: `scene_list_objects`, `scene_list_params(id)`, `scene_bbox`.
 - Unified addressing across tools: use `id` only with reserved ids 0=camera, 1=background, 2=axis, 3=global, ≥4=object ids. The legacy scope/object/group forms have been removed from Agent Tooling.
  - Generate a small plan‑only Python script using `atlas_agent.api` (SceneAPI/CameraAPI) to compute values (no writes). Run with `python_write_and_run` and print compact JSON.
  - Validate with tools: `scene_validate_params` or `camera_validate`; refine the script if invalid.
  - Generate an apply script to write keys/params; run; then verify (`scene_get_values(id)`, `animation_list_keys(id,json_key)`).
- Repair loop: On script errors (non‑zero exit), capture stdout/stderr, revise, and re‑run within guardrails (max attempts env `ATLAS_AGENT_CODEGEN_MAX_ATTEMPTS`, default 20; overall time budget). Stop early if errors repeat unchanged.
- Scripts should be focused, short, and use stdlib. Avoid monolithic “one‑shot” scripts.


Architecture Overview

- Main window (`ZMainWindow`) — 2D UI; hosts object manager, docks, menus.
- 3D window (`Z3DMainWindow`) — spawns a rendering thread and owns a `Z3DRenderingEngine` (moved to the rendering thread), and a `Z3DCanvas` on the UI thread.
- Rendering engine (`Z3DRenderingEngine`) — owns the offscreen GL context, global parameters, compositor, network evaluator, and per-object 3D views.
- Parameter system (`ZParameter` + subclasses) — typed, QObject-based, with signals/slots and JSON (de)serialization.
  - Each parameter can now describe its own value JSON Schema via `ZParameter::valueSchema()`.
    - Default is permissive (any JSON). Subclasses override for precision (numeric scalars/vectors/spans, options, transforms, camera).
    - The Animation3D schema dumper queries `valueSchema()` and attaches `description()` when present.

Lookup Tables (LUTs)

- Colormaps (`ZColorMap`) and transfer functions (`Z3DTransferFunction`) are CPU-only. They expose:
  - `buildLUTBGRA8(width)` to generate an RGBA8 LUT in CPU memory.
  - `generation()` that increments on change for cache invalidation.
- Renderers/pipeline contexts create and cache backend LUT textures:
  - OpenGL: per-renderer 1D RGBA8 textures for colormaps/transfer functions.
- Vulkan: pipeline contexts create 2D Nx1 `ZVulkanTexture`s via a small helper (MoltenVK portability — Metal lacks native 1D); LUTs are uploaded as RGBA8 and bound through descriptor sets.
  - Vulkan descriptor arena (Stage 2): pipeline contexts must allocate descriptor sets from the backend’s per-frame arena via `Z3DRendererVulkanBackend::allocateFrameDescriptorSet(layout)`. Do not create per-context descriptor pools. The arena is reset once per frame (scheduled in `endRender()`, applied at the backend’s frame-completion safe point after the frame fence signals).
  - Frame-completion safe point: the backend defines a frame-slot “completion safe point” (`applyPendingArenaReset`) that is reached after the executor observes a slot fence as complete (slot reuse, explicit waits, and opportunistic pumping after `pollCompletions()`). At that point it:
    - resets per-frame descriptor resources,
    - drains all “after completion” hooks with a barrier (hooks may run on their own executors),
    - then wakes `awaitCurrentFrameCompletion()` awaiters for the previous generation.
  - Scratch-pool recycling: Vulkan scratch image leases are released only after the submitting frame’s fence signals. The pool defers slot reuse by registering an “after completion” hook at the frame-completion safe point (via `registerAfterCurrentFrameCompletionHook()` through the backend-installed scratch scheduler).
  - Shared fullscreen quad: use `Z3DRendererVulkanBackend::fullscreenQuadVertexBuffer()` in full-screen passes (background, copy, blend, glow) instead of creating per-context VBOs.
  - Vulkan descriptor guardrails:
    - Do not write descriptors while a frame is recording. Persistent sets are write-once per frame; update only UBO contents via `copyData(...)` in record paths.
    - Route volatile inputs (peel/resolve/composite) through per-draw override sets allocated with `allocateOverrideDescriptorSet(...)`.
    - Always pass explicit samplers for combined image samplers unless layouts use immutable samplers.
    - Never free descriptor sets individually; rely on the per-frame pool reset. Clear any retained override sets before reset (backend handles this at frame start).

ImgRaycaster Vulkan

- Payloads are POD-only: the Vulkan raycaster no longer carries a `Z3DImgRaycasterRenderer*` in `ImgRaycasterPayload`.
  - Transfer functions are provided directly in the payload as `const std::vector<Z3DTransferFunction*>* transferFunctions`.
  - Vulkan caches per-channel transfer LUT textures by `(generation,width)` and only re-uploads when either changes (mirrors the ImgSlice colormap path).
  - Progressive bookkeeping is outside the Vulkan pipeline context. The context computes whether a progressive round finished and the backend calls back to the renderer using a stable `streamKey` identity to finalize (`finalizeProgressiveRound`).
  - GL paths are unchanged.
- On backend switch, `Z3DRendererBase::releaseBackendResources()` clears renderer caches; `Z3DImgFilter::switchRendererBackend` releases GL volume resources when switching to Vulkan.
  - Post‑fence CPU work that must run after submission completion callbacks (e.g., Block‑ID compaction parsing that assumes residency unpins have drained) should await the backend’s frame‑completion safe point (`applyPendingArenaReset`, pumped when the backend observes fence completion). This preserves ordering without depending on frame-slot reuse when `maxFramesInFlight > 1`.

Threading Model

- UI thread: widgets (`Z3DCanvas`, main window), menu actions, docks, drag-and-drop.
- Rendering thread: all engine code, rendering parameters, compositor, and object views.
- Cross-thread rules:
  - Do not manipulate engine or parameter QObjects directly from UI.
  - Use `QMetaObject::invokeMethod` to post to engine thread; use `Qt::BlockingQueuedConnection` if you must wait.
  - For parameter changes, queue to the parameter’s owning thread (see `ZParameterAnimation::setCurrentTime`).
  - For coroutine-based flows, prefer the engine’s render-thread executor:
    - `Z3DRenderingEngine::renderThreadExecutor()` provides a `ZQtExecutor` (a `folly::Executor`) that schedules onto the engine thread via Qt event posting.
    - Pipeline contexts and Vulkan backend code should use `currentRenderThreadExecutorKeepAlive(...)` at call sites that need a keep-alive token for `co_withExecutor(...)`.
    - Teardown: `Z3DRenderingEngine::drainVulkanFrameExecutorForTeardown()` must run on the engine thread before quitting it so fence-gated continuations can complete deterministically.

Pointer Nullability Contract

- Default non-null: treat all pointer and smart-pointer parameters as required (non-null) unless explicitly marked as nullable.
- Entry checks only: validate required pointers once at function entry with `CHECK(ptr)` (or `CHECK(ptr != nullptr)`), then use directly without `if (!ptr)` branches.
 - Explicitly nullable: annotate nullable parameters with `/*nullable*/` in the declaration. Optionally add a short preceding comment noting which parameters/return values are nullable. Handle the null path deliberately. No Doxygen is required.
- Smart pointers: apply the same rules. Required `std::unique_ptr<T>&` / `std::shared_ptr<T> const&` must pass `CHECK(ptr)`; nullable variants must be annotated `/*nullable*/`.
- Prefer `std::optional<T>` for value-semantics optional data; for non-owning optional references, prefer `/*nullable*/ T*`.

Examples

```cpp
// Required pointer: check once, then use directly
void renderPass(Z3DRendererBase* renderer, const ZScene* scene) {
  CHECK(renderer);
  CHECK(scene);
  renderer->beginPass(*scene);
}

// Explicitly nullable: annotation + deliberate handling
void setLabel(Z3DCanvas* canvas, /*nullable*/ const char* text) {
  CHECK(canvas);
  if (!text) return;  // documented nullable
  canvas->setStatusText(text);
}

// Smart pointer parameter with required contract
void attachTexture(const std::shared_ptr<ZTexture>& tex) {
  CHECK(tex);
  bindTexture(*tex);
}
```

Optional comment style (when a signature is crowded or readability benefits):

```cpp
// Nullable: text
void setLabel(Z3DCanvas* canvas, /*nullable*/ const char* text);
```

This mirrors the Coding Standards in `AGENTS.md` and helps simplify control flow while catching contract violations early.

Scene Load/Save (JSON)

- Load: `ZMainWindow::loadJsonSceneImpl`
  - Reads the `Doc` section via `ZDoc::read` which creates objects.
  - For 3D:
    - UI ensures 3D window is ready
    - Engine session starts via `beginScene3DApply()`
    - Applies `View3DGeneral` and per-object `View3D` via `applyView3D*` methods on the engine thread
    - Per-object JSON is queued if its 3D view isn’t ready; engine applies after `objViewReady`.
    - Optionally block until `scene3DApplyFinished()` using flag `--atlas_block_scene_3d_apply`.
- Save: UI collects 2D and 3D view JSON via direct 2D calls and engine-thread `write` calls (`BlockingQueuedConnection`).

Animation System

- A `Z3DAnimation` binds to the engine (view) and maintains timelines (`ZParameterAnimation` keys) for parameters (global: camera, etc.; object-specific).
- Binding and updates:
  - `ZAnimation::rebindView()` obtains parameter lists via `Z3DRenderingEngine::parametersOfViewSetting(id)` on the engine thread and binds animations without crossing QObject ownership.
  - `ZAnimation::tryLinkAnimationWith(id)` binds late objects and applies `m_currentTime` to their parameters immediately.
  - `ZParameterAnimation::setCurrentTime()` posts updates to the parameter’s thread.

Compositor and Rendering

- `Z3DCompositor` orchestrates geometry/image filters and render targets; supports transparency methods and axis/background.
- `Z3DRenderingEngine` owns a linear filter pipeline (object filters feeding the compositor) and drives progressive updates each frame.
- `Z3DGlobalParameters` holds camera, lights, fog, global cuts, device pixel ratio, and scratch resource pool.

Global Cut Mode (Binding)

- The global X/Y/Z cuts no longer infer intent from equality with min/max. Each axis has an explicit mode parameter (UI label: “Global X/Y/Z Cut Mode”) stored in `Z3DGlobalParameters`:
  - Absolute: hold absolute values; clamp to new bounds.
    - newLower = clamp(oldLower, min, max)
    - newUpper = clamp(oldUpper, min, max)
  - Track Edges: pin lower/upper edge independently via booleans (UI: “Global X/Y/Z Cut Pin Lower/Pin Upper”).
    - newLower = (PinLower ? min : clamp(oldLower, min, max))
    - newUpper = (PinUpper ? max : clamp(oldUpper, min, max))
  - Normalized [0..1]: store fractional endpoints f0,f1 and recompute.
    - newLower = min + (max − min) · f0
    - newUpper = min + (max − min) · f1
- Engine path: `Z3DRenderingEngine::updateBoundBox()` calls `Z3DGlobalParameters::applyBoundsForCuts(...)` to re-evaluate cuts deterministically when the scene bounds change.
- Defaults: Track Edges ON for both endpoints, which follows min/max and shows full range.
- Serialization: binding mode, toggles, and normalized spans persist with scenes.
- Invariants: Bounds are validated upstream; span parameters handle clamping and ordering.

Vulkan Notes

- Backend selection is a session-level switch. GL remains supported; Vulkan is the preferred backend for parity.
- Renderers expose backend‑neutral batch data via `enqueueRenderBatches`; Vulkan records via the explicit entry points in `Z3DRendererBase` (no implicit frame begin/end).
- Per-eye `Z3DScratchResourcePool` leases stay with each filter. Vulkan dynamic rendering targets are expressed via `RendererFrameState::ActiveSurface` and set with `setActiveSurfaceWithLoadStore(...)` at the call site.
- Keep renderer parameters persistent at the filter; renderer objects hold transient GPU resources only.
- Naming convention: cross‑backend code uses `Z3D*`; Vulkan-only uses `ZVulkan*`.
- Attachment end-of-pass usage must be explicit for Vulkan:
  - `AttachmentDesc::finalUse` is the backend-neutral signal describing how a produced attachment will be used after the pass (`RenderTarget`, `Sampled`, `TransferSrc`, `General`). `Unspecified` is a hard `CHECK` in Vulkan to avoid implicit layout assumptions.
  - The Vulkan backend derives `ZVulkanRenderingSegmentSpec` final layouts directly from `finalUse` to avoid label/shader-hook heuristics, which were a common source of state leakage (wrong params / flicker).
- Cross-pass image usage must be explicit for Vulkan:
  - `BackendPassDesc::externalImageUses` lists images a pass will access that are not bound as render-target attachments (sampled inputs for fullscreen compositing, storage/transfer ops, etc.).
  - Each use declares an `ExternalImageUseKind` (SampledRead, Storage*, Transfer*, General) and an `ExternalImageAspectHint` (Color/Depth/Stencil). Vulkan does not infer depth/stencil sampling layouts from formats; `Unspecified` is a hard `CHECK`.
  - The Vulkan backend transitions these images to the required layouts before recording the batch; renderers/compositor populate this metadata (including shader-hook driven inputs like DDP peel pings).
  - Invariant: external uses must not reference an image that is also bound as an active attachment in the same pass (no read-while-write feedback loop); this is enforced with a hard `CHECK`.
- Cross-pass buffer usage must be explicit for Vulkan:
  - `BackendPassDesc::externalBufferUses` lists buffers a pass will access that are not otherwise managed as part of the backend’s pass recording (scratch buffers shared across multiple passes, transfer staging, etc.).
  - The Vulkan backend inserts `vkCmdPipelineBarrier2` buffer barriers based on `ExternalBufferUseKind` (UniformRead, Storage*, Transfer*, General).
  - `ExternalBufferUseKind::Unspecified` is a hard `CHECK` to avoid implicit dependencies.
  - `CHECK`s enforce that declared buffer use kinds match the underlying Vulkan buffer usage flags (e.g., Storage* requires `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`).
- Texture upload final layouts must be explicit for Vulkan:
  - `ZVulkanTexture::UploadRegion::finalLayout` defaults to `vk::ImageLayout::eUndefined` and is a hard `CHECK` in `uploadSubImage` / `uploadData`.
  - Call sites must pass the intended post-upload layout (typically `eShaderReadOnlyOptimal` for sampled color images; `eDepthReadOnlyOptimal` when preparing depth for sampling) instead of relying on implicit defaults.
- Enforcement (fail-fast correctness):
  - When writing transient per-draw override descriptor sets during command recording, Vulkan `CHECK`s that any bound textures/storage images are already in the layout declared by the descriptor (or the explicit override layout). Missing usage metadata now trips deterministically instead of producing flicker/incorrect rendering due to undefined layout usage.
- Vulkan paging cache residency (Z3DImg):
  - `ZVulkanPagedImageBlockUploader` supports swapping large paged image caches (R8 `imageCache` textures) to host memory under device-local memory pressure; page directory / page table caches remain device-resident (small).
  - Best-effort budget comes from `VK_EXT_memory_budget` (queried via VMA) when available; otherwise eviction is driven by allocation failures.
  - Budget controls (gflags): `--atlas_vk_paged_image_cache_budget_bytes` (0=use device budget), `--atlas_vk_paged_image_cache_budget_ratio`, `--atlas_vk_paged_image_cache_budget_reserve_bytes`.

Vulkan Pipeline Invariants

- Dynamic rendering is used; a new segment is begun only when attachment sets change.
- Graphics pipeline keys include attachment formats: `colorFormats[]` and optional `depthFormat` are part of the key in all Vulkan pipeline contexts to avoid layout mismatches.
- Composite/resolve passes (DDP final, WA resolve, WB resolve) must write to exactly one color attachment; depth is disabled in the pipeline and no depth attachment is bound.
- Descriptor updates after binding are forbidden. Per‑draw overrides are allocated from the backend’s per‑frame arena and kept alive until the frame fence to satisfy validation rules.
- Dynamic UBO arena (Vulkan): all per‑draw UBOs are suballocated from a per‑frame, host‑visible "uniform arena" buffer. Capacity is fixed for the frame; exceeding it is a hard CHECK. The backend provisions a baseline capacity (default 256 KiB) and will pre‑size above it when needed (based on a cheap pre‑record estimate). Growth within a frame is not supported (would invalidate already‑bound descriptors).
- Backend validates that the pipeline’s attachment formats match the currently active dynamic rendering segment; mismatches are logged at VLOG(1) and the batch is skipped.

Performance Instrumentation

- Aggregated frame timing: the rendering engine emits a monotonically increasing token per user‑visible frame (one engine‑driven filter pipeline evaluation). The Vulkan backend tags each submission with this token and a submission index.
- Per‑submission CPU and GPU scopes are ingested and a single summary is logged once a token is safe to flush (typically on the next submission, after fences signal). Summaries appear at `VLOG(1)`.
- Modes (gflags):
  - `--atlas_perf_mode=off|light|full` (default `light`). `full` adds nested per‑filter GPU scopes inside compositor passes.
  - `--atlas_perf_trace=/path/to/trace.json` writes a Chrome trace file for each flushed frame (overwrites).

Descriptor & Recording Guardrails (Vulkan)

- No descriptor writes while a frame is recording. Persistent/frame descriptor sets are write-once before `vkCmdBeginRendering`; per‑draw override sets are allowed during recording.
- Volatile inputs must use per‑draw override sets (e.g., DDP peel/resolve, WA/WB resolves, glow/copy/blend sources).
- Prefer explicit or immutable samplers in set layouts to avoid platform-specific sampler class issues.
- Per‑frame descriptor arenas are monotonic: allocate during the frame, reset once after the frame fence. Clear transient override sets before reset.
- Validation/telemetry: end‑of‑frame VLOG may include segment counts, descriptor guardrail counters, and skip reasons (format mismatches, etc.).

Pipeline Context Recorder

- Use `ZVulkanPipelineCommandRecorder` to emit hermetic passes. Populate a `ZVulkanGraphicsPassSpec`/`ZVulkanComputePassSpec` with every state your shader relies on (pipeline, layout, descriptor sets, push constants, vertex/index buffers, dynamic state, attachment barriers) and call `recordGraphicsPass` / `recordComputePass`.
- Debug builds enforce the contract when `--atlas_vk_enforce_pipeline_context=true`: missing viewports/scissors, incomplete descriptor coverage, absent push constants, or unexpected active queries trigger a hard `CHECK`. Disable with the flag only when debugging third-party drivers.
- `buildStaticSecondary` and `buildStaticSecondaryAsync` wrap secondary command buffer creation (render-pass continue + simultaneous use by default), making it easy to pre-record background layers on worker threads (`folly::coro` ready).
- Example – graphics pass:

```cpp
vk::raii::CommandBuffer& cb = ...; // primary command buffer (already begun)
nim::ZVulkanPipelineCommandRecorder recorder(cb);

nim::ZVulkanGraphicsPassSpec pass{};
pass.pipeline = &gPipeline;
pass.pipelineLayout = &gLayout;
pass.renderArea = vk::Rect2D{{0, 0}, {width, height}};
pass.viewports = {vk::Viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f}};
pass.scissors = {vk::Rect2D{{0, 0}, {width, height}}};
pass.colorAttachments = {colorAttachmentInfo};
pass.depthStencilAttachment = depthAttachmentInfo;
pass.descriptorSets = {**frameSet};
pass.lineWidth = 1.0f;
pass.depthTestEnable = VK_TRUE;
pass.depthWriteEnable = VK_TRUE;
pass.topology = vk::PrimitiveTopology::eTriangleList;
pass.vertexBuffers = {positionBuffer};
pass.vertexOffsets = {0};
pass.indexBuffer = indexBuffer;
pass.indexType = vk::IndexType::eUint32;
pass.indexCount = indexCount;
recorder.recordGraphicsPass(pass);
```

- Example – compute dispatch:

```cpp
nim::ZVulkanComputePassSpec compute{};
compute.pipeline = &computePipeline;
compute.pipelineLayout = &computeLayout;
compute.descriptorSets = {**frameSet};
compute.requirePushConstants = true;
compute.pushConstantsData = &params;
compute.pushConstantsSize = sizeof(params);
compute.groupX = (workWidth + 7) / 8;
compute.groupY = (workHeight + 7) / 8;
nim::ZVulkanPipelineCommandRecorder recorder(cb);
recorder.recordComputePass(compute);
```

Vulkan Block-ID Compaction

- Append-only model: compaction uses a per-workgroup local dedupe + global append buffer. Hash/CAS variants are deprecated.
- Read sources (append only):
  - `--atlas_vk_blockid_compaction_source=buffer|storage|sampled`
    - `buffer` (default): Copy image → SSBO in-cmd then read from SSBO.
    - `storage`: Read via `uimage2D + imageLoad` (layout `GENERAL`).
    - `sampled`: Read via `usampler2D + texelFetch`.
- Deprecated flag: `--atlas_vk_blockid_compaction_mode`
  - Ignored; append-only. Keep for compatibility.
- Synchronization: ColorAttachmentWrite → Compute barrier; for buffer source, image transitions to `TRANSFER_SRC_OPTIMAL`, copy to SSBO, and transitions back. A Transfer→Compute buffer barrier makes the SSBO visible to compute.

- Compute → graphics hazards are spelled out in the attachment descriptors. Example: transition a storage image written by compute into a sampled image for the lighting pass.

```cpp
nim::ZVulkanAttachmentInfo gbufferNormal{};
gbufferNormal.image = **normalsImage;
gbufferNormal.view = **normalsView;
gbufferNormal.format = normalsFormat;
gbufferNormal.initialLayout = vk::ImageLayout::eGeneral; // compute writes
gbufferNormal.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
gbufferNormal.loadOp = vk::AttachmentLoadOp::eClear;
gbufferNormal.storeOp = vk::AttachmentStoreOp::eStore;
gbufferNormal.srcStage = vk::PipelineStageFlagBits2::eComputeShader;
gbufferNormal.srcAccess = vk::AccessFlagBits2::eShaderWrite;
gbufferNormal.dstStage = vk::PipelineStageFlagBits2::eFragmentShader;
gbufferNormal.dstAccess = vk::AccessFlagBits2::eShaderSampledRead;
lightingPass.colorAttachments = {gbufferNormal, colourAttachment, ...};
lightingRecorder.recordGraphicsPass(lightingPass);
```

- Static background recording pattern:

```cpp
nim::ZVulkanSecondaryBuildInfo info{
  .device = &device,
  .commandPool = &secondaryPool,
  .inheritance = vk::CommandBufferInheritanceInfo{}.setRenderPass(VK_NULL_HANDLE)
};
auto staticBackground = nim::buildStaticSecondary(info, [&](vk::raii::CommandBuffer& scb) {
  nim::ZVulkanPipelineCommandRecorder secondaryRecorder(scb);
  secondaryRecorder.recordGraphicsPass(backgroundPass);
});
  primaryCmd.executeCommands({*staticBackground});
  ```

 - Pair the recorder with back-end stats to guarantee no state leaks between passes; OpenGL-equivalent code should set the same dynamic values to ease diffing.

**Backend Segment Ownership**
- Backend/compositor exclusively owns dynamic rendering segments (attachments, clears, begin/end).
- Pipeline contexts are draw-only: they may not build attachments or call beginRendering/endRendering.
- Use `ZVulkanPipelineCommandRecorder::beginRenderingSegment/endRenderingSegment` in the backend, passing `ZVulkanRenderingSegmentSpec` with per-attachment transitions and final layouts.
- Contexts must use `ZVulkanPipelineCommandRecorder::recordGraphicsDraw` with complete state: descriptor coverage, push constants, viewports, scissors, and dynamic state as needed. The debug tracker asserts missing state in debug builds.
- Clear/load/store decisions are made at segment open; contexts must not gate clears.

**Atlas Frame → Vulkan Frame → Recording Session → Rendering Segment**
- Atlas frame: one end-to-end render request as seen by the engine/UI (“user changed a parameter, render until a stable image is ready”).
- Vulkan frame: one GPU submission slot managed by `ZVulkanFrameExecutor` (primary command buffer + fence/semaphores). A Vulkan frame may stay open across multiple passes to reduce submission overhead.
- Preferred callsite abstraction for orchestration (compositor + image filter pipelines such as raycaster/slice): `ZVulkanLinearScript` (`src/atlas/zvulkanlinearscript.h`).
  - Call sites should express logic linearly as segments + explicit CPU readback boundaries (for branching/loop control flow), without directly managing `beginVulkanFrame/endVulkanFrame` or toggling global readback wait policy.
  - CPU readback boundaries are the only place the script forces a fence wait for that submission; interactive presentation stays non-blocking by default.
  - Call sites that need cancellation (and other exceptions) to propagate must call `script.flush(...)` explicitly; the script destructor performs a best-effort flush but must not throw and therefore swallows cancellation.
  - Guideline (conceptual, not required for correctness): keep each script node small and single-purpose. In dynamic rendering there are no classic Vulkan "subpasses"; the closest equivalent is a rendering segment (`vkCmdBeginRendering`…`vkCmdEndRendering`) targeting one attachment set. Prefer one output surface / attachment set per `script.raster(...)` node, and split nodes when switching render targets so labels and load/store/final-use contracts remain easy to reason about.
- Recording session: one “batch collection + submit” scope inside an already-open Vulkan frame. `Z3DRendererBase::recordVulkanBatchesInActiveFrame(...)` (and the compositor helper `recordInVulkanFrame`) open a session, collect CPU batches, and call `Z3DRendererVulkanBackend::processBatches(...)` to emit commands into the active command buffer.
  - Recording-session sync point: `processBatches(...)` ends with no active dynamic rendering segment and flushes any scheduled upload→static copies (`flushScheduledCopies`). This ensures orchestration code (DDP/PPLL) can safely insert compute/copy work between sessions without accidentally recording inside `vkCmdBeginRendering`.
- Rendering segment: a single `vkCmdBeginRendering`/`vkCmdEndRendering` region inside one recording session.
  - Segment coalescing is allowed only when begin-rendering state matches: render area + the ordered attachment set + per-attachment load/store/clear + final-use contracts. Do not merge segments across incompatible pass metadata; doing so creates silent state leakage (wrong clears, wrong final layouts, or read-while-write feedback loops).
  - External resource dependencies are explicit: passes must declare external image/buffer uses in `BackendPassDesc` so Vulkan can insert layout transitions and buffer memory barriers without label/payload heuristics.
- Frame-completion safe point: after the submission fence signals, the backend reaches `applyPendingArenaReset` and runs after-completion hooks. Anything that depends on “GPU really finished” (scratch reuse, descriptor arena reset, block-ID compaction parsing) must be attached to this safe point, not to “session end”.

Vulkan Entry Points (explicit)

- OpenGL entry points remain `render(...)` and `renderPicking(...)`. These drive the GL path and may begin/end GL frames as needed.
- Vulkan uses explicit, collection‑only entry points in `Z3DRendererBase`:
- Execute (may open/close the frame): `executeVulkanBatches(fn, label)`
  - Begin the Vulkan frame if none is active, set a GPU scope label, run `fn`, submit, and end the frame unless already active.
  - Record within an already‑open frame (never opens/closes): `recordVulkanBatchesInActiveFrame(fn, label)`
    - Asserts an active frame (`beginVulkanFrame()` must have been called by the owner) and performs the same session invariants and submission.
  - Enqueue only: `renderVulkan(eye, ...)` / `renderPickingVulkan(eye, ...)`
- Enqueue backend‑neutral batches only. These assert Vulkan backend.
  - Aggregators may call `renderVulkan`/`renderPickingVulkan` on source renderers outside an active recording
    session to collect CPU batches only; submission (begin/end frame and emit) must be done by the owning renderer
    via `executeVulkanBatches` or `recordVulkanBatchesInActiveFrame`.
- Invariants:
- For Vulkan, call `renderVulkan`/`renderPickingVulkan` inside an execute/record block when emitting from the same
  renderer that will submit. Aggregation workflows can collect from other renderers out of session and then append
  those batches to the submitting renderer.
  - A valid active surface must be set before the first append in a recording session, or the first batch must carry attachments explicitly. Violations cause a CHECK and include the pass label.

Pass setup patterns (setActiveSurfaceWithLoadStore):
- Clear + write: `renderer.setActiveSurfaceWithLoadStore(surfaceOrLease, LoadOp::Clear, StoreOp::Store, LoadOp::Clear, StoreOp::Store, clear)`
- Overlay (preserve color, reset depth): `renderer.setActiveSurfaceWithLoadStore(surfaceOrLease, LoadOp::Load, StoreOp::Store, LoadOp::Clear, StoreOp::Store, clear)`
- Preserve per‑attachment policy (DDP/WA/WB surfaces): `renderer.setActiveSurfaceWithLoadStore(surfaceOrLease, Preserve)`

Example (pseudocode)

```
// Simple: execute (may open/close frame for you)
renderer.setCollectOnly(true);
// Clear+Store both color and depth at pass start
renderer.setActiveSurfaceWithLoadStore(lease, LoadOp::Clear, StoreOp::Store, LoadOp::Clear, StoreOp::Store, {});
renderer.executeVulkanBatches([&]{
  renderer.renderVulkan(eye, myRenderer);
}, "my_pass");
renderer.setCollectOnly(false);

// Advanced: owner controls the frame lifetime once
renderer.beginVulkanFrame();
auto guard = folly::makeGuard([&]{ renderer.endVulkanFrame(); });
renderer.setCollectOnly(true);
// Preserve per-attachment load/store on surfaces that encode policy (e.g., OIT)
renderer.setActiveSurfaceWithLoadStore(lease, Preserve);
renderer.recordVulkanBatchesInActiveFrame([&]{
  renderer.renderVulkan(eye, myRenderer);
}, "my_pass");
renderer.setCollectOnly(false);
```

Vulkan Descriptor Set/Binding Map

- See `src/atlas/zvulkanbindings.h` for canonical indices. Standard layout:
  - Set 0 — Inputs (sampled images):
    - Mesh textures (1D/2D/3D) and wideline texture (emulated 1D)
    - DDP peel: binding 0 = depth blender, 1 = front blender
    - WA resolve: binding 0 = accumulation, 1 = moments
    - WB resolve: binding 0 = accumulation, 1 = transmittance
  - Set 1 — Lighting UBO (std140):
    - `lighting_enabled`, `numLights`, fog parameters, per‑light arrays
    - `screen_dim_RCP`, `weighted_blended_depth_scale`, `ze_to_zw_a/b`
    - Fragment stage; see `Resources/shader/vulkan/include/lighting.glslinc`
  - Set 2 — Transforms/Material UBOs (std140):
    - Binding 0: `Transforms` (proj/view matrices, `pos_transform`, normal matrix, `parameters.x=sizeScale`)
    - Binding 1: `MaterialUBO` (sceneAmbient, materialAmbient/specular/shininess, alpha, optional customColor)
    - Vertex + fragment stages; see `Resources/shader/vulkan/include/matrices_material.glslinc`
  - Set 3 — OIT (DDP flag only):
    - Binding 1: `DDPFlag` SSBO used by dual-depth-peeling peel passes to mark any-pixel-updated

Guidelines
- Allocate frame‑scoped descriptor sets from the backend arena; avoid per‑context pools.
- Do not rewrite persistent descriptors during recording; update UBO contents only.
- Use per‑draw override sets for volatile inputs (peel/resolve/composite images).
- Keep this set ordering consistent across contexts.

Invalidation & Progressive Rendering

- A `Z3DFilter` tracks invalidation bits (mono/left/right). When a bit is set, the engine knows that eye needs processing.
- Causes of invalidation:
  - Parameter changes (wired in `Z3DFilter::addParameter`)
  - Global camera/viewport changes and engine output-size changes
  - Per-filter `updateSize(targetSize)` calls (size handling, then invalidates all results)
- Image filters request cancellation on invalidate and defer renderer resets to the next `process()` to avoid mutating state mid-pass.

Debug reason plumbing (debug builds only)

- `Z3DFilter` exposes `debugSetInvalidateReason` and `debugTakeInvalidateReason` (no-ops in release).
- `addParameter` tags a human-readable reason with a JSON snapshot.
- `Z3DImgFilter::invalidate` prints reasons (with `inv` and current `m_state`) and suppresses duplicate messages for the same state.
- For analysis only, `Z3DImgFilter` also skips global-cut invalidations that don’t change the effective cut against the image AABB (epsilon-based).

Canvas and Lifecycle

- `Z3DCanvas` posts UI events to engine. It updates its view on `renderingFinished`.
- Teardown (ordering and guards):
  - Queued signals can arrive after detaching/destroying engine.
  - `Z3DCanvas::renderingFinished` guards its engine pointer before access.
  - Engine destructor sets a shutdown flag (`m_shuttingDown`) so `event()` ignores late posts.
  - `detachCanvas()` first disconnects and clears the canvas, then adjusts `devicePixelRatio` to avoid signaling during teardown.
  - Watcher lifetime: engine tracks observed `ZWidgetsGroup*` in `m_observedWGs` and erases on `destroyed`. The set is declared before the compositor so it outlives the groups during destruction.

Logging

- Uses glog: `LOG`, `VLOG`, `LOG_FIRST_N`, `LOG_EVERY_N`.
- Notable info logs:
  - “3D scene parameters applied” — deferred scene apply queue drained.
  - “3D animation parameters bound” — first animation binding completed.
  - In debug builds (`ATLAS_DEBUG_VERSION`), you’ll also see: “image filter invalidate: …” with parameter/global reasons and state bits.
- Vulkan logging hygiene:
  - No-op sampled-read transitions are suppressed: `ensureSampledReadable` only logs when a layout change occurs.
  - Dynamic rendering ‘begin’ logs always carry a non-empty label; empty pass labels fall back to `<unnamed>`.
  - `TextureCopy` OIT UBO priming (`set=3,binding=0`) logs at most once per frame per pipeline context.
  - Upload arena capacity changes are logged; steady-state per-call capacity is not repeated.

Level Semantics (Vulkan)
- info (one line per submission/pass): CPU time, GPU total time, followed by per-scope GPU timings and concise counters appended at the end.
  - Example: `VK batches [frame#F sub#S] 'pass' CPU 1.234 ms GPU 0.987 ms | scopeA 0.456 ms | scopeB 0.531 ms | dsets=… ovsets=… pipes+=… bound=… segs=… clr=… ld=… dwr=… rew=… upload_hi=…B static=…B rb=…B rbinflight=…`
- vlog(1): lifecycle and decisions, but no per-draw spam.
  - Begin: `recordVulkanBatchesInActiveFrame('label') activeSurface colors=N depth=bool`.
  - End (aggregated): `pass_end pass='label' cpu=… ms draws=… segs=… clr=… ld=… dsets=… ovsets=… pipes_bound_delta=… dwr=… rew=… uploads_delta=…B static_delta=…B rb_delta=…B rbinflight_delta=… transitions=… noop=…`.
- vlog(2): deep internals (per-draw, per-descriptor, per-layout, allocations). Prefer gating behind `VLOG_IS_ON(2)` and skip unchanged/no-op cases.

Renderer Base surface logs (vlog(1))
- When the renderer sets or preserves the active surface, a short line is emitted with counts and load/store policies.
  - `activeSurface set: colors=N depth=bool colorLoad=Load colorStore=Store depthLoad=Clear depthStore=Store`
  - `activeSurface preserved: colors=N depth=bool`

Runtime Flags and Config Flagfile

- Atlas supports runtime configuration via a gflags-compatible flagfile. At startup, if present in the user config directory, Atlas reads `user_settings_flagfile.txt` (created from the template) and applies the flags for that session.
- Template path (in repo): `src/atlas/settings_flagfile.txt`. This file enumerates user-facing flags with short comments and their default values.
- Users create and edit their local copy via the UI:
  - Help → Generate Config File — copies the template into the config directory as `user_settings_flagfile.txt`.
  - Help → Open Config Folder — opens the directory so the user can edit the file in a text editor.
- File format is standard gflags:
  - One flag per line, `--name=value`.
  - `#` begins a comment; blank lines are allowed.
  - Booleans use `true/false`; numeric flags use integers or decimals as appropriate.
- Apply on restart: changes take effect the next time Atlas starts. Advise users to check startup logs for any flag parse errors.

Adding or updating flags for users

- Prefer exposing options that are safe to tweak without recompiling: performance limits, memory sizing, debug toggles, rendering heuristics that don’t alter file formats or scene serialization.
- When you add a new gflag intended for users:
  - Define the flag in code with a sensible default and a clear description.
  - Add it to `src/atlas/settings_flagfile.txt` with a brief, user-friendly comment. The default shown in the template must match the compiled default.
  - Keep naming consistent with existing prefixes: `atlas_*` for app/platform/runtime behavior, `zimg_*` for image/FFT stack, `atlas_debug_vulkan` for Vulkan.
  - Group related flags and avoid leaking internal or unsafe toggles (e.g., experimental invariants, crash-on-warning). If a flag is debug-only, make that clear in its comment.
  - Update documentation: briefly mention new user-togglable flags in `docs/USER_GUIDE.md` (configuration section) if they are likely useful to end users.
- Do not introduce telemetry or logging that could leak user data. Follow the security/privacy guidance in AGENTS.md.

Common examples in the template

- `--atlas_image_cache_memory_proportion` and `--atlas_image_region_cache_memory_proportion` — tune memory usage.
- `--atlas_debug_opengl` / `--atlas_debug_vulkan` — enable GL/Vulkan debugging aids.
- `--atlas_volume_rendering_maximum_round` — raise/lower ray-march rounds.
- `--v` — set global log verbosity.

Render Batch Contract

- Neutral batches: renderers enqueue backend‑neutral `RenderBatch` structs that describe state, bindings, and geometry.
- Viewport is authoritative: `BackendPassDesc::viewport` is the single source of truth for render area and depth range. The former `BackendPassDesc::extent` field has been removed.
- Attachments and images:
  - Framebuffer attachments are passed via `AttachmentHandle` (backend + opaque id).
  - Sampled images are passed via `SampledImageHandle` (backend + opaque id) or CPU pixels where explicitly supported (e.g., fonts).
- No GL pointers in payloads: payloads no longer carry `Z3DTexture*`. GL renderers still use GL textures internally for the OpenGL path, but Vulkan paths consume only handles.

Debug/Release Builds

- Define `ATLAS_DEBUG_VERSION` at compile time to enable extra diagnostics for invalidation attribution.
- Run with `--v=1` (or set env `GLOG_v=1`) to see `VLOG` output.

Adding a New 3D Object View

1) Create a `Z3DObjView` subclass (e.g., `Z3DNewTypeView`) for your document type; implement `hasObj`, `boundBoxOfObj`, `read/write(view JSON)`, and `viewSettingWidgetsGroupOf`.
2) Instantiate it in `Z3DRenderingEngine::init()` (or similar factory) and connect its signals to engine (`objViewReady`).
3) Ensure the view registers its filters with the engine (via `Z3DFilterView::filters()`/`updatePipeline()`), so the compositor sees them in its geometry/volume lists.
4) Ensure the view respects visibility/selection and updates the engine bound box.
5) Extend scene (de)serialization in the view for per-object 3D JSON.

Coding Guidelines

- Follow existing style (Qt, modern C++17/20).
- Keep UI and engine responsibilities separated; prefer posted calls over shared state.
- Prefer plain data transfer (ids, JSON, POD) across threads.
- When in doubt about a cross-thread call, add a `thread()` equality check and use `invokeMethod` accordingly.

Testing and Diagnostics

- Prefer small, focused tests for views/components (where available).
- Use verbose logging (`--v=1`) to trace rendering progress and apply order.
- For headless animation export, try small sizes first, then scale up with `--output_tile_size` and `--output_tile_border`.
Image Rendering Pipeline
Invariant Checks and GL Parity (Vulkan)

- Principle: Vulkan paths should match OpenGL renderer behavior for benign skips, and crash fast on invariants that “should never happen”. This helps surface pipeline issues early and keeps parity with long‑standing GL semantics.
- GL‑parity early returns (no crash):
  - Empty payloads (no geometry, no image, zero vertices/indices).
  - Picking passes without picking colors when GL also skips (e.g., fonts, lines, spheres, cones, ellipsoids).
  - Transient/paged resources not yet ready (e.g., paged volume textures) where GL progressively renders — Vulkan skips that draw safely.
- Invariant failures (CHECK crash):
  - Null `renderer` in non‑empty payloads.
  - Size mismatches between parallel arrays (positions/texcoords/colors/pickingColors, etc.).
  - Required descriptor set layouts/descriptor sets/buffers fail to allocate or are missing at record time.
  - Resolve/composite format contracts (e.g., WA/WB resolves require exactly one color attachment and no depth).
- Debug‑only guards (DCHECK):
  - Index ranges derived from CPU tensors.
  - Sanity assertions inside hot loops that are too expensive for release.

Notes
- OOM or external resource exhaustion may trip CHECKs: we treat these as fatal in Vulkan paths to avoid silent rendering fallthrough. If a path is expected to be optional/transient, prefer a guarded early return and document the GL parity.
- Per‑draw override descriptor sets are used to avoid update‑after‑bind hazards. Allocation failures are fatal (CHECK) because they indicate broken per‑frame descriptor arenas.

- Overview
  - 3D images are rendered via `Z3DImgFilter`, which hosts two renderer paths:
    - `Z3DImgRaycasterRenderer` for volumes and 2D images (depth==1) with transfer functions.
    - `Z3DImgSliceRenderer` for explicit plane slices with colormaps.
  - The filter outputs multiple renderings (`Image`, `OpaqueImage`, stereo variants) consumed by the compositor.
  - Internally, the compositor consumes image filters via engine-managed `Z3DImgFilter*` lists.
  - Vulkan orchestration: `Z3DImgFilter::process()` builds a per-eye `ZVulkanLinearScript` and records three nodes (raycaster → bound-box overlay → slice) targeting the filter’s own persistent scratch leases. This centralizes Vulkan submission boundaries like the compositor, enabling future backend/script optimization without changing filter call sites.

- Fast vs Full-Resolution
  - On load, large volumes are downsampled to fit GPU constraints. `Z3DImg::isVolumeDownsampled()` indicates this.
  - The UI toggle “Full Resolution Rendering” switches the filter/renderer into a progressive mode that:
    1) Renders a fast result first (downsampled path) for instant feedback.
    2) Iteratively fills a GPU cache of full-res blocks and accumulates a refined image across rounds per channel.
  - Progressive state and cache uploads are cancellation-aware through `Z3DGlobalParameters::cancellationSource`.

- Entry/Exit and Ray Setup
  - For 3D raycasting, the front/back faces of the clipped volume are rendered with `Z3DTextureAndEyeCoordinateRenderer` into a 2-layer 2D array texture (entry at layer 0, exit at layer 1). This is managed by `Z3DImgRaycasterRenderer::prepareEntryExit()`.
  - That entry/exit texture is sampled by `image3d_raycaster*.frag` to integrate along the ray per pixel.
  - To reduce GL object churn, temporary FBOs/textures are leased from `Z3DScratchResourcePool`.
  - Important: we retain the entry/exit lease until the next `prepareEntryExit()` call; see “Alias correctness” below.

- Block-ID pass and Full-Res Cache
  - When full-res is active (`!fast && isVolumeDownsampled()`), the renderer:
    - Renders a “block-id” pass (`image3d_raycaster_blockID.frag`) to discover which cache blocks are needed along the rays for the current view.
    - Downloads per-pixel block IDs to CPU, compacts/deduplicates them, and asks `Z3DImg::updateAndUploadPageDirectoryCaches()` to read/upload those blocks.
    - Then renders per-channel into a layer-array RT; multi-channel results are merged via `image2d_array_compositor.frag`.
  - `Z3DImg` owns:
    - Page directory and page table cache textures (3D integer textures) per channel.
    - The image block cache texture per channel (3D texture storing block bricks).
    - Mapping logic (`m_levelScales`, `m_posToBlockIDs`, `m_pageTableBlockSize`, etc.).

- Progressive Accumulation
  - Raycaster maintains per-eye `m_channelIdx[eye]` and `m_round[eye]` and persistent `m_progressiveLayerLease` across rounds.
  - Each round renders a subset of rays/blocks; after enough rounds or when all requested blocks are uploaded, the channel is complete.
  - Depth/color ping-pong RTs (`m_lastImageRenderTargets`/`m_currentImageRenderTargets`) support iterative integration.
  - Vulkan: channel/round progression is advanced by backend finalization callbacks. After recording work in `ZVulkanImgRaycasterPipelineContext` / `ZVulkanImgSlicePipelineContext`, the backend consumes `takePendingFinalization()` and calls `finalizeImgRaycasterRoundByKey(...)` / `finalizeImgSliceRoundByKey(...)` on the originating renderer.

- Aliases and Correctness
  - `ZImgDoc::makeAlias(id)` returns a new ID pointing to the same `ZImgPack` (shared backing data). Each 3D alias gets its own `Z3DImgFilter` with independent transforms and parameters.
  - Shared scratch resources (entry/exit RTs, layer-array RTs) must not be reused across aliases within the same frame while GPU work may still reference them.
  - Fix in `Z3DImgRaycasterRenderer`:
    - We release any old entry/exit lease at the start of `prepareEntryExit()`.
    - We no longer release the entry/exit lease at the end of a draw; it stays alive until the next `prepareEntryExit()` (move-assign releases the previous lease). This prevents the pool from handing the same underlying texture to another alias in the same pass, which previously caused a visible misplacement when mixing full-res and fast paths.

- Slices Path
  - `Z3DImgSliceRenderer` renders plane geometry with 3D texture coordinates and merges channels; full-res block-ID/voxel cache logic mirrors the raycaster’s but for slice geometry.
  - Vulkan slice backend uses GPU block-ID compaction (see `block_id_compact_*` shaders) and a raycaster-style progressive schedule:
    - First present records a fast preview (progress=0.5).
    - Missing-block discovery is recorded into the active frame command buffer; CPU parsing + `Z3DImg::updateAndUploadPageDirectoryCaches()` runs via a post-frame fence callback.
    - Full-res paging then refines one channel per frame (VRAM-friendly) while preserving other channel layers; channels are merged each frame.
    - The progressive path avoids `executeImmediate()` (no per-slice/per-channel submit+wait loops).

- Compositor Integration
  - Filters render into their own RTs per eye and then copy/blend into the compositing chain using `Z3DTextureCopyRenderer`.
  - Bound boxes are drawn as local overlay with depth test and alpha blend after the main pass.

User-Facing Behavior (summary)

- Toggling full-res on a downsampled volume triggers a quick preview followed by progressive refinement. Disabling full-res reverts to fast rendering.
- Aliases of the same image share memory but keep independent view settings and transforms. The Atlas agent can create aliases via the Scene RPC `MakeAlias` (tool: `scene_make_alias`) to stage multiple alternative views of the same backing data.

- Invalidation and Progressive Reset Policy

- Invalidation: Filters are invalidated by parameter changes, by global camera/viewport changes, and when the engine applies a new compositor output size.
  - `Z3DBoundedFilter` connects camera changes (see `z3dboundedfilter.cpp`) and calls `invalidateResult()` which marks outputs invalid.
  - `updateSize(targetSize)` on any filter is called by the engine when the compositor output size changes and ends with `invalidate(AllResultInvalid)` by default. Filters that need explicit sizing (e.g., `Z3DImgFilter`) override this to use `targetSize` as their desired render resolution.
- Cancellation-first policy (centralized in `Z3DImgFilter::invalidate`):
  - On invalidation, the image filter requests cancellation via `globalParas().cancellationSource->requestCancellation()` if a render is in progress.
  - Each `Z3DImgFilter` also sets a small internal flag so it can ask its renderers to reset at the start of the next `process()` call (a safe point). Renderers expose reset as an internal operation (friend access) — it is not part of the public API.
  - Renderers also periodically check the token and may throw a cancellation exception during long passes; they perform their own safe reset in the catch paths. Together, this guarantees a clean progressive restart across all image filters without mutating state mid-pass.
  - When orchestration is expressed via `ZVulkanLinearScript` (compositor/image filters), call sites must flush explicitly (`script.flush(...)`) so cancellation propagates; do not rely on destructor flush.

Scratch Resource Pool (`Z3DScratchResourcePool`)

- Purpose: reuse heavy render targets (block-id FBOs with multiple integer attachments, entry/exit 2D arrays, layer arrays, temp 2D FBOs) across passes and filters.
- Leases: move-only RAII (`RenderTargetLease`) that marks a slot in-use until released or destroyed; each lease records the producing `ScratchImageDescriptor` plus the backend (`RenderBackend`) so renderers can branch between `glRenderTarget()` and `vulkanScratchImage()` as the façade evolves.
- Growth: slots grow to match requested size/attachments; they don’t shrink until `trim()`.
- Debugging/memory: `describeMemoryUsage(detailed)` returns a breakdown; counters `creationCounter()` and `changeCounter()` help detect churn.
- Best practices:
  - Acquire–use–release within the same frame.
  - Prefer release-before-acquire when you know you’re about to re-request the same category to avoid transient duplication.

Transparency Methods

- Geometry transparency:
  - Blend No Depth Mask / Blend Delayed (dual FBO passes)
  - Dual Depth Peeling (multiple layers with depth/alpha peeling)
    - Vulkan has two implementations for DDP-quality transparency: classic multi-pass DDP and exact OIT via a per-pixel fragment list (PPLL).
      The selection is controlled by the `Transparency` parameter (`Dual Depth Peeling` vs `Per-Pixel Fragment List (PPLL Exact)`).
  - Weighted Average and Weighted Blended (OIT approximations)
- Images are blended via `Z3DTextureBlendRenderer` and the compositor’s merge shaders; image layers from multiple filters are collected/merged consistently.

Stereo and Screenshots

- Stereo: left/right eyes rendered separately; compositor holds per-eye ready/current targets.
- Screenshots: single shot uses current canvas size; tiled output computes normalized left/right/bottom/top and sets tile frustum on `Z3DCameraParameter` and compositor region, then composites tiles to an image (mono or stereo).

OpenGL Context and Shaders

- Context: offscreen `QOffscreenSurface` + `Z3DContext`; `glbinding` used for function resolution and optional debug callbacks.
- Shader headers: `Z3DRendererBase::generateHeader()` injects `#version`, fog, clip planes, light count; renderers add feature defines (NUM_VOLUMES, MAX_PROJ_MERGE, ISO, etc.).

Filter Wiring and Parameters

- Pipeline: `Z3DRenderingEngine` owns a linear pipeline of filters and a single `Z3DCompositor` at the end; it also tracks the current geometry and volume filters and exposes them to the compositor.
- Invalidation: filters emit `Z3DFilter::invalidated()` when their state changes; 3D views connect this signal directly to `Z3DCompositor::invalidateResult()` so only the compositor is invalidated, not other filters.
- Parameters: `ZParameter` subclasses emit `valueChanged`; `Z3DFilter::addParameter` wires them to `invalidateResult()` by default.
- WidgetsGroup: `ZWidgetsGroup` trees drive UI construction and change notifications; engine watches these groups to emit view-setting change signals.

Aliases

- Docs (e.g., `ZImgDoc::makeAlias`) create a new id referencing the same backing pack; views/filters are instantiated per-id, so transforms and rendering parameters are independent.
- Rendering state (progressive caches) remains per-filter; scratch resources are shared via the pool but guarded by lease lifetime to prevent cross-alias reuse bugs.

Performance Tips (dev)

- Avoid reallocating FBOs mid-frame — stick to pool leases.
- Keep `m_outputSize` consistent across renderers that collaborate in a pass.
- Use `--v=1` to sample stage timings; wrap expensive sections with `ZBenchTimer`.
- For very large volumes, tune `atlas_image_block_size` and sampling rates; avoid over-aggressive sampling in DVR.

Vulkan Raycaster (paging) debug flag

- To rule out descriptor persistence/lifetime issues for the Page UBO, you can force per-draw override binding:
  - `--atlas_vk_force_page_ubo_override=true` — always bind the Page UBO via an override descriptor (no persistent set). Default is false.

Additional Architecture Notes

- Object/Pack/View separation
  - Documents own object lifecycles and actions; packs back data; views/filters encapsulate render logic and parameters.
  - Aliases share packs only; everything above the pack (parameters, transforms, selections) is per-ID.

- Frame orchestration
  - Rendering thread drives a loop of: size propagation → invalidation → progressive processing → compositor blend.
  - Engine exposes `renderFast()` (single progressive step) and `render()` (loop until complete or canceled).

- Device Pixel Ratio (HiDPI)
  - `Z3DGlobalParameters::devicePixelRatio` feeds into pixel-to-eye conversion (e.g., `ze_to_screen_pixel_voxel_size`) to keep sampling consistent across displays.

- Debugging GL state
  - Enable `--atlas_debug_opengl` for per-call error checks (costly); `--atlas_log_glbinding_context_switch` to audit context switches.
  - When diagnosing rendering differences across devices, log `Z3DGpuInfo::instance().logGpuInfo()` output for driver/features.
  - GPU caps source: when the Vulkan backend is active, `Z3DGpuInfo` populates generic limits (max 2D/3D texture size, array
    layers, anisotropy, approximate VRAM) from the selected Vulkan physical device. OpenGL-specific strings and flags in that
    log are only meaningful under the OpenGL backend.
  - For Vulkan-specific details (device name, driver, limits, features, and extensions), call `ZVulkanContext::logGpuInfo()`.
    This logs a concise summary at INFO and full feature/extension detail at `--v=1`.

Vulkan device selection

- On initialization, all physical devices are enumerated and logged. Devices are sorted by preference: larger dedicated VRAM
  first, then discrete > integrated > virtual > CPU, then higher API version. The first suitable device (Vulkan 1.3,
  required extensions, required queue families) is selected and used to create the logical device and queues.
- `ZVulkanContext::physicalDevice()` returns the currently selected device. `deviceCount()` and `physicalDevice(index)` can be
  used for explicit per-device introspection. The selected index is exposed via `selectedDeviceIndex()`.
- Override device selection at startup via `--atlas_vk_device_index=N` (sorted order). When set to a suitable device index,
  the context selects that device instead of the auto-selected one.
- Runtime switching: call `Z3DRenderingEngine::switchVulkanDeviceIndex(N)` at a safe point (no in-flight rendering) to switch
  to device N. The engine waits idle, recreates the logical device wrapper, updates the scratch pool device, refreshes
  backend‑agnostic GPU caps, and logs the new device inventory.

Compositor Pass Graph (Vulkan)

- Offscreen only; no swapchain.
- Per frame, the Vulkan compositor executes a simple pass ordering: Background → Opaque Geometry → Transparency → Glow → Overlays.
- Background + geometry now record via a single driver (`executeCompositorPassesVulkan`), which reduces dynamic rendering begin/end churn by coalescing compatible batches.
- Image raycaster/slice are orchestrated by `Z3DImgFilter` via `ZVulkanLinearScript` and render into per-filter targets; they are intentionally kept outside the compositor pass graph (for now).
- Backend VLOG(1) counters help validate improvements: per-frame segments begun and attachments cleared vs loaded.
- Load/store policy: first writer to an attachment clears; subsequent writers load. The backend emits exactly one `beginRender`/`endRender` per frame; dynamic rendering segments only begin when attachment sets change.

Vulkan async readback (offscreen only)

- The compositor requests an end-of-frame GPU copy of the final color attachment into a host-visible staging buffer. The CPU reads the mapped memory after the frame fence signals (default 1-frame latency) and updates the BGRA8 local buffer for UI consumption.
- Flags:
- VLOG(1) includes `readback_bytes_copied` and `readback_slots_in_flight` to track throughput.
### RPC: Parameter metadata (ListParams/Capabilities)

- Parameter proto now carries a single authoritative `value_schema` (google.protobuf.Struct) describing the parameter value using JSON Schema conventions.
- Legacy numeric/vector/span fields have been removed: `number_min`, `number_max`, `number_step`, `decimal`, `vector_min`, `vector_max`, `span_min`, `span_max`, `span_step`.
- Legacy option fields have been removed: `option_names`, `option_data`. Enumerations should be represented via `enum` inside `value_schema`.
- Enumerated options remain (`option_names`, `option_data`), but enums should also appear in `value_schema` when applicable.
- The server populates `value_schema` via `ZParameter::valueSchema()` for all parameter types. Composite types (e.g., `3DTransform`, Camera) expose an object schema with canonical subfield names.
- Client guidance: consume `value_schema` for validation, UI hints (ranges/steps), and LLM guidance. Do not infer constraints from type strings.
- Scene.Capabilities now includes a `camera` field that lists the camera (id=0) parameters alongside `background`, `axis`, and `global` so clients/agents can discover camera schema via the same endpoint.
 - Camera validation API: `CameraValidate` accepts either:
   - paired `times[]` and `values[]` (lengths may differ; pairs are matched up to `min(len(times), len(values))`), or
   - only `times[]` with an empty `values[]`, in which case the server samples camera values from the current animation’s camera track at those times (falls back to the current camera when no animation exists).

Agent tooling dependency
- The Python agent validates parameter values with `jsonschema` (Draft 2020‑12 or Draft‑7). The CLI enforces this dependency and exits if it is missing. Install via `pip install jsonschema`.
### RPC Scope IDs and Animation Binding

- Scope IDs used by RPC parameter/timeline operations:
  - 0 = Camera
  - 1 = Background
  - 2 = Axis
  - 3 = Global/Lighting
  - ≥4 = Scene object ids
- Implementation notes:
  - These ids are defined in `src/atlas/zrpcsceneids.h` as constants (`kZRpcScopeCamera`, `kZRpcScopeBackground`, `kZRpcScopeAxis`, `kZRpcScopeGlobal`). Avoid magic numbers.
  - Camera is treated as a parameter scope (id=0) for RPC and live introspection. It is NOT represented as an `AnimationObj` entry; camera keys live in the dedicated camera track (`m_globalParaAnimations`).
  - `ValidateSceneParams` and `ApplySceneParams` require a non-empty `json_key` for all scopes, including camera (id=0). Clients should discover the canonical camera parameter key via `Capabilities`/`ListParams` (typically `"Camera 3DCamera"`).
  - In the animation model, `AnimationObj.boundId == 0` remains a sentinel for “unbound/non‑scene” and is unrelated to the camera scope.
  - Do not add an `AnimationObj` for id=0; timeline ops for camera continue to use the camera track APIs.

LLVM/Clang versioning

- Linux builds use Clang by default. The version can be overridden with env `ATLAS_CLANG_MAJOR` (e.g., `20`).
