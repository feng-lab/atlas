#include "zvulkanmultidevicetilecoordinator.h"

#include "z3dcameraparameter.h"
#include "z3drenderingengine.h"
#include "z3dtiledescriptor.h"
#include "zcancellation.h"
#include "zdoc.h"
#include "zrenderthreadexecutor_tls.h"

#include <QThread>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace nim {

struct ZVulkanMultiDeviceTileCoordinator::Worker
{
  struct Assignment
  {
    size_t tileIndex;
    uint64_t renderFrameToken;
  };

  explicit Worker(std::unique_ptr<Z3DRenderingEngine> workerEngine)
    : engine(std::move(workerEngine))
  {
    CHECK(engine != nullptr);
  }

  std::unique_ptr<Z3DRenderingEngine> engine;
  std::optional<Assignment> assignment;
};

ZVulkanMultiDeviceTileCoordinator::ZVulkanMultiDeviceTileCoordinator(Z3DRenderingEngine& canonicalEngine)
  : m_canonicalEngine(canonicalEngine)
{
  m_workers.push_back(std::make_unique<Worker>(m_canonicalEngine.createVulkanTileWorker()));
}

ZVulkanMultiDeviceTileCoordinator::ZVulkanMultiDeviceTileCoordinator(
  Z3DRenderingEngine& canonicalEngine,
  std::span<const ZVulkanDeviceSupport::DeviceSelection> workerSelections)
  : m_canonicalEngine(canonicalEngine)
{
  CHECK(!workerSelections.empty()) << "A Vulkan tile coordinator requires at least one worker adapter";
  const auto compatibleSelections = m_canonicalEngine.compatibleVulkanTileWorkerSelections();

  std::vector<ZVulkanDeviceSupport::DeviceSelection> acceptedSelections;
  acceptedSelections.reserve(workerSelections.size());
  m_workers.reserve(workerSelections.size());
  for (const auto& selection : workerSelections) {
    CHECK(std::find(compatibleSelections.begin(), compatibleSelections.end(), selection) != compatibleSelections.end())
      << "A Vulkan tile coordinator received an adapter outside the canonical compatible-device set";
    for (const auto& accepted : acceptedSelections) {
      CHECK(accepted.preferenceIndex != selection.preferenceIndex)
        << "A Vulkan tile coordinator cannot select one preference index more than once";
      CHECK(accepted.expectedDeviceUuid != selection.expectedDeviceUuid)
        << "A Vulkan tile coordinator cannot select one physical-device UUID more than once";
    }
    acceptedSelections.push_back(selection);
    m_workers.push_back(std::make_unique<Worker>(m_canonicalEngine.createVulkanTileWorker(selection)));
  }
}

ZVulkanMultiDeviceTileCoordinator::~ZVulkanMultiDeviceTileCoordinator() = default;

bool ZVulkanMultiDeviceTileCoordinator::coordinates(const Z3DRenderingEngine& engine) const noexcept
{
  return &m_canonicalEngine == &engine;
}

void ZVulkanMultiDeviceTileCoordinator::synchronizeWorkersFromCanonical()
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread())
    << "A Vulkan tile coordinator must run on the canonical engine thread";
  CHECK(currentRenderThreadExecutorOrNull() == &m_canonicalEngine.renderThreadExecutor())
    << "A Vulkan tile coordinator requires the canonical rendering-thread executor";
  CHECK(!m_workers.empty()) << "A Vulkan tile coordinator has no healthy workers";

  json::object generalState;
  m_canonicalEngine.write(generalState);
  CHECK(generalState.contains("Compositor"));
  CHECK(generalState.contains("Global"));

  struct ObjectState
  {
    size_t objectId;
    json::object state;
  };
  std::vector<ObjectState> objectStates;
  for (const auto objectId : m_canonicalEngine.doc().objs()) {
    json::object state;
    m_canonicalEngine.write(objectId, state);
    CHECK(state.empty() || state.contains("ViewObjType"));
    if (!state.empty()) {
      objectStates.push_back(ObjectState{objectId, std::move(state)});
    }
  }

  const auto devicePixelRatio = m_canonicalEngine.globalParas().devicePixelRatio.get();
  Z3DCameraParameter cameraSnapshot(QStringLiteral("Vulkan tile batch camera"));
  cameraSnapshot.setSameAs(m_canonicalEngine.camera());

  for (const auto& workerRecord : m_workers) {
    CHECK(workerRecord != nullptr);
    CHECK(workerRecord->engine != nullptr);
    CHECK(!workerRecord->assignment.has_value())
      << "Canonical state cannot be synchronized while a worker tile is outstanding";
    Z3DRenderingEngine& worker = *workerRecord->engine;

    for (const auto& object : objectStates) {
      json::object workerObjectState;
      worker.write(object.objectId, workerObjectState);
      CHECK(workerObjectState.contains("ViewObjType"))
        << "Worker engine is missing a canonical 3D object view for object " << object.objectId;
      CHECK(workerObjectState.at("ViewObjType") == object.state.at("ViewObjType"))
        << "Canonical and worker object-view types differ for object " << object.objectId;
      worker.read(object.objectId, object.state);
    }

    worker.globalParas().setDevicePixelRatio(devicePixelRatio);

    // Bound-dependent global parameters are applied after object transforms so
    // absolute cuts use the final canonical ranges.
    worker.read(generalState);
    CHECK(static_cast<RenderBackend>(worker.globalParas().renderBackend.associatedData()) == RenderBackend::Vulkan)
      << "A Vulkan tile worker cannot consume non-Vulkan canonical state";

    // Scene JSON omits derived camera fields such as near/far. The parameter
    // snapshot retains the complete camera and is applied last.
    worker.camera().setSameAs(cameraSnapshot);
  }
}

