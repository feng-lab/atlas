# Vulkan Migration Plan

This plan guides the migration of the Z3D OpenGL renderer to a Vulkan backend, in incremental, verifiable phases. It aims to reach feature parity with the current OpenGL implementation while keeping the application usable throughout.

## Goals

- Preserve the existing rendering feature set (camera, geometry, volumes, transparency, picking, screenshots, stereo) with Vulkan.
- Keep OpenGL running during migration; enable selecting backend for A/B comparison.
- Adopt explicit, robust resource management, validation, and repeatable shader compilation to SPIR-V.
- Maintain clean abstractions that mirror existing Z3D architecture to lower risk and churn.

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
- [ ] Minimal background clear frame using dynamic rendering, plus CPU readback test.
- [ ] Fullscreen triangle pipeline for background gradient (optional, parity with GL background renderer).
- [ ] Finalize `ZVulkanLineRenderer` (buffers, descriptors, pipelines, draw calls).
- [ ] Implement per-frame UBO for camera matrices, fog, lights; push constants for small params.
- [ ] Implement `ZVulkanMeshRenderer` with lighting.
- [ ] Implement Vulkan image/volume paths (2D, slice, raycaster v1).
- [ ] Add `ZVulkanCompositor` mirroring GL compositor functionality and transparency methods.
- [ ] Backend switch and engine integration (screenshots, tiling, stereo, picking, progressive).
- [ ] Validation, perf polishing, MSAA/resolve, packaging.

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
