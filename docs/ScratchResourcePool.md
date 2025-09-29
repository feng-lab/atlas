Scratch Resource Pool: Design and Rollout Plan

Overview

- Goal: Reduce GPU memory by pooling and sharing temporary rendering targets/textures across the renderer stack (image raycaster/slicer and compositor). Focus first on high-cost Block ID FBOs; expand to layer/entry-exit and compositor temps.
- Access: Pool lives on `Z3DGlobalParameters` for universal access through `Z3DRendererBase::globalParas()`.
- Defaults: No quality changes by default.
  - `atlas_blockid_rt_max_attachments = 8`
  - `atlas_blockid_rt_scale = 1.0`
  - Entry/exit precision unchanged (keep RGBA32F for now)
  - Expose explicit action to trim scratch memory.

Scope (Phase 1 → Phase 3)

- Phase 1 (landed/wip):
  - Remove 7 unused Block ID attachments in slicer.
  - Add a minimal scratch pool with Block ID RT acquire/release.
  - Switch slicer/raycaster Block ID passes to acquire from the pool.
- Phase 2:
  - Add layer array RT (`RGBA16` + `DEPTH24`) and entry/exit (2-layer RGBA32F) helpers.
  - Migrate image renderers to use pooled layer/entry-exit RTs.
- Phase 3:
  - Replace compositor temporaries (temp RTs, glow, DDP/WA/WB intermediates) with pool acquires.
  - Add memory budget/eviction and telemetry.

Design

- Ownership:
  - `Z3DScratchResourcePool` manages lifetime of cached `Z3DRenderTarget` instances and their attached textures.
  - Callers acquire an `RTLease` (RAII) which marks a resource in-use; release returns it to the pool.
  - Requests are expressed through `ScratchImageDescriptor` (usage, extent, layers, attachment list). Both OpenGL and Vulkan backends satisfy the same descriptor and wrap their native resources behind the lease.

- Block ID RT policy:
  - FBO with N color attachments (`GL_RGBA32UI`, NEAREST).
  - Size = ceil(viewport × scale), exact. `scale` default: `FLAGS_atlas_blockid_rt_scale` (1.0).
  - Attachment count default: `FLAGS_atlas_blockid_rt_max_attachments` (8). Slicer requests 1 explicitly.
  - Pool manages a dynamic number of slots to match demand; each free slot can be resized to the exact requested size on acquire (may grow or shrink). Attachment capacity grows as needed; shrink of attachments happens only on `trim()` (to avoid churn).

- API (current):
  - `RenderTargetLease acquireBlockIdRenderTarget(glm::uvec2 viewport, int requestedAttachments=-1, double scale=-1.0, RenderBackend backend=RenderBackend::OpenGL)`
    - Integer color attachments (`GL_RGBA32UI`, NEAREST). Size = `ceil(viewport * scale)`.
  - `RenderTargetLease acquireEntryExitRenderTarget(glm::uvec2 size, uint32_t layers=2, ScratchFormat colorFormat=ScratchFormat::RGBA32F, RenderBackend backend=RenderBackend::OpenGL)`
    - One 2D array color attachment with `layers` depth. No depth attachment, NEAREST filtering.
  - `RenderTargetLease acquireLayerArrayRenderTarget(glm::uvec2 size, uint32_t layers, ScratchFormat colorFormat=ScratchFormat::RGBA16, ScratchFormat depthFormat=ScratchFormat::Depth24, RenderBackend backend=RenderBackend::OpenGL)`
    - One 2D array color attachment and one 2D array depth attachment; array depth equals `layers`.
  - `RenderTargetLease acquireTempRenderTarget2D(glm::uvec2 size, ScratchFormat colorFormat=ScratchFormat::RGBA16, ScratchFormat depthFormat=ScratchFormat::Depth24, RenderBackend backend=RenderBackend::OpenGL)`
    - One 2D color and one 2D depth attachment (non-array). Intended for compositor/raster temp passes.
  - `void setDefaultBackend(RenderBackend backend)` / `RenderBackend defaultBackend() const` — configure the backend the pool will use when callers omit the optional backend argument.
  - `void reset()` — drop all cached slots (GL + Vulkan) and invalidate description caches; used when the rendering engine switches GPU backend.
