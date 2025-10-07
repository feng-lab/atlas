# Vulkan Migration Plan

This plan guides the migration of the Z3D OpenGL renderer to a Vulkan backend, in incremental, verifiable phases. It aims to reach feature parity with the current OpenGL implementation while keeping the application usable throughout.

## Goals

- Preserve the existing rendering feature set (camera, geometry, volumes, transparency, picking, screenshots, stereo) with Vulkan.
- Keep OpenGL running during migration; enable selecting backend for A/B comparison.
- Adopt explicit, robust resource management, validation, and repeatable shader compilation to SPIR-V.
- Maintain clean abstractions that mirror existing Z3D architecture to lower risk and churn.
- **2025-05 update:** Z3DCompositor keeps the original OpenGL rendering path intact. A dedicated Vulkan compositor implementation is being scaffolded (`processVulkan`) and will be wired up in subsequent steps using the existing scratch-pool lease APIs.
  - Status: Vulkan compositor now gathers per-filter image layers from each Z3DImgFilter’s Vulkan scratch leases and blends them with `ZVulkanTextureBlendPipelineContext` (MIP/DepthTestBlending parity). Filters render their final per-eye outputs into their own Vulkan leases; the compositor no longer renders image batches directly to the compositor surface.
- Implementation language baseline is modern C++20; all new APIs should use standard library facilities (`std::span`, `std::variant`, etc.) rather than third-party helpers when equivalents exist.

### 2025-10 Performance Update

- Introduced device-local static buffer arenas for vertices/indices managed by the Vulkan backend. Streams uploaded to the per-frame host-visible arena can now be copied into persistent device-local buffers and rebound across frames.
- Added per-stream generation counters to payloads to enable selective restaging (e.g., only colors). Initial wiring covers meshes and spheres; other primitive renderers can adopt the same pattern.
- Guarded optional VK_EXT_vertex_input_dynamic_state: pipelines omit `eVertexInputEXT` unless supported, and `cmd.setVertexInputEXT` is only called when enabled. MoltenVK paths fall back to fixed vertex input.

### Development Notes (CRITICAL - ALWAYS FOLLOW)

These instructions are mandatory for every migration change; do not deviate from them.

- You can use `cmake --build build/Release` to verify compositor changes still compile before pushing them further.
- **Never run `git checkout`**; we might lose progress forever.
- Build Vulkan features by translating the existing GL renderer data. Avoid rewriting the GL draw paths; expose the geometry/state they already use and feed that into Vulkan.
- Dynamic rendering stays the foundation for command recording. Renderers provide backend-neutral batches (via the `enqueueRenderBatches` hook). The Vulkan path records through explicit entry points (`recordVulkanBatches`/`recordVulkanPass`) and enqueues via `renderVulkan`/`renderPickingVulkan`.

#### 2025-10 Strictness Updates

- Descriptor writes during recording now crash unless using a per-draw override set. All persistent/frame descriptor sets must be fully written before `vkCmdBeginRendering` on the current command buffer. Any violation triggers a CHECK.
- Dynamic rendering invariants are enforced: if a pipeline’s attachment formats do not match the currently open dynamic rendering segment, the backend aborts with a CHECK instead of skipping.
- Surface invariants (new): Vulkan batches must never record without attachments. A pass must set an active surface before the first append, or the batch must provide attachments explicitly. `recordVulkanBatches` applies any pending surface before beginning a Vulkan frame; missing attachments on the first append cause a CHECK that includes the pass label and shader hook type.

#### Vulkan Entry Points (separation from GL)

- OpenGL path: use `render(...)` and `renderPicking(...)` (GL immediate mode), which may begin/end frames.
- Vulkan path: always wrap work with `recordVulkanBatches(fn, label)` or `recordVulkanPass(surfaceOrLease, fn, label)` and set `collectOnly=true` while recording. Inside the block, call `renderVulkan(eye, ...)` or `renderPickingVulkan(eye, ...)` to enqueue batches only (no begin/end).
- Calling GL entry points while the backend is Vulkan is an error and will CHECK in development builds to surface misuse early.
- Dual-depth-peeling (mesh peel) blender samplers moved to set=0, bindings=3/4 to avoid collisions with `mesh_func.glslinc` samplers (bindings 0/1/2). Mesh pipeline and shader sources updated accordingly.

#### 2025-10 Invariant Policy and GL Parity

To make Vulkan failures loud and actionable while maintaining user-visible parity with OpenGL:

- GL-parity early returns (no crash):
  - Empty or intentionally skipped work: empty geometry, zero indices, no visible channels, or paging inputs not ready yet (where GL also does nothing).
  - Picking passes where the GL path also skips when picking colors are absent or size-mismatched (fonts, lines, spheres, cones, ellipsoids).

- Enforced invariants (CHECK; no silent fallback):
  - Non-empty payloads with null `renderer`.
  - Parallel array size mismatches (e.g., positions vs. texcoords/colors/pickingColors), or out-of-range index issues (DCHECK for hot loops).
  - Descriptor set layouts/descriptor sets/buffers that fail to allocate or are missing during recording.
  - Resolve/composite format contracts (e.g., WA/WB resolve must bind exactly one color attachment and no depth).
  - Vulkan-only resource usage in Vulkan paths: do not fall back to CPU uploads or GL bridges when a Vulkan handle is expected.

