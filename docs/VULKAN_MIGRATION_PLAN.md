# Vulkan Migration Plan

This plan guides the migration of the Z3D OpenGL renderer to a Vulkan backend, in incremental, verifiable phases. It aims to reach feature parity with the current OpenGL implementation while keeping the application usable throughout.

## Goals

- Preserve the existing rendering feature set (camera, geometry, volumes, transparency, picking, screenshots, stereo) with Vulkan.
- Keep OpenGL running during migration; enable selecting backend for A/B comparison.
- Adopt explicit, robust resource management, validation, and repeatable shader compilation to SPIR-V.
- Maintain clean abstractions that mirror existing Z3D architecture to lower risk and churn.
- Historical note: legacy `z3dvolume*` classes are no longer in use; all image/volume rendering paths live under the `z3dimg*` families and should remain the focus during migration.
- Implementation language baseline is modern C++20; all new APIs should use standard library facilities (`std::span`, `std::variant`, etc.) rather than third-party helpers when equivalents exist.

### Development Notes (CRITICAL - ALWAYS FOLLOW)

These instructions are mandatory for every migration change; do not deviate from them.

- You can use `cmake --build build/Release` to verify compositor changes still compile before pushing them further.
- **Never run `git checkout`**; we might lose progress forever.

## Important Guideline (Review Before Coding)

> **Important:** Before writing new Vulkan code we must validate a cross-API graphics façade that both OpenGL and Vulkan can implement efficiently. The workflow is:
> - Refactor the existing rendering pipeline so scene filters talk only to an API-neutral command layer (no GL handles cross the boundary).
> - Stress-test every proposed interface and data contract against OpenGL’s state machine model and Vulkan’s explicit synchronization/descriptor requirements to ensure the design is feasible for both backends.
> - Confirm or adapt the current OpenGL implementation (via shims or refactors) to the new façade so we retain a working reference renderer while we build the Vulkan backend under the same contracts.
> - Treat the abstraction as the primary product: once it holds up for both APIs, implementing Vulkan becomes a mechanical backend task instead of a redesign.

## Parity And Swappable Backend

To ensure the Vulkan backend is a drop‑in replacement for the OpenGL engine (files starting with `z3d*`), we will:

- Preserve public APIs and parameter semantics of the OpenGL renderers when adding Vulkan counterparts.
- Provide adapter classes or matching method names on Vulkan renderers so existing UI, frontend, and configuration code do not change.
- Introduce a backend switch in `Z3DRenderingEngine` that selects OpenGL or Vulkan at runtime, without altering higher‑level code.

Implementation approach:

- Renderer API invariants: For each renderer family, define a short list of methods/parameters that must remain identical (names, value ranges, behavior). Vulkan renderers will expose the same signatures where feasible, or thin adapters will translate to Vulkan‑native settings.
- Parameter bridging: Reuse `Z3DGlobalParameters` as the single source of truth. Vulkan renderers consume the identical state (`camera`, fog, OIT, clip planes, `devicePixelRatio`, etc.) through `Z3DRendererBase` powered by a Vulkan backend implementation (`Z3DRendererVulkanBackend`), keeping GPU-specific code free of duplicate scene copies.
- Feature matching: When hardware features differ (e.g., wide lines), emulate in shaders/CPU so behavior stays consistent.


Renderer Base Façade (Revised)

- Filters continue to own their renderer members (e.g., `Z3DImgFilter::m_imgRaycasterRenderer`). Those renderers become API-neutral shells that store CPU-side state and interact with a backend driver; filters never touch GL types or Vulkan handles directly.
- `Z3DRendererBase` acts as the single façade. It holds a `std::unique_ptr<Z3DRendererBackend>` and orchestrates the primitive drivers that issue API calls. Swapping backends means asking the renderer base to tear down its current driver, create a new one for the requested backend, and replay cached CPU state.
- Backend drivers expose narrowly scoped hooks (`createPipelines`, `uploadBuffers`, `issueDraw`, `blit`, `readback`) that both OpenGL and Vulkan can implement efficiently. We keep the naming convention: OpenGL sources stay prefixed `z3d`, Vulkan counterparts `zvulkan`.
- Filters call the same renderer methods as today (`renderOpaque`, `renderTransparent`, `setData`, `setSliceState`, …). Prior to issuing work they call `renderer.ensureBackend(activeBackend)`, letting the renderer base perform backend switches without introducing extra per-filter glue objects.

**Render Target & Scratch Pool Abstraction**

