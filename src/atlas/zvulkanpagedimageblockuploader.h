#pragma once

#include "zvulkanimageblockuploader.h"

#include "zglmutils.h"
#include "zvulkanresidencymanager.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdint>
#include <vector>

namespace nim {

class ZVulkanDevice;

class ZVulkanPagedImageBlockUploader final : public ZVulkanImageBlockUploader
{
public:
  explicit ZVulkanPagedImageBlockUploader(ZVulkanDevice& device);
  ~ZVulkanPagedImageBlockUploader() override;

  void ensureImageResources(Z3DImg& image) override;

  void invalidatePageCaches(Z3DImg& image, size_t channel) override;

  size_t readAndUploadImageBlocks(Z3DImg& image,
                                  size_t channel,
                                  const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                  std::vector<glm::uvec3>* dirtyPageDirectoryCoords,
                                  std::vector<glm::uvec3>* dirtyPageTableBlockOrigins,
                                  const folly::CancellationToken& cancellationToken,
                                  ZBenchTimer& timer) override;

  ZVulkanPageCacheUploadBytes uploadPageCaches(Z3DImg& image, size_t channel, ZBenchTimer& timer) override;

  ZVulkanPageCacheUploadBytes uploadDirtyPageCaches(Z3DImg& image,
                                                    size_t channel,
                                                    const std::vector<glm::uvec3>& dirtyPageDirectoryCoords,
                                                    const std::vector<glm::uvec3>& dirtyPageTableBlockOrigins,
                                                    ZBenchTimer& timer) override;

  [[nodiscard]] ZVulkanTexture* pageDirectoryTexture(Z3DImg& image, size_t channel) override;

  [[nodiscard]] ZVulkanTexture* pageTableTexture(Z3DImg& image, size_t channel) override;

  [[nodiscard]] ZVulkanTexture* imageCacheTexture(Z3DImg& image, size_t channel) override;

private:
  struct ChannelResources
  {
    glm::uvec3 pageDirectorySize{0u};
    glm::uvec3 pageTableCacheSize{0u};
    std::unique_ptr<ZVulkanTexture> pageDirectory;
    std::unique_ptr<ZVulkanTexture> pageTableCache;
    bool pageDirectoryUploaded = false;
    bool pageTableCacheUploaded = false;
    uint32_t pageDirectoryPinCount = 0;
    uint32_t pageTableCachePinCount = 0;
    uint64_t pageDirectoryLastUsedTick = 0;
    uint64_t pageTableCacheLastUsedTick = 0;
  };

  struct ImageResources
  {
    bool destructionRegistered = false;
    std::vector<ChannelResources> channels;
  };

  void ensureChannelResourcesLocked(Z3DImg& image, size_t channel, ChannelResources& resources);
  void ensurePageDirectoryResident(Z3DImg& image, size_t channel, ZVulkanTexture& texture);
  void ensurePageTableResident(Z3DImg& image, size_t channel, ZVulkanTexture& texture);
  void pinMetadataTextureForActiveSubmission(Z3DImg& image, size_t channel, bool pageTable, ZVulkanTexture& texture);
  void unpinMetadataTexture(const Z3DImg* image, size_t channel, bool pageTable);

  [[nodiscard]] std::vector<ZVulkanResidencyManager::EvictionCandidate> metadataEvictionCandidates() const;
  [[nodiscard]] ZVulkanResidencyManager::ReclaimStats
  evictMetadataCandidate(const ZVulkanResidencyManager::EvictionCandidate& candidate, std::string_view reason);
  [[nodiscard]] ZVulkanResidencyManager::ResourceReport metadataMemoryReport() const;

  ImageResources& ensureImageResourcesLocked(Z3DImg& image);

  [[nodiscard]] const ImageResources* findImageResourcesLocked(const Z3DImg& image) const;

  ZVulkanDevice& m_device;
  mutable std::mutex m_mutex;
  std::unordered_map<const Z3DImg*, ImageResources> m_resources;
  std::shared_ptr<std::atomic_bool> m_aliveFlag;
  ZVulkanResidencyManager::ResourceProviderId m_metadataProviderId = 0;
  uint64_t m_usageTick = 1;
};

} // namespace nim
