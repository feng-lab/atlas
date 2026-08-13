#include "zvulkantileworkerpool_p.h"

#include "z3dcanvas.h"
#include "z3dcanvaseventlistener.h"
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
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
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

class FailureSelection final
{
public:
  void record(std::exception_ptr failure)
  {
    CHECK(failure != nullptr);
    const bool cancellation = isCancellationFailure(failure);
    std::scoped_lock lock(m_mutex);
    if (m_firstFailure == nullptr) {
      m_firstFailure = failure;
    }
    if (!cancellation && m_firstNonCancellationFailure == nullptr) {
      m_firstNonCancellationFailure = std::move(failure);
    }
  }

  [[nodiscard]] std::exception_ptr selected()
  {
    std::scoped_lock lock(m_mutex);
    return m_firstNonCancellationFailure != nullptr ? m_firstNonCancellationFailure : m_firstFailure;
  }

private:
  std::mutex m_mutex;
  std::exception_ptr m_firstFailure;
  std::exception_ptr m_firstNonCancellationFailure;
};

} // namespace

struct Z3DRenderingEngine::ZVulkanTileWorkerPool::PresentationLifetime
{
  std::mutex mutex;
  bool enabled = true;
};

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
    failures.record(std::move(failure));
    stop.store(true, std::memory_order_release);
  }

  [[nodiscard]] std::exception_ptr failure()
  {
    return failures.selected();
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

  FailureSelection failures;
};

struct Z3DRenderingEngine::ZVulkanTileWorkerPool::InteractiveBatch
{
  InteractiveBatch(std::span<const Z3DTileDescriptor> regionDescriptors,
                   std::span<const double> currentProgress,
                   folly::CancellationToken token,
                   Z3DCanvas& presentationCanvas,
                   std::shared_ptr<PresentationLifetime> lifetime)
    : regions(regionDescriptors)
    , cancellationToken(std::move(token))
    , progress(currentProgress.begin(), currentProgress.end())
    , canvas(&presentationCanvas)
    , presentationLifetime(std::move(lifetime))
  {
    CHECK_EQ(progress.size(), regions.size());
    CHECK(presentationLifetime != nullptr);
  }

  void recordFailure(std::exception_ptr failure)
  {
    failures.record(std::move(failure));
    stop.store(true, std::memory_order_release);
  }

  [[nodiscard]] std::exception_ptr failure()
  {
    return failures.selected();
  }

  std::span<const Z3DTileDescriptor> regions;
  folly::CancellationToken cancellationToken;
  std::atomic_bool stop{false};

  std::vector<double> progress;
  QPointer<Z3DCanvas> canvas;
  std::shared_ptr<PresentationLifetime> presentationLifetime;

  FailureSelection failures;
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

        const QMetaObject::Connection renderRequestConnection =
          QObject::connect(candidate.get(),
                           &Z3DRenderingEngine::sceneParaUpdated,
                           &canonicalEngine,
                           &Z3DRenderingEngine::handleVulkanTileWorkerNeedsRender,
                           Qt::QueuedConnection);
        CHECK(renderRequestConnection) << "Failed to forward a Vulkan worker render request";

        const QMetaObject::Connection runtimeErrorConnection = QObject::connect(
          candidate.get(),
          &Z3DRenderingEngine::renderingError,
          &canonicalEngine,
          [canonicalEngine = &canonicalEngine, preferenceIndex = selection.preferenceIndex](const QString& error) {
            canonicalEngine->reportRenderingError(QStringLiteral("Vulkan tile worker device %1: %2")
                                                    .arg(static_cast<qulonglong>(preferenceIndex))
                                                    .arg(error));
          },
          Qt::QueuedConnection);
        CHECK(runtimeErrorConnection) << "Failed to forward Vulkan worker rendering diagnostics";

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
  , m_presentationLifetime(std::make_shared<PresentationLifetime>())
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
  m_participants.reserve(selections.size());
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
      CHECK(!canonicalParticipates()) << "The canonical Vulkan device was selected more than once";
      m_participants.push_back(nullptr);
    } else {
      m_participants.push_back(std::make_unique<Lane>(canonicalEngine, selection));
    }
  }

  CHECK_EQ(m_participants.size(), selections.size());
}

