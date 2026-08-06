#pragma once

#include "z3drenderedframe.h"
#include "zvulkandevicesupport.h"

#include <folly/CancellationToken.h>
#include <memory>
#include <span>
#include <vector>

namespace nim {

class Z3DRenderingEngine;
class Z3DTileDescriptor;

// Owns complete headless Vulkan workers controlled serially from the canonical
// rendering thread. Independent device queues can execute submitted spatial
// tiles concurrently. The direct rendering path does not consult this object.
class ZVulkanMultiDeviceTileCoordinator final
{
public:
  explicit ZVulkanMultiDeviceTileCoordinator(Z3DRenderingEngine& canonicalEngine);
  ZVulkanMultiDeviceTileCoordinator(Z3DRenderingEngine& canonicalEngine,
                                    std::span<const ZVulkanDeviceSupport::DeviceSelection> workerSelections);
  ~ZVulkanMultiDeviceTileCoordinator();

  ZVulkanMultiDeviceTileCoordinator(const ZVulkanMultiDeviceTileCoordinator&) = delete;
  ZVulkanMultiDeviceTileCoordinator& operator=(const ZVulkanMultiDeviceTileCoordinator&) = delete;

  [[nodiscard]] bool coordinates(const Z3DRenderingEngine& engine) const noexcept;

  // Synchronously assemble one complete frame while internally refilling each
  // worker as its final readback is observed complete.
  [[nodiscard]] Z3DRenderedFrame renderFrame(std::span<const Z3DTileDescriptor> tiles,
                                             bool renderStereoPair = false,
                                             folly::CancellationToken cancellationToken = {});

private:
  struct Worker;
  void synchronizeWorkersFromCanonical();

  Z3DRenderingEngine& m_canonicalEngine;
  std::vector<std::unique_ptr<Worker>> m_workers;
};

} // namespace nim
