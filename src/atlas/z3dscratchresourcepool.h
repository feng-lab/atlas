#pragma once

#include "z3dtypes.h"
#include "zglmutils.h"
#include "zlog.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nim {

class ZVulkanScratchImage;
class ZVulkanDevice;
class ZVulkanContext;
class Z3DRenderTarget;

enum class ScratchImageUsage
{
  BlockId,
  EntryExit,
  LayerArray,
  RaycastAccumulator,
  Temp2D,
  DualDepthPeel,
  WeightedAverage,
  WeightedBlended
};

inline constexpr size_t kScratchUsageCount = 8;
static_assert(static_cast<size_t>(ScratchImageUsage::WeightedBlended) + 1 == kScratchUsageCount,
              "Scratch usage enum count must match Vulkan slot array size");

enum class ScratchImageDimension
{
  Tex2D,
  Tex2DArray
};

enum class ScratchAttachmentKind
{
  Color,
  Depth
};

enum class ScratchFormat
{
  RGBA8,
  RGBA32UI,
  RGBA32F,
  RGBA16,
  RGBA16F,
  RG32F,
  R32F,
  R16F,
  Depth24
};

struct ScratchAttachmentDesc
{
  ScratchAttachmentKind kind = ScratchAttachmentKind::Color;
  uint32_t index = 0;
  ScratchFormat format = ScratchFormat::RGBA32F;
};

inline bool operator==(const ScratchAttachmentDesc& lhs, const ScratchAttachmentDesc& rhs)
{
  return lhs.kind == rhs.kind && lhs.index == rhs.index && lhs.format == rhs.format;
}

inline bool operator!=(const ScratchAttachmentDesc& lhs, const ScratchAttachmentDesc& rhs)
{
  return !(lhs == rhs);
}