- Refactor `Z3DRenderTarget` into an API-neutral interface that exposes only cross-backend operations (size query, resize, clear, attachment access by index/role, readback helpers). The existing GL implementation moves to `Z3DRenderTargetGL`; the Vulkan path introduces `ZVulkanRenderTarget` with matching behaviour.
- Attachment access switches from raw GL enums to small descriptors (`enum class AttachmentRole { Color0, Color1, Depth, Feedback }`). Helpers on the interface expose typed accessors (`colorAttachment(size_t index)`, `depthAttachment()`) returning opaque `RenderAttachmentView` handles that drivers understand.
- `Z3DScratchResourcePool` keeps the existing `RenderTargetLease` API but stores `std::unique_ptr<Z3DRenderTarget>` in its slots. A new `Z3DScratchResourcePoolBackend` interface creates/resizes targets per backend and maps semantic requests (opaque/transparent/picking). Backends attach their own GPU resources without leaking handles back to filters.
- Temporary 2D surfaces expose a neutral selector (`TempRenderTargetKind`) so callers can request the default render target or the picking-friendly variant without passing GL enums; each backend maps those kinds to its preferred formats.
- On backend switch, the engine calls `scratchPool.setBackend(createScratchBackend(targetBackend))`. The pool releases outstanding slots, adopts the new backend, and lazily recreates render targets on the next acquisition.

**FilterRenderContext & Utilities**

- `FilterRenderContext` accompanies each draw call. Instead of exposing raw attachment handles, it now carries the active `RenderTargetLease` plus backend utilities (`RenderUtilities`) for common operations (clear, blit, readback, unproject).
- `RenderUtilities` is implemented by the active backend and provides the small set of stateful actions filters/compositor need (depth unprojection, fullscreen blits, resolve helpers, readbacks). OpenGL implementations wrap existing helper renderers; Vulkan implementations record commands or schedule transfers.

**Renderer/Filter Interaction**

1. **Initialization** – filters construct their renderer members exactly as today. `Z3DRendererBase` lazily instantiates a GL backend driver so existing behaviour is preserved.
2. **Per-frame** – the compositor builds a `FilterRenderContext` for each draw pass. Filters forward it to their renderers, then call the usual render entry points. The renderer base hands the context to its backend driver, which binds/uses the leased render target, uploads dirty resources, and records draw calls.
3. **Backend Switch** – `Z3DRenderingEngine` broadcasts `switchBackend(RenderBackend)`. Each renderer base releases its current driver, creates the requested backend driver, and replays cached CPU state (vertex/index data, shader defines, texture metadata). Filters do not special-case the switch.
4. **CPU Data Updates** – when filter data changes, existing invalidation paths call renderer setters. The renderer base marks its cached state dirty; the active backend driver performs the necessary buffer/image updates on the next frame.
5. **Readback/Picking** – filters request readback through `RenderUtilities` (`readColor`, `readDepth`, `unprojectDepth`). GL uses PBOs; Vulkan stages buffers. The caller sees identical semantics.

**Compositor Integration**

- The compositor still acquires temporary targets from `Z3DScratchResourcePool`, but leases now wrap API-neutral `Z3DRenderTarget` objects. The compositor never binds FBOs directly; it asks backend utilities to begin or resolve passes using the lease.
- Blend/OIT paths become high-level intents (`beginTransparencyPass(mode, lease)`, `resolveTransparency(lease, dst)`) that backend drivers translate to GL state changes or Vulkan subpasses.
- Glow, axis, texture copy, and blend helpers already go through `Z3DRendererBase`; once their GL calls move into backend drivers they automatically obey the façade.
- Picking/screenshot readback routes through `RenderUtilities`, removing compositor-owned PBO management.

### Vulkan Prototype Roadmap

- **Stage 0 – Foundation**
  - Stand up a `VkDeviceContext` helper (logical device, queues, allocator hooks) isolated from the shipping renderer.
  - Add a small utilities header for command buffer submission (`ImmediateSubmission`, transient fences) so the prototypes can upload resources without touching the GL path.

- **Stage 1 – Texture Primitives**
  - Implement `ZVulkanTexture` that encapsulates a `VkImage`, `VkImageView`, and sampler creation.
  - Provide helper methods for cube/array/3D images, including staged uploads of subregions (mirroring `Z3DTexture::setSubImage`).
  - Expose descriptor info (`VkDescriptorImageInfo`) so later prototypes can bind textures without re-querying state.

- **Stage 2 – Render Target Wrapper**
  - Design `ZVulkanRenderTarget` around dynamic rendering: store color/depth images, view, and render-area metadata.
  - Support attachment clear/load policies analogous to the GL path and expose a `beginRendering/endRendering` pair that records commands on an injected command buffer.
  - Handle multisample resolve images internally so parity features (progressive targets, accumulation buffers) are available.