Z3DRenderingEngine::ZVulkanTileWorkerPool::~ZVulkanTileWorkerPool()
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread())
    << "A Vulkan tile-worker pool must be destroyed on the canonical engine thread";
  CHECK(m_presentationLifetime != nullptr);
  std::unique_lock lock(m_presentationLifetime->mutex);
  CHECK(m_presentationLifetime->enabled);
  m_presentationLifetime->enabled = false;
  m_participants.clear();
}

void Z3DRenderingEngine::ZVulkanTileWorkerPool::renderLane(Z3DRenderingEngine& engine,
                                                           /*nullable*/ const VulkanTileRenderState* workerState,
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

void Z3DRenderingEngine::ZVulkanTileWorkerPool::prepareInteractiveParticipant(
  Z3DRenderingEngine& engine,
  /*nullable*/ const VulkanTileRenderState* workerState,
  const Z3DTileDescriptor& region)
{
  if (workerState != nullptr) {
    engine.applyVulkanTileRenderState(*workerState);
  } else {
    CHECK(engine.m_role == Role::Canonical);
  }
  engine.prepareVulkanInteractiveRegion(region);
}

void Z3DRenderingEngine::ZVulkanTileWorkerPool::renderInteractiveParticipant(
  Z3DRenderingEngine& engine,
  const std::shared_ptr<InteractiveBatch>& batch,
  size_t participantIndex) noexcept
{
  CHECK(batch != nullptr);
  CHECK_LT(participantIndex, batch->regions.size());

  bool regionPending = false;
  try {
    if (batch->stop.load(std::memory_order_acquire)) {
      return;
    }
    maybeCancel(batch->cancellationToken);

    const FrameProcessResult frameResult =
      engine.submitVulkanInteractiveRegion(batch->regions[participantIndex], batch->cancellationToken);
    CHECK_GT(frameResult.renderFrameToken, 0u);
    CHECK_GE(frameResult.progress, 0.0);
    CHECK_LE(frameResult.progress, 1.0);
    regionPending = true;

    constexpr auto kCompletionPollInterval = std::chrono::milliseconds(1);
    while (!engine.isVulkanTileReady(frameResult.renderFrameToken)) {
      std::this_thread::sleep_for(kCompletionPollInterval);
    }
    engine.collectVulkanInteractiveRegion(frameResult.renderFrameToken);
    regionPending = false;

    // A collected regional frame is complete and independently presentable.
    const QPointer<Z3DCanvas> canvas = batch->canvas;
    if (canvas) {
      Z3DRenderingEngine* const source = &engine;
      const Z3DTileDescriptor region = batch->regions[participantIndex];
      const std::shared_ptr<PresentationLifetime> presentationLifetime = batch->presentationLifetime;
      CHECK(presentationLifetime != nullptr);
      const bool queued = QMetaObject::invokeMethod(
        canvas.data(),
        [canvas, source, participantIndex, region, presentationLifetime]() {
          std::scoped_lock lock(presentationLifetime->mutex);
          if (!presentationLifetime->enabled || !canvas) {
            return;
          }
          canvas->presentRegionalRendering(source, participantIndex, region);
        },
        Qt::QueuedConnection);
      CHECK(queued) << "Failed to queue regional 3D presentation";
    }
    batch->progress[participantIndex] = frameResult.progress;
  }
  catch (...) {
    batch->recordFailure(std::current_exception());
    if (regionPending) {
      try {
        engine.abandonPendingVulkanInteractiveRegionAfterFailure();
        regionPending = false;
      }
      catch (...) {
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

  const size_t workerParticipantCount = participantCount() - static_cast<size_t>(canonicalParticipates());
  std::shared_ptr<const VulkanTileRenderState> publishedState;
  if (workerParticipantCount > 0u) {
    publishedState = m_canonicalEngine.publishVulkanTileRenderState();
    CHECK(publishedState != nullptr);
  }
  auto batch = std::make_shared<Batch>(tiles, renderStereoPair, cancellationToken);

  std::vector<std::future<void>> workerTasks;
  workerTasks.reserve(workerParticipantCount);
  for (const auto& participant : m_participants) {
    Lane* const lane = participant.get();
    if (lane == nullptr) {
      continue;
    }
    CHECK(lane->engine != nullptr);
    workerTasks.push_back(lane->invoke([engine = lane->engine, publishedState, batch]() {
      renderLane(*engine, publishedState.get(), batch);
    }));
  }
  CHECK_EQ(workerTasks.size(), workerParticipantCount);

  for (const auto& participant : m_participants) {
    if (participant == nullptr) {
      renderLane(m_canonicalEngine, nullptr, batch);
    }
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

size_t Z3DRenderingEngine::ZVulkanTileWorkerPool::participantCount() const noexcept
{
  return m_participants.size();
}

bool Z3DRenderingEngine::ZVulkanTileWorkerPool::canonicalParticipates() const noexcept
{
  return std::ranges::any_of(m_participants, [](const auto& participant) {
    return participant == nullptr;
  });
}

void Z3DRenderingEngine::ZVulkanTileWorkerPool::detachRegionalPresentationSources()
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread());
  CHECK(m_presentationLifetime != nullptr);

  auto replacement = std::make_shared<PresentationLifetime>();
  {
    std::unique_lock lock(m_presentationLifetime->mutex);
    CHECK(m_presentationLifetime->enabled);
    m_presentationLifetime->enabled = false;
  }
  m_presentationLifetime = std::move(replacement);
}

void Z3DRenderingEngine::ZVulkanTileWorkerPool::invalidateInteractiveRegions()
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread());
  m_interactiveRegions.clear();
  m_interactiveProgress.clear();
  m_interactiveQueriesReady = false;
}

void Z3DRenderingEngine::ZVulkanTileWorkerPool::invalidateInteractiveQueries()
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread());
  m_interactiveQueriesReady = false;
}

