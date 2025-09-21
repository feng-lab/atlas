#pragma once

#include "z3dgl.h"
#include "z3drendertarget.h"
#include <functional>
#include <memory>
#include <string>

namespace nim {

// Scratch resource pool focused on sharing heavy RenderTargets
//
// Usage pattern:
// - Callers acquire a temporary render target via acquire*(), which returns a
//   move-only RenderTargetLease. The lease marks the underlying slot as in-use
//   and guarantees it won't be reused by others until released (explicitly via
//   lease.release() or implicitly on destruction).
// - The pool resizes/retargets existing FBOs to match the requested shape and
//   formats; it prefers reusing the closest free slot by size to reduce churn.
// - The pool is intended for per-frame scratch usage (block IDs, layer arrays,
//   entry/exit, compositor temps). Long-lived outputs (frame buffers presented
//   to the UI) should remain owned by their components.
// - Not thread-safe: a single GL context/thread is expected. Acquire/release
//   must be performed from the rendering thread.
class Z3DScratchResourcePool
{
public:
  Z3DScratchResourcePool();
  ~Z3DScratchResourcePool();

  // RAII lease for a render target
  struct RenderTargetLease
  {
    Z3DRenderTarget* renderTarget = nullptr;
    uint32_t attachments = 0; // number of color attachments available
    std::function<void()> releaser;

    // Move-only: explicit RAII ownership over a pooled slot
    RenderTargetLease() = default;
    RenderTargetLease(const RenderTargetLease&) = delete;
    RenderTargetLease& operator=(const RenderTargetLease&) = delete;
    RenderTargetLease(RenderTargetLease&& other) noexcept
      : renderTarget(other.renderTarget)
      , attachments(other.attachments)
      , releaser(std::move(other.releaser))
    {
      other.renderTarget = nullptr;
      other.attachments = 0;
      other.releaser = nullptr;
    }
    RenderTargetLease& operator=(RenderTargetLease&& other) noexcept
    {
      if (this != &other) {
        release();
        renderTarget = other.renderTarget;
        attachments = other.attachments;
        releaser = std::move(other.releaser);
        other.renderTarget = nullptr;
        other.attachments = 0;
        other.releaser = nullptr;
      }
      return *this;
    }
    void release()
    {
      if (releaser) {
        releaser();
        releaser = nullptr;
      }
      renderTarget = nullptr;
      attachments = 0;
    }
    ~RenderTargetLease()
    {
      release();
      // VLOG(1) << "lease released";
    }
    explicit operator bool() const
    {
      return renderTarget != nullptr;
    }
  };

  // Acquire a Block ID FBO sized to viewport*scale with the requested number
  // of color attachments (RGBA32UI). The pool returns a lease to a slot that can have
  // capacity >= requested; lease.attachments equals min(requested, capacity).
  // If requestedAttachments < 0, pool uses the configured default. If scale <= 0, pool
  // uses the configured default. Slots only grow (size/capacity) and never shrink until trim().
  RenderTargetLease
  acquireBlockIdRenderTarget(const glm::uvec2& viewport, int requestedAttachments = -1, double scale = -1.0);

  // Acquire an entry/exit RenderTarget with a single 2D array color attachment of 'layers' depth
  // (default 2), with specified internal color format (default GL_RGBA32F). Filters set
  // to NEAREST. No depth attachment.
  RenderTargetLease acquireEntryExitRenderTarget(const glm::uvec2& size,
                                                 uint32_t layers = 2,
                                                 GLint colorInternalFormat = GLint(GL_RGBA32F));

  // Acquire a layer array RenderTarget with a color 2D array attachment (colorInternalFormat)
  // and a depth 2D array attachment (depthInternalFormat). Array depth equals 'layers'.
  RenderTargetLease acquireLayerArrayRenderTarget(const glm::uvec2& size,
                                                  uint32_t layers,
                                                  GLint colorInternalFormat = GLint(GL_RGBA16),
                                                  GLint depthInternalFormat = GLint(GL_DEPTH_COMPONENT24));

