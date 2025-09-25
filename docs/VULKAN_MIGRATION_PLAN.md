# Vulkan Migration Plan

This plan guides the migration of the Z3D OpenGL renderer to a Vulkan backend, in incremental, verifiable phases. It aims to reach feature parity with the current OpenGL implementation while keeping the application usable throughout.

## Goals

- Preserve the existing rendering feature set (camera, geometry, volumes, transparency, picking, screenshots, stereo) with Vulkan.
- Keep OpenGL running during migration; enable selecting backend for A/B comparison.
- Adopt explicit, robust resource management, validation, and repeatable shader compilation to SPIR-V.
- Maintain clean abstractions that mirror existing Z3D architecture to lower risk and churn.
- Historical note: legacy `z3dvolume*` classes are no longer in use; all image/volume rendering paths live under the `z3dimg*` families and should remain the focus during migration.
- Implementation language baseline is modern C++20; all new APIs should use standard library facilities (`std::span`, `std::variant`, etc.) rather than third-party helpers when equivalents exist.

### Development Notes

- You can use `cmake --build build/Release` to verify compositor changes still compile before pushing them further.
- We do not use `z3dcanvaspainter`; ignore that file during the migration so no `Z3DRenderTarget` accessors need to leak out of `Z3DCompositorFilter` or `Z3DCompositorBase`.
- Be careful with `git checkout` while iterating—running it mid-refactor can discard uncommitted progress we still rely on for diffing the legacy compositor.

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
- Parameter bridging: Reuse `Z3DGlobalParameters` as the single source of truth. Vulkan renderers read the same settings (camera, fog, OIT, clip planes, devicePixelRatio, etc.) from a bridge in `ZVulkanRendererBase`.
- Feature matching: When hardware features differ (e.g., wide lines), emulate in shaders/CPU so behavior stays consistent.

Data/API Abstraction Layer

- Preserve renderer method names across backends (e.g., `setData`, `setDataColors`, `setLineWidth`, etc.) so filters/compositor can keep calling the same functions.
- Extract an interface from `Z3DCompositor` so that Vulkan compositor can implement the same surface used by the engine. Introduce `src/atlas/z3dcompositorbase.h` (abstract) as the target interface.
- Run a feasibility check for each abstraction decision (resource ownership, frame contexts, synchronization) to confirm it maps cleanly to both OpenGL and Vulkan without hidden performance cliffs.

## GL Dependency Classification (Current Engine)

- API‑independent core (keep as is):
  - Engine scaffolding (minus GL readback): `z3drenderingengine.*`
  - Scene/camera/parameters/UI: `z3dscene.*`, `z3dcamera.*`, `z3dglobalparameters.*`, `z*parameter.*`, widget groups
  - Graph: `z3dport.*` (ports), `z3dnetworkevaluator.*`
  - Views/docs: `z3d*view.*`, `z*doc.*`
- OpenGL‑specific (keep GL here; add Vulkan counterparts):
  - GL context/helpers: `z3dgl.*`, `z3dcontext.*`
  - Shaders: `z3dshaderprogram.*`, `z3dshadergroup.*`, `z3dshadermanager.*`
  - Buffers/VAO: `z3dvertexarrayobject.*`, `z3dvertexbufferobject.*`
  - Textures/FBO: `z3dtexture.*`, `z3drendertarget.*`, `z3drenderport.*`
  - Renderers: `z3d*renderer.*` (background, line, mesh, image/volume paths inside `z3dimg*`)
  - Compositor: `z3dcompositor.*`
- Mixed areas still being cleaned: residual GL includes in a few filters and any helper utilities that still assume OpenGL-only state.

Recent cleanups worth carrying forward:

- `z3dport.h` no longer includes `z3dfilter.h`; it forward-declares the filter class and keeps GL headers in source files.
- `z3dfilter` base removed its GL helper dependency so headers stay backend-neutral.
- `Z3DMeshFilter` already delegates all GL work to renderers/compositor; use it as the pattern for the remaining filters.

Essentials for the ongoing effort:

- Continue sweeping headers in API-neutral layers, replacing GL includes with forward declarations and moving includes into `.cpp`.
- Confine new backend-specific code to delegates, renderers, and the compositor; filters should only manage CPU state and call delegate helpers.
- Maintain a slim `Z3DCompositorBase` interface so both backends share the same engine integration pathway.