void Z3DRenderingEngine::ZVulkanTileWorkerPool::beginInteractiveRegions(std::span<const Z3DTileDescriptor> regions)
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread())
    << "Vulkan interactive regions must start on the canonical engine thread";
  CHECK_EQ(regions.size(), participantCount())
    << "Vulkan interactive rendering requires exactly one fixed region per selected participant";
  validateTileBatch(regions);
  m_interactiveQueriesReady = false;

  const size_t workerParticipantCount = participantCount() - static_cast<size_t>(canonicalParticipates());
  std::shared_ptr<const VulkanTileRenderState> publishedState;
  if (workerParticipantCount > 0u) {
    publishedState = m_canonicalEngine.publishVulkanTileRenderState();
    CHECK(publishedState != nullptr);
  }

  std::vector<std::future<void>> workerTasks;
  workerTasks.reserve(workerParticipantCount);
  for (size_t participantIndex = 0u; participantIndex < m_participants.size(); ++participantIndex) {
    Lane* const lane = m_participants[participantIndex].get();
    if (lane == nullptr) {
      continue;
    }
    CHECK(lane->engine != nullptr);
    workerTasks.push_back(lane->invoke([engine = lane->engine, publishedState, region = regions[participantIndex]]() {
      prepareInteractiveParticipant(*engine, publishedState.get(), region);
    }));
  }
  CHECK_EQ(workerTasks.size(), workerParticipantCount);

  FailureSelection failures;

  for (size_t participantIndex = 0u; participantIndex < m_participants.size(); ++participantIndex) {
    if (m_participants[participantIndex] != nullptr) {
      continue;
    }
    try {
      prepareInteractiveParticipant(m_canonicalEngine, nullptr, regions[participantIndex]);
    }
    catch (...) {
      failures.record(std::current_exception());
    }
  }

  for (auto& task : workerTasks) {
    try {
      task.get();
    }
    catch (...) {
      failures.record(std::current_exception());
    }
  }

  if (const std::exception_ptr failure = failures.selected(); failure != nullptr) {
    std::rethrow_exception(failure);
  }

  m_interactiveRegions.assign(regions.begin(), regions.end());
  m_interactiveProgress.assign(regions.size(), 0.0);
}