- Descriptor update rules:
  - Persistent/frame descriptor sets are write-once prior to recording; per-draw override sets carry volatile inputs (peels, resolves, composites). Allocation failure of an override set is fatal (CHECK).

- Debug-only (DCHECK):
  - Use for expensive assertions (e.g., index range checks) to avoid runtime cost in Release.

Rationale: Silent returns mask bugs and create visual uncertainty. This policy trades early crashes for faster diagnosis while keeping GL-parity skips intact for content that was not meant to render.

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
- Glow, texture copy, and blend helpers currently remain GL-only. Once the primitive translators are stable, document the data they need so Vulkan equivalents can be implemented without disturbing the GL path.
- The axis overlay now renders through the Vulkan compositor; future work can optimize the dedicated Vulkan path without affecting the GL implementation.
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
   - Keep GL entry points intact for GL. For Vulkan, renderers publish batch data through `enqueueRenderBatches`, which `Z3DRendererBase` consumes via `renderVulkan`/`renderPickingVulkan`.

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
  - Texture copy (colour + depth) now feeds Vulkan via `ZVulkanTextureCopyPipelineContext`, reusing the fullscreen quad geometry and discard/divide/multiply specialization constants. Inputs are Vulkan `AttachmentHandle`s from scratch-pool leases; no CPU GL↔Vulkan texture bridge is used.
  - Texture blend (dual colour/depth compositing) routes through `ZVulkanTextureBlendPipelineContext`, mapping the GL blend/priority modes onto a single specialization constant (`COMPOSE_MODE`). Inputs are Vulkan `AttachmentHandle`s; no CPU upload bridge is used.
- Texture glow (blur + compositing) executes the two-pass separable blur and final glow combine in `ZVulkanTextureGlowPipelineContext`, translating blur parameters into push constants and using Vulkan attachments end-to-end (temporary blur images are Vulkan scratch textures). No CPU upload bridge is used.
- Volume slices now render through `ZVulkanImgSlicePipelineContext`, matching the GL block-ID paging flow, per-channel layer array merge, and fast-path descriptors via the shared upload helpers.
- Volume raycaster fast-path renders through `ZVulkanImgRaycasterPipelineContext`, producing entry/exit layers and sampling transfer functions. The Vulkan path now mirrors the GL multi-channel merge, shading each channel into a layer array and resolving it via `image2d_array_compositor`; progressive paging rounds reuse the same layer aggregation so multi-channel progressive parity is available alongside the existing block-ID staging.

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
     - Payload cleanup: render payloads no longer carry OpenGL `Z3DTexture*` pointers. All inputs are expressed as `AttachmentHandle` (framebuffer attachments) or `SampledImageHandle` (sampled images). `BackendPassDesc::viewport` is the single source of truth for the render area; the old `BackendPassDesc::extent` has been removed.

  7. **Documentation & TODOs**
     - Capture remaining gaps in this plan/the issue tracker (e.g., GL fallback removal) so later work is well-scoped.

  8. **Remove CPU Bridges**
     - After the compositor emits Vulkan attachments end-to-end, delete the CPU texture upload scaffolding and confirm both Vulkan and GL builds succeed.

## Vulkan Compositor Parity Tasks — Progress (2025-10-03)

Status legend: [Done], [In‑Progress], [Todo], [Blocked]

- Per‑filter image leases in Vulkan [Done]
  - Z3DImgFilter Vulkan path writes to per‑eye leases:
    - Slices → `m_opaqueTargets[eye]` (RGBA16 + Depth24) with clear on first use.
    - DVR/2D → `m_transparentTargets[eye]` with clear on first use; bound box overlays recorded on same active surface.
  - File refs: src/atlas/z3dimgfilter.cpp:920, 1194

- Image filter copy to active surface (Vulkan) [Done]
  - `renderOpaque`/`renderTransparent` set AttachmentHandles and call `m_rendererBase.render(...)` so compositor passes can invoke them like GL.
  - File refs: src/atlas/z3dimgfilter.cpp:1132, 1163

- Compositor: geometry vs. image pass parity [Done]
  - No image‑transparent layers → single compositor pass draws geometry + image‑opaque.
  - With image‑transparent layers and OIT disabled → geometry in a pass, then image layers blended.
  - With image‑transparent layers and OIT enabled → image layers participate directly in Vulkan OIT helpers (DDP/WA/WB) alongside geometry; no post‑blend step.
  - File refs: src/atlas/z3dcompositor.cpp:1000, src/atlas/z3dcompositor.cpp:1120

- Vulkan image layer blending parity (non‑OIT) [Done]
  - Collect per‑filter AttachmentHandles (color+depth) and blend using TextureBlend renderer (DepthTest/MIP modes) with Vulkan attachments when OIT is disabled.
  - Multi‑layer merge via pairwise passes into pooled Vulkan temps, then blend over output; gated so it skips when OIT already consumed the layers.

