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
 - Post‑fence callbacks (e.g., Block‑ID compaction parsing) are drained as soon as the submission fence signals, independent of the descriptor‑arena reset. This avoids an extra frame of latency when `maxFramesInFlight > 1` and keeps progressive channel bookkeeping in lockstep (advance/skip decisions are applied before the next record).

Threading Model

- UI thread: widgets (`Z3DCanvas`, main window), menu actions, docks, drag-and-drop.
- Rendering thread: all engine code, rendering parameters, compositor, and object views.
- Cross-thread rules:
  - Do not manipulate engine or parameter QObjects directly from UI.
  - Use `QMetaObject::invokeMethod` to post to engine thread; use `Qt::BlockingQueuedConnection` if you must wait.
  - For parameter changes, queue to the parameter’s owning thread (see `ZParameterAnimation::setCurrentTime`).

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
- `Z3DNetworkEvaluator` executes the filter graph and drives progressive updates.
- `Z3DGlobalParameters` holds camera, lights, fog, global cuts, device pixel ratio, and scratch resource pool.

Vulkan Notes

- Backend selection is a session-level switch. GL remains supported; Vulkan is the preferred backend for parity.
- Renderers expose backend‑neutral batch data via `enqueueRenderBatches`; Vulkan records via the explicit entry points in `Z3DRendererBase` (no implicit frame begin/end).
- Per-eye `Z3DScratchResourcePool` leases stay with each filter. Vulkan dynamic rendering targets are expressed via `RendererFrameState::ActiveSurface` and set with `setActiveSurfaceWithLoadStore(...)` at the call site.
- Keep renderer parameters persistent at the filter; renderer objects hold transient GPU resources only.
- Naming convention: cross‑backend code uses `Z3D*`; Vulkan-only uses `ZVulkan*`.

Vulkan Pipeline Invariants

- Dynamic rendering is used; a new segment is begun only when attachment sets change.
- Graphics pipeline keys include attachment formats: `colorFormats[]` and optional `depthFormat` are part of the key in all Vulkan pipeline contexts to avoid layout mismatches.
- Composite/resolve passes (DDP final, WA resolve, WB resolve) must write to exactly one color attachment; depth is disabled in the pipeline and no depth attachment is bound.
- Descriptor updates after binding are forbidden. Per‑draw overrides are allocated from the backend’s per‑frame arena and kept alive until the frame fence to satisfy validation rules.
- Backend validates that the pipeline’s attachment formats match the currently active dynamic rendering segment; mismatches are logged at VLOG(1) and the batch is skipped.

Performance Instrumentation

- Aggregated frame timing: the rendering engine emits a monotonically increasing token per user‑visible frame (one `Z3DNetworkEvaluator::process()` call). The Vulkan backend tags each submission with this token and a submission index.
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
- Vulkan logging hygiene:
  - No-op sampled-read transitions are suppressed: `ensureSampledReadable` only logs when a layout change occurs.
  - Dynamic rendering ‘begin’ logs always carry a non-empty label; empty pass labels fall back to `<unnamed>`.
  - `TextureCopy` OIT UBO priming (`set=3,binding=0`) logs at most once per frame per pipeline context.
  - Immediate execution of deferred callbacks (no active frame) is logged at `VLOG(2)` instead of `VLOG(1)`.
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