double Z3DRenderingEngine::ZVulkanTileWorkerPool::renderInteractiveRegions(folly::CancellationToken cancellationToken,
                                                                           Z3DCanvas& canvas)
{
  CHECK(QThread::currentThread() == m_canonicalEngine.thread())
    << "Vulkan interactive regions must render on the canonical engine thread";
  CHECK_EQ(m_interactiveRegions.size(), participantCount())
    << "Vulkan interactive rendering requires a prepared fixed region for every participant";
  CHECK_EQ(m_interactiveProgress.size(), participantCount());
  maybeCancel(cancellationToken);

  auto batch = std::make_shared<InteractiveBatch>(m_interactiveRegions,
                                                  m_interactiveProgress,
                                                  cancellationToken,
                                                  canvas,
                                                  m_presentationLifetime);

  std::vector<std::future<void>> workerTasks;
  workerTasks.reserve(participantCount() - static_cast<size_t>(canonicalParticipates()));
  for (size_t participantIndex = 0u; participantIndex < m_participants.size(); ++participantIndex) {
    Lane* const lane = m_participants[participantIndex].get();
    if (lane == nullptr) {
      continue;
    }
    if (m_interactiveProgress[participantIndex] == 1.0) {
      continue;
    }
    CHECK(lane->engine != nullptr);
    workerTasks.push_back(lane->invoke([engine = lane->engine, batch, participantIndex]() {
      renderInteractiveParticipant(*engine, batch, participantIndex);
    }));
  }

  for (size_t participantIndex = 0u; participantIndex < m_participants.size(); ++participantIndex) {
    if (m_participants[participantIndex] != nullptr) {
      continue;
    }
    if (m_interactiveProgress[participantIndex] < 1.0) {
      renderInteractiveParticipant(m_canonicalEngine, batch, participantIndex);
    }
  }

  for (auto& task : workerTasks) {
    task.get();
  }

  if (const std::exception_ptr failure = batch->failure(); failure != nullptr) {
    std::rethrow_exception(failure);
  }
  maybeCancel(cancellationToken);

  double minimumProgress = 1.0;
  for (const double participantProgress : batch->progress) {
    CHECK_GE(participantProgress, 0.0);
    CHECK_LE(participantProgress, 1.0);
    minimumProgress = std::min(minimumProgress, participantProgress);
  }
  m_interactiveProgress = std::move(batch->progress);
  m_interactiveQueriesReady = true;
  if (minimumProgress < 1.0) {
    return minimumProgress;
  }

  m_interactiveProgress.clear();
  return minimumProgress;
}

std::optional<glm::uvec2> Z3DRenderingEngine::ZVulkanTileWorkerPool::fullOutputPixel(glm::ivec2 logicalPosition,
                                                                                     double devicePixelRatio) const
{
  CHECK(std::isfinite(devicePixelRatio));
  CHECK_GE(devicePixelRatio, 1.0);
  CHECK(!m_interactiveRegions.empty());
  CHECK_EQ(m_interactiveRegions.size(), participantCount());
  if (logicalPosition.x < 0 || logicalPosition.y < 0) {
    return std::nullopt;
  }
  const glm::uvec2 fullExtent = m_interactiveRegions.front().fullOutputExtent();
  const double physicalX = static_cast<double>(logicalPosition.x) * devicePixelRatio;
  const double physicalY = static_cast<double>(logicalPosition.y) * devicePixelRatio;
  if (physicalX >= fullExtent.x || physicalY >= fullExtent.y) {
    return std::nullopt;
  }
  return glm::uvec2(static_cast<uint32_t>(physicalX), static_cast<uint32_t>(physicalY));
}

size_t Z3DRenderingEngine::ZVulkanTileWorkerPool::participantForPixel(glm::uvec2 pixel) const
{
  CHECK_EQ(m_interactiveRegions.size(), participantCount());
  std::optional<size_t> owner;
  for (size_t index = 0u; index < m_interactiveRegions.size(); ++index) {
    if (!m_interactiveRegions[index].containsTopLeftOutputPixel(pixel)) {
      continue;
    }
    CHECK(!owner.has_value()) << "A Vulkan interactive pixel belongs to more than one fixed region";
    owner = index;
  }
  CHECK(owner.has_value()) << "A Vulkan interactive pixel has no fixed-region owner";
  return *owner;
}

Z3DRenderingEngine& Z3DRenderingEngine::ZVulkanTileWorkerPool::participantEngine(size_t participantIndex) const
{
  CHECK_LT(participantIndex, m_participants.size());
  const auto& participant = m_participants[participantIndex];
  if (participant == nullptr) {
    return m_canonicalEngine;
  }
  CHECK(participant->engine != nullptr);
  return *participant->engine;
}