## Descriptor Guardrails (Permanent)

- No descriptor writes while a frame is recording.
  - Global guard in `ZVulkanDescriptorSet`: during recording, persistent sets become write-once; subsequent writes are blocked and logged. Per-draw override sets remain writable.
  - Telemetry counters per frame: `descriptor_writes_while_recording` and `bound_set_rewrite_attempts` (write attempts on persistent sets after initialization).
- Volatile inputs route through per‑draw override sets only.
  - DDP peel: override set 0 binds depth/front blender inputs (with explicit sampler).
  - WA/WB resolves: override set 0 binds accum/transmittance or accum/moments inputs.
  - Glow/copy/blend: all sampled inputs use an override set per draw.
  - If override allocation fails for a required batch, skip the batch rather than mutating shared sets.
- Persistent sets are write‑once; update only UBO contents per draw.
  - Lighting/Transforms/OIT descriptors are set when the set is first allocated each frame; per‑draw updates use buffer `copyData(...)` only.
- Samplers are explicit or immutable.
  - For combined image samplers, pass an explicit sampler or use immutable samplers in the set layout. DDP peel (geometry), DDP blend/final, and WA/WB resolve layouts now use immutable samplers bound to the backend default sampler to avoid MSL sampler class issues.
- Arena lifecycle is monotonic per frame.
  - Per‑frame descriptor sets are allocated from a single pool and never individually freed; the pool is reset once after the frame fence signals. Transient override sets are cleared before the reset.
- Validation/telemetry
  - End‑of‑frame VLOG includes: segments, overrides, skipped_format_mismatch, and descriptor guardrail counters.
  - Guard logs include file/function to surface violations early during development.

Acceptance targets

- No “bound VkDescriptorSet … destroyed or updated” errors and no “_Smplr” MSL issues on MoltenVK across DDP/WA/WB/glow/copy/blend flows.
- Composite/resolve invariants are preserved: single color attachment; depth disabled.

 

- OIT parity for geometry/images [Done]
  - Dual depth peeling / weighted average / weighted blended now active for cones, ellipsoids, lines, meshes, spheres, and volume image layers on Vulkan.
  - `processVulkan` always reuses collected non‑opaque image leases when OIT is enabled so volume outputs participate in the same Vulkan OIT helpers before opaque/transparent compositing. The compositor skips the legacy post‑blend path in this case.
  - Vulkan execution stays isolated in the `renderTransparent*Vulkan` helpers, keeping the GL path untouched for reference.
  - File refs: src/atlas/z3dcompositor.cpp:1000, src/atlas/z3dcompositor.cpp:1120

- Axis overlay (Vulkan) [Done]
  - Axis renderer now mirrors the GL corner viewport + scissor handling and disables depth testing so the overlay matches GL layering.
  - File ref: src/atlas/z3dcompositor.cpp:1424

- MSAA / 2x2 supersample parity [Done]
  - Implemented 2x2 supersample parity in the Vulkan compositor by rendering the scene to a 2x viewport‐sized scratch lease and downsampling into the compositor output via `Z3DTextureCopyRenderer`.
  - Applies to background, opaque/transparent geometry, OIT paths (DDP/WA/WB), image‑layer blending, glow overlays, and axis overlay.
  - Vulkan sample count remains 1; parity uses supersampled intermediate attachments, matching the GL flow.
  - File refs: src/atlas/z3dcompositor.cpp:1006, src/atlas/z3dcompositor.cpp:1429

- Picking path (Vulkan) [In‑Progress → MVP]
  - Added Vulkan picking target acquisition via persistent RGBA8+Depth24 leases and recorded picking batches for geometry and handle overlays, with first‑on‑top composition to a dedicated picking target.
  - Implemented `savePickingBufferToImage` for Vulkan by downloading the picking attachment and saving via QImage.
  - Next: wire interactive queries to read back individual pixels for widget interactions (current change focuses on recording + save).
  - File refs: src/atlas/z3dcompositor.cpp:1468, src/atlas/z3dcompositor.cpp:257

- Screenshot/readback (Vulkan) [Todo]
  - Readback from Vulkan output attachments for screenshots (mono/stereo, tiled).

## 2025-10-04 Update

- Final-frame handoff parity [Done]
  - Vulkan compositor now downloads the final color attachment to the same CPU local buffer format as the GL path (BGRA8), swaps ready/current buffers, sets `hasNewRendering`, and emits `renderingFinished`. UI continues to render from local buffers unchanged.
  - File refs: src/atlas/z3dcompositor.cpp (end of `processVulkan`)

- Vulkan screenshots/readback [Done]
  - Added helpers to save output color/depth directly from the compositor for both backends. Engine wrappers provided for convenience.
  - Color: converts to planar RGBA with ZImg and flips Y to match GL.
  - Depth: handles D32Sfloat (normalized) and D24UnormS8.
  - File refs: src/atlas/z3dcompositor.cpp (saveOutputColorToImage/saveOutputDepthToImage),
               src/atlas/z3drenderingengine.cpp (saveCurrentFrameColor/saveCurrentFrameDepth)

