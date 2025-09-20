# Vulkan Migration Plan

This plan guides the migration of the Z3D OpenGL renderer to a Vulkan backend, in incremental, verifiable phases. It aims to reach feature parity with the current OpenGL implementation while keeping the application usable throughout.

## Goals

- Preserve the existing rendering feature set (camera, geometry, volumes, transparency, picking, screenshots, stereo) with Vulkan.
- Keep OpenGL running during migration; enable selecting backend for A/B comparison.
- Adopt explicit, robust resource management, validation, and repeatable shader compilation to SPIR-V.
- Maintain clean abstractions that mirror existing Z3D architecture to lower risk and churn.

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
  - Renderers: `z3d*renderer.*` (background, line, mesh, images/volumes)
  - Compositor: `z3dcompositor.*`
- Mixed (to clean):
  - `z3dfilter.*` previously included GL in header; refactor to avoid leaking GL.

Status of decoupling (done):

- `z3dport.h` no longer includes `z3dfilter.h` (avoids pulling GL transitively); forward‑declares `Z3DFilter` instead. Impl includes moved to cpp.
- Removed GL helper from `z3dfilter` base; header no longer includes `z3dgl.h`.

Progress Update — GL out of Z3DMeshFilter (Completed)

- Removed all filter-owned GL render targets and copy path from `Z3DMeshFilter`.
  - Deleted private `Z3DRenderOutputPort` members and `Z3DTextureCopyRenderer` from the filter.
  - Removed the filter-owned `Z3DTextureGlowRenderer`; kept a small set of glow parameters on the filter for UI/state only.
- Centralized glow in the compositor with a single hook:
  - Added `renderTransparentFilter(filter, target, eye)` in `Z3DCompositor` that decides glow vs normal rendering.
  - Uses pooled glow temps via `Z3DScratchResourcePool` (per-glow leases) and a depth-aware compositor shader (`m_alphaBlendRenderer`) to blend glow results, avoiding fixed-function blend pitfalls.
  - Ensures depth test and depth writes are enabled for the glow geometry prepass, and restores previous GL state after.
- OIT compatibility:
  - Glow is precomputed before OIT into a single color/depth pair and merged with any existing image pair, then passed to OIT as `imageColorTex/imageDepthTex`.
  - During OIT init/peel passes the compositor does not switch FBOs; it feeds the merged image pair into the existing OIT pipeline (DDP/WA/WB).
- Result:
  - `Z3DMeshFilter` now contains no GL calls; filters are backend-agnostic coordinators of data/parameters.
  - All GL state is owned by renderers and the compositor, matching the Vulkan migration goal.

Next refactors:

- Sweep headers included by API‑independent layers; replace GL includes with forward declarations + cpp includes.
- Keep all GL calls in compositor + renderer layers; remove GL code from filters by moving those bits into compositor.
- In engine, isolate GL readback behind a tiny helper to be matched by Vulkan compositor readback later.

## Filters Backend‑Agnostic (In Progress)

Goal: Filters own data/parameters and describe what to render; renderers/compositor own GPU resources and GL/Vulkan calls. Filters must not include GL types in headers.

Why: Enables a swappable backend without duplicating filter logic; reduces churn by confining API‑specific code to renderers/compositor.

Approach (incremental):

- Header decoupling: convert filter headers to forward‑declare renderer classes and use pointers/`pimpl` for API‑specific members; move includes to `.cpp`.
- Resource ownership move: shift FBO/texture creation/resize/attach from filters into their corresponding renderers (raycaster/slice/volume) and/or compositor scratch targets.
- Compositor hooks: add seam(s) so compositor orchestrates opaque/transparent/picking passes, not filters directly binding FBOs or toggling state.
- Output handoff: ensure filters communicate through existing API-neutral ports/parameters and avoid binding backend-specific resources directly inside filters.
- Picking: centralize picking in compositor/renderer; filters register objects and provide IDs/boxes only.

Status by filter:

- `Z3DMeshFilter`: GL calls removed; compositor controls glow. Header still includes `z3dmeshrenderer.h` (acceptable short‑term) — convert to forward decl later.
- `Z3DImgFilter`: Heavy GL in header and ctor (FBOs/textures, blend, cull). Next to tackle.
- `Z3DVolumeFilter`: Same pattern as image; follows after image.
- `Z3DSwcFilter`/`Z3DPunctaFilter`/`Z3DAnimationFilter`: Mostly renderer‑driven already; ensure headers forward‑declare renderers and push GL state to compositor.

Immediate Work Items

1) Z3DImgFilter — header decoupling
   - Replace by‑value renderer members with `std::unique_ptr<...>` and forward‑declare renderer classes in header.
   - Hide GL targets/textures behind a private `pimpl` declared in `.cpp`.
   - Move GL includes (`z3d*renderer.h`, `z3drenderport.h`, `z3drendertarget.h`, `z3dtexture.h`) from header to `.cpp`.

2) Z3DImgFilter — resource ownership move (step 1)
   - Move creation/resize/attach of `m_entryExitTarget`, `m_layerTarget`, blockID textures, and image render targets into `Z3DImgRaycasterRenderer`/`Z3DImgSliceRenderer` as internal scratch.
   - Expose minimal methods on renderers: `initTargets(size)`, `resizeTargets(size)`, `setEntryExitInfo(...)`, `getOutput(eye)`.
   - Filter calls renderer methods but does not touch GL/FBOs directly.

3) Z3DImgFilter — compositor orchestration (step 2)
   - Replace `bindTarget/clearTarget` and blend/cull toggles in the filter with a compositor call: `compositor.renderTransparentFilter(*this, eye)`.
   - Compositor chooses glow/OIT path and manages GL state; filter only sets renderer parameters and invokes renderer.

4) Z3DVolumeFilter — repeat (header decouple → resource move → compositor seam).

5) Convert remaining filters (Swc/Puncta/Animation): forward‑decl headers; push any stray GL state to compositor.

Acceptance:

- Filter headers compile without including GL types. No direct FBO/texture creation in filters. Visual parity maintained on representative scenes (opaque/transparent/picking).

Progress Update — Compositor Façade Simplification (In Progress)

- Introduced `Z3DCompositorBase`/`Z3DCompositorGLBackend` under the new façade and scaffolded `Z3DCompositorFilter` so the engine can hold an adapter instead of the GL compositor directly.
- Trimmed the shared interface to expose only CPU-side readback (`mono/left/rightReadyLocalBuffer()`); GL render targets stay backend-private and are accessed only when the canvas is using the legacy GL compositor directly.
- Reworked `Z3DRenderingEngine` screenshots/tiling to consume the compositor’s staged `Z3DLocalColorBuffer` objects, eliminating ad-hoc texture downloads and keeping the readback path API-neutral.

Upcoming Tasks (proposed order)

1) Move GL resource ownership out of image/volume filters
   - `Z3DImgFilter`/`Z3DVolumeFilter` currently create FBOs/textures (entry/exit/layer, blockID targets) directly.
   - Action: Shift creation/resize/attach into their renderers (raycaster/slice/volume), expose init/resize methods; filters set parameters and call renderers only.

2) Extract a compositor interface (`Z3DCompositorBase`)
   - Define a small abstract interface implemented by `Z3DCompositor` (GL) and to be implemented by `ZVulkanCompositor`.
   - `Z3DRenderingEngine` depends on the interface to allow backend switching.

3) Vulkan compositor skeleton
   - Add `ZVulkanCompositor` stub that mirrors the interface; start by wiring background + line rendering with the existing Vulkan renderer base.

4) Backend switch in engine
   - Add a runtime/backend setting in `Z3DRenderingEngine` to create GL or Vulkan compositor.

5) OIT in Vulkan (later)
   - Port DDP/WA/WB paths to Vulkan once the compositor scaffolding is in place.

Render Surface Port Work (to be reimagined)

- Define a lightweight descriptor/handle model for cross-API surfaces once Vulkan outputs need to flow through the network again.
- Provide explicit helpers for GL↔Vulkan format translation instead of relying on opaque `uint32_t` casts.
- Keep GL-specific headers in `.cpp` files; expose only minimal texture accessors from filters.
- When the new façade is ready, add convenience helpers and validation utilities plus unit/integration coverage.

