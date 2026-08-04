#include "zvulkanmultidevicetilecoordinator.h"

#include "z3drenderingengine.h"
#include "zdoc.h"

#include <utility>

namespace nim {

ZVulkanMultiDeviceTileCoordinator::ZVulkanMultiDeviceTileCoordinator(Z3DRenderingEngine& canonicalEngine)
  : m_canonicalEngine(canonicalEngine)
{
  auto worker = m_canonicalEngine.createVulkanTileWorker();
  synchronizeWorkerFromCanonical(*worker);
  m_worker = std::move(worker);
}

ZVulkanMultiDeviceTileCoordinator::~ZVulkanMultiDeviceTileCoordinator() = default;

void ZVulkanMultiDeviceTileCoordinator::synchronizeWorkerFromCanonical(Z3DRenderingEngine& worker)
{
  json::object generalState;
  m_canonicalEngine.write(generalState);
  CHECK(generalState.contains("Compositor"));
  CHECK(generalState.contains("Global"));

  for (const auto objectId : m_canonicalEngine.doc().objs()) {
    json::object objectState;
    m_canonicalEngine.write(objectId, objectState);
    CHECK(objectState.empty() || objectState.contains("ViewObjType"));
    if (objectState.empty()) {
      continue;
    }
    json::object workerObjectState;
    worker.write(objectId, workerObjectState);
    CHECK(workerObjectState.contains("ViewObjType"))
      << "Worker engine is missing a canonical 3D object view for object " << objectId;
    CHECK(workerObjectState.at("ViewObjType") == objectState.at("ViewObjType"))
      << "Canonical and worker object-view types differ for object " << objectId;
    worker.read(objectId, objectState);
  }

  worker.globalParas().setDevicePixelRatio(m_canonicalEngine.globalParas().devicePixelRatio.get());

  // Apply bound-dependent global parameters only after every object transform
  // has established the worker's canonical ranges. Otherwise an absolute cut
  // can be irreversibly clamped against provisional bounds.
  worker.read(generalState);
  CHECK(static_cast<RenderBackend>(worker.globalParas().renderBackend.associatedData()) == RenderBackend::Vulkan)
    << "A Vulkan tile worker cannot consume non-Vulkan canonical state";

  // Apply this last: scene JSON intentionally omits derived camera fields such
  // as near/far. setSameAs() copies the complete camera object rather than
  // relying on its UI-facing equality operator.
  worker.camera().setSameAs(m_canonicalEngine.camera());
}

Z3DRenderedTile ZVulkanMultiDeviceTileCoordinator::renderTile(const Z3DTileDescriptor& tile,
                                                              bool renderStereoPair,
                                                              folly::CancellationToken cancellationToken)
{
  CHECK(m_worker != nullptr);
  try {
    // Complete-engine views retain ordinary document lifetime/data
    // subscriptions. Reapply canonical renderer state immediately before each
    // same-thread synchronous tile call so every attempt starts from complete
    // canonical state.
    synchronizeWorkerFromCanonical(*m_worker);
    return m_worker->renderVulkanTile(tile, renderStereoPair, std::move(cancellationToken));
  }
  catch (...) {
    // Synchronization or rendering failure leaves worker state untrusted, so
    // discard the worker before propagating the failure.
    m_worker.reset();
    throw;
  }
}

} // namespace nim