- **Stage 3 – Scratch Pool Prototype**
  - Create `ZVulkanScratchResourcePool` that mirrors the GL lease API but tracks per-frame image lifetime, layout transitions, and queue ownership.
  - Start with a single `Temp2D` surface role, then extend to entry/exit, layer arrays, and picking surfaces.
  - Integrate a lightweight cache eviction policy based on frame counters so long-lived Vulkan images are recycled safely.

- **Stage 4 – Prototype Render Flow**
  - Build a self-contained sample that records a command buffer: acquire render target from the Vulkan pool, bind a pipeline, draw a fullscreen triangle, and read back via staging buffer.
  - Validate synchronization (pipeline barriers, queue submissions) with the validation layers before attempting integration with the main renderer.

### Vulkan Prototype Rendering Pipeline Sketch

1. **Frame Bootstrap**
   - Acquire a command buffer + frame fence from the prototype device context.
   - Transition scratch-pool images to the required layouts (color attachment, sampled, transfer) via helper utilities.
2. **Render Pass Encoding**
   - Call `ZVulkanRenderTarget::beginRendering` to emit `vkCmdBeginRendering` with the leased attachments.
   - Bind descriptor sets populated from `ZVulkanTexture` instances, record draw calls through the prototype pipeline state.
   - End rendering, transition the render target to a sampled or transfer src layout as needed.
3. **Readback / Presentation**
   - If CPU readback is required, issue `vkCmdCopyImageToBuffer` into a scratch staging buffer, submit, and map once the fence signals.
   - Otherwise, transition images to the appropriate layout for subsequent passes (e.g., sampled in a composition step).
4. **Cleanup**
   - Return leases to the scratch pool; pool defers destruction until it is safe (tracked via submitted fence values).
   - Reset the command buffer for the next frame.

This roadmap keeps the prototypes isolated—none of these steps touch the live GL renderer—yet exercises the full set of capabilities (`Z3DTexture`, `Z3DRenderTarget`, scratch leasing) so the eventual integration can swap in the Vulkan implementations when we are ready.
4. **Render Target Parity**
   - Implement `ZVulkanRenderTarget` (mono/left/right) with color/depth attachments, resolve images, and CPU staging buffers analogous to GL `Z3DRenderTarget`.
   - Mirror progressive ping-pong management (`m_monoCurrentTarget`, etc.) inside the Vulkan compositor while exposing ready local buffers for engine readback (render targets remain backend-private).
   - Integrate `Z3DScratchResourcePool` usage for transient buffers so allocation patterns stay identical.

5. **Progressive & Invalidation Flow**
   - Ensure the shell propagates `invalidate(State)` to the backend, toggles progressive rendering, and broker requests from `Z3DNetworkEvaluator` exactly as the GL compositor did.
   - Vulkan backend should honour mono/stereo flags even if stereo rendering initially falls back to mono.
   - Keep signal semantics (`sceneParaUpdated`, `renderingFinished`, `renderingError`) consistent across backends.

6. **Widget Groups & Parameters**
   - Populate Vulkan widget groups with real `ZParameter` instances backed by `Z3DGlobalParameters` (background/axis/fog toggles, colors, etc.).
   - Reuse existing parameter binding logic so UI controls work identically regardless of backend.

7. **Picking & Screenshot Readback**
   - Implement Vulkan picking renders + CPU readback; reuse GL helper logic for saving PNGs/hashes to avoid UI regressions.
   - Ensure local color buffers use the same layout and alignment so downstream consumers remain unchanged.

8. **Renderer Parity Milestones**
   - **Lines:** Ensure devicePixelRatio scaling, picking outputs, and progressive invalidation hooks mirror GL behaviour.
   - **Meshes:** Port material/light UBOs, depth/cull states, and transparency paths; add placeholder features (e.g., OIT) with stubs until Vulkan parity lands.
  - **Images (including volume rendering within `Z3DImgFilter`):** Rebuild 2D/3D pipelines around Vulkan dynamic rendering or subpasses, matching GL render target chaining.
   - Each Vulkan renderer should expose GL-compatible entry points to minimise filter churn.

9. **Backend Selection & Persistence**
   - Add `RenderBackend` enum to settings, serialize the chosen backend with docs/views, and surface in preferences/UI.
   - Provide runtime switch that tears down and recreates the compositor shell while preserving filter/back-end state where possible.
   - Runtime toggle flow (handled by `Z3DRenderingEngine`):
     1. `Z3DGlobalParameters::renderBackend` emits `valueChanged` when the user flips the UI switch.
     2. The engine pauses in-flight renders (`Z3DNetworkEvaluator::cancelPending`, compositor filter `abortProgressive()`), captures the active frame context (viewport, progressive counters, scratch leases), and signals the current backend to quiesce via `Z3DCompositorBase::beginBackendSwitch()`.
     3. The engine instantiates the requested backend through a central factory (`createCompositorBackend(RenderBackend)`), injects shared services (scratch pool, renderer cache, picking manager), reapplies persisted state (camera, invalidation flags, mono/stereo mode), and reconnects filter/back-end signal wiring.
     4. If backend creation fails, the engine rolls back to the previous backend and surfaces the error; otherwise it calls `Z3DCompositorBase::endBackendSwitch()` and issues an immediate `requestRender()` so the new backend produces the next frame.
     5. Backend selection is cached in documents/preferences so reopening a project restores the same backend without reconfiguration.

