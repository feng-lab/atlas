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
- Build Vulkan features by translating the existing GL renderer data. Avoid rewriting the GL draw paths; instead, expose the geometry/state they already use and feed that into Vulkan.
- Dynamic rendering stays the foundation for command recording. Renderers provide backend-neutral batches (via the `enqueueRenderBatches` hook) so the Vulkan backend can cache/create pipelines and issue draws without pre-declared render passes.

## Important Guideline (Review Before Coding)

> **Important:** Our Vulkan work now layers on top of the existing OpenGL renderers without refactoring them into a new façade.
> - Keep every GL renderer focused on its immediate-mode path. Avoid introducing façade-only abstractions or shared state that complicates the proven GL flow.
> - Add lightweight descriptor helpers that expose the data each GL renderer already consumes (geometry spans, material parameters, fixed-function state). These helpers are the sole inputs to the Vulkan translation layer.
> - Drive Vulkan pipeline contexts from those helpers via `zvulkanrenderconversions.*`, which maps Atlas state to Vulkan enums (load/store ops, polygon modes, blend state, attachment formats).
> - Validate each helper against the GL output before wiring it to Vulkan. Any discrepancy must be documented in this plan together with parity test coverage.
> - Keep documentation in sync: record newly translated renderers, required follow-ups, and known gaps so the migration backlog stays actionable.
> - **Status update (2025-04)**: `zvulkanrenderconversions.*` centralises GL→Vulkan state mapping, and the line/mesh/ellipsoid pipeline contexts now key their caches on attachment formats. GL renderers continue to render directly; the Vulkan backend consumes the translation helpers as they come online.

## Parity And Swappable Backend

To ensure the Vulkan backend is a drop‑in replacement for the OpenGL engine (files starting with `z3d*`), we will:

- Preserve public APIs and parameter semantics of the OpenGL renderers when adding Vulkan counterparts.
- Provide adapter classes or matching method names on Vulkan renderers so existing UI, frontend, and configuration code do not change.
- Expose a backend switch through `Z3DRenderingEngine`, but have the compositor orchestrate the actual swap so filters stay insulated from engine internals.

Implementation approach:

- Renderer API invariants: For each renderer family, define a short list of methods/parameters that must remain identical (names, value ranges, behavior). Vulkan renderers will expose the same signatures where feasible, or thin adapters will translate to Vulkan‑native settings.
- Parameter bridging: Reuse `Z3DGlobalParameters` as the single source of truth. Vulkan renderers consume the identical state (`camera`, fog, OIT, clip planes, `devicePixelRatio`, etc.) through `Z3DRendererBase` powered by a Vulkan backend implementation (`Z3DRendererVulkanBackend`), keeping GPU-specific code free of duplicate scene copies.
- Feature matching: When hardware features differ (e.g., wide lines), emulate in shaders/CPU so behavior stays consistent.
- Mesh parity guardrails: `Z3DMeshRenderer` is the single owner of split meshes and custom color buffers; backend code should never clone vertex data outside the `prepareMesh*` helpers. When adding new mesh features, extend those helpers instead of introducing duplicate CPU storage.


Renderer Base Status

- GL renderers keep their immediate-mode entry points. They own the GPU resources they need and call straight into OpenGL.
- `Z3DRendererBase` still wires a backend pointer so we can host alternative implementations (Vulkan) alongside GL. Until the translators land, the Vulkan backend is a stub that records command buffers using the helper data described in this plan.
- Filters should continue to pass CPU state (frame/view/scene snapshots, parameter structs) into their renderers; use the same snapshots when building translation helpers so GL and Vulkan read identical inputs.
**Compositor Integration**

- The compositor still acquires temporary targets from `Z3DScratchResourcePool`, but leases now wrap API-neutral `Z3DRenderTarget` objects. The compositor never binds FBOs directly; it asks backend utilities to begin or resolve passes using the lease.
- Blend/OIT paths become high-level intents (`beginTransparencyPass(mode, lease)`, `resolveTransparency(lease, dst)`) that backend drivers translate to GL state changes or Vulkan subpasses.
- Glow, axis, texture copy, and blend helpers currently remain GL-only. Once the primitive translators are stable, document the data they need so Vulkan equivalents can be implemented without disturbing the GL path.
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
   - The runtime `RenderBackend` toggle remains planned but not yet wired to a hot-swappable façade. For now the switch is a session-level knob that restarts the engine.
   - When we revisit live switching, the existing compositor must keep GL as the authoritative implementation and invoke Vulkan through the translation helpers. Document the required teardown/rebuild sequence before enabling the toggle in the UI.

10. **Validation, Tooling, and CI Hooks**
   - Expand the existing Vulkan smoke test to hash staged buffers for representative scenes (background-only, lines, mesh sample).
   - Add optional developer script comparing GL vs Vulkan readbacks for a canned network to catch regressions early.
   - Document expected warnings (OpenCV fat binaries, etc.) so CI logs stay actionable.
   - Encapsulate screenshot/picking readback buffers behind value-returning APIs so the filter does not touch GL ids.
   - Provide explicit `reset`/`onDeviceLost` hooks to ease future backend swaps or context loss handling.

### Translation Backlog

