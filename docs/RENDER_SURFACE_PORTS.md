Render Surface Ports (Historical Notes)

Overview

- The original façade (`Z3DRenderSurfacePort`) provided API-neutral leases/descriptors for sharing render surfaces between OpenGL and Vulkan paths.
- After a round of simplification we moved to `Z3DRenderOutputPort` wrappers, but those have now been removed entirely.
- Filters own their render targets directly (using persistent leases from `Z3DScratchResourcePool`), and consumers query the filter for the textures they need.
- Vulkan experiments still use compositor-internal helpers only; a future façade will be reintroduced once cross-API sharing is ready again.

Current Usage

- **OpenGL pipeline**
  - Producers: filters keep per-eye `Z3DRenderTarget` leases (acquired from the scratch pool) and bind them directly when rendering.
  - Consumers: call helpers such as `transparentTarget(eye)` / `opaqueTarget(eye)` on the filter and read the returned textures.
- **Vulkan experiments**
  - `ZVulkanCompositor` keeps its attachments internal; future façade work will define how to expose them.

Guidelines

- Keep GL headers out of consumer headers; expose `Z3DRenderTarget` accessors in `.cpp` files only.
- When the viewport changes, release/reacquire leases so the scratch pool can hand out correctly sized targets.
- Treat the old façade classes (`Z3DRenderSurfaceDescriptor`, `Z3DRenderSurfaceLease`, `Z3DRenderSurfaceHandles`) as dormant utilities until the new cross-API design lands.

Future Work

1. Redesign an API-neutral lease/descriptor system that can wrap both GL and Vulkan outputs without adding indirection to the hot GL path.
2. Update filters/compositor to adopt the new façade once Vulkan surfaces can be shared broadly.
3. Document conversion steps and provide helpers that translate façade descriptors into backend binding structs.
