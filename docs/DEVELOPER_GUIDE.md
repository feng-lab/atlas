Atlas Developer Guide

Build, Run, and Layout

- Build instructions: see `readme.md` (macOS/Linux/Windows, Qt 6.9.x, Intel oneAPI, Vulkan SDK, Ninja, Conda recipe for `zimg`).
- Source layout (selected):
  - `src/atlas/` — application code (UI, engine, docs, filters, views)
  - `src/img/` — image I/O/processing utilities
  - `docs/` — documentation
  - `util/` — build scripts

Architecture Overview

- Main window (`ZMainWindow`) — 2D UI; hosts object manager, docks, menus.
- 3D window (`Z3DMainWindow`) — spawns a rendering thread and owns a `Z3DRenderingEngine` (moved to the rendering thread), and a `Z3DCanvas` on the UI thread.
- Rendering engine (`Z3DRenderingEngine`) — owns the offscreen GL context, global parameters, compositor, network evaluator, and per-object 3D views.
- Parameter system (`ZParameter` + subclasses) — typed, QObject-based, with signals/slots and JSON (de)serialization.

Lookup Tables (LUTs)

- Colormaps (`ZColorMap`) and transfer functions (`Z3DTransferFunction`) are CPU-only. They expose:
  - `buildLUTBGRA8(width)` to generate an RGBA8 LUT in CPU memory.
  - `generation()` that increments on change for cache invalidation.
- Renderers/pipeline contexts create and cache backend LUT textures:
  - OpenGL: per-renderer 1D RGBA8 textures for colormaps/transfer functions.
- Vulkan: pipeline contexts create 1D `ZVulkanTexture`s via a small helper; LUTs are uploaded as RGBA8 and bound through descriptor sets.
  - Vulkan descriptor arena (Stage 2): pipeline contexts must allocate descriptor sets from the backend’s per-frame arena via `Z3DRendererVulkanBackend::allocateFrameDescriptorSet(layout)`. Do not create per-context descriptor pools. The arena is reset once per frame (scheduled in `endRender()`, applied on the next `beginRender()` after the frame fence signals).
  - Scratch-pool recycling: Vulkan scratch image leases are released only after the submitting frame’s fence signals. The pool defers slot reuse via a callback provided by the backend each frame.
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

Threading Model

- UI thread: widgets (`Z3DCanvas`, main window), menu actions, docks, drag-and-drop.
- Rendering thread: all engine code, rendering parameters, compositor, and object views.
- Cross-thread rules:
  - Do not manipulate engine or parameter QObjects directly from UI.
  - Use `QMetaObject::invokeMethod` to post to engine thread; use `Qt::BlockingQueuedConnection` if you must wait.
  - For parameter changes, queue to the parameter’s owning thread (see `ZParameterAnimation::setCurrentTime`).

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
- `Z3DNetworkEvaluator` executes the filter graph and drives progressive updates.
- `Z3DGlobalParameters` holds camera, lights, fog, global cuts, device pixel ratio, and scratch resource pool.

Vulkan Migration Snapshots

- Detailed migration backlog now lives in `docs/VULKAN_MIGRATION_PLAN.md`. Treat it as the canonical task list for Vulkan parity and update it whenever plans change.
- Backend selection remains a session-level switch. GL stays the default renderer while the Vulkan translation layer comes online; runtime hot-swapping will be revisited once the translators cover the major primitives.
- Filters continue to own their GL renderers. When we add Vulkan support, renderers expose backend‑neutral batch data via `enqueueRenderBatches` which the Vulkan path consumes. We do not refactor GL draw paths; instead we provide explicit Vulkan entry points in `Z3DRendererBase` that collect batches only (no implicit frame begin/end).
- Render-surface façade work is paused. Per-eye `Z3DRenderTarget` leases stay with each filter, and consumers pull textures via existing helper accessors. Vulkan dynamic rendering targets are bound via `RendererFrameState::ActiveSurface` and are applied before recording through explicit pass helpers.
- Several `ZParameter` instances still live inside renderers (GL only). As Vulkan coverage expands, audit each renderer, move persistent parameter state to its owning filter (or a shared bundle), and keep only transient GPU resources inside the renderer so backend resets don’t drop user-facing state.
- Naming convention: 3D/shared classes use the `Z3D` prefix (e.g., `Z3DImgFilter`, `Z3DRenderSurfaceOutputPort`), while Vulkan-specific helpers use the `ZVulkan` prefix (e.g., `ZVulkanLinePipelineContext`). Keep new files aligned with this scheme for clarity across backends.
- Renderer subclasses still implement `createResources(RenderBackend backend)` and must guard against unsupported APIs (current GL implementations simply return when `backend != RenderBackend::OpenGL`). This keeps future backend transitions from instantiating GL shaders/VAOs during staging.

