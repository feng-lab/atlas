#pragma once

#include "z3drenderedtile.h"

#include <folly/CancellationToken.h>
#include <memory>

namespace nim {

class Z3DRenderingEngine;
class Z3DTileDescriptor;

// Owns one complete headless Vulkan worker initialized from a canonical engine.
// The direct rendering path does not allocate or consult this object.
class ZVulkanMultiDeviceTileCoordinator final
{
public:
  explicit ZVulkanMultiDeviceTileCoordinator(Z3DRenderingEngine& canonicalEngine);
  ~ZVulkanMultiDeviceTileCoordinator();

  ZVulkanMultiDeviceTileCoordinator(const ZVulkanMultiDeviceTileCoordinator&) = delete;
  ZVulkanMultiDeviceTileCoordinator& operator=(const ZVulkanMultiDeviceTileCoordinator&) = delete;

  [[nodiscard]] Z3DRenderedTile renderTile(const Z3DTileDescriptor& tile,
                                           bool renderStereoPair = false,
                                           folly::CancellationToken cancellationToken = {});

private:
  void synchronizeWorkerFromCanonical(Z3DRenderingEngine& worker);

  Z3DRenderingEngine& m_canonicalEngine;
  std::unique_ptr<Z3DRenderingEngine> m_worker;
};

} // namespace nim