Z3DPickingManager::PickingObject Z3DRenderingEngine::ZVulkanTileWorkerPool::objectAt(glm::ivec2 logicalPosition,
                                                                                     double devicePixelRatio) const
{
  if (!m_interactiveQueriesReady) {
    return {};
  }
  const std::optional<glm::uvec2> pixel = fullOutputPixel(logicalPosition, devicePixelRatio);
  if (!pixel.has_value()) {
    return {};
  }
  const size_t participantIndex = participantForPixel(*pixel);
  Z3DRenderingEngine& engine = participantEngine(participantIndex);
  VulkanRegionalPickingHit hit;
  const glm::uvec2 attachmentPixel = m_interactiveRegions[participantIndex].topLeftAttachmentPixel(*pixel);
  if (&engine == &m_canonicalEngine) {
    hit = engine.queryVulkanRegionalPickingObject(attachmentPixel);
  } else {
    m_participants[participantIndex]
      ->invoke([&engine, attachmentPixel, &hit]() {
        hit = engine.queryVulkanRegionalPickingObject(attachmentPixel);
      })
      .get();
  }
  return m_canonicalEngine.resolveVulkanRegionalPickingHit(hit);
}

GLfloat Z3DRenderingEngine::ZVulkanTileWorkerPool::depthAt(glm::ivec2 logicalPosition, double devicePixelRatio) const
{
  if (!m_interactiveQueriesReady) {
    return 1.f;
  }
  const std::optional<glm::uvec2> pixel = fullOutputPixel(logicalPosition, devicePixelRatio);
  if (!pixel.has_value()) {
    return 1.f;
  }
  const size_t participantIndex = participantForPixel(*pixel);
  Z3DRenderingEngine& engine = participantEngine(participantIndex);
  GLfloat depth = 1.f;
  const glm::uvec2 attachmentPixel = m_interactiveRegions[participantIndex].topLeftAttachmentPixel(*pixel);
  if (&engine == &m_canonicalEngine) {
    return engine.queryVulkanRegionalPickingDepth(attachmentPixel);
  }
  m_participants[participantIndex]
    ->invoke([&engine, attachmentPixel, &depth]() {
      depth = engine.queryVulkanRegionalPickingDepth(attachmentPixel);
    })
    .get();
  return depth;
}