  // Acquire a raycast accumulator RenderTarget consisting of two RG color attachments.
  // Attachment 0 uses GL_RGBA16, attachment 1 uses GL_RG32F. No depth attachment is provided.
  // Intended for volume raycasting ping-pong buffers.
  RenderTargetLease acquireRaycastAccumulatorRenderTarget(const glm::uvec2& size);

  // Acquire a simple 2D temp RenderTarget with a single 2D color attachment
  // (GL_TEXTURE_2D) and a 2D depth attachment. Intended for compositor passes.
  RenderTargetLease acquireTempRenderTarget2D(const glm::uvec2& size,
                                              GLint colorInternalFormat = GLint(GL_RGBA16),
                                              GLint depthInternalFormat = GLint(GL_DEPTH_COMPONENT24));

  // Free any cached resources to reduce memory usage.
  void trim();

  // Current config values (from flags).
  uint32_t blockIdMaxAttachments() const;
  double blockIdScale() const;

  // Describe current memory usage for logging/diagnostics.
  // detailed=false: single-line total; detailed=true: breakdown per slot/category.
  [[nodiscard]] std::string describeMemoryUsage(bool detailed = false) const;
  // Monotonic counters that increment when slot topology changes.
  // creationCounter(): slots created
  // changeCounter(): size/attachment/layers/format changed
  [[nodiscard]] uint64_t creationCounter() const
  {
    return m_creationCounter;
  }
  [[nodiscard]] uint64_t changeCounter() const
  {
    return m_changeCounter;
  }

private:
  struct BlockIdRenderTargetSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    uint32_t attachments = 0;
    bool inUse = false;
    uint64_t lastUseTick = 0;
  };

  BlockIdRenderTargetSlot* acquireFreeBlockIdSlot(const glm::uvec2& size, uint32_t requiredAttachments);
  void growSlotIfNeeded(BlockIdRenderTargetSlot& slot, const glm::uvec2& exactSize, uint32_t requiredAttachments);

private:
  // Keep a small number of slots to allow limited concurrency and avoid thrash.
  // Store slots via unique_ptr to keep Slot* addresses stable across vector reallocations.
  std::vector<std::unique_ptr<BlockIdRenderTargetSlot>> m_blockIdRenderTargetSlots;
  uint64_t m_usageTick = 0;
  uint64_t m_creationCounter = 0;
  uint64_t m_changeCounter = 0;

  // Entry/exit RenderTarget slots
  struct EntryExitRenderTargetSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    uint32_t layers = 0;
    GLint colorFormat = GLint(GL_RGBA32F);
    bool inUse = false;
  };
  std::vector<std::unique_ptr<EntryExitRenderTargetSlot>> m_entryExitRenderTargetSlots;

  // Layer array RenderTarget slots (color+depth array attachments)
  struct LayerArrayRenderTargetSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    uint32_t layers = 0;
    GLint colorFormat = GLint(GL_RGBA16);
    GLint depthFormat = GLint(GL_DEPTH_COMPONENT24);
    bool inUse = false;
  };
  std::vector<std::unique_ptr<LayerArrayRenderTargetSlot>> m_layerArrayRenderTargetSlots;

  // 2D temp RenderTarget slots (single 2D color + depth attachments)
  struct Temp2DRenderTargetSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    GLint colorFormat = GLint(GL_RGBA16);
    GLint depthFormat = GLint(GL_DEPTH_COMPONENT24);
    bool inUse = false;
  };
  std::vector<std::unique_ptr<Temp2DRenderTargetSlot>> m_temp2DRenderTargetSlots;

  // Raycast accumulator slots (dual color attachments, no depth)
  struct RaycastAccumulatorSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    GLint colorFormat = GLint(GL_RGBA16);
    GLint accumulatorFormat = GLint(GL_RG32F);
    bool inUse = false;
  };
  std::vector<std::unique_ptr<RaycastAccumulatorSlot>> m_raycastAccumulatorSlots;
};

} // namespace nim