10. **Validation, Tooling, and CI Hooks**
   - Expand the existing Vulkan smoke test to hash staged buffers for representative scenes (background-only, lines, mesh sample).
   - Add optional developer script comparing GL vs Vulkan readbacks for a canned network to catch regressions early.
   - Document expected warnings (OpenCV fat binaries, etc.) so CI logs stay actionable.

### Z3DCompositor Refactor Plan

**Goal** Move compositor responsibilities into a reusable filter/back-end split so OpenGL and Vulkan implementations can plug into the same façade without rippling through the network graph or UI.

1. **Freeze Legacy Implementation As A Safety Net**
   - Keep the existing `Z3DCompositor` sources compilable but mark them as “legacy GL reference” in file comments and documentation so no new dependencies accrete.
   - Seal the public construction paths by moving factory ownership to the new filter; only tests should instantiate the legacy compositor directly during parity checks.
   - Maintain compile-time coverage (build + unit tests) so we can diff behaviour during migration, but ensure runtime wiring no longer reaches these classes.

2. **Elevate `Z3DCompositorFilter` To The Network-Facing Node**
   - Derive from `Z3DBoundedFilter` and recreate the original port layout: scene inputs, camera, scratch pool, mono/stereo render targets, thumbnail and screenshot outputs.
   - Mirror the public API of the old compositor (background/axis parameters, progressive toggles, screenshot helpers, picking entry points) so existing call sites compile unchanged.
   - Import stateful members (progressive frame counters, invalidation flags, stereo configuration, widget groups) and rewire them to drive the backend instead of owning GL state directly.
   - Handle JSON serialization/deserialization of compositor-related parameters inside the filter so document persistence continues to work without touching backends.

3. **Codify The Backend Contract With `Z3DCompositorBase`**
   - Introduce an abstract base exposing lifecycle (`initialize`, `shutdown`), render (`renderFrame`, `renderTransparentFilter`, `renderPicking`), resize, and readback/screenshot hooks required by the filter.
   - Define plain-data structs for frame context (camera matrices, viewport, progressive metadata, scratch resource handles) that the filter populates once per frame and hands to any backend.
   - Include capability queries (`supportsStereo`, `supportsOIT`, `backendName`) so UI/engine logic can adjust behaviour without RTTI or backend-specific includes.

4. **Refactor `Z3DCompositorGLBackend` To Own All OpenGL Resources**
   - Transplant FBO, texture, shader, renderer, and OIT management from the legacy compositor into the backend; ensure lifetime is scoped to the backend object.
   - Recreate the frame graph (background, axes, glow, transparent passes, picking, screenshots) within the backend, accepting only filter-provided objects or handles.
   - Encapsulate screenshot/picking readback buffers behind value-returning APIs so the filter does not touch GL ids.
   - Provide explicit `reset`/`onDeviceLost` hooks to ease future backend swaps or context loss handling.

5. **Wire Backend Selection And Hot Swap Logic**
   - Extend `Z3DGlobalParameters` with a validated `RenderBackend` enum, persistence, and UI widget (already stubbed) so users can flip between OpenGL and Vulkan.
   - Connect the filter to the global parameter change signal; when it fires, tear down the current backend, instantiate the requested one, and reinitialize using existing state (camera, viewport, scratch pool).
   - Expose `setBackendForTesting(RenderBackend)` on the filter to let automated tests and developer utilities pin a backend without relying on global state changes.

6. **Update Engine And Call Sites**
   - Change `Z3DRenderingEngine` to own `std::unique_ptr<Z3DCompositorFilter>`, adjusting construction order and dependency injection (network evaluator, scrach pool, etc.).
   - Audit all codepaths (views, screenshot helpers, pickers) that used to depend on `Z3DCompositor`; reinclude the filter header and call the mirrored API.
   - Remove or rewrite helper functions that returned the legacy compositor pointer; they should now return the filter or backend-neutral abstractions.