## Detailed Migration Backlog (mirrors OpenGL pipeline)

1. **Compositor Filter Shell**
   - Introduce `Z3DCompositorFilter` derived from `Z3DBoundedFilter` that owns invalidation, progressive flags, widget plumbing, JSON I/O, and render requests.
   - The shell holds `std::unique_ptr<Z3DCompositorBase>` and delegates to either `Z3DCompositorGLBackend` or `ZVulkanCompositor` depending on runtime config.
   - Adjust `Z3DRenderingEngine` to construct the shell, keep its pointer instead of the GL compositor, and expose accessor helpers that forward to the active backend.

2. **Filter/Compositor Decoupling (No DTO layer)**
   - Continue moving GL resource ownership out of filters into renderers/compositor while keeping filter data structures unchanged.
   - Audit filters to ensure headers stay backend-agnostic and only expose existing parameter/state surfaces.

3. **Render Target Parity**
   - Implement `ZVulkanRenderTarget` (mono/left/right) with color/depth attachments, resolve images, and CPU staging buffers analogous to GL `Z3DRenderTarget`.
   - Mirror progressive ping-pong management (`m_monoCurrentTarget`, etc.) inside the Vulkan compositor while exposing ready local buffers for engine readback (render targets remain backend-private).
   - Integrate `Z3DScratchResourcePool` usage for transient buffers so allocation patterns stay identical.

4. **Progressive & Invalidation Flow**
   - Ensure the shell propagates `invalidate(State)` to the backend, toggles progressive rendering, and broker requests from `Z3DNetworkEvaluator` exactly as the GL compositor did.
   - Vulkan backend should honour mono/stereo flags even if stereo rendering initially falls back to mono.
   - Keep signal semantics (`sceneParaUpdated`, `renderingFinished`, `renderingError`) consistent across backends.

5. **Widget Groups & Parameters**
   - Populate Vulkan widget groups with real `ZParameter` instances backed by `Z3DGlobalParameters` (background/axis/fog toggles, colors, etc.).
   - Reuse existing parameter binding logic so UI controls work identically regardless of backend.

6. **Picking & Screenshot Readback**
   - Implement Vulkan picking renders + CPU readback; reuse GL helper logic for saving PNGs/hashes to avoid UI regressions.
   - Ensure local color buffers use the same layout and alignment so downstream consumers remain unchanged.

7. **Renderer Parity Milestones**
   - **Lines:** Ensure devicePixelRatio scaling, picking outputs, and progressive invalidation hooks mirror GL behaviour.
   - **Meshes:** Port material/light UBOs, depth/cull states, and transparency paths; add placeholder features (e.g., OIT) with stubs until Vulkan parity lands.
   - **Images/Volumes:** Rebuild 2D/3D pipelines around Vulkan dynamic rendering or subpasses, matching GL render target chaining.
   - Each Vulkan renderer should expose GL-compatible entry points to minimise filter churn.

8. **Backend Selection & Persistence**
   - Add `RenderBackend` enum to settings, serialize the chosen backend with docs/views, and surface in preferences/UI.
   - Provide runtime switch that tears down and recreates the compositor shell while preserving filter/back-end state where possible.

9. **Validation, Tooling, and CI Hooks**
   - Expand the existing Vulkan smoke test to hash staged buffers for representative scenes (background-only, lines, mesh sample).
   - Add optional developer script comparing GL vs Vulkan readbacks for a canned network to catch regressions early.
   - Document expected warnings (OpenCV fat binaries, etc.) so CI logs stay actionable.

### Renderer Parity Checklist

Background renderer (OpenGL: `Z3DBackgroundRenderer`, Vulkan: `ZVulkanBackgroundRenderer`)

- Mode: Uniform vs Gradient
- Gradient orientation: LeftToRight, RightToLeft, TopToBottom, BottomToTop
- Colors: first/second color, and region `{x0, xScale, y0, yScale}`
- Screen‑size awareness: uses `screen_dim_RCP` consistently

Vulkan mapping:

- Use specialization constants (constant IDs 30–34) to select mode/orientation in `background_func.glslinc`.
- Push constants carry colors and region, same as GL uniforms.