- Backend switch ergonomics [Done]
  - Centralized backend switch path: `Z3DRenderingEngine::applyBackendSwitch` resets the scratch pool, sets default backend, then calls `Z3DCompositor::switchBackend` which propagates to connected filters. `Z3DImgFilter::switchRendererBackend` releases GL resources on Vulkan switch and rebuilds them on GL. This notifies renderers/data owners to release backend-specific resources.
  - Minimal prewarm on switch: `Z3DCompositor::ensureOutputTargets` acquires persistent temp render targets via the scratch pool after the backend change, which pre-creates Vulkan attachments for the compositor outputs to reduce first-frame latency.
  - File refs: src/atlas/z3drenderingengine.cpp:1499, src/atlas/z3dcompositor.cpp:1866, src/atlas/z3dcompositor.cpp:1851, src/atlas/z3dimgfilter.h:75

- CPU bridges removal (compositor paths) [Done]
  - Compositor-side Vulkan paths (texture copy/blend/glow) now consume Vulkan `AttachmentHandle`s and never stage GL textures through CPU. Verified in pipeline contexts:
    - Texture copy: src/atlas/zvulkantexturecopypipelinecontext.cpp:41
    - Texture blend: src/atlas/zvulkantextureblendpipelinecontext.cpp:62
    - Glow: src/atlas/zvulkantextureglowpipelinecontext.cpp:56
  - Note: CPU uploads may still appear in non-compositor translators (e.g., mesh texture uploads) and are tracked separately.

- Z3DImg GL resource release on backend switch [Done]
  - Added `Z3DImg::releaseGLResources()` and wired `Z3DImgFilter::switchRendererBackend` to free GL textures (channel and paging) when switching to Vulkan to reduce VRAM usage and avoid stale resources.
  - Channel textures now lazily re-create on GL demand, avoiding hard coupling.
  - File refs: src/atlas/z3dimg.cpp/.h, src/atlas/z3dimgfilter.h

- API cleanup [Done]
  - Removed `channelVolumeImage` and standardized on `channelImageShared`; call sites dereference shared pointers.
  - File refs: src/atlas/z3dimg.h/.cpp, src/atlas/zvulkanimgraycasterpipelinecontext.cpp, src/atlas/zvulkanimgslicepipelinecontext.cpp

- CPU-only LUTs [Done]
  - ZColorMap/Z3DTransferFunction no longer own GL textures. They produce CPU RGBA8 LUTs (`buildLUTBGRA8`) and expose `generation()` for cache invalidation.
  - GL renderers maintain per-renderer 1D LUT caches; Vulkan pipeline contexts build/upload 1D images on demand using a shared helper.
  - File refs: src/atlas/zcolormap.h/.cpp, src/atlas/z3dtransferfunction.h/.cpp, src/atlas/z3dimgslicerenderer.cpp, src/atlas/z3dimgraycasterrenderer.cpp, src/atlas/zvulkanlututils.h, src/atlas/zvulkanimgslicepipelinecontext.cpp, src/atlas/zvulkanimgraycasterpipelinecontext.cpp

## Vulkan Dynamic Rendering Optimization Roadmap (2025 Q4)

This roadmap replaces the ad-hoc backlog with a staged plan targeting predictable frame pacing and lower CPU/GPU overhead while keeping parity with the OpenGL path. Each stage calls out prerequisites, scope, validation, and doc/reporting impact. Stages must land sequentially; later stages assume earlier infrastructure is in place.

### Stage 0 – Instrument & Baseline (owner: Vulkan backend team, 1 sprint)

- **Prereqs:** Current Vulkan compositor builds on macOS/Linux; scratch pool stats logging available (already in place).
- **Scope:**
  - Wire GPU timestamp queries around compositor phases (background, opaque, transparency, glow, picking, readback).
  - Add per-frame CPU timers for command recording/submit in `Z3DRendererVulkanBackend`.
  - Capture reference traces (mono + stereo scenes) and archive in issue tracker for comparison after each stage.
- **Validation:** Manual run on representative scenes; store timing CSV + captured screenshots. No behavioural change expected.
- **Docs:** Update this plan with measured baselines and flag any hotspots uncovered.
- **Status (2025-10):** Vulkan backend now logs per-batch CPU timings together with GPU timestamp scopes for background, geometry (opaque/transparency), glow overlays, picking paths, supersample downsample, and final readback (`VLOG(1)` output). `collectFrameTimings` now pulls timestamp data via the device dispatcher so macOS x86_64 builds link cleanly (see `src/atlas/z3drenderervulkanbackend.cpp:895`). Use these traces as the baseline for Stage 0 comparisons; next action is to capture and archive the reference timing CSV/screenshots called out in the validation bullet.

### Stage 1 – Frame-Level Command Submission (owner: backend/runtime, 2 sprints)

