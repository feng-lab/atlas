#pragma once

#include "zglmutils.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nim {

class Z3DImg;
class ZImg;
class ZVulkanDevice;
class ZVulkanTexture;

// Vulkan-only GPU residency broker.
//
// The device owns one broker for large evictable Vulkan resources. It owns
// host-restorable managed textures directly and coordinates other resource
// classes through registered providers, giving render paths a driver-like
// contract: request resident resources, pin them for the submission, and let
// the broker reclaim cold/unpinned backing when device-local memory is tight.
class ZVulkanResidencyManager final
{
public:
  enum class TextureKind : uint8_t
  {
    PagedImageCacheR8 = 0,
    DenseVolumeR8,
    DenseImage2DR8,
  };

  enum class ResourceClass : uint8_t
  {
    TransientUploadPage,
    ScratchBacking,
    StaticGeometry,
    PagedImageMetadataTexture,
    DenseImageTexture,
    DenseVolumeTexture,
    PagedImageCacheR8,
    ReadbackStaging,
    PersistentCompositorTarget,
  };

  struct TextureKey
  {
    const void* owner = nullptr;
    TextureKind kind = TextureKind::PagedImageCacheR8;
    uint32_t index = 0;

    bool operator==(const TextureKey& other) const
    {
      return owner == other.owner && kind == other.kind && index == other.index;
    }
  };

  struct ReclaimStats
  {
    uint32_t resourcesReleased = 0;
    uint64_t bytesReleased = 0;

    void add(const ReclaimStats& other)
    {
      resourcesReleased += other.resourcesReleased;
      bytesReleased += other.bytesReleased;
    }
  };

  enum class ReclaimScope : uint8_t
  {
    PressureLadder,
    RequestedClassOnly,
  };

  struct ReclaimRequest
  {
    ResourceClass requestClass = ResourceClass::PersistentCompositorTarget;
    uint64_t requestedBytes = 0;
    bool force = false;
    std::string_view reason{};
    ReclaimScope scope = ReclaimScope::PressureLadder;
  };

  struct AllocationPressure
  {
    uint64_t usageBytes = 0;
    uint64_t budgetBytes = 0;
    uint64_t allocationBytes = 0;
    uint64_t reclaimBytes = 0;

    [[nodiscard]] bool needsReclaim() const
    {
      return reclaimBytes > 0u;
    }
  };

  struct ResourceReport
  {
    ResourceClass resourceClass = ResourceClass::PersistentCompositorTarget;
    uint32_t residentObjects = 0;
    uint32_t pinnedObjects = 0;
    uint64_t residentBytes = 0;
    std::string label;
  };

  struct EvictionCandidate
  {
    ResourceClass resourceClass = ResourceClass::PersistentCompositorTarget;
    uint32_t priority = 0;
    uint64_t residentBytes = 0;
    uint64_t lastUsedEpoch = 0;
    uint32_t pinCount = 0;
    bool restoreAvailable = true;
    uint64_t userKey0 = 0;
    uint64_t userKey1 = 0;
    std::string label;
  };

  struct ResourceProvider
  {
    ResourceClass resourceClass = ResourceClass::PersistentCompositorTarget;
    uint32_t priority = 0;
    const void* owner = nullptr;
    std::string label;
    std::function<ReclaimStats(const ReclaimRequest&)> reclaim;
    std::function<std::vector<EvictionCandidate>(const ReclaimRequest&)> collectCandidates;
    std::function<ReclaimStats(const EvictionCandidate&, const ReclaimRequest&)> evictCandidate;
    std::function<ResourceReport()> report;
  };

  using ResourceProviderId = uint64_t;

  class TextureProtectionScope final
  {
  public:
    TextureProtectionScope() = default;
    TextureProtectionScope(ZVulkanResidencyManager* manager, std::vector<ZVulkanTexture*> textures);
    ~TextureProtectionScope();

    TextureProtectionScope(const TextureProtectionScope&) = delete;
    TextureProtectionScope& operator=(const TextureProtectionScope&) = delete;
    TextureProtectionScope(TextureProtectionScope&& other) noexcept;
    TextureProtectionScope& operator=(TextureProtectionScope&& other) noexcept;

  private:
    void release();

    ZVulkanResidencyManager* m_manager = nullptr;
    std::vector<ZVulkanTexture*> m_textures;
  };