Line renderer (OpenGL: `Z3DLineRenderer`, Vulkan: `ZVulkanLineRenderer`)

- Data: `setData(std::vector<glm::vec3>*)`, `setDataColors(std::vector<glm::vec4>*)`, optional `setTexture(1D)`
- Picking colors: `setDataPickingColors(std::vector<glm::vec4>*)`
- Line width: scalar and per‑segment array; follows `sizeScale` and devicePixelRatio; 1px fallback when smooth off
- Modes: smooth wide lines, screen‑aligned, round caps; line strip vs line list
- Multisample interaction: width adjustment for MSAA modes

Vulkan mapping:

- Emulate wide lines with a quad strip (no dependence on `VK_EXT_line_rasterization`). Two shader paths:
  - Geometry‑free expansion: `wideline1.vert + wideline.frag` (preferred; Metal‑friendly)
  - Optional GS path: `wideline.vert + wideline.geom + wideline.frag` (where supported)
- Specialization constants for `USE_1DTEXTURE`, `ROUND_CAP`, `LIGHTING_ENABLED` (see `wideline_func1.glslinc`).
- Vertex attributes: p0/p1 endpoints, per‑endpoint colors, corner flags; CPU builds batches exactly as in GL non‑GS path.
- Screen‑aligned mode: same math as GL macro `LINE_SCREEN_ALIGNED` in shaders.
- Picking: separate pipeline with a minimal FS writing picking color output.
- Per‑segment width array: provide secondary attribute or storage buffer and pass as push constant when constant width; scale with `sizeScale` and devicePixelRatio.

Acceptance for parity: visual match on reference scenes for widths, caps, and smoothness; identical picking IDs.

Mesh renderer (OpenGL: `Z3DMeshRenderer`, Vulkan: `ZVulkanMeshRenderer`)

- Data: positions, normals, colors/UVs, triangle indices; front/back cull and depth settings.
- Lighting/material: same UBOs as GL. Push constants for small params.

Vulkan mapping:

- Stage mesh vertex/index data into device-local buffers with staging copies. Keep transforms/material/lighting UBOs aligned with GL.

### Backend Switch Plan

- Add an enum `RenderBackend { OpenGL, Vulkan }` and a runtime switch in `Z3DRenderingEngine`.
- Provide `ZVulkanCompositor` mirroring `Z3DCompositor` APIs invoked by the engine.
- Expose factory helpers in the engine/compositor that construct either `Z3D*` or `ZVulkan*` renderers but return a common interface (or adapters exposing the `Z3D*` methods).
- Keep all UI code using existing `Z3D*` method names; adapters translate calls to Vulkan renderers.

Minimal invasive path:

- For each Vulkan renderer, add methods named like the GL counterpart where feasible so adapters are trivial.
- Where not feasible, implement a tiny adapter class that implements the GL signature and forwards to the Vulkan object.

## Shader Strategy

## Current State Summary

- OpenGL engine is mature with:
  - Orchestration: `Z3DRenderingEngine`, `Z3DCompositor` (blends volumes + geometries), `Z3DRendererBase` (globals), primitive renderers, textures/FBOs/shaders.
  - Advanced blending (OIT variants): Dual-Depth Peeling, Weighted Average, Weighted Blended.
  - Stereo, progressive rendering, screenshots/tiling, picking.

- Vulkan scaffolding in place:
  - `ZVulkanContext`, `ZVulkanDevice`, `ZVulkanBuffer`, `ZVulkanTexture`, `ZVulkanSwapChain` (offscreen), `ZVulkanRendererBase`, `ZVulkanRenderer`.
  - Vulkan line renderer skeleton (`ZVulkanLineRenderer`).
  - GLSL sources for Vulkan shaders live at `Resources/shader/vulkan/` with `build_shaders.sh`.
  - Implemented now: `ZVulkanShader` loads SPIR-V and provides valid shader stages for pipeline creation (see Changes section below).

## Architecture Mapping