- **Prereqs:** Stage 0 timings; agreement on max frames in flight (target: 2).
- **Scope:**
  - Replace per-pass single-time command buffers with a per-frame command context (command buffer + fence + semaphore pool).
  - Submit once per frame; wait on fences instead of device-wide `waitIdle`.
  - Move `ZVulkanDevice::beginSingleTimeCommands` usage to new `FrameExecutor`; retire legacy helper after migration.
- **Validation:**
  - Unit test: ensure fence wait path handles device-loss exceptions.
  - Manual: verify no deadlocks when rapidly toggling backend; confirm frame pacing improves in profiler.
- **Docs:** Record fence configuration, failure-handling policy, and any new developer tooling (e.g., `--atlas_vk_max_frames_in_flight`).
- **Status (2025-10-08):** Vulkan frames now stay inside a single command buffer per eye: `Z3DRendererBase::beginVulkanFrame`/`recordVulkanBatches` gate command recording so the compositor only submits once at `endVulkanFrame` (`src/atlas/z3drendererbase.cpp#L354`, `src/atlas/z3dcompositor.cpp#L1237`, `src/atlas/z3dcompositor.cpp#L1821`). `Z3DRendererVulkanBackend` borrows `ZVulkanFrameExecutor::ActiveFrame` instances (command buffer + fence; acquire/release semaphores reserved for WSI) for every render call (`src/atlas/z3drenderervulkanbackend.cpp`). `ZVulkanSwapChain` will submit via the same executor, signalling the shared release semaphore and exposing both sync points for presentation wiring (`src/atlas/zvulkanswapchain.cpp`).

  Regression fixes post-merge:
  - Immediate commands isolation: `ZVulkanFrameExecutor::executeImmediate` no longer reuses the in‑flight frame’s command buffer. It allocates a transient primary command buffer + fence, records, submits, and waits. This avoids accidental `reset()`/`begin()` on the active frame mid‑record, which surfaced as `vkCmdBindVertexBuffers(): was called before vkBeginCommandBuffer()` on MoltenVK (`src/atlas/zvulkanframeexecutor.cpp`).
  - Offscreen submit semaphores: In Stage 1 (no swapchain), `endRender()` does not wait on the frame’s acquire semaphore and does not export a release semaphore by default. Waiting on an unsignalled acquire semaphore caused `vkQueueSubmit ... pWaitSemaphores[] ... has no way to be signaled` followed by GPU timeout/device loss on macOS. Once WSI is integrated, plumb an explicit flag to arm `pWaitSemaphores`/`pSignalSemaphores` and thread the release semaphore to the presentation path (`src/atlas/z3drenderervulkanbackend.cpp`).

  Next:
  - Thread presentation semaphores through `ZVulkanSwapChain` once the window‑system swapchain lands; re‑enable signalling in `endRender()` behind a guarded path.
  - Add a small test scene to ensure batches always receive a valid active surface before append; log warns otherwise and skip draw (non‑fatal today).
- **Stage 1 Regression Coverage – Fence Failure Harness:**
  - Add `test/zvulkanframeexecutor_tests.cpp` with a GoogleTest fixture that provisions a lightweight headless `ZVulkanContext`/`ZVulkanDevice` (reusing the application’s context helpers but skipping swapchain creation).
  - Inject a `vk::Queue` wrapper that forwards to the real graphics queue but is able to simulate a fence failure by returning `vk::Result::eErrorDeviceLost` on the first `submit`. This can be achieved by installing a custom `vk::DispatchLoaderDynamic` with a shimmed `vkQueueSubmit` in the test build.
  - Exercise `ZVulkanFrameExecutor::beginFrame`/`markSubmitted`/`waitForCompletion` by recording a no-op command list, forcing the simulated `submit` failure, and verifying the executor logs the warning, leaves the frame marked as not in-flight, and allows the next `beginFrame` to succeed without hanging.
  - Extend the test to cover swapchain integration by creating a `ZVulkanSwapChain` against the same device, toggling `configureAcquireWait(true, stageMask)` and confirming that a simulated acquire semaphore handshake results in the release semaphore being signalled (observed via `vkGetSemaphoreCounterValue` or by waiting on the fence exposed by the executor).
  - Document the harness under `test/README.md` once added so downstream contributors know how to run the Vulkan executor regression locally (requires enabling the `ATLAS_ENABLE_VULKAN_TESTS` option and having the Vulkan SDK available).

### Stage 2 – Per-Frame Resource Recycling (owner: scratch pool/backend, 2–3 sprints)

- **Prereqs:** Stage 1 merged.
- **Scope:**
  - Introduce frame indices into `Z3DScratchResourcePool` leases; recycle temp render targets when their submit fence signals.
  - Create descriptor arena per frame; contexts pull descriptor sets from a resettable pool.
  - Centralise immutable sampler cache under `Z3DRendererVulkanBackend`.
  - Reuse shared geometry buffers for common full-screen quads (background, texture copy/blend/glow) to avoid per-context tiny VBOs.
- **Validation:**
  - Stress test with rapid OIT + volume toggles to ensure no leak and no reuse-before-signal.
  - Add logging guard that asserts descriptor pool resets exactly once per frame in debug builds.
- **Docs:** Update developer guide with new lifecycle expectations for leases/descriptor usage.