- **Per-renderer translators**
  - Lines: reuse the existing helper routines (`buildLinePayload`, `buildWideLineGeometry`) for both smooth and thin paths, including picking. Vulkan should consume these spans directly without re-tessellating.
  - Meshes: expose indexed vertex spans, normals, colours, and texture coordinate arrays so Vulkan uploads from CPU memory instead of GL-owned VBO helpers. Document any per-material gaps (wireframe overlays, OIT) while wiring the translator.
  - Ellipsoids & cones: surface the axis/centre/material buffers that already feed the GL upload and share them with Vulkan.
- **State propagation**
  - Confirm `RendererFrameState`, `RendererViewState`, and `RendererSceneState` carry every scalar the Vulkan backend requires (viewport matrices, fog parameters, multisample flags, `devicePixelRatio`). Extend them instead of reaching back into global singletons.
  - Keep load/store policy, attachment formats, and raster state conversions centralised in `zvulkanrenderconversions` so both backends stay aligned.
- **Validation**
  - Capture GL reference frames before switching a renderer to Vulkan and log intentional gaps here so parity debt stays visible.
  - Add CPU-side unit hooks for the translator helpers to catch regressions without needing GL contexts.
- **Implemented translators**
  - Lines, meshes, spheres, background quads, ellipsoids, and cones now publish Vulkan-ready batches. `ZVulkanSpherePipelineContext` mirrors the GL box-correction path and disables lighting during picking; `ZVulkanBackgroundPipelineContext` drives the pass shader via specialization constants and push constants; `ZVulkanConePipelineContext` covers all cap styles and reuses picking colours by toggling lighting/material state.
  - Texture copy (colour + depth) now feeds Vulkan via `ZVulkanTextureCopyPipelineContext`, reusing the fullscreen quad geometry, the discard/divide/multiply specialization constants, and staging GL textures through a CPU readback/upload bridge until native Vulkan-backed attachments are available.
  - Texture blend (dual colour/depth compositing) routes through `ZVulkanTextureBlendPipelineContext`, mapping the GL blend/priority modes onto a single specialization constant (`COMPOSE_MODE`) and uploading both layers’ textures via the same CPU bridge while we lack Vulkan-native render targets.
- Texture glow (blur + compositing) executes the two-pass separable blur and final glow combine in `ZVulkanTextureGlowPipelineContext`, translating blur parameters into push constants and reusing CPU-uploaded textures alongside internally managed Vulkan scratch images for the intermediate glow map.
- Volume slices now render through `ZVulkanImgSlicePipelineContext`, matching the GL block-ID paging flow, per-channel layer array merge, and fast-path descriptors via the shared upload helpers.
- Volume raycaster fast-path (single-channel, non-paged) renders through `ZVulkanImgRaycasterPipelineContext`, producing entry/exit layers and sampling transfer functions; progressive paging rounds remain to be ported.

### Renderer Parity Focus

- Backgrounds: verify gradient/uniform output and orientation flags match GL when the Vulkan backend consumes the shared helpers.
- Lines: cover smooth/wide, per-segment widths, textured lines, and picking IDs. Track open items (round caps, MSAA, dashed paths) here.
- Meshes: validate material/light UBOs, wireframe overlays, and transparency parity.
- Volumes & slices: slice renderer parity now verifies against GL captures (fast + paged); raycaster fast-path parity is available for single-channel DVR/MIP, with progressive accumulation and paged updates still tracked.
- Post effects (axis, glow, screenshot readback): port after primitive renderers are reliable; document any temporary fallbacks.
- **Upcoming Vulkan Compositor Port Plan**
  1. **Inventory Existing GL Flow**
     - Map each compositor phase that touches texture-copy/blend/glow (inputs, outputs, scratch usage, parameter propagation).
     - Document shader parameters and ordering so Vulkan paths stay behaviourally identical.

  2. **Extend Scratch Pool for Vulkan Surfaces**
     - Teach `Z3DScratchResourcePool` helpers (e.g. `acquireTempRenderTarget2D`) to populate Vulkan attachment handles when the default backend is Vulkan.
     - Ensure leases describe both GL and Vulkan resources; add TODOs where GL-only code remains.

  3. **Add Vulkan Render Entry Points**
     - In `Z3DRendererBase`, provide a helper that accepts Vulkan attachments and dispatches Vulkan batches without touching GL.
     - Keep the existing GL path unchanged.

  4. **Upgrade Renderers**
     - For each compositor primitive, expose `renderVulkan(...)` plus payload/batch builders that accept `AttachmentHandle`s.
     - Leave GL `render(...)` logic untouched so both paths can coexist.

  5. **Integrate in the Compositor**
     - For each compositor pass that currently binds a `Z3DRenderTarget`, add a Vulkan branch: acquire a Vulkan lease, set the active surface, call the renderer’s Vulkan entry point, and reuse Vulkan intermediates across multi-pass flows.
     - Guard with backend checks to avoid disturbing the GL path.

  6. **Pipeline Adjustments**
     - Verify all Vulkan pipeline contexts consume attachment handles exclusively, with no CPU upload fallback.
     - Audit layout transitions and descriptor usage for the new Vulkan intermediates.

  7. **Documentation & TODOs**
     - Capture remaining gaps in this plan/the issue tracker (e.g., GL fallback removal) so later work is well-scoped.

  8. **Remove CPU Bridges**
     - After the compositor emits Vulkan attachments end-to-end, delete the CPU texture upload scaffolding and confirm both Vulkan and GL builds succeed.