struct ScratchImageDescriptor
{
  ScratchImageUsage usage = ScratchImageUsage::Temp2D;
  ScratchImageDimension dimension = ScratchImageDimension::Tex2D;
  glm::uvec2 size{};
  uint32_t layers = 1;
  std::vector<ScratchAttachmentDesc> attachments;
};

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
    ScratchImageDescriptor descriptor;
    RenderBackend backend;
    Z3DRenderTarget* renderTarget = nullptr;
    ZVulkanScratchImage* vulkanImage = nullptr;
    uint32_t attachments = 0; // number of color attachments available
    // TODO(nim): replace with std::move_only_function once we move to C++23.
    struct Releaser
    {
      void (*call)(void*) = nullptr;
      void* payload = nullptr;

      void operator()()
      {
        if (call) {
          call(payload);
        }
      }

      void reset()
      {
        call = nullptr;
        payload = nullptr;
      }

      explicit operator bool() const
      {
        return call != nullptr;
      }

      template<typename Slot>
      static Releaser forSlot(Slot* slot)
      {
        return {[](void* ptr) {
                  static_cast<Slot*>(ptr)->inUse = false;
                },
                slot};
      }
    };

    Releaser releaser;

    // Move-only: explicit RAII ownership over a pooled slot
    RenderTargetLease()
      : backend(RenderBackend::OpenGL)
    {}
    RenderTargetLease(const RenderTargetLease&) = delete;
    RenderTargetLease& operator=(const RenderTargetLease&) = delete;
    RenderTargetLease(RenderTargetLease&& other) noexcept
      : descriptor(std::move(other.descriptor))
      , backend(other.backend)
      , renderTarget(other.renderTarget)
      , vulkanImage(other.vulkanImage)
      , attachments(other.attachments)
      , releaser(std::move(other.releaser))
    {
      other.backend = RenderBackend::OpenGL;
      other.renderTarget = nullptr;
      other.vulkanImage = nullptr;
      other.attachments = 0;
      other.releaser.reset();
    }
    RenderTargetLease& operator=(RenderTargetLease&& other) noexcept
    {
      if (this != &other) {
        release();
        descriptor = std::move(other.descriptor);
        backend = other.backend;
        renderTarget = other.renderTarget;
        vulkanImage = other.vulkanImage;
        attachments = other.attachments;
        releaser = std::move(other.releaser);
        other.backend = RenderBackend::OpenGL;
        other.renderTarget = nullptr;
        other.vulkanImage = nullptr;
        other.attachments = 0;
        other.releaser.reset();
      }
      return *this;
    }
    void release()
    {
      if (releaser) {
        releaser();
        releaser.reset();
      }
      descriptor = ScratchImageDescriptor{};
      backend = RenderBackend::OpenGL;
      renderTarget = nullptr;
      vulkanImage = nullptr;
      attachments = 0;
    }
    ~RenderTargetLease()
    {
      release();
      // VLOG(1) << "lease released";
    }
    explicit operator bool() const
    {
      return renderTarget != nullptr || vulkanImage != nullptr;
    }

    [[nodiscard]] bool hasGLRenderTarget() const
    {
      return renderTarget != nullptr;
    }

    [[nodiscard]] Z3DRenderTarget& glRenderTarget() const
    {
      CHECK(renderTarget != nullptr) << "GL render target not available for this lease";
      return *renderTarget;
    }

    [[nodiscard]] bool hasVulkanImage() const
    {
      return vulkanImage != nullptr;
    }

    [[nodiscard]] ZVulkanScratchImage& vulkanScratchImage() const
    {
      CHECK(vulkanImage != nullptr) << "Vulkan scratch image not available for this lease";
      return *vulkanImage;
    }
  };

  // Acquire a Block ID FBO sized to viewport*scale with the requested number
  // of color attachments (RGBA32UI). The pool returns a lease to a slot that can have
  // capacity >= requested; lease.attachments equals min(requested, capacity).
  // If requestedAttachments < 0, pool uses the configured default. If scale <= 0, pool
  // uses the configured default. Slots only grow (size/capacity) and never shrink until trim().
  RenderTargetLease acquireBlockIdRenderTarget(const glm::uvec2& viewport,
                                               int requestedAttachments = -1,
                                               double scale = -1.0,
                                               std::optional<RenderBackend> backend = std::nullopt);

  // Acquire an entry/exit RenderTarget with a single 2D array color attachment of 'layers' depth
  // (default 2), with specified internal color format (default GL_RGBA32F). Filters set
  // to NEAREST. No depth attachment.
  RenderTargetLease acquireEntryExitRenderTarget(const glm::uvec2& size,
                                                 uint32_t layers = 2,
                                                 ScratchFormat colorFormat = ScratchFormat::RGBA32F,
                                                 std::optional<RenderBackend> backend = std::nullopt);

  // Acquire a layer array RenderTarget with a color 2D array attachment (colorInternalFormat)
  // and a depth 2D array attachment (depthInternalFormat). Array depth equals 'layers'.
  RenderTargetLease acquireLayerArrayRenderTarget(const glm::uvec2& size,
                                                  uint32_t layers,
                                                  ScratchFormat colorFormat = ScratchFormat::RGBA16,
                                                  ScratchFormat depthFormat = ScratchFormat::Depth24,
                                                  std::optional<RenderBackend> backend = std::nullopt);

  // Acquire a raycast accumulator RenderTarget consisting of two RG color attachments.
  // Attachment 0 uses GL_RGBA16, attachment 1 uses GL_RG32F. No depth attachment is provided.
  // Intended for volume raycasting ping-pong buffers.
  RenderTargetLease acquireRaycastAccumulatorRenderTarget(const glm::uvec2& size,
                                                          std::optional<RenderBackend> backend = std::nullopt);

  // Acquire a simple 2D temp RenderTarget with a single 2D color attachment
  // (GL_TEXTURE_2D) and a 2D depth attachment. Intended for compositor passes.
  RenderTargetLease acquireTempRenderTarget2D(const glm::uvec2& size,
                                              ScratchFormat colorFormat = ScratchFormat::RGBA16,
                                              ScratchFormat depthFormat = ScratchFormat::Depth24,
                                              std::optional<RenderBackend> backend = std::nullopt);

  // Acquire compositor-specific RenderTargets.
  RenderTargetLease acquireDualDepthPeelRenderTarget(const glm::uvec2& size,
                                                     std::optional<RenderBackend> backend = std::nullopt);
  RenderTargetLease acquireWeightedAverageRenderTarget(const glm::uvec2& size,
                                                       std::optional<RenderBackend> backend = std::nullopt);
  RenderTargetLease acquireWeightedBlendedRenderTarget(const glm::uvec2& size,
                                                       std::optional<RenderBackend> backend = std::nullopt);

  void setDefaultBackend(RenderBackend backend)
  {
    m_defaultBackend = backend;
  }

  RenderBackend defaultBackend() const
  {
    return m_defaultBackend;
  }

  // Free any cached resources to reduce memory usage.
  void trim();
  void reset();

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
    ScratchImageDescriptor descriptor;
  };

  BlockIdRenderTargetSlot* acquireFreeBlockIdSlot(const glm::uvec2& size, uint32_t requiredAttachments);
  void growSlotIfNeeded(BlockIdRenderTargetSlot& slot, const ScratchImageDescriptor& descriptor);

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
    ScratchFormat colorFormat = ScratchFormat::RGBA32F;
    bool inUse = false;
    uint64_t lastUseTick = 0;
    ScratchImageDescriptor descriptor;
  };
  std::vector<std::unique_ptr<EntryExitRenderTargetSlot>> m_entryExitRenderTargetSlots;

  // Layer array RenderTarget slots (color+depth array attachments)
  struct LayerArrayRenderTargetSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    uint32_t layers = 0;
    ScratchFormat colorFormat = ScratchFormat::RGBA16;
    ScratchFormat depthFormat = ScratchFormat::Depth24;
    bool inUse = false;
    uint64_t lastUseTick = 0;
    ScratchImageDescriptor descriptor;
  };
  std::vector<std::unique_ptr<LayerArrayRenderTargetSlot>> m_layerArrayRenderTargetSlots;

  // 2D temp RenderTarget slots (single 2D color + depth attachments)
  struct Temp2DRenderTargetSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    ScratchFormat colorFormat = ScratchFormat::RGBA16;
    ScratchFormat depthFormat = ScratchFormat::Depth24;
    bool inUse = false;
    uint64_t lastUseTick = 0;
    ScratchImageDescriptor descriptor;
  };
  std::vector<std::unique_ptr<Temp2DRenderTargetSlot>> m_temp2DRenderTargetSlots;

  // Raycast accumulator slots (dual color attachments, no depth)
  struct RaycastAccumulatorSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    ScratchFormat colorFormat = ScratchFormat::RGBA16;
    ScratchFormat accumulatorFormat = ScratchFormat::RG32F;
    bool inUse = false;
    uint64_t lastUseTick = 0;
    ScratchImageDescriptor descriptor;
  };
  std::vector<std::unique_ptr<RaycastAccumulatorSlot>> m_raycastAccumulatorSlots;

  struct DualDepthPeelSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    bool inUse = false;
    uint64_t lastUseTick = 0;
    ScratchImageDescriptor descriptor;
  };
  std::vector<std::unique_ptr<DualDepthPeelSlot>> m_dualDepthPeelSlots;

  struct WeightedAverageSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    bool inUse = false;
    uint64_t lastUseTick = 0;
    ScratchImageDescriptor descriptor;
  };
  std::vector<std::unique_ptr<WeightedAverageSlot>> m_weightedAverageSlots;

  struct WeightedBlendedSlot
  {
    std::unique_ptr<Z3DRenderTarget> fbo;
    bool inUse = false;
    uint64_t lastUseTick = 0;
    ScratchImageDescriptor descriptor;
  };
  std::vector<std::unique_ptr<WeightedBlendedSlot>> m_weightedBlendedSlots;

  struct DescriptionCacheEntry
  {
    bool valid = false;
    uint64_t creationCounter = 0;
    uint64_t changeCounter = 0;
    std::string text;
  };

  mutable std::array<DescriptionCacheEntry, 2> m_descriptionCache{};

  struct VulkanScratchSlot
  {
    std::unique_ptr<ZVulkanScratchImage> image;
    ScratchImageDescriptor descriptor;
    bool inUse = false;
    uint64_t lastUseTick = 0;
  };

  struct VulkanEnvironment
  {
    std::unique_ptr<ZVulkanContext> context;
    std::unique_ptr<ZVulkanDevice> device;
  };

  VulkanEnvironment& ensureVulkanEnvironment();
  std::vector<std::unique_ptr<VulkanScratchSlot>>& vulkanSlotsForUsage(ScratchImageUsage usage);
  RenderTargetLease acquireVulkanScratchImage(const ScratchImageDescriptor& descriptor);

  std::unique_ptr<VulkanEnvironment> m_vulkanEnvironment;
  std::array<std::vector<std::unique_ptr<VulkanScratchSlot>>, kScratchUsageCount> m_vulkanSlots;
  RenderBackend m_defaultBackend = RenderBackend::OpenGL;

  static constexpr uint32_t kTrimAcquireInterval = 512;
  static constexpr uint64_t kTrimAgeTicks = 1024;

  void maybeTrimAfterAcquire();
  size_t performTrim(uint64_t ageThreshold, bool logSummary);

  template<typename Slot>
  void markSlotAcquired(Slot& slot)
  {
    slot.inUse = true;
    slot.lastUseTick = ++m_usageTick;
  }
};

} // namespace nim