Prioritisation and Plan (Next Focus)
- Prioritised Stage: Stage 2 – Per-Frame Resource Recycling. This yields the highest architectural payoff now by reducing per-frame allocations, descriptor churn, and VRAM pressure.
- Implementation Plan (targeted, incremental):
  1) Backend descriptor arena
     - Add a resettable per-frame descriptor pool arena to `Z3DRendererVulkanBackend` and thread a lightweight handle to pipeline contexts.
     - Update contexts (line, mesh, sphere, ellipsoid, cone, background, texture copy/blend/glow, image slice/raycast) to allocate descriptor sets from the per-frame arena.
  2) Lease fence integration
     - Extend `Z3DScratchResourcePool` leases with the frame key/fence; trigger recycle after frame completion via `ZVulkanFrameExecutor::waitForCompletion`.
  3) Shared fullscreen quad VBO
     - Provide a backend-shared static quad vertex buffer and index buffer; switch background/texture* pipeline contexts to use it.
  4) Sampler/placeholder consolidation
     - Maintain a small set of default samplers (linear clamp, nearest clamp) under the backend and ensure all contexts use them.
  5) Instrumentation and guards
     - Add VLOG(1) counters for per-frame descriptor sets allocated and pooled; add debug-only CHECK that arena reset happens exactly once per frame.

Acceptance Criteria
- No per-context creation of tiny placeholder textures/samplers or fullscreen quad VBOs during steady-state rendering.
- Descriptor set allocation count stabilises across frames; per-frame reset occurs once in `endRender`.
- No resource reuse before signal; no leaks when rapidly toggling OIT modes or switching backend.
- Docs updated (this file + `docs/DEVELOPER_GUIDE.md`) to reflect the new lifecycle and APIs.

Risks and Mitigations
- Descriptor pool exhaustion: size the arena conservatively and add fallback slow-path (local pool) with a debug log.
- Cross-thread safety: ensure all allocations occur on the rendering thread; pass handles by value or immutable reference only.

Status Tracking
- Small win already applied: centralized placeholder sampling resources (shared 1x1 RGBA8 texture + linear clamp sampler) under the backend, reused by common contexts.

2025-10-05 Progress — Stage 2 (completed)
- Backend descriptor arena [Done]:
  - Per-frame descriptor arena lives in `Z3DRendererVulkanBackend`; contexts allocate via `allocateFrameDescriptorSet(layout)`.
  - Arena reset is scheduled once in `endRender()` and applied in the next `beginRender()` after the frame fence signals.
  - VLOG(1): per-frame descriptor set allocations and arena reset/lease recycle counters.
- Context conversion [Done]:
  - Background, Texture Copy, Texture Blend, Texture Glow, Line, Sphere, Ellipsoid, Cone, Mesh, Image Slice, and Image Raycaster now allocate descriptor sets from the per-frame arena. All per-context descriptor pools removed from steady state.
- Lease fence integration [Done]:
  - `Z3DScratchResourcePool` defers Vulkan scratch slot reuse until the owning frame’s fence signals. The backend installs a per-frame scheduler; deferred releases execute at the next `beginRender()`.
  - VLOG(1) includes `lease_recycle_queued`/`executed` counters per frame.
- Shared fullscreen quad [Done]:
  - Backend provides a static fullscreen quad VBO used by background + texture-based contexts (copy/blend/glow).
- Samplers [Done]:
  - Backend exposes linear-clamp and nearest-clamp samplers; contexts use backend samplers and stop creating per-context ones.

Small wins already applied:
- Centralized placeholder sampling resources: a shared 1x1 RGBA8 texture and linear clamp sampler now live under `Z3DRendererVulkanBackend` and are reused by common pipeline contexts (line, cone, sphere, ellipsoid). This avoids repeatedly creating tiny textures/samplers per context and reduces CPU overhead and VRAM churn.

### Stage 3 – Compositor Pass Graph Simplification (owner: compositor team, 2 sprints)

- Status: Done (MVP)
  - Pass graph driver (`executeCompositorPassesVulkan`) records background and geometry in one collect/submit for non‑OIT; in OIT flow, driver draws background on the scene surface and records opaque geometry into an intermediate lease (clear-only, no background) before OIT.
  - Vulkan backend coalesces dynamic rendering segments across compatible batches; background + geometry share a single `beginRendering` when targeting the same attachments.
  - Instrumentation (VLOG=1): per‑frame counters for segments begun and attachments cleared vs loaded; descriptor arena stats unchanged (Stage 2).
  - GL path unchanged; Vulkan remains offscreen; no WSI.
  - Glow handling: non‑OIT path overlays glow via the pass‑graph driver; OIT path overlays glow after OIT resolution into the scene surface.
- Next:
  - Fold self‑managed contexts (glow, slice, raycaster) under the driver without losing their intermediate passes.
  - Expose pipeline reuse/creation counters across pipeline contexts.
  - Broaden parity/soak tests and collect new baselines.
- Validation: Compare Stage 0/2 baselines and observe fewer dynamic rendering begin/end segments; stable descriptor counts; one arena reset per frame.