std::vector<Z3DPickingManager::PickingObject>
Z3DRenderingEngine::ZVulkanTileWorkerPool::objectsByDistance(glm::ivec2 logicalPosition,
                                                             int logicalRadius,
                                                             bool ascend,
                                                             double devicePixelRatio) const
{
  if (!m_interactiveQueriesReady) {
    return {};
  }
  const std::optional<glm::uvec2> centerPixel = fullOutputPixel(logicalPosition, devicePixelRatio);
  if (!centerPixel.has_value()) {
    return {};
  }
  const glm::uvec2 center = *centerPixel;
  std::optional<uint32_t> physicalRadius;
  if (logicalRadius >= 0) {
    const double scaledRadius = std::ceil(static_cast<double>(logicalRadius) * devicePixelRatio);
    CHECK_LE(scaledRadius, static_cast<double>(std::numeric_limits<uint32_t>::max()));
    physicalRadius = static_cast<uint32_t>(scaledRadius);
  }

  boost::unordered_flat_map<Z3DPickingManager::PickingObject, uint64_t, Z3DPickingManager::PickingObjectHash>
    minimumDistances;
  for (size_t participantIndex = 0u; participantIndex < m_interactiveRegions.size(); ++participantIndex) {
    const Z3DTileDescriptor& region = m_interactiveRegions[participantIndex];
    const glm::uvec2 origin = region.topLeftAssemblyOrigin();
    const glm::uvec2 extent = region.validOutputExtent();
    const uint64_t radius = physicalRadius.value_or(std::numeric_limits<uint32_t>::max());
    const uint64_t centerX = center.x;
    const uint64_t centerY = center.y;
    if (physicalRadius.has_value() && (centerX + radius < origin.x || centerY + radius < origin.y ||
                                       centerX > static_cast<uint64_t>(origin.x) + extent.x - 1u + radius ||
                                       centerY > static_cast<uint64_t>(origin.y) + extent.y - 1u + radius)) {
      continue;
    }

    Z3DRenderingEngine& engine = participantEngine(participantIndex);
    std::vector<VulkanRegionalPickingDistance> regional;
    const glm::uvec2 validAttachmentOrigin = region.validAttachmentOrigin();
    const glm::i64vec2 attachmentReferencePixel(
      static_cast<int64_t>(validAttachmentOrigin.x) + static_cast<int64_t>(center.x) - origin.x,
      static_cast<int64_t>(validAttachmentOrigin.y) + static_cast<int64_t>(center.y) - origin.y);
    glm::uvec4 attachmentSearchRect(validAttachmentOrigin.x, validAttachmentOrigin.y, extent.x, extent.y);
    if (physicalRadius.has_value()) {
      const uint64_t radiusValue = *physicalRadius;
      const glm::uvec2 fullExtent = region.fullOutputExtent();
      const uint32_t fullBeginX =
        static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(center.x) - static_cast<int64_t>(radiusValue)));
      const uint32_t fullBeginY =
        static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(center.y) - static_cast<int64_t>(radiusValue)));
      const uint32_t fullEndX =
        static_cast<uint32_t>(std::min<uint64_t>(fullExtent.x - 1u, static_cast<uint64_t>(center.x) + radiusValue));
      const uint32_t fullEndY =
        static_cast<uint32_t>(std::min<uint64_t>(fullExtent.y - 1u, static_cast<uint64_t>(center.y) + radiusValue));
      const uint32_t intersectionBeginX = std::max(fullBeginX, origin.x);
      const uint32_t intersectionBeginY = std::max(fullBeginY, origin.y);
      const uint32_t intersectionEndX = std::min(fullEndX, origin.x + extent.x - 1u);
      const uint32_t intersectionEndY = std::min(fullEndY, origin.y + extent.y - 1u);
      CHECK_LE(intersectionBeginX, intersectionEndX);
      CHECK_LE(intersectionBeginY, intersectionEndY);
      const glm::uvec2 attachmentBegin =
        region.topLeftAttachmentPixel(glm::uvec2(intersectionBeginX, intersectionBeginY));
      attachmentSearchRect = glm::uvec4(attachmentBegin.x,
                                        attachmentBegin.y,
                                        intersectionEndX - intersectionBeginX + 1u,
                                        intersectionEndY - intersectionBeginY + 1u);
    }
    if (&engine == &m_canonicalEngine) {
      regional = engine.queryVulkanRegionalPickingObjectsByDistance(attachmentReferencePixel, attachmentSearchRect);
    } else {
      m_participants[participantIndex]
        ->invoke([&engine, attachmentReferencePixel, attachmentSearchRect, &regional]() {
          regional = engine.queryVulkanRegionalPickingObjectsByDistance(attachmentReferencePixel, attachmentSearchRect);
        })
        .get();
    }
    for (const auto& item : regional) {
      const Z3DPickingManager::PickingObject object = m_canonicalEngine.resolveVulkanRegionalPickingHit(item.hit);
      if (object.object == nullptr) {
        continue;
      }
      auto [it, inserted] = minimumDistances.try_emplace(object, item.squaredPhysicalPixelDistance);
      if (!inserted) {
        it->second = std::min(it->second, item.squaredPhysicalPixelDistance);
      }
    }
  }

  std::vector<std::pair<uint64_t, Z3DPickingManager::PickingObject>> sorted;
  sorted.reserve(minimumDistances.size());
  for (const auto& [object, distance] : minimumDistances) {
    sorted.emplace_back(distance, object);
  }
  std::ranges::sort(sorted, [ascend](const auto& lhs, const auto& rhs) {
    return ascend ? lhs.first < rhs.first : lhs.first > rhs.first;
  });
  std::vector<Z3DPickingManager::PickingObject> result;
  result.reserve(sorted.size());
  for (const auto& [distance, object] : sorted) {
    result.push_back(object);
  }
  return result;
}