7. **Validation, Tooling, And Safety Nets**
   - Add temporary regression tests that instantiate the filter with the GL backend, render a canned scene, and byte-compare outputs against captures from the legacy compositor.
   - Manually test backend hot swapping (OpenGL ↔ Vulkan) during interaction-heavy workflows: progressive refinement, axis toggles, glow filters, OIT scenes, screenshot export.
   - Document outstanding parity gaps (e.g., Vulkan backend TODOs) both in this plan and as code `TODO` markers so the Vulkan backend work can proceed without rediscovery.

### OpenGL Pipeline Refactor Plan

**Current pressure points**

- `Z3DCompositor` intermixes UI parameters, GL renderers, scratch-pool coordination, and frame orchestration in a single class (`src/atlas/z3dcompositor.cpp:13`). `process()` pulls filters, partitions them by transparency, toggles GL state directly, and drives every pass in-line (`src/atlas/z3dcompositor.cpp:338`).
- `Z3DBoundedFilter` drags OpenGL renderers into its public header (`src/atlas/z3dboundedfilter.h:3`), constructs them, and wires UI state to renderer flags in its ctor (`src/atlas/z3dboundedfilter.cpp:12`), tying filter lifetime to GL resources.
- `Z3DRendererBase` mixes parameter ownership, GL state binding, shader header generation, and draw dispatch (`src/atlas/z3drendererbase.cpp:12`), making it hard to supply the same data to non-GL backends.
- Picking, screenshots, and handle overlays each reach behind abstractions to grab `Z3DTexture` or FBO ids (`src/atlas/z3dcompositor.cpp:287`), preventing backend neutrality.

**Renderer Base / Primitive Strategy (in progress)**

- Treat `Z3DBoundedFilter` as the sole owner of global parameters; renderer bases receive precomputed POD snapshots instead of reading Qt parameters directly. This keeps filters as the bridge and makes renderer headers backend-neutral.
- [x] Split renderer state into explicit structs:
  - `RendererFrameState` (viewport, frame index, progressive flags)
  - `RendererViewState` (camera matrices, eye-specific data)
  - `RendererSceneState` (lighting, fog, multisample policy, transparency mode)
  These structs live in the filter layer and are cached/compared there before dispatching to the backend.
- [x] `Z3DRendererBase` becomes a lean cache for these structs plus core transforms/material scalars; it emits change signals when the structs mutate but exposes no GL helpers.
- Introduced filter-owned `RendererParameters` with a `ParameterState` hand-off into `Z3DRendererBase`, so snapshot updates stay backend-neutral while existing GL change signals remain intact.
- [x] Backend implementations (`Z3DRendererGLBackend`, future `ZVulkanRendererBackend`) consume the cached structs to populate their API-specific resources (uniforms, descriptors, push constants) via a shared `RendererBackendContext` interface.
- Primitive renderers switch from direct `gl*` calls to backend-agnostic commands (`updateResources(RendererFrameState&)`, `recordOpaque(CommandList&)`, etc.). The GL backend provides a thin command list that still executes immediate GL calls, while the Vulkan backend records command buffers.
- [x] Clip-plane, fog, and shader-header logic move into the GL backend; Vulkan derives pipeline variants from the same state structs without relying on GLSL macro injection.
- Added a default `Z3DRendererBackend` interface with a GL implementation that `Z3DRendererBase` instantiates, so filters stay API-neutral while legacy GLSL helpers live in the backend.
- Renderer snapshots now keep device pixel ratio plus multisample/transparency enums; scratch-pool and cancellation-token access live on the shared `Z3DRenderGlobalState`, so renderers grab them on demand without bloating the per-frame state.
- Upcoming refactor: migrate camera and viewport ownership out of `Z3DRendererBase`. Filters (or other callers like `Z3DCanvasPainter`) will own the authoritative camera, precompute per-eye view/projection data and viewport matrices when assembling `RendererViewState`/`RendererFrameState`, then hand those snapshots to the base. This requires:
  * Auditing all `rendererBase.camera()`/`rendererBase.viewport()` users and switching them to the filter’s camera or to the snapshot values.
  * Extending the frame/view state structs to carry any derived matrices currently produced by the base (`viewportMatrix`, normal matrices, etc.).
  * Removing `m_camera`, `m_sharedCamera`, `m_viewport`, and the legacy helpers from `Z3DRendererBase` once all call sites consume the snapshots.
- **Next session TODOs / status:**
  1. [x] Sweep the remaining call sites to drop `Z3DRendererBase::globalParas()`/legacy parameter queries in favour of the new frame/view/scene state snapshots; the transitional accessor on `Z3DRendererBase` has been removed.
  2. [x] Thread device pixel ratio through renderer snapshots and route scratch-pool/cancellation access via the shared infrastructure instead of `Z3DGlobalParameters`.
  3. [ ] Start introducing the backend-neutral command interface for primitive renderers (`updateResources`, `recordOpaque`/`recordTransparent`) and migrate an initial renderer (e.g., `Z3DImage2DRenderer`) as the spike.