  explicit ZVulkanResidencyManager(ZVulkanDevice& device);
  ~ZVulkanResidencyManager();

  ZVulkanResidencyManager(const ZVulkanResidencyManager&) = delete;
  ZVulkanResidencyManager& operator=(const ZVulkanResidencyManager&) = delete;

  // Ensure a paged image-cache texture (R8 3D) is resident and ready for sampling.
  // The returned pointer is owned by the manager.
  [[nodiscard]] ZVulkanTexture* pagedImageCacheTexture(Z3DImg& owner, uint32_t channel, glm::uvec3 cacheSize);

  // Record an image-cache block upload in the CPU shadow used to restore paged
  // cache backing after Vulkan memory-pressure eviction.
  void recordPagedImageCacheBlockUpload(Z3DImg& owner,
                                        uint32_t channel,
                                        const glm::uvec4& pageTableEntryKey,
                                        glm::uvec3 extent,
                                        const void* data,
                                        size_t size);

  void recordPagedImageCacheBlockUpload(Z3DImg& owner,
                                        uint32_t channel,
                                        const glm::uvec4& pageTableEntryKey,
                                        glm::uvec3 extent,
                                        std::shared_ptr<const ZImg> imageBlock);

  void recordPagedImageCacheBlockRestoredFromShadow(Z3DImg& owner,
                                                    uint32_t channel,
                                                    const glm::uvec4& pageTableEntryKey,
                                                    glm::uvec3 extent);

  [[nodiscard]] bool copyPagedImageCacheBlockShadow(Z3DImg& owner,
                                                    uint32_t channel,
                                                    const glm::uvec4& pageTableEntryKey,
                                                    glm::uvec3 extent,
                                                    std::vector<uint8_t>& out);

  // Ensure a dense, host-backed R8 3D volume texture is resident. The returned
  // pointer is owned by the manager and remains stable across eviction/recreate
  // cycles for the same owner/channel/extent.
  [[nodiscard]] ZVulkanTexture*
  denseVolumeTexture(Z3DImg& owner, uint32_t channel, std::shared_ptr<const ZImg> image, uint64_t generation);

  // Ensure a host-backed R8 2D image texture is resident. This uses the same
  // broker-managed lifetime as dense volume textures, so 2D image rendering is
  // not a separate always-resident path.
  [[nodiscard]] ZVulkanTexture*
  denseImage2DTexture(Z3DImg& owner, uint32_t channel, std::shared_ptr<const ZImg> image, uint64_t generation);

  // Pin/unpin a managed texture by pointer. Pinning prevents eviction. These are
  // intended to be scoped to a GPU submission fence by the Vulkan backend.
  [[nodiscard]] bool pinIfManaged(ZVulkanTexture* texture);
  void unpinIfManaged(ZVulkanTexture* texture);

  // Temporarily protect managed textures while assembling a submission hot set.
  // Protection prevents the broker from evicting one hot texture while another
  // hot texture is being restored. It is CPU-side only; callers must still pin
  // textures for the actual GPU submission.
  [[nodiscard]] TextureProtectionScope protectTextures(std::span<ZVulkanTexture* const> textures);
  [[nodiscard]] bool ensureResidentIfManaged(ZVulkanTexture* texture, std::string_view reason);

  // Last-resort memory-pressure hook for non-texture allocation failures. Callers
  // must have reached a point where unpinned textures are not referenced by
  // in-flight submissions.
  [[nodiscard]] ReclaimStats reclaimMemory(const ReclaimRequest& request);

  [[nodiscard]] ResourceProviderId registerResourceProvider(ResourceProvider provider);
  void unregisterResourceProvider(ResourceProviderId id);

  [[nodiscard]] std::vector<ResourceReport> memoryReports() const;
  [[nodiscard]] std::string describeMemoryByClass() const;
  [[nodiscard]] std::string describeMemoryByLabel() const;
  [[nodiscard]] uint64_t effectiveBrokerBudgetBytes() const;
  [[nodiscard]] bool strictBudgetActive() const;
  [[nodiscard]] AllocationPressure allocationPressureFor(uint64_t allocationBytes) const;

  [[nodiscard]] static std::string_view resourceClassName(ResourceClass resourceClass);
  [[nodiscard]] static bool resourceClassCountsAgainstStrictBudget(ResourceClass resourceClass);