- OpenGL `Z3DContext` → Vulkan `ZVulkanContext` (instance/device/queues/command pool).
- OpenGL `Z3DRenderTarget` (FBO + attachments) → Vulkan images used with dynamic rendering via `ZVulkanSwapChain` for offscreen.
- OpenGL `Z3DShaderProgram` → Vulkan shader module + pipeline (`ZVulkanShader` + `ZVulkanPipeline`) with:
  - Push constants for fast-updating matrices/parameters.
  - UBOs for lights/fog (mapped from `Z3DRendererBase` parameters).
- OpenGL `Z3DRendererBase` → `ZVulkanRendererBase` (camera, viewport, coord transform, globals).
- Primitive renderers (lines, mesh, fonts, images/volumes) port one by one on top of `ZVulkanRenderer` base.
- `Z3DCompositor` → `ZVulkanCompositor` (new) to orchestrate Vulkan passes, render targets, transparency, and readbacks.

## Shader Strategy

- Compile Vulkan GLSL to SPIR-V offline using `glslangValidator` (via `build_shaders.sh`).
- Store outputs under `Resources/shader/vulkan/spv/` and load them at runtime using `ZVulkanShader`.
- Replace GLSL header preambles used in OpenGL (`Z3DRendererBase::generateHeader`) with:
  - Specialization constants where appropriate, or
  - Multiple SPIR-V variants or preprocessor definitions during compilation.

## OpenGL Dynamic Shaders → Vulkan

In OpenGL we inject `#version` and a set of `#define` flags per run, then compile GLSL. In Vulkan we must feed SPIR-V to the driver, but we can preserve the same flexibility without losing functionality using a staged approach:

1) Short-term (skip — no runtime compiler): Do not compile at runtime; rely on offline SPIR-V only

- Keep sources at `#version 450`. Do not inject `#version` or `#define`s at runtime.
- Replace most `#define`s with specialization constants and push constants (below).
- For interface-changing options, prebuild a small set of variants offline.

2) Medium-term: Move many `#define`s to Vulkan specialization/push constants

- Use SPIR-V Specialization Constants for compile‑time constants that don’t change interface/layout (e.g., toggles like `USE_FOG`, small numeric params like kernel sizes, max light count within bounds). Set via `vk::SpecializationInfo` during pipeline creation, avoiding shader recompiles while keeping static branches.
- Use Push Constants or UBOs for runtime params (e.g., thresholds, colors, mode enums). This removes many permutations entirely and keeps pipelines stable.
- Only keep separate shader variants when the interface changes (e.g., different descriptor/binding sets, different input/outputs).

When to choose this: when we want to reduce the number of SPIR-V permutations and avoid runtime compilation, while still getting static branch performance where it matters.

3) Long-term: Prebuild a bounded set of SPIR-V variants offline

- For features that truly alter interfaces (extra textures, storage buffers, geometry shader on/off), generate a small set of offline SPIR-V permutations at build time. Select the module at runtime by a compact “variant key”.
- Combine with specialization constants and push constants to keep the cross‑product manageable.

Variant Key and Caching (applies to all options)

- Define a small POD key struct for each shader family, e.g.:
  - `stage`, `usesGeometryShader`, `oitMethod`, `numLights`, `colorSpace`, etc.
- Add a stable hash function to key the compiler/cache and the pipeline cache.
- Persist both shader cache (SPIR-V) and `vk::PipelineCache` blobs to disk to amortize startup costs across runs.

Practical Mapping From OpenGL Macros

- `#version`: fix to `#version 450` (Vulkan GLSL). Remove dynamic versioning.
- Feature toggles (`#ifdef USE_*`): prefer specialization constants; fallback to push constants + dynamic `if` for non‑critical paths.
- Numeric limits (`MAX_LIGHTS`, kernel sizes): specialization constants; cap to upper bound and allocate arrays to max size.
- Interface/layout changes (extra bindings, different varyings): separate SPIR-V modules/pipelines (distinct variants).
- State toggles that are pipeline state in Vulkan (blend, depth, cull): use dynamic pipeline state; keep shaders identical.

Notes and Caveats

- Descriptor set/binding locations are part of the SPIR-V interface and must be fixed per module. Don’t try to make these dynamic; use separate variants if the set/binding layout changes.
- `#include` is not standardized in GLSL; use shaderc’s includer to resolve includes relative to `Resources/shader`.
- If shipping `shaderc` is undesirable for production, keep the runtime path for developer builds and switch to offline compilation in release builds using the same variant key logic.