Vulkan Pipeline Invariants

- Dynamic rendering is used; a new segment is begun only when attachment sets change.
- Graphics pipeline keys include attachment formats: `colorFormats[]` and optional `depthFormat` are part of the key in all Vulkan pipeline contexts to avoid layout mismatches.
- Composite/resolve passes (DDP final, WA resolve, WB resolve) must write to exactly one color attachment; depth is disabled in the pipeline and no depth attachment is bound.
- Descriptor updates after binding are forbidden. Per‑draw overrides are allocated from the backend’s per‑frame arena and kept alive until the frame fence to satisfy validation rules.
- Backend validates that the pipeline’s attachment formats match the currently active dynamic rendering segment; mismatches are logged at VLOG(1) and the batch is skipped.

Vulkan Entry Points (explicit)

- OpenGL entry points remain `render(...)` and `renderPicking(...)`. These drive the GL path and may begin/end GL frames as needed.
- Vulkan uses explicit, collection‑only entry points in `Z3DRendererBase`:
  - `recordVulkanBatches(fn, label)` / `recordVulkanPass(surfaceOrLease, fn, label)`: apply pending surfaces, begin/end the Vulkan frame if needed, set a GPU scope label, run `fn`, and submit.
  - `renderVulkan(eye, ...)` / `renderPickingVulkan(eye, ...)`: enqueue backend‑neutral batches only. These assert that the backend is Vulkan and `collectOnly==true`.
- Invariants:
  - For Vulkan, call `renderVulkan`/`renderPickingVulkan` only inside a `recordVulkanBatches`/`recordVulkanPass` block with `collectOnly=true`.
  - A valid active surface must be set before the first append in a recording session, or the first batch must carry attachments explicitly. Violations cause a CHECK and include the pass label.

Example (pseudocode)