  // Release all managed textures belonging to owner. Intended to be called from
  // owner destruction callbacks.
  void releaseOwner(const void* owner);

private:
  struct TextureKeyHash
  {
    size_t operator()(const TextureKey& key) const noexcept;
  };

  struct ManagedTexture
  {
    struct PagedBlockKey
    {
      uint32_t x = 0;
      uint32_t y = 0;
      uint32_t z = 0;
      uint32_t level = 0;

      bool operator==(const PagedBlockKey& other) const
      {
        return x == other.x && y == other.y && z == other.z && level == other.level;
      }
    };

    struct PagedBlockKeyHash
    {
      size_t operator()(const PagedBlockKey& key) const noexcept;
    };

    struct HostBlockShadow
    {
      glm::uvec3 extent{0u};
      std::vector<uint8_t> data;
      std::shared_ptr<const ZImg> imageBlock;
    };

    TextureKey key{};
    glm::uvec3 logicalSize{0u};
    std::unique_ptr<ZVulkanTexture> texture;

    std::shared_ptr<const ZImg> sourceImage;
    uint64_t sourceGeneration = 0;
    bool dirty = false;
    bool knownZero = true;
    bool hostValid = false;
    uint64_t lastUsedTick = 0;
    uint32_t pinCount = 0;
    std::unordered_map<PagedBlockKey, HostBlockShadow, PagedBlockKeyHash> hostBlocks;
  };

  struct ResourceProviderRecord
  {
    ResourceProvider provider;
    uint32_t refCount = 1;
  };

  [[nodiscard]] uint64_t effectiveResidencyBudgetBytesLocked() const;
  [[nodiscard]] uint64_t managedResidentBytesLocked() const;
  void ensurePagedImageCacheResidentLocked(const TextureKey& key, ManagedTexture& record, glm::uvec3 cacheSize);
  void ensureDenseImageResidentLocked(const TextureKey& key,
                                      ManagedTexture& record,
                                      std::shared_ptr<const ZImg> image,
                                      uint64_t generation);

  void evictForBudgetLocked(const TextureKey& excludeKey,
                            ResourceClass requestClass,
                            uint64_t bytesNeeded,
                            std::string_view reason);
  void enforceBudgetAfterManagedAllocationLocked(ResourceClass requestClass,
                                                 uint64_t allocationBytes,
                                                 std::string_view reason) const;
  [[nodiscard]] uint64_t evictLeastRecentlyUsedLocked(const TextureKey& excludeKey);
  void evictLocked(ManagedTexture& victim);
  [[nodiscard]] bool textureProtectedLocked(const ManagedTexture& record) const;
  void releaseTextureProtections(std::span<ZVulkanTexture* const> textures);
  [[nodiscard]] ReclaimStats
  reclaimManagedTexturesLocked(ResourceClass resourceClass, uint64_t targetBytes, std::string_view reason);
  [[nodiscard]] ResourceReport managedTextureReportLocked(ResourceClass resourceClass) const;

  [[nodiscard]] ManagedTexture& ensureRecordLocked(const TextureKey& key);
  void resetRecordForSizeLocked(ManagedTexture& record, glm::uvec3 cacheSize);
  void registerImageOwnerDestructionCallbackLocked(Z3DImg& owner);
  [[nodiscard]] AllocationPressure allocationPressureForLocked(uint64_t allocationBytes) const;
  void
  reclaimBeforeManagedTextureAllocation(ResourceClass requestClass, uint64_t allocationBytes, std::string_view reason);

  ZVulkanDevice& m_device;
  std::shared_ptr<std::atomic_bool> m_aliveFlag;
  mutable std::mutex m_mutex;
  std::unordered_map<TextureKey, ManagedTexture, TextureKeyHash> m_textures;
  std::unordered_map<ZVulkanTexture*, TextureKey> m_reverse;
  std::unordered_map<ZVulkanTexture*, uint32_t> m_protectedTextures;
  std::unordered_set<const void*> m_imageOwnerCallbacksRegistered;
  std::unordered_map<ResourceProviderId, ResourceProviderRecord> m_resourceProviders;
  uint64_t m_usageTick = 1;
  ResourceProviderId m_nextResourceProviderId = 1;
};

} // namespace nim