## Pipeline Invariants & Simplifications (Plan)

Goal: Reduce state-coupling errors and make Vulkan behavior predictable by enforcing a small set of hard invariants. Keep dynamic rendering and dynamic state, but lock in what Vulkan still expects to be fixed.

Why: Dynamic Rendering decouples render passes, and many states are dynamic, but these are fixed and must match at recording time: pipeline layout (descriptor set layouts + push constants), attachment formats/count, per‑attachment blend state (unless `independentBlend` is enabled), and image layouts. Descriptor sets bound in a command buffer must be treated as immutable for the duration of that frame.

Invariants

- Pipelines
  - Key pipelines by: (shader variant, wireframe flag, fog mode) + (vector of color formats, optional depth format).
  - Composite/resolves (background/copy/blend/glow/OIT resolves): single color attachment only, depth test/write disabled.
- Descriptor Sets
  - Never update a descriptor set after it’s bound within a frame. If bindings must change (DDP peel, glow), allocate per‑draw override sets from the per‑frame arena and keep them alive until the frame fence signals.
  - Always bind explicit samplers for combined image samplers (MoltenVK consistency).
  - Standardize bindings across contexts:
    - DDP peel (set 0): binding 0 = depth blender, binding 1 = front blender.
    - WA resolve (set 0): binding 0 = accumulation, binding 1 = moments.
    - WB resolve (set 0): binding 0 = accumulation, binding 1 = transmittance.
    - OIT params UBO (set 3, binding 0) in all OIT resolve/final and peel shaders.
- Attachments & Segments
  - Before pipeline selection/use, assert that the current dynamic rendering attachments (count + vk::Format per attachment and depth presence) match the pipeline’s creation formats.
  - Clear/load policy: first writer clears; subsequent writers load. Composite/resolve passes must never clear.
- Recording
  - Exactly one frame command buffer; pipeline contexts never begin/end frames.
  - Only begin a new dynamic rendering segment when the attachment set changes; otherwise reuse the open segment.
  - GPU timestamps only while the command buffer is recording (guarded).
- OIT
  - Unify scratch attachment ordering across contexts. DDP mesh peel uses the same set/binding conventions as cones/spheres.
  - WB final includes set 3 (OIT UBO). Init paths avoid `independentBlend` or use single‑attachment initialization.
- Features
  - `independentBlend` disabled by default. If differing per‑attachment blend is required, refactor to a single‑attachment approach or explicitly enable the feature (prefer the former for portability).

Tasks

1) Centralize descriptor set/binding constants for OIT/composites (header used by all contexts).
2) Backend: include (color formats, depth format) in pipeline key and assert formats match at batch processing time.
3) Composite pipelines: enforce single color attachment, depth off; update keys and remove depth paths.
4) Descriptor override utility: per‑draw sets for dynamic sampled inputs; track them in a per‑frame transient list to keep alive until fence.
5) OIT alignment:
   - DDP peel: align mesh bindings to (0=depth, 1=front) with explicit samplers.
   - WA resolve: (0=accumulation, 1=moments).
   - WB resolve: (0=accumulation, 1=transmittance) and set 3 UBO bound.
6) Preflight checks: add `assertFormatsMatch(batch, segment)`; log at VLOG(1) and skip batch when mismatched in release builds.
7) Instrumentation: VLOG counters when a pipeline is skipped due to format mismatch or when an override descriptor set is allocated.

Acceptance Criteria

- No descriptor update/invalid set errors across DDP/WA/WB paths.
- No layout/attachment format mismatches at pipeline creation or segment begin.
- WB final pipelines create successfully on MoltenVK (set 3 layout present); DDP peel shaders compile with explicit samplers.
- Composite passes render with single color attachment; depth disabled.
- VLOG shows fewer beginRendering segments; no “skipping segment with no attachments” in normal flows.

### Status/Progress
- Dynamic Rendering coalescing in place; VLOG per‑frame segment/counter logging wired (segments, clears/loads, pipelines, overrides, skipped_format_mismatch).
- Pipeline keys extended in ZVulkan* contexts to include colorFormats[] and optional depthFormat; reuse only on format match.
- Runtime checks: backend asserts current beginRendering formats match pipeline formats; mismatches VLOG(1) and skip batch.
- Composite/resolve passes (DDP Final, WA Final, WB Final) forced to single color attachment; depth disabled in pipelines and surfaces; OIT UBO bound at set 3.
- Standardized bindings via `src/atlas/zvulkanbindings.h`:
  - DDP peel (set 0): binding 0 = depth blender, 1 = front blender (explicit sampler)
  - WA resolve (set 0): binding 0 = accumulation, 1 = moments
  - WB resolve (set 0): binding 0 = accumulation, 1 = transmittance; set 3 = OIT UBO
- Descriptor immutability: per‑draw override sets from the per‑frame arena; retained until frame fence. Implemented for DDP peel, OIT resolves, Glow.

Risks & Performance