## Detailed Migration Backlog (mirrors OpenGL pipeline)

1. **Cross-API Rendering Abstraction Design**
   - Document the API-neutral command/data interfaces (frame context structs, renderer entry points, resource handles) that filters and the compositor rely on.
   - Evaluate each interface proposal against both OpenGL’s stateful model and Vulkan’s explicit render pass/synchronization requirements to guarantee feasibility and avoid future performance cliffs.
   - Produce an adaptation checklist for the existing OpenGL path (shims, ownership moves, lifetime rules) so we can converge GL onto the façade before advancing Vulkan work.

2. **Compositor Filter Shell**
   - Introduce `Z3DCompositorFilter` derived from `Z3DBoundedFilter` that owns invalidation, progressive flags, widget plumbing, JSON I/O, and render requests.
   - The shell holds `std::unique_ptr<Z3DCompositorBase>` and delegates to either `Z3DCompositorGLBackend` or `ZVulkanCompositor` depending on runtime config.
   - Adjust `Z3DRenderingEngine` to construct the shell, keep its pointer instead of the GL compositor, and expose accessor helpers that forward to the active backend.

3. **Filter/Compositor Decoupling (No DTO layer)**
   - Continue moving GL resource ownership out of filters into renderers/compositor while keeping filter data structures unchanged.
   - Audit filters to ensure headers stay backend-agnostic and only expose existing parameter/state surfaces.

**Filter/Renderer Refactor Blueprint**
- **Filter structure (API-neutral):**
  - Data ownership stays in the filter (`std::vector` geometry, image bricks, parameter state). Filters continue to expose the CPU data they already manage (vectors, parameter structs, cached configs) directly to their delegates—no extra abstraction layer is introduced.
  - Each filter owns a small `RenderState` struct (material flags, shader toggles, invalidation counters) that is serialisable and independent of backend.
  - Filters no longer include GL renderer headers. Instead they reference abstract delegate interfaces, resolved at runtime through the `FilterBackendBridge`.
- **Renderer delegates (per backend):**
  - Define `Z3DFilterDelegate` in `src/atlas/z3dfilterdelegate.h` with pass-oriented methods (`beginFrame`, `renderOpaque`, `renderTransparent`, `renderDepthPrepass`, `endFrame`, `shutdown`). Filter-specific data upload methods stay on the derived delegate types so they can consume the existing CPU data each filter already manages.
  - Provide concrete implementations adhering to the naming convention `Z3D…` for OpenGL artefacts and `ZVulkan…` for Vulkan artefacts. Every filter type gains a matching pair (e.g., `Z3DMeshFilterDelegate`, `ZVulkanMeshFilterDelegate`; `Z3DImgFilterDelegate`, `ZVulkanImgFilterDelegate`; file names `z3dmeshfilterdelegate.*`, `zvulkanmeshfilterdelegate.*`). All delegate sources remain in `src/atlas/`; the prefix signals the backend while the delegate encapsulates VAO/VBO creation, descriptor management, and draw commands.
  - Delegates keep long-lived GPU objects (VBOs, pipelines) and refresh allocations only when the incoming CPU data changes size/layout or when a backend switch occurs.
- **Filter execution flow after refactor:**
  1. During `onBackendSwitch`, the filter calls `bridge.createDelegate(filterKind(), filterId(), bootstrapCtx)` to obtain a backend-specific delegate. The filter immediately pushes its existing CPU data through the delegate’s update methods so the GPU resources are rebuilt under the new backend.
  2. On `filter->render(eye)`, the filter forwards the current frame context and eye directly to its delegate’s render method. The filter does not issue GL calls; it just updates CPU-side state and invalidation flags.
  3. If filter data mutates (new vertices, parameter changes), the filter calls the delegate’s update method with the same CPU data it already owns. Delegates refresh GPU buffers incrementally and rebuild only when necessary.
  4. When the filter disconnects, it calls `delegate->shutdown()` and the bridge releases backend resources via `releaseBackendResources(filterId)`.
