#pragma once

#include <tuple>
#include <vector>

#include "zglmutils.h"

namespace folly {
class CancellationToken;
}

namespace nim {

class Z3DImg;
class ZBenchTimer;
class ZVulkanTexture;

class ZVulkanImageBlockUploader
{
public:
  virtual ~ZVulkanImageBlockUploader() = default;

  virtual void ensureImageResources(Z3DImg& image) = 0;

  void bindToImage(Z3DImg& image);

  virtual size_t readAndUploadImageBlocks(Z3DImg& image,
                                          size_t channel,
                                          const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                          const folly::CancellationToken& cancellationToken,
                                          ZBenchTimer& timer) = 0;

  virtual void uploadPageCaches(Z3DImg& image, size_t channel, ZBenchTimer& timer) = 0;

  [[nodiscard]] virtual ZVulkanTexture* pageDirectoryTexture(Z3DImg& image, size_t channel) = 0;

  [[nodiscard]] virtual ZVulkanTexture* pageTableTexture(Z3DImg& image, size_t channel) = 0;

  [[nodiscard]] virtual ZVulkanTexture* imageCacheTexture(Z3DImg& image, size_t channel) = 0;
};

} // namespace nim
