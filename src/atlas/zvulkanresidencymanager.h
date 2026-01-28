#pragma once

#include "zglmutils.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nim {

class ZVulkanDevice;
class ZVulkanTexture;

// Vulkan-only GPU residency manager.
//
// Initial scope: host-backed eviction/recreate for large paged image-cache textures
// used by Z3DImg paging (R8 3D caches). This provides "driver-like" behavior for
// clients: request a texture, and the manager ensures it is resident, evicting
// other unpinned caches under memory pressure.
class ZVulkanResidencyManager final
{
public:
  enum class TextureKind : uint8_t
  {
    PagedImageCacheR8 = 0,
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

  explicit ZVulkanResidencyManager(ZVulkanDevice& device);
  ~ZVulkanResidencyManager();

  ZVulkanResidencyManager(const ZVulkanResidencyManager&) = delete;
  ZVulkanResidencyManager& operator=(const ZVulkanResidencyManager&) = delete;

  // Ensure a paged image-cache texture (R8 3D) is resident and ready for sampling.
  // The returned pointer is owned by the manager.
  [[nodiscard]] ZVulkanTexture* pagedImageCacheTexture(const void* owner, uint32_t channel, glm::uvec3 cacheSize);

  // Notify the manager that the paged image-cache contents for (owner, channel)
  // have been modified on GPU (host backup no longer matches).
  void notifyPagedImageCacheWritten(const void* owner, uint32_t channel);

  // Pin/unpin a managed texture by pointer. Pinning prevents eviction. These are
  // intended to be scoped to a GPU submission fence by the Vulkan backend.
  [[nodiscard]] bool pinIfManaged(ZVulkanTexture* texture);
  void unpinIfManaged(ZVulkanTexture* texture);

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
    TextureKey key{};
    glm::uvec3 logicalSize{0u};
    std::unique_ptr<ZVulkanTexture> texture;

    bool dirty = false;
    bool knownZero = true;
    bool hostValid = false;
    uint64_t lastUsedTick = 0;
    uint32_t pinCount = 0;
    std::vector<uint8_t> hostData;
  };

  [[nodiscard]] uint64_t effectivePagedImageCacheBudgetBytesLocked() const;
  void ensurePagedImageCacheResidentLocked(const TextureKey& key, ManagedTexture& record, glm::uvec3 cacheSize);

  [[nodiscard]] bool evictLeastRecentlyUsedLocked(const TextureKey& excludeKey);
  void evictLocked(ManagedTexture& victim);

  [[nodiscard]] ManagedTexture& ensureRecordLocked(const TextureKey& key);
  void resetRecordForSizeLocked(ManagedTexture& record, glm::uvec3 cacheSize);

  ZVulkanDevice& m_device;
  mutable std::mutex m_mutex;
  std::unordered_map<TextureKey, ManagedTexture, TextureKeyHash> m_textures;
  std::unordered_map<ZVulkanTexture*, TextureKey> m_reverse;
  uint64_t m_usageTick = 1;
};

} // namespace nim
