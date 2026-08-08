#include "zvulkantileworkerpool_p.h"

#include "z3dtiledescriptor.h"
#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"
#include "zvulkancontext.h"

#include <QMetaObject>
#include <QObject>
#include <QStringList>
#include <QThread>

#include <folly/OperationCancelled.h>
#include <folly/ScopeGuard.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace nim {

namespace {

void validateTileBatch(std::span<const Z3DTileDescriptor> tiles)
{
  CHECK(!tiles.empty()) << "A Vulkan tile-worker frame requires at least one tile";

  const glm::uvec2 fullOutputExtent = tiles.front().fullOutputExtent();
  uint64_t coveredPixelCount = 0u;
  for (size_t tileIndex = 0u; tileIndex < tiles.size(); ++tileIndex) {
    const Z3DTileDescriptor& tile = tiles[tileIndex];
    CHECK(tile.fullOutputExtent() == fullOutputExtent)
      << "Every tile in one worker batch must share the full output extent";
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
      CHECK(!overlaps) << "A Vulkan tile-worker frame contains overlapping output rectangles";
    }
  }

  const uint64_t fullPixelCount = static_cast<uint64_t>(fullOutputExtent.x) * fullOutputExtent.y;
  CHECK_EQ(coveredPixelCount, fullPixelCount)
    << "A Vulkan tile-worker frame must cover every output pixel exactly once";
}

[[nodiscard]] bool isCancellationFailure(const std::exception_ptr& failure) noexcept
{
  CHECK(failure != nullptr);
  try {
    std::rethrow_exception(failure);
  }
  catch (const ZCancellationException&) {
    return true;
  }
  catch (const folly::OperationCancelled&) {
    return true;
  }
  catch (...) {
    return false;
  }
}

} // namespace

struct Z3DRenderingEngine::ZVulkanTileWorkerPool::Batch
{
  Batch(std::span<const Z3DTileDescriptor> tileDescriptors, bool stereo, folly::CancellationToken token)
    : tiles(tileDescriptors)
    , renderStereoPair(stereo)
    , cancellationToken(std::move(token))
    , frame(tileDescriptors.front().fullOutputExtent(), stereo)
    , completedTiles(tileDescriptors.size(), false)
  {}

  void recordFailure(std::exception_ptr failure)
  {
    CHECK(failure != nullptr);
    const bool cancellation = isCancellationFailure(failure);
    {
      std::scoped_lock lock(failureMutex);
      if (firstFailure == nullptr) {
        firstFailure = failure;
      }
      if (!cancellation && firstNonCancellationFailure == nullptr) {
        firstNonCancellationFailure = std::move(failure);
      }
    }
    stop.store(true, std::memory_order_release);
  }

  [[nodiscard]] std::exception_ptr failure()
  {
    std::scoped_lock lock(failureMutex);
    return firstNonCancellationFailure != nullptr ? firstNonCancellationFailure : firstFailure;
  }

  std::span<const Z3DTileDescriptor> tiles;
  bool renderStereoPair;
  folly::CancellationToken cancellationToken;
  std::atomic<size_t> nextTileIndex{0u};
  std::atomic_bool stop{false};

  std::mutex assemblyMutex;
  Z3DRenderedFrame frame;
  std::vector<bool> completedTiles;
  size_t completedTileCount = 0u;

  std::mutex failureMutex;
  std::exception_ptr firstFailure;
  std::exception_ptr firstNonCancellationFailure;
};

struct Z3DRenderingEngine::ZVulkanTileWorkerPool::Lane
{
  Lane(Z3DRenderingEngine& canonicalEngine, const ZVulkanDeviceSupport::DeviceSelection& selection)
    : thread(std::make_unique<QThread>())
    , dispatcher(new QObject())
  {
    thread->setObjectName(QStringLiteral("Vulkan tile worker %1").arg(selection.preferenceIndex));
    dispatcher->moveToThread(thread.get());
    QObject::connect(thread.get(), &QThread::finished, dispatcher, &QObject::deleteLater);
    thread->start();

    try {
      invoke([this, &canonicalEngine, selection]() {
        auto candidate = std::unique_ptr<Z3DRenderingEngine>(
          new Z3DRenderingEngine(canonicalEngine.m_doc, Role::VulkanTileWorker, selection, dispatcher));

        QStringList initializationErrors;
        const QMetaObject::Connection errorConnection = QObject::connect(
          candidate.get(),
          &Z3DRenderingEngine::renderingError,
          candidate.get(),
          [&initializationErrors](const QString& error) {
            initializationErrors.push_back(error);
          },
          Qt::DirectConnection);
        auto errorConnectionGuard = folly::makeGuard([errorConnection]() {
          QObject::disconnect(errorConnection);
        });
        candidate->init();
        if (!initializationErrors.empty()) {
          throw ZException(
            fmt::format("Vulkan tile worker initialization failed: {}", initializationErrors.join("; ").toStdString()));
        }
        engine = candidate.release();
      }).get();
    }
    catch (...) {
      // A throwing constructor does not run Lane::~Lane(). Stop the thread
      // here so QThread member unwinding never destroys a running thread.
      thread->quit();
      CHECK(thread->wait()) << "A failed Vulkan tile-worker thread did not stop";
      dispatcher = nullptr;
      throw;
    }
  }