- Per‑draw override descriptor sets come from the per‑frame arena (O(1) alloc), kept alive until fence. Overhead is negligible and avoids expensive validation errors.
- Single‑attachment composites reduce pipeline key variety and improve cache hits.
- Format assertions fail fast, improving debuggability with minimal runtime cost.

Timeline

- Week 1: Constants header, backend format asserts, composite pipeline unification.
- Week 2: OIT binding alignment across contexts, override descriptor utility, logging, doc updates.

Test Plan (Stage 3 MVP)

- Binaries/Flags
  - Run: `atlas_app --renderer vulkan --v=1` (offscreen)
- Scenes
  - Background + opaque mesh
  - Geometry with transparency disabled (BlendDelayed/NoDepthMask)
  - Transparency modes (DDP, WA, WB) with volumes and meshes
  - Glow: enabled on one opaque and one transparent filter
- Verify
  - Visual parity vs GL and Stage 2 screenshots (color/depth; no background regression)
  - Logs show one backend begin/end per frame; fewer `beginRendering` segments (VLOG counters)
  - Attachments: first writer clears; subsequent writers load (VLOG counters)
  - Descriptor arena: stable per‑frame allocation counts; exactly one arena reset scheduled (VLOG line)
  - Supersample 2×2 path renders to intermediate, then resolves into output
  - Glow is applied exactly once per filter (no double overlay)
  - No leaks: scratch‑pool recycle counts match queued executions after fence

Deferred/Notes

- Self‑managed pipeline contexts (slice, raycaster) still own local dynamic rendering segments and will be folded into the driver in a follow‑up.
- Screenshots/readback improvements are tracked under Stage 4.

### Stage 4 – Async Readback & Picking Optimisation (owner: runtime/UI, 1–2 sprints)

- **Prereqs:** Stage 3 merged; persistent command context available.
- **Scope:**
  - Ring-buffered staging buffers (preferred) or linear images for readback: allocate N host-visible, HOST_COHERENT (and HOST_CACHED when available) buffers sized for the target format (e.g., RGBA8) and persistently map them.
  - Record readback in-frame: at the end of the per-frame command buffer, transition color attachment to TRANSFER_SRC, issue vkCmdCopyImageToBuffer into the current ring buffer, then transition back as needed. Use the frame fence (or a per-frame fence token) to know when the buffer is ready.
  - Readback handoff: UI/CPU consumption occurs after the frame fence signals. Immediate UI update is used by default (no lag knob).
  - Optional timeline semaphore path: when available and stable on the platform, replace/augment fence waits with a per-frame timeline semaphore counter for finer-grained readiness tracking. Fallback remains fence-based.
  - Row pitch control: set `bufferRowLength` to the image width (no padding) and copy RGBA8 when possible to reduce CPU swizzling; otherwise, perform a single swizzle step on CPU outside the render thread.
  - Picking batching: accumulate hover-pick requests within the same frame and service them off the most recent ready buffer rather than blocking on the GPU.
  - Defer/skip readbacks: add UI/environment toggles to disable readbacks when the output is not visible; keep a synchronous fallback for headless capture tools.
- **Validation:** Manual interactive test + automated picking regression harness. Verify:
  - No render-thread stalls; readback happens after fence signal on a worker thread.
  - Stable one-frame latency (configurable); ring buffers rotate without reuse-before-signal.
  - Data correctness (checksums on a few frames) and consistent pitches.
- **Docs:** Update USER_GUIDE with new screenshot/picking options; document environment toggles.

Implementation status (2025-10):

- End-of-frame color readback enqueued into host-visible staging buffers; UI consumes after the frame fence (default N=1).
- Single command buffer per frame preserved; copies are recorded late in-frame before `vk::raii::CommandBuffer::end()`.
- Ring-backed staging with persistent mapping; slots are not reused before the owning frame’s fence signals.
- Picking color is batched similarly; interactive hover/select reads from the most recent cached CPU buffer with a fallback to synchronous 1×1 when not yet ready.
  (No user-facing flags; immediate UI update by default.)

### Stage 5 – Deferred Enhancements (owner: shared, backlog)

- MSAA/MSRR parity explorations, adaptive volume sampling, shader specialization toggles remain backlog items after Stage 4.
- Re-evaluate once timing goals met; keep tracking in this doc.

## Validation Tracker

- Stage 0 baselines (TBD) – capture after instrumentation lands.
- Stage 1 fence/submit validation (TBD).
- Stage 2 recycling soak test (TBD).
- Stage 3 pass graph regression suite (TBD).
- Stage 4 asynchronous readback regression (TBD).
### Offscreen Rendering Policy (No Swapchain/WSI)

- Atlas renders offscreen only and does not use a swapchain/presentation. All frame outputs are produced into internal attachments, then read back for CPU-side display.
- Stage 1 semaphore policy: do not wait on acquire semaphores and do not signal release semaphores by default. These synchronization points are reserved for potential future WSI/external hand-offs. For offscreen submission, the frame fence is the single synchronization primitive used to determine completion.
- Readback policy: prefer enqueueing GPU copies to host-visible staging buffers inside the frame command buffer and consuming results after the frame fence signals. Avoid synchronous immediate submissions that block the render thread.
