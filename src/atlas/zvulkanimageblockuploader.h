#pragma once

#include <tuple>
#include <vector>

#include <glm/glm.hpp>

namespace folly {
class CancellationToken;
}

namespace nim {

class Z3DImg;
class ZBenchTimer;

class ZVulkanImageBlockUploader
{
public:
  virtual ~ZVulkanImageBlockUploader() = default;

  virtual size_t readAndUploadImageBlocks(Z3DImg& image,
                                          size_t channel,
                                          const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                          const folly::CancellationToken& cancellationToken,
                                          ZBenchTimer& timer) = 0;
};

} // namespace nim