  ~Lane()
  {
    CHECK(thread != nullptr);
    CHECK(dispatcher != nullptr);
    CHECK(QThread::currentThread() != thread.get()) << "A Vulkan worker lane cannot join its own thread";

    if (engine != nullptr) {
      invoke([this]() {
        delete engine;
        engine = nullptr;
      }).get();
    }

    thread->quit();
    CHECK(thread->wait()) << "A Vulkan tile-worker thread did not stop";
    dispatcher = nullptr;
  }

  [[nodiscard]] std::future<void> invoke(std::function<void()> operation)
  {
    CHECK(operation != nullptr);
    CHECK(dispatcher != nullptr);
    CHECK(thread != nullptr);
    CHECK(thread->isRunning()) << "A Vulkan tile-worker thread is not running";

    auto completion = std::make_shared<std::promise<void>>();
    std::future<void> result = completion->get_future();
    const bool queued = QMetaObject::invokeMethod(
      dispatcher,
      [completion, operation = std::move(operation)]() mutable {
        try {
          operation();
          completion->set_value();
        }
        catch (...) {
          completion->set_exception(std::current_exception());
        }
      },
      Qt::QueuedConnection);
    CHECK(queued) << "Failed to queue work on a Vulkan tile-worker thread";
    return result;
  }

  std::unique_ptr<QThread> thread;
  QObject* dispatcher = nullptr;
  Z3DRenderingEngine* engine = nullptr;
};

Z3DRenderingEngine::ZVulkanTileWorkerPool::ZVulkanTileWorkerPool(
  Z3DRenderingEngine& canonicalEngine,
  std::span<const ZVulkanDeviceSupport::DeviceSelection> selections)
  : m_canonicalEngine(canonicalEngine)
{
  CHECK(QThread::currentThread() == canonicalEngine.thread())
    << "Vulkan tile workers must be configured on the canonical engine thread";
  CHECK(canonicalEngine.m_role == Role::Canonical);
  CHECK(!selections.empty()) << "A Vulkan tile-worker pool requires at least one device";

  const auto compatibleSelections = canonicalEngine.compatibleVulkanTileWorkerSelections();
  CHECK(canonicalEngine.m_vkContext != nullptr);
  const auto canonicalSelection = canonicalEngine.m_vkContext->selectedDeviceSelection();

  std::vector<ZVulkanDeviceSupport::DeviceSelection> acceptedSelections;
  acceptedSelections.reserve(selections.size());
  m_workerLanes.reserve(selections.size());
  for (const auto& selection : selections) {
    CHECK(std::find(compatibleSelections.begin(), compatibleSelections.end(), selection) != compatibleSelections.end())
      << "A Vulkan tile-worker device is outside the canonical compatible-device set";
    for (const auto& accepted : acceptedSelections) {
      CHECK(accepted.preferenceIndex != selection.preferenceIndex)
        << "A Vulkan tile-worker device index may be selected only once";
      CHECK(accepted.expectedDeviceUuid != selection.expectedDeviceUuid)
        << "A physical device may be selected for Vulkan tile work only once";
    }
    acceptedSelections.push_back(selection);

    if (selection == canonicalSelection) {
      CHECK(!m_canonicalParticipates) << "The canonical Vulkan device was selected more than once";
      m_canonicalParticipates = true;
    } else {
      m_workerLanes.push_back(std::make_unique<Lane>(canonicalEngine, selection));
    }
  }

  CHECK(m_canonicalParticipates || !m_workerLanes.empty());
}

Z3DRenderingEngine::ZVulkanTileWorkerPool::~ZVulkanTileWorkerPool()
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread())
    << "A Vulkan tile-worker pool must be destroyed on the canonical engine thread";
}