Minimal API Sketch (implemented where needed)

```cpp
// In GLSL (Vulkan):
// layout(constant_id = 0) const bool USE_FOG = false;
// layout(constant_id = 1) const int  NUM_LIGHTS = 4;
// layout(push_constant) uniform Push { mat4 mvp; int mode; float threshold; } pc;

// In C++:
ZVulkanShader shader(device, ".../mesh.vert.spv", ".../mesh.frag.spv");
std::vector<vk::SpecializationMapEntry> entries = {
  { .constantID = 0, .offset = 0, .size = sizeof(uint32_t) },
  { .constantID = 1, .offset = 4, .size = sizeof(int32_t) }
};
std::array<uint32_t, 2> data = { useFog ? 1u : 0u, static_cast<uint32_t>(numLights) };
shader.setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                  entries,
                                  std::vector<uint8_t>(reinterpret_cast<uint8_t*>(data.data()),
                                                       reinterpret_cast<uint8_t*>(data.data()) + sizeof(data)));
```

## API‑Independent Compositor Interface

Introduce a minimal, engine‑facing façade to eliminate GL leakage from engine/UI code. Backends (OpenGL, Vulkan) implement this façade behind the scenes.

Interface sketch (QObject to preserve signals):

- Lifecycle/size
  - `setOutputSize(glm::uvec2)`, `outputSize() const`
  - `setRenderingRegion(double left, double right, double bottom, double top)`
  - `setProgressiveRenderingMode(bool)`
- Rendering
  - `requestRender(bool stereo)` or alternatively `process(Z3DEye)`
- Readback (engine screenshot paths)
  - `monoReadyLocalBuffer()`, `leftReadyLocalBuffer()`, `rightReadyLocalBuffer()`
- Signals
  - `sceneParaUpdated`, `renderingFinished`, `renderingError(QString)`

Notes:
- Keep interface small and tailored to existing engine usage.
- Do not expose low‑level concepts (FBOs, pipelines, descriptors) across the façade.

GL Adapter (Phase 3):
- `Z3DCompositorGLBackend` implements the façade by delegating to `Z3DCompositor` with 1:1 behavior, wiring signals through.
- No behavior change; keeps all graph/ports inside the existing GL compositor.

Vulkan Implementation (Phase 4+):
- `ZVulkanCompositor` implements the façade using Vulkan renderers/targets.
- Start with background + axis lines; then add Lines → Mesh → Images/Volumes → OIT → Picking.
- For readback, provide RGBA8 buffers identical to GL’s local color buffers.

## Migration Phases & Milestones

### Phase 0 — Foundations (DONE/ONGOING)
1. Vulkan context/device/queues/command pool (existing code).
2. SPIR-V shader module loading (DONE): `ZVulkanShader` reads `.spv` and creates `vk::ShaderModule` instances.
3. Ensure build finds Vulkan SDK; verify runtime loads (validation optional).

Deliverable: A pipeline can be created with real shader modules.

### Phase 1 — Minimal Rendering Sanity
1. Create a minimal frame: begin dynamic rendering, clear color/depth, end frame; copy to CPU for inspection.
2. Add a tiny “background” pass (fullscreen triangle if needed).

Deliverable: Verified pixel buffer readback matches clear color and window size.

### Phase 2 — Basic Primitive: Lines
1. Finalize `ZVulkanLineRenderer` with:
   - Vertex/index buffers, descriptor set (optional 1D texture color), push constants.
   - Two pipelines: with and without geometry shader (based on device features).
   - SPIR-V loading via `ZVulkanShader`.
2. Render simple lines (strip and list) over cleared background.

Deliverable: Visual parity for line thickness, round caps, smoothness.

### Phase 3 — Mesh Renderer
1. Implement a `ZVulkanMeshRenderer` with per-vertex attributes (pos/normal/color/uv).
2. Lighting (material, lights arrays) via per-frame UBO; depth test and culling.

Deliverable: Mesh rendering comparable to OpenGL path.