- **Compositor interaction:**
  - `Z3DCompositorFilter` continues to orchestrate passes but keeps relying on the existing helper predicates: `filter->isReady(eye)`, `filter->hasOpaque(eye)`, and `filter->hasTransparent(eye)`. These flags, together with the legacy stay-on-top/transparency metadata, already encode the pass ordering the compositor needs, so no additional `prepareRenderPlan()` layer is introduced.
  - Progressive rendering: filters maintain their CPU-side progressive counters; delegates expose `supportsProgressive()` so the compositor can decide whether to reuse previous frames or request incremental uploads.
  - Detailed call path:
    1. **Frame kickoff:** engine calls `Z3DCompositorFilter::process(eye)`. The filter gathers connected filters from its ports and builds a `FrameContext` (camera, viewport, frame index, progressive state) for the current eye.
    2. **Pass bucketing:** using existing predicates (`isReady`, `hasOpaque`, `hasTransparent`, `isStayOnTop`), the compositor partitions filters into opaque/transparent/on-top buckets exactly as the current OpenGL code does.
    3. **Backend scheduling:** for each bucket the active `Z3DCompositorBase` allocates or reuses render targets, then for every filter in draw order performs:
       - `auto* delegate = filter.delegateHandle();`
       - `bridge.connectDelegate(*delegate)` to expose backend utilities for the pass (bind in-flight command buffer/FBO, hand out scratch resources, descriptor allocators, progressive state).
       - `delegate->beginFrame(frameCtx)` (using the context assembled in step 1).
       - `delegate->renderOpaque(attachments)` or `delegate->renderTransparent(attachments)` based on the bucket.
    4. **Pass execution:** the delegate binds buffers, descriptors, and issues draw commands on the backend. Filters stay unaware of FBO/descriptor details.
    5. **Cleanup:** after each delegate call the backend invokes `bridge.disconnectDelegate(*delegate)` to release per-frame bindings, and once all passes finish it publishes composed render targets back to the compositor filter for readback/signaling.
- **Migration approach:**
  - Start with geometry filters (`Z3DMeshFilter`, `Z3DLineFilter`), converting them to the delegate model while keeping the GL delegate wired to existing renderer code. Once stable, build Vulkan delegates that reuse the same update methods and CPU data.
  - After geometry filters, port image filters; ensure render target creation moves entirely into delegates so filters only describe required attachments (formats, dimensions) and the compositor backend provides the concrete target handles.
  - Add unit tests validating that CPU data changes trigger delegate updates and that backend toggles recreate delegates without losing filter state.

### API Sketch — Filter Delegates & Bridge

**Header layout (all names follow the `Z3D…` / `ZVulkan…` convention and live directly under `src/atlas/`):**

- `src/atlas/z3dfilterdelegate.h`
  - Declares the API-neutral base classes shared across backends.
- `src/atlas/z3dfilterbackendbridge.h`
  - Declares the bridge interface used by filters/compositor to talk to the active backend.
- Backend-specific source files share the same directory, relying on prefix alone for clarity (e.g., `z3dmeshfilterdelegate.cpp`, `zvulkanmeshfilterdelegate.cpp`).

**Core enums & aliases:**

```cpp
enum class RenderBackend { OpenGL, Vulkan };

enum class FilterKind {
  Mesh,
  Line,
  Img,
  Puncta,
  // extend as new filter families appear
};

using FilterId = QUuid; // filters already have UUIDs; reuse for backend bookkeeping

struct FrameContext {
  const Z3DCamera* camera;
  glm::uvec2 viewport;
  uint64_t frameIndex;
  bool progressiveEnabled;
  Z3DEye eye;
};

struct PassAttachments {
  AttachmentHandle color; // opaque handle provided by compositor backend
  AttachmentHandle depth;
  AttachmentHandle accum; // optional OIT accumulation
  AttachmentHandle reveal; // optional OIT revealage
};
```

`AttachmentHandle` remains an opaque backend-defined token (GL texture id + target pair, or Vulkan image/view). Delegates obtain binding helpers for it through the bridge.

**API-neutral delegate interfaces (`z3dfilterdelegate.h`):**

```cpp
class Z3DFilterDelegate
{
public:
  virtual ~Z3DFilterDelegate() = default;

  virtual void beginFrame(const FrameContext& ctx) = 0;
  virtual void renderOpaque(const PassAttachments& attachments) = 0;
  virtual void renderTransparent(const PassAttachments& attachments) = 0;
  virtual void renderDepthPrepass(const PassAttachments& attachments) = 0;
  virtual void endFrame() = 0;

  virtual bool supportsProgressive() const { return false; }
  virtual void shutdown() = 0;
};
```