- [x] Adopt lightweight generation counters to avoid redundant uploads: filters bump a counter when a state struct changes, and primitive renderers only resend data when the generation increases. (Tuning still needed to skip identical snapshots.)
- [x] Extended the snapshot hand-off to canvas/raycaster utility paths so every render call now pushes frame/view/scene state; `Z3DRendererBase` no longer falls back to `Z3DGlobalParameters` when populating shader uniforms.

**Refactor actions (incremental, keep GL live while migrating)**

1. **Carve out `Z3DCompositorGLBackend`**
   - Move the GL renderers, shaders, and render-target ownership out of `Z3DCompositor` into a backend that implements `Z3DCompositorBase`. Keep `Z3DCompositor::process` focused on gathering filters, building the `FrameContext`, and delegating passes through the bridge.

2. **Split filter responsibilities**
   - Keep renderer members on `Z3DBoundedFilter`, but ensure they own only backend-neutral state and talk to `Z3DRendererBase` drivers for API-specific work. Filters stay responsible for CPU data/parameters and avoid direct GL/Vulkan calls.
   - Transition filter headers to forward declarations only, keeping backend includes in implementation files so the public surface stays neutral.

3. **Clarify Z3DRendererBase ownership (todo)**
   - [x] Relocated renderer-facing UI parameters (`Coord Transform`, `Size Scale`, `Opacity`, material controls, legacy render method) from `Z3DRendererBase` into `Z3DBoundedFilter`, exposing them via `rendererParameters()` so filters own lifetime, widgets, and serialization.
   - [x] Trimmed `Z3DRendererBase` down to plain rendering state (matrices, floats, colors) with lightweight setters/getters and a `RenderMethod` enum; setters now emit the existing change signals and legacy invalidations without depending on `ZParameter`.
- [x] Updated filters and widgets to consume the filter-owned parameters directly; the helper accessors on `Z3DBoundedFilter` were removed so call sites now reach into `m_rendererParameters` (and its `coordTransform`) explicitly before handing data to the renderer base.
   - [ ] Follow-up: audit any non-`Z3DBoundedFilter` renderers for lingering direct `Z3DRendererBase` parameter coupling and document additional Vulkan expectations for the new setters if they arise.
4. **Normalize resource handles and readbacks**
   - Replace direct FBO/texture access in picking/screenshots with backend-managed readback tickets. Filters request readbacks through `Z3DCompositorBase`; each backend maps the ticket to its native transfer path (GL PBO, Vulkan staging image, etc.).

5. **Pass orchestration**
  - Keep the compositor backend responsible for named passes (`renderOpaqueGeometry`, `renderVolumetricImages`, `resolveGlow`, `composeOnTop`) so filters remain API-neutral coordinators.

6. **Testing hooks**
   - Expand lightweight unit/integration tests that exercise renderer-base drivers with mock filters to validate bucketing, invalidation, and progressive behaviour.
   - Capture GL outputs before/after major refactors to guarantee parity while we reshape the pipeline.

### Renderer Parity Focus

- Backgrounds: verify uniform/gradient rendering (including region scaling) matches GL once specialization and push-constant wiring is complete.
- Lines: ensure wide-line emulation, caps, strip batching, 1D texture colour, and picking IDs align across backends.
- Meshes: confirm material/lighting UBOs, cull/depth states, and wireframe overlays behave identically.
- Images (including volumetric modes inside `Z3DImgFilter`): compare slice/raycast outputs, transfer functions, and entry/exit blending against GL captures.

### Backend Switch Execution

- Backend toggles are driven by `Z3DGlobalParameters::renderBackend`; `Z3DRenderingEngine` listens, pauses work, and asks the compositor/backend managers to swap renderer drivers.
- Filters implement `onBackendSwitch` to drop backend-specific caches, ensure their renderer bases recreate GPU resources, and immediately push their CPU data to the new driver.
- Backend swap failures surface via error callbacks, prompting an automatic rollback to the previous backend with a user-visible error.


## Detailed Task Breakdown (Checklist)