void Z3DRenderingEngine::ZVulkanTileWorkerPool::renderLane(Z3DRenderingEngine& engine,
                                                           const VulkanTileRenderState* workerState,
                                                           const std::shared_ptr<Batch>& batch) noexcept
{
  CHECK(batch != nullptr);
  const bool logWorkerSummary = VLOG_IS_ON(1);
  const auto workerStart =
    logWorkerSummary ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  size_t renderedTileCount = 0u;
  uint64_t renderedValidPixels = 0u;
  uint64_t renderedAttachmentPixels = 0u;
  bool exportActive = false;
  try {
    if (workerState != nullptr) {
      engine.applyVulkanTileRenderState(*workerState);
    } else {
      CHECK(engine.m_role == Role::Canonical);
    }

    engine.beginVulkanTileExport(batch->tiles.front().fullOutputExtent(), batch->cancellationToken);
    exportActive = true;

    constexpr auto kCompletionPollInterval = std::chrono::milliseconds(1);
    while (!batch->stop.load(std::memory_order_acquire)) {
      if (batch->cancellationToken.isCancellationRequested()) {
        batch->stop.store(true, std::memory_order_release);
        break;
      }

      const size_t tileIndex = batch->nextTileIndex.fetch_add(1u, std::memory_order_relaxed);
      if (tileIndex >= batch->tiles.size()) {
        break;
      }

      maybeCancel(batch->cancellationToken);
      const uint64_t renderFrameToken =
        engine.submitVulkanTile(batch->tiles[tileIndex], batch->renderStereoPair, batch->cancellationToken);
      CHECK_GT(renderFrameToken, 0u);

      // A submitted tile owns Vulkan resources until its final readback is
      // published. Finish that ownership boundary even when another lane asks
      // the batch to stop; cancellation is observed before the next tile.
      while (!engine.isVulkanTileReady(renderFrameToken)) {
        std::this_thread::sleep_for(kCompletionPollInterval);
      }
      Z3DRenderedTile renderedTile = engine.collectVulkanTile(renderFrameToken);
      if (logWorkerSummary) {
        const glm::uvec2 validExtent = batch->tiles[tileIndex].validOutputExtent();
        const glm::uvec2 attachmentExtent = batch->tiles[tileIndex].attachmentExtent();
        ++renderedTileCount;
        renderedValidPixels += static_cast<uint64_t>(validExtent.x) * validExtent.y;
        renderedAttachmentPixels += static_cast<uint64_t>(attachmentExtent.x) * attachmentExtent.y;
      }

      if (batch->cancellationToken.isCancellationRequested()) {
        batch->stop.store(true, std::memory_order_release);
      }
      if (!batch->stop.load(std::memory_order_acquire)) {
        std::scoped_lock lock(batch->assemblyMutex);
        if (!batch->stop.load(std::memory_order_relaxed) && !batch->cancellationToken.isCancellationRequested()) {
          CHECK(!batch->completedTiles[tileIndex]) << "A Vulkan tile index completed more than once";
          batch->frame.pasteTile(batch->tiles[tileIndex], renderedTile);
          batch->completedTiles[tileIndex] = true;
          ++batch->completedTileCount;
        }
      }
    }

    exportActive = false;
    engine.endVulkanTileExport();
    if (logWorkerSummary) {
      CHECK(engine.m_vkContext != nullptr);
      const auto deviceSelection = engine.m_vkContext->selectedDeviceSelection();
      VLOG(1) << fmt::format(
        "ATLAS_VULKAN_TILE_WORKER_FINISHED device_index={} tiles={} valid_pixels={} "
        "attachment_pixels={} elapsed_ms={:.3f}",
        deviceSelection.preferenceIndex,
        renderedTileCount,
        renderedValidPixels,
        renderedAttachmentPixels,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - workerStart).count());
    }
  }
  catch (...) {
    const std::exception_ptr failure = std::current_exception();
    batch->recordFailure(failure);

    if (exportActive) {
      try {
        engine.abandonVulkanTileExportAfterFailure();
        exportActive = false;
      }
      catch (...) {
        // Preserve a non-cancellation cleanup failure so ordinary cancellation
        // observed by another lane cannot mask it.
        batch->recordFailure(std::current_exception());
      }
    }
  }
}

Z3DRenderedFrame Z3DRenderingEngine::ZVulkanTileWorkerPool::renderFrame(std::span<const Z3DTileDescriptor> tiles,
                                                                        bool renderStereoPair,
                                                                        folly::CancellationToken cancellationToken)
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread())
    << "A Vulkan tile-worker batch must start on the canonical engine thread";
  validateTileBatch(tiles);
  maybeCancel(cancellationToken);

  std::shared_ptr<const VulkanTileRenderState> publishedState;
  if (!m_workerLanes.empty()) {
    publishedState = m_canonicalEngine.publishVulkanTileRenderState();
    CHECK(publishedState != nullptr);
  }
  auto batch = std::make_shared<Batch>(tiles, renderStereoPair, cancellationToken);

  std::vector<std::future<void>> workerTasks;
  workerTasks.reserve(m_workerLanes.size());
  for (const auto& lane : m_workerLanes) {
    CHECK(lane != nullptr);
    CHECK(lane->engine != nullptr);
    workerTasks.push_back(lane->invoke([engine = lane->engine, publishedState, batch]() {
      renderLane(*engine, publishedState.get(), batch);
    }));
  }

  if (m_canonicalParticipates) {
    renderLane(m_canonicalEngine, nullptr, batch);
  }

  for (auto& task : workerTasks) {
    task.get();
  }

  if (const std::exception_ptr failure = batch->failure(); failure != nullptr) {
    std::rethrow_exception(failure);
  }
  maybeCancel(cancellationToken);

  CHECK_EQ(batch->completedTileCount, tiles.size())
    << "A Vulkan tile-worker batch completed without a result for every tile";
  for (const bool completed : batch->completedTiles) {
    CHECK(completed);
  }
  return std::move(batch->frame);
}

} // namespace nim
