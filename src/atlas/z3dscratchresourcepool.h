#pragma once

#include "z3dtypes.h"
#include "zglmutils.h"
#include "zlog.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nim {

class ZVulkanScratchImage;
class ZVulkanTexture;
class ZVulkanDevice;
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
  Depth24,
  Depth32F
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
  explicit Z3DScratchResourcePool(RenderBackend defaultBackend = RenderBackend::OpenGL);
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
      // Vulkan-only: defer slot release until GPU frame fence signals.
      // The pool pointer must be valid and implement scheduleDeferredRelease for the Slot type.
      template<typename Slot>
      static Releaser forVulkanSlotDeferred(Z3DScratchResourcePool* pool, Slot* slot)
      {
        struct Payload
        {
          Z3DScratchResourcePool* pool;
          void* slot;
        };
        auto* payload = new Payload{pool, slot};
        return {[](void* p) {
                  auto* pl = static_cast<Payload*>(p);
                  if (pl && pl->pool) {
                    pl->pool->scheduleDeferredRelease(static_cast<Slot*>(pl->slot));
                  }
                  delete pl;
                },
                payload};
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

    [[nodiscard]] ZVulkanTexture* colorAttachment(uint32_t index) const;

    [[nodiscard]] ZVulkanTexture* depthAttachmentTexture() const;

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
                                                  ScratchFormat depthFormat = ScratchFormat::Depth32F,
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
                                              ScratchFormat depthFormat = ScratchFormat::Depth32F,
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

  ZVulkanDevice* vulkanDevice();
  ZVulkanDevice& ensureVulkanDevice();

  // Stage 2: Defer Vulkan scratch slot reuse until the backend reaches the
  // "frame completion safe point" (after the submission fence is signalled and
  // fence-gated completion callbacks have drained). The backend reaches this
  // safe point when a frame-slot is reused, after explicit waits, and may also
  // opportunistically pump it after polling completions.
  //
  // The backend installs a scheduler that enqueues closures to run at that safe
  // point. The pool uses it to delay marking Vulkan slots as free.
  void setVulkanReleaseScheduler(std::function<void(std::function<void()>)> scheduler)
  {
    m_vulkanReleaseScheduler = std::move(scheduler);
  }

  enum class VulkanScratchReclaimMode
  {
    PollCompleted,
    WaitForIdle,
  };

  struct VulkanScratchBackingReclaimStats
  {
    uint32_t slotsEvicted = 0;
    uint64_t bytesReleased = 0;
  };

  struct VulkanScratchBackingReport
  {
    uint32_t residentSlots = 0;
    uint32_t inUseSlots = 0;
    uint32_t releasePendingSlots = 0;
    uint32_t protectedSlots = 0;
    uint64_t residentBytes = 0;
  };

  struct VulkanScratchPassEstimate
  {
    uint32_t textureCount = 0;
    uint32_t hotImageCount = 0;
    uint64_t hotTotalBytes = 0;
    uint64_t missingBytes = 0;
  };

  struct VulkanScratchBackingCandidate
  {
    ScratchImageUsage usage = ScratchImageUsage::BlockId;
    size_t slotIndex = 0;
    uint64_t residentBytes = 0;
    uint64_t lastUseTick = 0;
    uint32_t pinCount = 0;
    bool inUse = false;
    bool releasePending = false;
    std::string label;
  };

  struct VulkanScratchTextureUse
  {
    ZVulkanTexture* texture = nullptr;
    // True when the pass must observe the existing image contents before it
    // writes, samples, copies, or otherwise reads the texture. Clear/dont-care
    // render-target writes can set this false.
    bool contentsRequired = true;
  };

  class VulkanScratchProtectionScope final
  {
  public:
    VulkanScratchProtectionScope() = default;
    VulkanScratchProtectionScope(Z3DScratchResourcePool* pool, std::vector<ZVulkanTexture*> textures);
    ~VulkanScratchProtectionScope();

    VulkanScratchProtectionScope(const VulkanScratchProtectionScope&) = delete;
    VulkanScratchProtectionScope& operator=(const VulkanScratchProtectionScope&) = delete;
    VulkanScratchProtectionScope(VulkanScratchProtectionScope&& other) noexcept;
    VulkanScratchProtectionScope& operator=(VulkanScratchProtectionScope&& other) noexcept;

    [[nodiscard]] bool active() const
    {
      return m_pool != nullptr && !m_textures.empty();
    }

  private:
    void release();

    Z3DScratchResourcePool* m_pool = nullptr;
    std::vector<ZVulkanTexture*> m_textures;
  };

  // Backend hook used by the scratch pool when it needs completed Vulkan
  // submissions to reach their frame-completion safe point before deciding
  // whether a new VkImage allocation is really necessary.
  void setVulkanMemoryPressureHandler(std::function<void(VulkanScratchReclaimMode)> handler)
  {
    m_vulkanMemoryPressureHandler = std::move(handler);
  }

  // Reclaim Vulkan scratch cache. The non-blocking mode is used generally while
  // rendering; WaitForIdle is intended for explicit memory-pressure boundaries
  // such as export tile boundaries. Normal reclaim only pumps completion safe
  // points so free slots become reusable; it does not trim. Only
  // allocation-failure recovery may evict all free backing memory. Reclaim keeps
  // scratch texture object identities alive so bindless descriptor indices can
  // be reused when the slot is made resident again.
  void reclaimVulkanScratchMemory(VulkanScratchReclaimMode mode);

  // Broker-facing reclaim hook. This releases backing memory for free Vulkan
  // scratch slots only; slot objects and bindless identities remain intact and
  // are recreated on the next acquire.
  [[nodiscard]] VulkanScratchBackingReclaimStats reclaimFreeVulkanScratchBacking(std::string_view reason = {},
                                                                                 uint64_t targetBytes = 0);
  [[nodiscard]] VulkanScratchBackingReclaimStats
  reclaimColdVulkanScratchBacking(std::span<ZVulkanTexture* const> protectedTextures,
                                  std::string_view reason = {},
                                  uint64_t targetBytes = 0);
  [[nodiscard]] std::vector<VulkanScratchBackingCandidate>
  vulkanScratchBackingCandidates(bool includeLeasedScratchBacking) const;
  [[nodiscard]] VulkanScratchBackingReclaimStats
  reclaimVulkanScratchBackingCandidate(ScratchImageUsage usage, size_t slotIndex, std::string_view reason = {});
  [[nodiscard]] VulkanScratchProtectionScope protectVulkanScratchTextures(std::span<ZVulkanTexture* const> textures);
  [[nodiscard]] VulkanScratchPassEstimate
  estimateVulkanScratchTexturesForPass(std::span<const VulkanScratchTextureUse> uses) const;
  void prepareVulkanScratchTexturesForPass(std::span<const VulkanScratchTextureUse> uses, std::string_view reason = {});
  [[nodiscard]] VulkanScratchBackingReport vulkanScratchBackingReport() const;

  // Vulkan scratch slot (public because used in public method signature)
  struct VulkanScratchSlot
  {
    std::unique_ptr<ZVulkanScratchImage> image;
    ScratchImageDescriptor descriptor;
    bool inUse = false;
    bool releasePending = false;
    uint64_t lastUseTick = 0;
  };

  // Internal hook used by RenderTargetLease::Releaser for Vulkan slots
  void scheduleDeferredRelease(VulkanScratchSlot* slot);

  // Inject a shared Vulkan device owned by the rendering engine. When set, the
  // pool will use this device and will not create its own context/device.
  void setVulkanDevice(ZVulkanDevice* device)
  {
    m_externalVkDevice = device;
  }

  // Describe current memory usage for logging/diagnostics.
  // detailed=false: single-line total; detailed=true: breakdown per slot/category.
  [[nodiscard]] std::string describeMemoryUsage(bool detailed = false) const;
  [[nodiscard]] std::string describeReuseStats(bool detailed = false) const;
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
  [[nodiscard]] uint64_t reuseStatsCounter() const
  {
    return m_reuseStatsCounter;
  }

private:
  enum class ScratchAcquireKind
  {
    ExactReuse,
    CompatibleReuse,
    RetargetReuse,
    NewSlot
  };

  struct ScratchUsageLiveCounts
  {
    uint64_t slots = 0;
    uint64_t inUseSlots = 0;
    uint64_t textures = 0;
    uint64_t inUseTextures = 0;
    uint64_t residentSlots = 0;
    uint64_t residentTextures = 0;
  };

  struct ScratchUsageReuseStats
  {
    uint64_t acquisitions = 0;
    uint64_t exactReuses = 0;
    uint64_t compatibleReuses = 0;
    uint64_t retargetReuses = 0;
    uint64_t newSlots = 0;
    uint64_t residentRecreates = 0;
    uint64_t allocationRecoveries = 0;
    uint64_t budgetTrimEvents = 0;
    uint64_t budgetTrimSlots = 0;
    uint64_t evictAllEvents = 0;
    uint64_t evictAllSlots = 0;
    uint64_t peakSlots = 0;
    uint64_t peakTextures = 0;
    uint64_t peakInUseSlots = 0;
    uint64_t peakInUseTextures = 0;
    uint64_t peakResidentSlots = 0;
    uint64_t peakResidentTextures = 0;
  };

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
  uint64_t m_reuseStatsCounter = 0;

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
    ScratchFormat depthFormat = ScratchFormat::Depth32F;
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
    ScratchFormat depthFormat = ScratchFormat::Depth32F;
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
  std::array<ScratchUsageReuseStats, kScratchUsageCount> m_glReuseStats{};
  std::array<ScratchUsageReuseStats, kScratchUsageCount> m_vulkanReuseStats{};

  struct VulkanEnvironment
  {
    std::unique_ptr<ZVulkanDevice> device;
  };

  VulkanEnvironment& ensureVulkanEnvironment();
  std::vector<std::unique_ptr<VulkanScratchSlot>>& vulkanSlotsForUsage(ScratchImageUsage usage);
  RenderTargetLease acquireVulkanScratchImage(const ScratchImageDescriptor& descriptor);

  std::unique_ptr<VulkanEnvironment> m_vulkanEnvironment;
  std::array<std::vector<std::unique_ptr<VulkanScratchSlot>>, kScratchUsageCount> m_vulkanSlots;
  std::vector<ZVulkanTexture*> m_vulkanResidencyProtectedTextures;
  RenderBackend m_defaultBackend = RenderBackend::OpenGL;
  ZVulkanDevice* m_externalVkDevice = nullptr; // non-owning
  std::function<void(std::function<void()>)> m_vulkanReleaseScheduler; // installed by backend per-frame
  std::function<void(VulkanScratchReclaimMode)> m_vulkanMemoryPressureHandler;

  static constexpr uint32_t kTrimAcquireInterval = 512;
  static constexpr uint64_t kTrimAgeTicks = 1024;

  void maybeTrimAfterAcquire();
  size_t performTrim(uint64_t ageThreshold, bool logSummary);
  void pumpVulkanScratchReleases(VulkanScratchReclaimMode mode);
  void releaseVulkanScratchTextureProtections(std::span<ZVulkanTexture* const> textures);
  VulkanScratchBackingReclaimStats
  evictAllFreeVulkanSlotsWithStats(const VulkanScratchSlot* protectedSlot, bool logSummary, uint64_t targetBytes = 0);
  size_t evictAllFreeVulkanSlots(const VulkanScratchSlot* protectedSlot, bool logSummary);
  void recordScratchAcquire(RenderBackend backend,
                            const ScratchImageDescriptor& descriptor,
                            ScratchAcquireKind kind,
                            bool residentRecreated);
  void recordVulkanAllocationRecovery(ScratchImageUsage usage);
  void recordVulkanBudgetTrim(ScratchImageUsage usage, uint64_t slots);
  void recordVulkanEvictAll(ScratchImageUsage usage, uint64_t slots);
  [[nodiscard]] ScratchUsageLiveCounts scratchLiveCounts(RenderBackend backend, ScratchImageUsage usage) const;

  template<typename Slot>
  void markSlotAcquired(Slot& slot)
  {
    slot.inUse = true;
    if constexpr (requires { slot.releasePending; }) {
      slot.releasePending = false;
    }
    slot.lastUseTick = ++m_usageTick;
  }
};

} // namespace nim