```
renderer.setCollectOnly(true);
renderer.setActiveSurfaceForNextPass(lease);
renderer.recordVulkanPass(lease, [&](){
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
    - Fragment stage; see `Resources/shader/vulkan/include/lighting.glslinc`
  - Set 2 — Transforms/Material UBOs (std140):
    - Binding 0: `Transforms` (proj/view matrices, `pos_transform`, normal matrix, `parameters.x=sizeScale`)
    - Binding 1: `MaterialUBO` (sceneAmbient, materialAmbient/specular/shininess, alpha, optional customColor)
    - Vertex + fragment stages; see `Resources/shader/vulkan/include/matrices_material.glslinc`
  - Set 3 — OIT Params UBO (std140):
    - Binding 0: `OITParams` (`screen_dim_RCP`, `ze_to_zw_a/b`, `weighted_blended_depth_scale`)
    - Fragment stage; see `Resources/shader/vulkan/include/oit_params.glslinc`

Guidelines
- Allocate frame‑scoped descriptor sets from the backend arena; avoid per‑context pools.
- Do not rewrite persistent descriptors during recording; update UBO contents only.
- Use per‑draw override sets for volatile inputs (peel/resolve/composite images).
- Keep this set ordering consistent across contexts.

Invalidation & Progressive Rendering

- A `Z3DFilter` tracks invalidation bits (mono/left/right). When a bit is set, the network evaluator knows that eye needs processing.
- Causes of invalidation:
  - Parameter changes (wired in `Z3DFilter::addParameter`)
  - Upstream port invalidations (output → connected inputs)
  - Global camera/viewport changes
  - `updateSize()` (propagates sizes, then invalidates all results)
- Image filters request cancellation on invalidate and defer renderer resets to the next `process()` to avoid mutating state mid-pass.

Debug reason plumbing (debug builds only)

- `Z3DFilter` exposes `debugSetInvalidateReason` and `debugTakeInvalidateReason` (no-ops in release).
- `addParameter` tags a human-readable reason with a JSON snapshot; ports tag their own reasons on propagation.
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
  - In debug builds (`ATLAS_DEBUG_VERSION`), you’ll also see: “image filter invalidate: …” with parameter/global/port reasons and state bits.

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
3) Connect view outputs to compositor input ports.
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
  - The filter outputs multiple ports (`Image`, `OpaqueImage`, stereo variants) consumed by the compositor.

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

- Aliases and Correctness
  - `ZImgDoc::makeAlias(id)` returns a new ID pointing to the same `ZImgPack` (shared backing data). Each 3D alias gets its own `Z3DImgFilter` with independent transforms and parameters.
  - Shared scratch resources (entry/exit RTs, layer-array RTs) must not be reused across aliases within the same frame while GPU work may still reference them.
  - Fix in `Z3DImgRaycasterRenderer`:
    - We release any old entry/exit lease at the start of `prepareEntryExit()`.
    - We no longer release the entry/exit lease at the end of a draw; it stays alive until the next `prepareEntryExit()` (move-assign releases the previous lease). This prevents the pool from handing the same underlying texture to another alias in the same pass, which previously caused a visible misplacement when mixing full-res and fast paths.

- Slices Path
  - `Z3DImgSliceRenderer` renders plane geometry with 3D texture coordinates and merges channels; full-res block-ID/voxel cache logic mirrors the raycaster’s but for slice geometry.

- Compositor Integration
  - Filters render into their own RTs per eye and then copy/blend into the compositing chain using `Z3DTextureCopyRenderer`.
  - Bound boxes are drawn as local overlay with depth test and alpha blend after the main pass.

User-Facing Behavior (summary)

- Toggling full-res on a downsampled volume triggers a quick preview followed by progressive refinement. Disabling full-res reverts to fast rendering.
- Aliases of the same image share memory but keep independent view settings and transforms.

Invalidation and Progressive Reset Policy

- Invalidation: Filters are invalidated by upstream changes (ports/parameters) and by global camera/viewport changes.
  - `Z3DBoundedFilter` connects camera changes (see `z3dboundedfilter.cpp`) and calls `invalidateResult()` which marks outputs invalid.
  - `updateSize()` on any filter propagates expected sizes and ends with `invalidate(AllResultInvalid)`.
- Cancellation-first policy (centralized in `Z3DImgFilter::invalidate`):
  - On invalidation, the image filter requests cancellation via `globalParas().cancellationSource->requestCancellation()` if a render is in progress.
  - Each `Z3DImgFilter` also sets a small internal flag so it can ask its renderers to reset at the start of the next `process()` call (a safe point). Renderers expose reset as an internal operation (friend access) — it is not part of the public API.
  - Renderers also periodically check the token and may throw a cancellation exception during long passes; they perform their own safe reset in the catch paths. Together, this guarantees a clean progressive restart across all image filters without mutating state mid-pass.

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
  - Weighted Average and Weighted Blended (OIT approximations)
- Images are blended via `Z3DTextureBlendRenderer` and the compositor’s merge shaders; image layers from multiple filters are collected/merged consistently.

Stereo and Screenshots

- Stereo: left/right eyes rendered separately; compositor holds per-eye ready/current targets.
- Screenshots: single shot uses current canvas size; tiled output computes normalized left/right/bottom/top and sets tile frustum on `Z3DCameraParameter` and compositor region, then composites tiles to an image (mono or stereo).

OpenGL Context and Shaders

- Context: offscreen `QOffscreenSurface` + `Z3DContext`; `glbinding` used for function resolution and optional debug callbacks.
- Shader headers: `Z3DRendererBase::generateHeader()` injects `#version`, fog, clip planes, light count; renderers add feature defines (NUM_VOLUMES, MAX_PROJ_MERGE, ISO, etc.).

Ports and Parameters

- Ports: `Z3DInputPortBase`/`Z3DOutputPortBase` connect filters; invalidation propagates downstream from outputs to connected inputs.
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

Compositor Pass Graph (Vulkan)

- Offscreen only; no swapchain.
- Per frame, the Vulkan compositor executes a simple pass ordering: Background → Opaque Geometry → Transparency → Glow → Overlays.
- Background + geometry now record via a single driver (`executeCompositorPassesVulkan`), which reduces dynamic rendering begin/end churn by coalescing compatible batches.
- Some pipeline contexts (image slice/raycast and glow) manage their own dynamic rendering segments today and will be folded into the graph later.
- Backend VLOG(1) counters help validate improvements: per-frame segments begun and attachments cleared vs loaded.
- Load/store policy: first writer to an attachment clears; subsequent writers load. The backend emits exactly one `beginRender`/`endRender` per frame; dynamic rendering segments only begin when attachment sets change.

Vulkan async readback (offscreen only)

- The compositor requests an end-of-frame GPU copy of the final color attachment into a host-visible staging buffer. The CPU reads the mapped memory after the frame fence signals (default 1-frame latency) and updates the BGRA8 local buffer for UI consumption.
- Flags:
- VLOG(1) includes `readback_bytes_copied` and `readback_slots_in_flight` to track throughput.