std::optional<Z3DPickingManager::ImageDepthSample>
Z3DRenderingEngine::ZVulkanTileWorkerPool::imageDepthAtPhysicalInput(size_t imageObjectId,
                                                                     glm::ivec2 physicalInputPosition) const
{
  if (!m_interactiveQueriesReady) {
    return std::nullopt;
  }
  CHECK(!m_interactiveRegions.empty());
  CHECK_EQ(m_interactiveRegions.size(), participantCount());
  const glm::uvec2 fullPhysicalExtent = m_interactiveRegions.front().fullOutputExtent();
  const glm::uvec2 samplePixel =
    Z3DPickingManager::ImageDepthSample::samplePixelForPhysicalInput(physicalInputPosition, fullPhysicalExtent);
  const size_t participantIndex = participantForPixel(samplePixel);
  Z3DRenderingEngine& engine = participantEngine(participantIndex);
  std::optional<float> depth;
  const glm::uvec2 attachmentPixel = m_interactiveRegions[participantIndex].topLeftAttachmentPixel(samplePixel);
  if (&engine == &m_canonicalEngine) {
    depth = engine.queryVulkanRegionalImageDepth(imageObjectId, attachmentPixel);
  } else {
    m_participants[participantIndex]
      ->invoke([&engine, imageObjectId, attachmentPixel, &depth]() {
        depth = engine.queryVulkanRegionalImageDepth(imageObjectId, attachmentPixel);
      })
      .get();
  }
  if (!depth.has_value()) {
    return std::nullopt;
  }
  return Z3DPickingManager::ImageDepthSample{.depth = *depth,
                                             .topLeftPhysicalPixel = samplePixel,
                                             .fullPhysicalExtent = fullPhysicalExtent};
}

void Z3DRenderingEngine::ZVulkanTileWorkerPool::dispatchCanonicalInteractiveEvent(QEvent* event,
                                                                                  int fullLogicalWidth,
                                                                                  int fullLogicalHeight,
                                                                                  double devicePixelRatio)
{
  CHECK(event != nullptr);
  CHECK(QThread::currentThread() == m_canonicalEngine.thread());
  CHECK_GT(fullLogicalWidth, 0);
  CHECK_GT(fullLogicalHeight, 0);
  CHECK(std::isfinite(devicePixelRatio));
  CHECK_GE(devicePixelRatio, 1.0);

  std::optional<std::pair<glm::ivec2, Z3DPickingManager::PickingObject>> pointQueryCache;
  Z3DPickingManager::QueryOverride queryOverride{
    .objectAtWidgetPos =
      [this, devicePixelRatio, &pointQueryCache](glm::ivec2 position) {
        if (!pointQueryCache.has_value() || pointQueryCache->first.x != position.x ||
            pointQueryCache->first.y != position.y) {
          pointQueryCache = std::pair{position, objectAt(position, devicePixelRatio)};
        }
        return pointQueryCache->second;
      },
    .depthAtWidgetPos =
      [this, devicePixelRatio](glm::ivec2 position) {
        return depthAt(position, devicePixelRatio);
      },
    .sortObjectsByDistanceToPos =
      [this, devicePixelRatio](glm::ivec2 position, int radius, bool ascend) {
        return objectsByDistance(position, radius, ascend, devicePixelRatio);
      },
    .imageDepthAtPhysicalInput =
      [this](size_t imageObjectId, glm::ivec2 physicalInputPosition) {
        return imageDepthAtPhysicalInput(imageObjectId, physicalInputPosition);
      }};
  auto queryScope = m_canonicalEngine.m_globalParas->pickingManager.scopedQueryOverride(queryOverride);

  const std::optional<Z3DTileDescriptor> canonicalRegion = m_canonicalEngine.m_vulkanInteractiveRegion;
  m_canonicalEngine.camera().get().setTileFrustum();
  auto regionalFrustumGuard = folly::makeGuard([this, canonicalRegion]() {
    if (canonicalRegion.has_value()) {
      m_canonicalEngine.camera().get().setTileFrustum(canonicalRegion->normalizedLeft(),
                                                      canonicalRegion->normalizedRight(),
                                                      canonicalRegion->normalizedBottom(),
                                                      canonicalRegion->normalizedTop());
    } else {
      m_canonicalEngine.camera().get().setTileFrustum();
    }
  });

  event->ignore();
  for (Z3DCanvasEventListener* const listener : m_canonicalEngine.m_listeners) {
    CHECK(listener != nullptr);
    listener->onEvent(event, fullLogicalWidth, fullLogicalHeight);
    if (event->isAccepted()) {
      break;
    }
  }
}

} // namespace nim