Z3DRenderedFrame ZVulkanMultiDeviceTileCoordinator::renderFrame(std::span<const Z3DTileDescriptor> tiles,
                                                                bool renderStereoPair,
                                                                folly::CancellationToken cancellationToken)
{
  CHECK(!m_workers.empty()) << "A Vulkan tile coordinator has no healthy workers";
  CHECK(!tiles.empty()) << "A coordinated frame requires at least one tile";

  const glm::uvec2 fullOutputExtent = tiles.front().fullOutputExtent();
  uint64_t coveredPixelCount = 0u;
  for (size_t tileIndex = 0u; tileIndex < tiles.size(); ++tileIndex) {
    const Z3DTileDescriptor& tile = tiles[tileIndex];
    CHECK(tile.fullOutputExtent() == fullOutputExtent)
      << "Every tile in one coordinated batch must share the full output extent";
    coveredPixelCount +=
      static_cast<uint64_t>(tile.validOutputExtent().x) * static_cast<uint64_t>(tile.validOutputExtent().y);

    const glm::uvec2 tileOrigin = tile.validOutputOrigin();
    const glm::uvec2 tileEnd = tileOrigin + tile.validOutputExtent();
    for (size_t previousIndex = 0u; previousIndex < tileIndex; ++previousIndex) {
      const Z3DTileDescriptor& previous = tiles[previousIndex];
      const glm::uvec2 previousOrigin = previous.validOutputOrigin();
      const glm::uvec2 previousEnd = previousOrigin + previous.validOutputExtent();
      const bool overlaps = tileOrigin.x < previousEnd.x && previousOrigin.x < tileEnd.x &&
                            tileOrigin.y < previousEnd.y && previousOrigin.y < tileEnd.y;
      CHECK(!overlaps) << "A coordinated frame contains overlapping tile output rectangles";
    }
  }
  const uint64_t fullPixelCount = static_cast<uint64_t>(fullOutputExtent.x) * static_cast<uint64_t>(fullOutputExtent.y);
  CHECK_EQ(coveredPixelCount, fullPixelCount) << "A coordinated frame must cover every output pixel exactly once";

  try {
    maybeCancel(cancellationToken);
    synchronizeWorkersFromCanonical();
    for (const auto& workerRecord : m_workers) {
      maybeCancel(cancellationToken);
      workerRecord->engine->beginVulkanTileExport(fullOutputExtent, cancellationToken);
    }

    Z3DRenderedFrame frame(fullOutputExtent, renderStereoPair);
    std::vector<bool> completedTiles(tiles.size(), false);
    size_t nextTileIndex = 0u;
    size_t completedTileCount = 0u;

    auto submitNextTile = [&](Worker& worker) {
      CHECK(!worker.assignment.has_value());
      CHECK_LT(nextTileIndex, tiles.size());
      const size_t tileIndex = nextTileIndex;
      const uint64_t renderFrameToken =
        worker.engine->submitVulkanTile(tiles[tileIndex], renderStereoPair, cancellationToken);
      CHECK_GT(renderFrameToken, 0u);
      worker.assignment = Worker::Assignment{tileIndex, renderFrameToken};
      ++nextTileIndex;
    };

    for (const auto& workerRecord : m_workers) {
      if (nextTileIndex == tiles.size()) {
        break;
      }
      maybeCancel(cancellationToken);
      submitNextTile(*workerRecord);
    }

    constexpr auto kCompletionPollInterval = std::chrono::milliseconds(1);
    while (completedTileCount < tiles.size()) {
      maybeCancel(cancellationToken);
      bool madeProgress = false;

      for (const auto& workerRecord : m_workers) {
        Worker& worker = *workerRecord;
        if (!worker.assignment.has_value()) {
          continue;
        }

        const Worker::Assignment assignment = *worker.assignment;
        if (!worker.engine->isVulkanTileReady(assignment.renderFrameToken)) {
          continue;
        }

        CHECK(!completedTiles[assignment.tileIndex]) << "A coordinated tile index completed more than once";
        const Z3DRenderedTile renderedTile = worker.engine->collectVulkanTile(assignment.renderFrameToken);
        frame.pasteTile(tiles[assignment.tileIndex], renderedTile);
        completedTiles[assignment.tileIndex] = true;
        worker.assignment.reset();
        CHECK_LT(completedTileCount, tiles.size());
        ++completedTileCount;
        madeProgress = true;

        maybeCancel(cancellationToken);
        if (nextTileIndex < tiles.size()) {
          submitNextTile(worker);
        }
      }

      const bool hasOutstandingTile = std::any_of(m_workers.begin(), m_workers.end(), [](const auto& worker) {
        return worker->assignment.has_value();
      });
      if (completedTileCount < tiles.size()) {
        CHECK(hasOutstandingTile) << "A coordinated tile batch stalled with unassigned work and no outstanding tile";
      }
      if (!madeProgress && hasOutstandingTile) {
        std::this_thread::sleep_for(kCompletionPollInterval);
      }
    }

    maybeCancel(cancellationToken);
    for (const auto& workerRecord : m_workers) {
      workerRecord->engine->endVulkanTileExport();
    }

    for (const bool completed : completedTiles) {
      CHECK(completed) << "A coordinated tile batch completed without a result for every input tile";
    }
    return frame;
  }
  catch (...) {
    // A batch is all-or-nothing. Engine teardown drains known submissions
    // before the failed batch's worker resources are destroyed.
    m_workers.clear();
    throw;
  }
}

} // namespace nim