- `void trim()` — destroys cached RTs/textures to reclaim memory.
  - `uint32_t blockIdMaxAttachments() const` / `double blockIdScale() const` — current defaults.

- Safety:
  - Single GL thread expected; pool not thread-safe.
  - Acquire marks a slot `inUse`; a different free slot is returned if available; otherwise a new slot is created. No fixed upper bound is imposed (subject to overall memory).
  - Rollout uses pooling only where use is sequential per frame (Block ID phases), so no contention.

Configuration

- Flags:
  - `--atlas_blockid_rt_max_attachments`: Max color attachments on Block ID RT (default 8).
  - `--atlas_blockid_rt_scale`: Size scale relative to viewport (default 1.0).
  - Entry/exit precision: no change (remains RGBA32F). A future flag `--atlas_entry_exit_half_float` can be added if desired.
- Auto-trim:
  - Every 512 successful acquires the pool scans for idle slots and frees those that have not been reused in the last 2048 acquires. Manual `trim()` still clears all free slots immediately.

- UI integration:
  - No direct UI hook yet; leverage the automatic tick-based trim policy once implemented.

Migration Details

- Slicer (Phase 1):
  - Removed 7 unused attachments; uses pool `acquireBlockIdRT(viewport, 1)` to render and read Block IDs.
  - Stops resizing its private Block ID RT (no full-res allocation kept around).

- Raycaster (Phase 1):
  - Uses pool `acquireBlockIdRT(viewport)` for multi-attachment Block ID pass.
  - DrawBuffers count and readback loop adapt to actual attachment count from the lease.
  - Keeps existing entry/exit precision and layer targets.

- Compositor (Phase 3):
  - Replaced `m_tempRenderTarget*` and glow intermediates with pooled acquires.
  - DDP/WA/WB remain compositor-owned for now (only one compositor lives in the engine, so pooling gives limited benefit). Can revisit later if desired.

Future Enhancements

- Memory budget + LRU eviction:
  - Track total bytes; evict least-recently used RTs when exceeding `budgetMB`.
  - Provide `setBudgetMB()`; default to ~25–35% of VRAM.

- Additional helpers:
  - `acquireLayerArrayRT(w,h,layers,colorFmt,depthFmt)`
  - `acquireEntryExitRT(w,h,halfFloat)`
  - `acquireTempRT(w,h,colorFmt,depthFmt)`
  - `acquireDDPRT(w,h)` / `acquireWeightedAverageRT(w,h)`

- Precision options (opt-in):
  - Entry/exit `RGBA16F` flag for memory saving if acceptable.
  - Block ID `RGBA16UI` when block count < 65535 (dynamic fall back to 32-bit otherwise).

File Touchpoints

- New:
  - `src/atlas/z3dscratchresourcepool.h/.cpp`
- Modified:
  - `src/atlas/z3dglobalparameters.h/.cpp` — holds pool and exposes `scratchPool()`.
  - `src/atlas/z3dimgslicerenderer.h/.cpp` — removed unused Block ID attachments; now uses pool in Block ID pass.
  - `src/atlas/z3dimgraycasterrenderer.h/.cpp` — uses pool for Block ID passes; removed direct resize of per-renderer Block ID RT.

Validation Plan

- Functional:
  - Verify block ID accumulation and progressive rendering are unchanged (IDs found, rounds converge).
  - Confirm slicer path still uploads required blocks and renders correctly.
- Memory:
  - Measure VRAM before/after at 1440p/4K.
  - Expect ≈7/8 reduction for slicer Block ID RT and sharing between slicer/raycaster (single FBO instead of two).
- Performance:
  - Ensure no extra synchronizations; pool only changes allocation site, not usage.

Renderer De-ownership Plan

- Goal: renderers no longer allocate or resize heavy FBO attachments; all temporary images come from the scratch pool.
- Raycaster:
  - Constructor: removed layer/block-id/entry-exit attachments; leases acquired only when needed.
  - ensureInternalTargets: now a no-op for pooled targets; per-eye ping-pong buffers remain unchanged.
  - Progressive 3D: added a persistent `RenderTargetLease` (`m_progressiveLayerLease`) held across rounds and released when done or on error.
  - All non-progressive compositing paths already use pooled layer-array targets.
