#pragma once

#include "z3drenderedframe.h"
#include "z3drenderingengine.h"

#include <folly/CancellationToken.h>

#include <memory>
#include <span>
#include <vector>

namespace nim {

// Concrete implementation detail of Z3DRenderingEngine. Every non-canonical
// lane owns a complete single-device engine on its own rendering thread.
class Z3DRenderingEngine::ZVulkanTileWorkerPool final
{
public:
  ZVulkanTileWorkerPool(Z3DRenderingEngine& canonicalEngine,
                        std::span<const ZVulkanDeviceSupport::DeviceSelection> selections);
  ~ZVulkanTileWorkerPool();

  ZVulkanTileWorkerPool(const ZVulkanTileWorkerPool&) = delete;
  ZVulkanTileWorkerPool& operator=(const ZVulkanTileWorkerPool&) = delete;

  [[nodiscard]] Z3DRenderedFrame renderFrame(std::span<const Z3DTileDescriptor> tiles,
                                             bool renderStereoPair,
                                             folly::CancellationToken cancellationToken);

private:
  struct Batch;
  struct Lane;

  static void renderLane(Z3DRenderingEngine& engine,
                         const VulkanTileRenderState* workerState,
                         const std::shared_ptr<Batch>& batch) noexcept;

  Z3DRenderingEngine& m_canonicalEngine;
  bool m_canonicalParticipates = false;
  std::vector<std::unique_ptr<Lane>> m_workerLanes;
};

} // namespace nim