### Phase 4 — Image/Volume Pipeline
1. Port 2D image renderer (simple textured quads and samplers).
2. Port 3D volume slice; then basic raycaster path (start with single-channel).
3. Bind transfer functions as sampled 1D textures; add entry/exit targeting.

Deliverable: Equivalent image/volume visuals and performance within acceptable delta.

### Phase 5 — Compositor & Transparency
1. Implement `ZVulkanCompositor` to manage Vulkan render targets and passes.
2. Start with Weighted Blended OIT (simpler), then Weighted Average, then Dual-Depth Peeling.
3. Add background, axis, and composition paths; finalize readback for screenshots/tiling.

Deliverable: Full scene composition with mixed volumes + geometries.

### Phase 6 — Integration, Stereo, Picking, Progressive
1. Backend switch in `Z3DRenderingEngine` (OpenGL/Vulkan).
2. Stereo views by per-eye rendering.
3. Picking pass (color-encoded IDs) and pixel readback.
4. Progressive rendering buffers and signaling.

Deliverable: Feature parity for app usage; comparable UX.

### Phase 7 — QA, Performance, Packaging
1. Validation layers and error scanning; frame captures.
2. Performance: staging buffers, pipeline caches, descriptor reuse, MSAA/resolve if needed.
3. Finalize shader compilation pipeline; CI integration.
4. Packaging Vulkan loader/layers where appropriate.

Deliverable: Stable and performant Vulkan backend in releases.

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
 - [ ] Background renderer parity: wire specialization constants for Uniform/Gradient orientations; API shim for `mode`/`orientation`/`region`.
- [ ] Finalize `ZVulkanLineRenderer` (buffers, descriptors, pipelines, draw calls).
- [ ] Line renderer parity: smooth wide lines, round caps, screen‑aligned mode, per‑segment widths, 1D texture color, line strip batching; picking pass.
- [ ] Implement per-frame UBO for camera matrices, fog, lights; push constants for small params.
- [ ] Implement `ZVulkanMeshRenderer` with lighting.
- [ ] Implement Vulkan image/volume paths (2D, slice, raycaster v1).
- [ ] Add `ZVulkanCompositor` mirroring GL compositor functionality and transparency methods.
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
- [ ] Render-surface façade (reset after pipeline simplification):
  - [ ] Reintroduce an API-neutral lease system that can wrap scratch-pool render targets and future Vulkan surfaces without adding indirection to the hot GL path.
  - [ ] Define a minimal descriptor/handle vocabulary that covers size/formats/sample count while keeping consumers free of backend headers.
  - [ ] Update filters/compositor to adopt the redesigned façade once Vulkan surfaces are available.
- [ ] Renderer parameter audit: hoist persistent `ZParameter` state out of GL renderers so backend swaps don’t drop user settings (see checklist below).
  - [x] Image raycaster: sampling rate, ISO value, local MIP threshold, and compositing mode now live on `Z3DImgFilter` and are injected into the renderer.
  - [x] Volume raycaster: sampling rate, ISO value, local MIP threshold, and compositing mode are filter-owned (`Z3DVolumeFilter`) and passed into the renderer via setter hooks.
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
- P4: Image/volume appearance matches OpenGL outputs on sample datasets.
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

## Next Action

- Implement a minimal Vulkan “clear + CPU readback” path using dynamic rendering and the existing `vulkan/background.frag` + `pass.vert` pair.
  - Add descriptor set layouts for existing UBOs (lights/fog `set=1`, transforms/material `set=2`) and the new OIT params UBO (`set=3`) where needed.
  - Create a simple graphics pipeline via `ZVulkanPipeline` using the SPIR-V modules for background.
  - Record a command buffer that:
    1) Transitions an offscreen color+depth image to attachment layouts
    2) Begins dynamic rendering, binds the pipeline, sets viewport/scissor, pushes background PC, draws a fullscreen tri
    3) Ends rendering, transitions color image to transfer-src, copies to a staging buffer, maps and verifies pixels.
  - Log success and a small hash of the readback contents for quick validation.

- After the background smoke test, proceed to Phase 2: finalize `ZVulkanLineRenderer` and bind per-frame UBOs.
