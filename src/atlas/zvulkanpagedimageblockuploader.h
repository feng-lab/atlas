#pragma once

#include "zvulkanimageblockuploader.h"

#include "zglmutils.h"

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

  size_t readAndUploadImageBlocks(Z3DImg& image,
                                  size_t channel,
                                  const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                  const folly::CancellationToken& cancellationToken,
                                  ZBenchTimer& timer) override;

  void uploadPageCaches(Z3DImg& image, size_t channel, ZBenchTimer& timer) override;

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
  };

  struct ImageResources
  {
    bool destructionRegistered = false;
    std::vector<ChannelResources> channels;
  };

  void ensureChannelResourcesLocked(Z3DImg& image, size_t channel, ChannelResources& resources);

  ImageResources& ensureImageResourcesLocked(Z3DImg& image);

  [[nodiscard]] const ImageResources* findImageResourcesLocked(const Z3DImg& image) const;

  ZVulkanDevice& m_device;
  mutable std::mutex m_mutex;
  std::unordered_map<const Z3DImg*, ImageResources> m_resources;
  std::shared_ptr<std::atomic_bool> m_aliveFlag;
};

} // namespace nim
