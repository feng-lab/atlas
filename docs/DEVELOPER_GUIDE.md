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
- Filters continue to own their GL renderers. When we add Vulkan support, expose lightweight translation helpers instead of pushing façade-only abstractions into the filters.
- Render-surface façade work is paused. Per-eye `Z3DRenderTarget` leases stay with each filter, and consumers pull textures via existing helper accessors (see `docs/RENDER_SURFACE_PORTS.md` for historical notes).
- Several `ZParameter` instances still live inside renderers (GL only). As Vulkan coverage expands, audit each renderer, move persistent parameter state to its owning filter (or a shared bundle), and keep only transient GPU resources inside the renderer so backend resets don’t drop user-facing state.
- Naming convention: 3D/shared classes use the `Z3D` prefix (e.g., `Z3DImgFilter`, `Z3DRenderSurfaceOutputPort`), while Vulkan-specific helpers use the `ZVulkan` prefix (e.g., `ZVulkanLinePipelineContext`). Keep new files aligned with this scheme for clarity across backends.
- Renderer subclasses still implement `createResources(RenderBackend backend)` and must guard against unsupported APIs (current GL implementations simply return when `backend != RenderBackend::OpenGL`). This keeps future backend transitions from instantiating GL shaders/VAOs during staging.

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