Filter-specific delegates inherit from `Z3DFilterDelegate` and add the strongly typed upload hooks they require. Example:

```cpp
struct Z3DMeshCpuData;
struct Z3DMeshRenderState;

class Z3DMeshFilterDelegate : public Z3DFilterDelegate
{
public:
  virtual void updateMeshData(const Z3DMeshCpuData& data) = 0;
  virtual void updateRenderState(const Z3DMeshRenderState& state) = 0;
};

class Z3DImgFilterDelegate : public Z3DFilterDelegate
{
public:
  virtual void updateImageConfig(const Z3DImgConfig& config) = 0;
  virtual void updateSliceState(const Z3DImgSliceState& slice) = 0;
};
```

Backends derive concrete implementations (`Z3DMeshFilterDelegate`, `ZVulkanMeshFilterDelegate`, etc.) that plug into the existing renderer logic without inventing new intermediate containers. Each delegate reuses the filter’s existing CPU data structures.

**Bridge responsibilities (`z3dfilterbackendbridge.h`):**

```cpp
class Z3DFilterBackendBridge
{
public:
  virtual ~Z3DFilterBackendBridge() = default;

  // Called during backend switch / filter creation.
  virtual std::unique_ptr<Z3DFilterDelegate> createDelegate(FilterKind kind,
                                                            FilterId id,
                                                            const FrameContext& bootstrapCtx) = 0;

  virtual void connectDelegate(Z3DFilterDelegate& delegate,
                               const FrameContext& ctx,
                               const PassAttachments& attachments) = 0;
  virtual void disconnectDelegate(Z3DFilterDelegate& delegate) = 0;

  virtual ReadbackTicket requestReadback(FilterId id,
                                         const ReadbackDescriptor& desc) = 0;
  virtual void releaseBackendResources(FilterId id) = 0;
};
```

- `ReadbackDescriptor` represents a semantic readback request (picking, screenshot, histogram). Backends map it to the appropriate pipeline stage.
- Geometry/texture uploads are triggered directly by filters via their typed delegate methods (`updateMeshData`, `updateImageConfig`, etc.), keeping ownership with the filter classes that already manage those CPU structures.

**Filter base additions (`z3dboundedfilter.*` planned changes):**

```cpp
class Z3DBoundedFilter : public Z3DFilter
{
public:
  void setDelegate(std::unique_ptr<Z3DFilterDelegate> delegate);
  Z3DFilterDelegate* delegateHandle() const { return m_delegate.get(); }

protected:
  virtual FilterKind filterKind() const = 0;

private:
  std::unique_ptr<Z3DFilterDelegate> m_delegate;
};
```

- Derived filters reuse their existing invalidation paths (`invalidateGeometry`, `invalidateImageConfig`, etc.) to call the appropriate delegate update methods with the CPU data they already own.
- `filterKind()` lets the backend registry choose the correct concrete delegate during backend switch.

**Backend factory responsibilities:**

- Each backend exposes a factory function, e.g.,

```cpp
std::unique_ptr<Z3DFilterDelegate> createZ3DDelegate(FilterKind kind,
                                                     FilterId id,
                                                     const FrameContext& ctx,
                                                     Z3DFilterBackendBridge& bridge);

std::unique_ptr<Z3DFilterDelegate> createZVulkanDelegate(FilterKind kind,
                                                         FilterId id,
                                                         const FrameContext& ctx,
                                                         Z3DFilterBackendBridge& bridge);
```

These register immutable upload helpers, share renderer caches, and ensure we follow the naming rules (`Z3D…`/`ZVulkan…`). The bridge owns whichever factory matches the active backend.

**Rendering sequence summary:**

1. Backend switch: compositor asks bridge to create delegates for each filter using `filter.filterKind()`.
2. Frame rendering: compositor buckets filters via `hasOpaque/hasTransparent`, obtains `delegateHandle()`, calls `bridge.connectDelegate(*delegate)` passing `FrameContext` + attachments.
3. Delegate renders the requested pass (opaque/transparent/etc.).
4. Backend calls `bridge.disconnectDelegate(*delegate)` and moves to the next filter.
5. When a filter disconnects from the graph, compositor calls `delegate->shutdown()` and `bridge.releaseBackendResources(filterId)`.