- [x] Implement SPIR-V shader module loading in `ZVulkanShader`.
- [x] Add CMake/custom target to compile Vulkan GLSL to SPIR-V offline.
- [ ] Map OpenGL `#define`s to specialization constants or push constants; keep only interface-changing options as variants.
- [ ] Define per-shader “variant key” structs (only for interface-changing bits) and persist `vk::PipelineCache`.
- [x] Add/minify shader compilation script outputs to `Resources/shader/vulkan/spv/`.
- [x] Minimal background clear frame using dynamic rendering, plus CPU readback test.
- [x] Fullscreen triangle pipeline for background gradient base.
- [x] Fix line renderer pipeline layout and descriptor binding (always include set=0 combined image sampler).
- [x] Convert 1D LUT usage to 2D Wx1 to improve portability.
- [ ] Background renderer parity: finalize specialization constants for Uniform/Gradient orientations; ensure the renderer backend wires `mode`/`orientation`/`region` consistently.
- [ ] Finalize `ZVulkanLineRenderer` (buffers, pipelines, draw calls, picking path).
- [ ] Line renderer parity: match smooth wide lines, caps, strip batching, per-segment widths, 1D texture colours, picking IDs.
- [ ] Implement per-frame UBO/push-constant plumbing for camera matrices, fog, lights.
- [ ] Implement `ZVulkanMeshRenderer` with lighting.
- [ ] Implement Vulkan image pipeline (2D slices + raycaster v1) to mirror `Z3DImgFilter` paths.
- [ ] Flesh out `ZVulkanCompositor` so transparency/OIT paths reach feature parity with GL.
- [x] Add a minimal Vulkan compositor with background + lines (`ZVulkanCompositor`).
- [ ] Backend switch and engine integration (screenshots, tiling, stereo, picking, progressive).
- [ ] Validation, perf polishing, MSAA/resolve, packaging.

Abstraction/Reuse tasks

- [x] Add abstract compositor interface extracted from Z3D (`z3dcompositorbase.h`).
- [x] Implement minimal Vulkan compositor to produce RGBA readbacks (background + axis lines).
- [x] Simplify compositor façade/readback to rely on `Z3DLocalColorBuffer` snapshots (engine screenshots now API-neutral).
- [x] Replace `ZVulkanRendererBase` with `Z3DRendererVulkanBackend` so Vulkan shares the `Z3DRendererBase` state machinery.
  - [x] Introduce a Vulkan `Z3DRendererBackend` implementation that wraps device/swapchain setup (`beginFrame`, `endFrame`, readback helpers).
- [x] Update `ZVulkanRenderer` and renderer backend drivers to consume `RendererFrameState`, `RendererViewState`, and `RendererSceneState` from the host `Z3DRendererBase`.
  - [x] Switch filter/backend factories (`Z3DBoundedFilter::refreshRendererBackend`, compositor bridge) to create the Vulkan backend and drop direct `ZVulkanRendererBase` construction.
  - [x] Delete or shim the legacy `ZVulkanRendererBase` once all call sites are migrated.
  - [x] Hoist lighting-state assembly into `Z3DRenderGlobalState` so shared renderer state builds lighting without calling back into global parameters.
- [ ] Expand Vulkan compositor to provide equivalent outputs (ready targets/readback).
- [ ] Migrate engine to hold a `std::unique_ptr` to the facade and switch backend at runtime.
- [ ] Audit filters for GL leakage and continue moving API-specific ownership into renderers/compositor.
- [ ] Move UI-facing parameters out of `Z3DRendererBase` into filters, and keep the renderer base as plain rendering state updated via filter-owned signals.
- [ ] Update scratch render-target handling so both backends share common helpers via the bridge (no new abstraction layer beyond existing scratch pool).
- [ ] Renderer parameter audit: hoist persistent `ZParameter` state out of GL renderers so backend swaps don’t drop user settings (see checklist below).
  - [x] Image raycaster: sampling rate, ISO value, local MIP threshold, and compositing mode now live on `Z3DImgFilter` and are injected into the renderer.
- [x] Volume raycaster: sampling rate, ISO value, local MIP threshold, and compositing mode now live on `Z3DImgFilter` and are injected into the renderer through backend driver hooks.
  - [x] Background renderer: compositor holds mode/colors/orientation parameters and injects them into `Z3DBackgroundRenderer`.
  - [x] Mesh renderer: wireframe mode/color now live on `Z3DMeshFilter`; renderer consumes filter state through new setters.
  - [x] Sphere/Ellipsoid renderers: dynamic-material toggles moved to owning filters (`Z3DPunctaFilter`, SWC renderer setup) and are injected via setters.
  - [x] Axis font overlay: axis filter/compositor own font/shadow/outline parameters; `Z3DFontRenderer` exposes simple setters and POD state.
  - [x] Fixed-width line renderer: width/color configured via filter-managed state and injected through new hooks.
  - [x] Cone/arrow and texture blend renderers: cap style/blend mode stored as plain values; renderer-side options removed.

Renderer parameter ownership checklist

