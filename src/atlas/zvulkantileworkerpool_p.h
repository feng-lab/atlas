#pragma once

#include "z3drenderedframe.h"
#include "z3drenderingengine.h"
#include "z3dtiledescriptor.h"

#include <folly/CancellationToken.h>

#include <cstddef>
#include <memory>
#include <optional>
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
  [[nodiscard]] size_t participantCount() const noexcept;
  [[nodiscard]] bool canonicalParticipates() const noexcept;
  void detachRegionalPresentationSources();
  void invalidateInteractiveRegions();
  void invalidateInteractiveQueries();
  void beginInteractiveRegions(std::span<const Z3DTileDescriptor> regions);
  [[nodiscard]] double renderInteractiveRegions(folly::CancellationToken cancellationToken, Z3DCanvas& canvas);
  void dispatchCanonicalInteractiveEvent(QEvent* event,
                                         int fullLogicalWidth,
                                         int fullLogicalHeight,
                                         double devicePixelRatio);

private:
  struct Batch;
  struct InteractiveBatch;
  struct Lane;
  struct PresentationLifetime;

  static void renderLane(Z3DRenderingEngine& engine,
                         /*nullable*/ const VulkanTileRenderState* workerState,
                         const std::shared_ptr<Batch>& batch) noexcept;
  static void prepareInteractiveParticipant(Z3DRenderingEngine& engine,
                                            /*nullable*/ const VulkanTileRenderState* workerState,
                                            const Z3DTileDescriptor& region);
  static void renderInteractiveParticipant(Z3DRenderingEngine& engine,
                                           const std::shared_ptr<InteractiveBatch>& batch,
                                           size_t participantIndex) noexcept;
  [[nodiscard]] std::optional<glm::uvec2> fullOutputPixel(glm::ivec2 logicalPosition, double devicePixelRatio) const;
  [[nodiscard]] size_t participantForPixel(glm::uvec2 fullOutputPixel) const;
  [[nodiscard]] Z3DRenderingEngine& participantEngine(size_t participantIndex) const;
  [[nodiscard]] Z3DPickingManager::PickingObject objectAt(glm::ivec2 logicalPosition, double devicePixelRatio) const;
  [[nodiscard]] GLfloat depthAt(glm::ivec2 logicalPosition, double devicePixelRatio) const;
  [[nodiscard]] std::vector<Z3DPickingManager::PickingObject>
  objectsByDistance(glm::ivec2 logicalPosition, int logicalRadius, bool ascend, double devicePixelRatio) const;
  [[nodiscard]] std::optional<Z3DPickingManager::ImageDepthSample>
  imageDepthAtPhysicalInput(size_t imageObjectId, glm::ivec2 physicalInputPosition) const;

  Z3DRenderingEngine& m_canonicalEngine;
  std::shared_ptr<PresentationLifetime> m_presentationLifetime;
  // A null lane denotes the canonical engine without transferring ownership.
  std::vector<std::unique_ptr<Lane>> m_participants;
  std::vector<Z3DTileDescriptor> m_interactiveRegions;
  std::vector<double> m_interactiveProgress;
  bool m_interactiveQueriesReady = false;
};

} // namespace nim