This API sketch keeps filter headers clean, mirrors today’s execution order, and makes it straightforward to add the Vulkan implementations without rewriting filter logic.

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

**Filter Backend Handshake (all upstream filters)**
- Interface definition lives in `src/atlas/z3dfilterbackendbridge.h` (new). It declares pure virtual hooks every backend supplies:
  - `std::unique_ptr<Z3DFilterDelegate> createDelegate(FilterKind, FilterId, const FrameContext&)` — factory used during backend switch or filter insertion.
  - `void releaseBackendResources(FilterId)` — invoked when a filter is removed or before a backend swap so the backend can destroy buffers, framebuffers, pipelines, or cached descriptors tied to that filter.
  - `ReadbackTicket requestReadback(const ReadbackDescriptor&)` — queue a CPU readback (screenshots, picking, histograms). Implementations enqueue either a GL PBO read or a Vulkan transfer/fence and return an opaque ticket the filter can poll.
- `Z3DCompositorBase` owns the concrete bridge; the compositor backend stores a delegate pointer for each connected filter and, before rendering, invokes `bridge.connectDelegate(*delegate)` (see detailed call path below) so the delegate sees the correct in-flight command context.
- Every bounded filter implements `onBackendSwitch(RenderBackend newBackend, FilterBackendBridge& bridge)`. The renderer pipeline calls this in two spots:
  1. **Backend toggle:** within the engine’s runtime switch flow (see above step 2), the compositor filter iterates all connected filters. For each filter it first calls `filter->beginBackendSwitch()`, then `onBackendSwitch(newBackend, bridge)`, followed by `filter->finishBackendSwitch(success)`. During the hook the filter drops backend-owned caches and asks the bridge to create a new delegate; the filter immediately feeds its existing CPU data into the delegate so GPU resources are recreated under the new backend.
  2. **Filter insertion/removal:** when a filter connects/disconnects to the compositor port, `Z3DCompositorFilter` invokes `onBackendSwitch(currentBackend, bridge)` so the new filter instantly materializes backend resources via its delegate, and calls `releaseBackendResources` when it departs.
- `requestReadback` is used by filters that need CPU-visible output (picking buffers, thumbnails, screenshot export). For example, the compositor filter’s picking path can ask the bridge for a readback ticket after rendering the ID target; volume/image filters typically skip this helper because their results stay in backend-owned render targets consumed by the compositor.
- If any bridge operation throws or returns an error code, the filter reports it via `FilterBackendBridge::reportError`. The engine interprets that as a failed switch, reverts to the prior backend, and surfaces the problem to the user.

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
- Split renderer state into explicit structs:
  - `RendererFrameState` (viewport, frame index, progressive flags)
  - `RendererViewState` (camera matrices, eye-specific data)
  - `RendererSceneState` (lighting, fog, multisample policy, transparency mode)
  These structs live in the filter layer and are cached/compared there before dispatching to the backend.
- `Z3DRendererBase` becomes a lean cache for these structs plus core transforms/material scalars; it emits change signals when the structs mutate but exposes no GL helpers.
- Backend implementations (`Z3DRendererGLBackend`, future `ZVulkanRendererBackend`) consume the cached structs to populate their API-specific resources (uniforms, descriptors, push constants) via a shared `RendererBackendContext` interface.
- Primitive renderers switch from direct `gl*` calls to backend-agnostic commands (`updateResources(RendererFrameState&)`, `recordOpaque(CommandList&)`, etc.). The GL backend provides a thin command list that still executes immediate GL calls, while the Vulkan backend records command buffers.
- Clip-plane, fog, and shader-header logic move into the GL backend; Vulkan derives pipeline variants from the same state structs without relying on GLSL macro injection.
- Adopt lightweight generation counters to avoid redundant uploads: filters bump a counter when a state struct changes, and primitive renderers only resend data when the generation increases.

**Refactor actions (incremental, keep GL live while migrating)**

1. **Carve out `Z3DCompositorGLBackend`**
   - Move the GL renderers, shaders, and render-target ownership out of `Z3DCompositor` into a backend that implements `Z3DCompositorBase`. Keep `Z3DCompositor::process` focused on gathering filters, building the `FrameContext`, and delegating passes through the bridge.