- `Z3DImgRaycasterRenderer` / `Z3DImgSliceRenderer`: move visualization toggles (lighting, clip planes, transfer helpers) into `Z3DImgFilter` and keep renderers GPU-only.
- `Z3DLineRenderer`: expose width/size scaling parameters at the filter layer; renderer maintains only buffers/pipelines.
- `Z3DMeshRenderer`: relocate material/light parameters into `Z3DMeshFilter`; renderer responsible for geometry upload and draw state.
- Background/axis renderers already source data from `Z3DGlobalParameters`; verify no renderer-owned parameters remain before cloning for Vulkan.

## API Parity Acceptance Criteria

- Background: identical color/gradient appearance across modes and orientations on the same inputs; region respected. No UI changes.
- Line: identical thickness, caps, strip behavior, and color mapping on representative datasets; matching picking IDs. No UI or configuration changes.
- Engine switch: Users can toggle OpenGL/Vulkan in the same UI; scenes render equivalently; screenshots and tiling produce the same outputs within small numerical tolerances.

## Success Criteria per Phase

- P0: Pipeline creation with actual shader modules succeeds; device info logged.
- P1: CPU readback matches clear color for requested output size.
- P2: Lines visually match OpenGL (width, smoothness, caps) within tolerance.
- P3: Mesh normals/lighting match reference scenes.
- P4: Image pipeline (including volumetric modes inside `Z3DImgFilter`) matches OpenGL outputs on sample datasets.
- P5: Transparency and full composition order are correct and stable.
- P6: Feature parity for app workflows (screenshots, stereo, picking, progressive renders).
- P7: No validation errors; acceptable performance; distributable binaries with Vulkan loader/layers.

## Risks & Mitigation

- Shader parity: Different NDC/depth conventions (OpenGL vs Vulkan). Mitigate with camera coordinate system support (already present) and shader adjustments.
- Transparency: OIT methods require careful translation; start with Weighted Blended.
- Readbacks: Synchronization and layout transitions. Use staging buffers & fences; centralize helpers.
- Platform packaging: Loader and validation layers availability. Bake into app bundles on macOS/Windows; ship resources on Linux.

## Changes Made So Far

- `ZVulkanShader` now loads SPIR-V files and owns `vk::ShaderModule`s. Pipelines receive valid `vk::PipelineShaderStageCreateInfo` with `module` set.
  - Header: `src/atlas/zvulkan.h:219`
  - Impl: `src/atlas/zvulkan.cpp:880`

Progress Update — Image Filter GL Decoupling (Completed)

- z3dimgfilter is now GL-agnostic for image/volume rendering paths:
  - All GL work for images/volumes moved into `Z3DImgRaycasterRenderer` and `Z3DImgSliceRenderer`.
  - No direct `glReadPixels`; picking depth fetched via `Z3DRenderTarget::depthAtPos()`.
- Entry/Exit is produced inside the raycaster:
  - Raycaster owns an entry/exit render target (2-layer RGBA32F) and prepares it via `prepareEntryExit()`.
- Renderer-owned FBOs/textures:
  - Layer and block-ID targets live in renderers; per-eye ping-pong image targets are created in the raycaster constructor and resized on demand.
- Local GL state in renderers:
  - Raycaster and slicer manage `GL_BLEND` and `GL_DEPTH_TEST` in their render paths.
- Bound box overlay depth/blend (temporary):
  - The image filter enables depth/blend only around `renderBoundBox()` to ensure proper depth; this can be centralized in `Z3DBoundedFilter` later.

Progress Update — Renderer Scene Defaults (Completed)

- `RendererSceneState` seeds the same default lighting, fog, and multisample configuration as `Z3DGlobalParameters`.
  - Ensures freshly constructed renderers provide `LIGHT_COUNT > 0` before filters sync state, maintaining shader compatibility during refactors.
  - Scene ambient, transparency mode, and MSAA defaults now match the UI baseline so both backends start from identical state.

Progress Update — Renderer Backend Abstraction (Completed)

  - `Z3DRendererBase` now talks to pluggable backends; the GLSL implementation lives behind `createGLRendererBackend()` and remains responsible for API-specific glue (clip planes, shader headers, global uniforms).
- Filters own POD state blocks (`RendererFrameState`, `RendererViewState`, `RendererSceneState`, `ParameterState`) and feed them to their renderers/render bases. Parameter reads no longer happen inside the renderers.
- The global state singleton is now just a cache: filters/compositors build fresh scene/view blocks from live parameters when needed, eliminating stale lighting/camera data.
- All OIT/dual-depth compositor passes avoid leaking global camera uniforms; fullscreen shaders receive explicit screen-space uniforms.
- Next focus:
  1. Expand the Vulkan compositor to deliver parity outputs (transparency, readbacks, OIT).
  2. Migrate engine ownership to a backend-agnostic facade that can swap OpenGL/Vulkan at runtime.
  3. Continue auditing filters for residual GL dependencies and move backend-specific ownership into renderer backend drivers.