- Slicer:
  - Constructor: removed layer/block-id attachments; leases acquired only when needed.
  - ensureInternalTargets: now a no-op for pooled targets.

Scratch Pool Simplification

- Added internal helpers to reduce duplication and prepare for compositor pooling:
  - `findClosestFreeSlot(slots, size)`: returns the best free slot by pixel delta.
  - `findClosestFreeSlotIf(slots, size, predicate)`: like above, but only considers slots matching `predicate` (e.g., internal format equality).
  - Resize FBOs inline at acquire time by calling `fbo.resize(size)` and updating the slot’s `size`.
  - Attachments for Entry/Exit, Layer Array, and Temp2D are ensured inline in the acquire functions (no helper wrappers).
  - `ensureUintColorAttachments(...)`: grow-only integer RGBA attachments (BlockID category).
- `acquireEntryExitRenderTarget`, `acquireLayerArrayRenderTarget`, and `acquireTempRenderTarget2D` now select free slots only if their existing internal formats match the requested formats; otherwise the search skips them and a new slot may be created.
- 2D array attachment policy: X/Y always resized to match exactly. If X/Y are unchanged, Z only grows (never shrinks). If X/Y change, Z is set exactly to the requested value (may shrink) since the texture is reallocated anyway.

Usage Examples

- Block IDs (slicer/raycaster):

  ```cpp
  // Request 1 attachment at current viewport size
  auto lease = global.scratchPool().acquireBlockIdRenderTarget(viewport, /*attachments*/1);
  lease.renderTarget->bind();
  // draw with glDrawBuffers({GL_COLOR_ATTACHMENT0});
  lease.renderTarget->release(); // automatic in lease destructor as well
  ```

- Entry/Exit (ray entry/exit as RGBA32F array with 2 layers):

  ```cpp
  auto lease = global.scratchPool().acquireEntryExitRenderTarget(size, /*layers*/2, ScratchFormat::RGBA32F);
  lease.renderTarget->attachSlice(0); // front-facing
  lease.renderTarget->bind();
  // render front faces
  lease.renderTarget->release();
  lease.renderTarget->attachSlice(1); // back-facing
  lease.renderTarget->bind();
  // render back faces
  lease.renderTarget->release();
  const Z3DTexture* entryExit = lease.renderTarget->attachment(GL_COLOR_ATTACHMENT0);
  ```

- Layer Array (multi-channel compositing):

  ```cpp
  auto lease = global.scratchPool().acquireLayerArrayRenderTarget(size,
                                                                  /*layers*/numChannels,
                                                                  GL_RGBA16,
                                                                  GL_DEPTH_COMPONENT24);
  for (uint32_t i = 0; i < numChannels; ++i) {
    lease.renderTarget->attachSlice(i);
    lease.renderTarget->bind();
    // render channel i
    lease.renderTarget->release();
  }
  const Z3DTexture* colorArray = lease.renderTarget->attachment(GL_COLOR_ATTACHMENT0);
  const Z3DTexture* depthArray = lease.renderTarget->attachment(GL_DEPTH_ATTACHMENT);
  ```

- 2D Temp (compositor blend/copy passes):

  ```cpp
  auto lease = global.scratchPool().acquireTempRenderTarget2D(size, ScratchFormat::RGBA16, ScratchFormat::Depth24);
  lease.renderTarget->bind();
  lease.renderTarget->clear();
  // run a full-screen pass
  lease.renderTarget->release();
  m_textureCopyRenderer.setColorTexture(lease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
  m_textureCopyRenderer.setDepthTexture(lease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
  ```

Notes

- Leases are move-only RAII: copying is disabled; moving transfers ownership. Each lease carries the `ScratchImageDescriptor` that produced it and the backend in use, so GL callers can grab `lease.glRenderTarget()` while the Vulkan path uses `lease.vulkanScratchImage()`. Releasing is automatic in the destructor but can be done explicitly via `lease.release()`.
- `Z3DRenderTarget::attachment(GLenum)` returns `nullptr` if the attachment is not present on the FBO (safe to probe).

Next Steps (Compositor)

- Replace compositor temporary render targets (temp passes, glow, DDP/WA/WB) with pooled acquires.
- Leverage the new helpers to keep acquisition code minimal and consistent.
- Optionally add a soft budget and telemetry to observe and contain scratch memory usage.