2. **Split filter responsibilities**
   - Replace GL renderer members on `Z3DBoundedFilter` with backend-agnostic delegate handles. Filters keep CPU data/state and forward them via strongly typed delegate update methods.
   - Transition filter headers to forward declarations only, keeping backend includes in implementation files.

3. **Clarify Z3DRendererBase ownership (todo)**
   - [ ] Relocated renderer-facing UI parameters (`Coord Transform`, `Size Scale`, `Opacity`, material controls, legacy render method) from `Z3DRendererBase` into `Z3DBoundedFilter`, exposing them via `rendererParameters()` so filters own lifetime, widgets, and serialization.
   - [ ] Trimmed `Z3DRendererBase` down to plain rendering state (matrices, floats, colors) with lightweight setters/getters and a `RenderMethod` enum; setters now emit the existing change signals and legacy invalidations without depending on `ZParameter`.
   - [ ] Updated filters and widgets to consume the filter-owned parameters (`coordTransformPara()`, `rendererParameters()`), ensuring programmatic changes flow through the filter parameters before hitting the renderer base.
   - [ ] Follow-up: audit any non-`Z3DBoundedFilter` renderers for lingering direct `Z3DRendererBase` parameter coupling and document additional Vulkan expectations for the new setters if they arise.
4. **Normalize resource handles and readbacks**
   - Replace direct FBO/texture access in picking/screenshots with backend-managed readback tickets. Filters request readbacks through `Z3DCompositorBase`; each backend maps the ticket to its native transfer path (GL PBO, Vulkan staging image, etc.).

5. **Pass orchestration**
  - Keep the compositor backend responsible for named passes (`renderOpaqueGeometry`, `renderVolumetricImages`, `resolveGlow`, `composeOnTop`) so filters remain API-neutral coordinators.

6. **Testing hooks**
   - Expand lightweight unit/integration tests that exercise the delegate bridge with mock filters to validate bucketing, invalidation, and progressive behaviour.
   - Capture GL outputs before/after major refactors to guarantee parity while we reshape the pipeline.

### Renderer Parity Focus

- Backgrounds: verify uniform/gradient rendering (including region scaling) matches GL once specialization and push-constant wiring is complete.
- Lines: ensure wide-line emulation, caps, strip batching, 1D texture colour, and picking IDs align across backends.
- Meshes: confirm material/lighting UBOs, cull/depth states, and wireframe overlays behave identically.
- Images (including volumetric modes inside `Z3DImgFilter`): compare slice/raycast outputs, transfer functions, and entry/exit blending against GL captures.

### Backend Switch Execution

- Backend toggles are driven by `Z3DGlobalParameters::renderBackend`; `Z3DRenderingEngine` listens, pauses work, and asks the compositor bridge to swap delegates/backends.
- Filters implement `onBackendSwitch` to drop backend-specific caches, request a new delegate, and immediately push their existing CPU data so GPU resources are recreated under the new backend.
- Bridge failures surface via `reportError`, prompting an automatic rollback to the previous backend with a user-visible error.


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
- [ ] Background renderer parity: finalize specialization constants for Uniform/Gradient orientations; ensure delegate wires `mode`/`orientation`/`region` consistently.
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
- [ ] Expand Vulkan compositor to provide equivalent outputs (ready targets/readback).
- [ ] Migrate engine to hold a `std::unique_ptr` to the facade and switch backend at runtime.
- [ ] Audit filters for GL leakage and continue moving API-specific ownership into renderers/compositor.
- [ ] Move UI-facing parameters out of `Z3DRendererBase` into filters, and keep the renderer base as plain rendering state updated via filter-owned signals.
- [ ] Update scratch render-target handling so both backends share common helpers via the bridge (no new abstraction layer beyond existing scratch pool).
- [ ] Renderer parameter audit: hoist persistent `ZParameter` state out of GL renderers so backend swaps don’t drop user settings (see checklist below).
  - [x] Image raycaster: sampling rate, ISO value, local MIP threshold, and compositing mode now live on `Z3DImgFilter` and are injected into the renderer.
  - [x] Volume raycaster: sampling rate, ISO value, local MIP threshold, and compositing mode now live on `Z3DImgFilter` and are injected into the renderer via delegate hooks.
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
