#include "z3dmeshfilter.h"

#include "z3dmeshrenderer.h"
#include "z3dtextureglowrenderer.h"
#include "zcancellation.h"
#include "zlog.h"
#include "zmesh.h"
#include "zneuroglancerremotecontext.h"
#include "zrandom.h"
#include "zregionannotation.h"
#include "zfolly.h"

#include "zcommandlineflags.h"

#include <QFileInfo>

#include <folly/OperationCancelled.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/WithCancellation.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

ABSL_FLAG(
  uint32_t,
  atlas_ng_mesh_runtime_lod_in_flight_mb_per_thread_interaction,
  8,
  "Runtime NG mesh LOD: max encoded fragment bytes in flight per CPU thread while interacting (0 disables byte budgeting)");

ABSL_FLAG(
  uint32_t,
  atlas_ng_mesh_runtime_lod_in_flight_mb_per_thread_idle,
  32,
  "Runtime NG mesh LOD: max encoded fragment bytes in flight per CPU thread when idle (0 disables byte budgeting)");

ABSL_FLAG(
  bool,
  atlas_ng_mesh_runtime_lod_gpu_transform,
  true,
  "Runtime NG mesh LOD: keep chunk vertices in Neuroglancer's stored quantized fragment coordinates and apply the "
  "per-chunk vertex transform on GPU (OpenGL + Vulkan) instead of CPU-transforming every vertex.");

namespace nim {

namespace {

// Runtime Neuroglancer mesh LOD uses two screen-space targets:
// - a looser interaction target to keep camera motion responsive
// - Neuroglancer's default per-pixel target once motion settles
//
// Neuroglancer's `renderScaleTarget` is not a large absolute number; the
// default is `1`, meaning the chosen mesh resolution should be no worse than
// the current pixel footprint. Our first Atlas integration accidentally used
// the large numbers from unit-test examples (`1000`/`4000`) as runtime policy
// values, which made the coarsest loaded rows effectively "good enough" for
// almost every view and prevented visible refinement.
constexpr float kRuntimeNgInteractionDetailCutoff = 4.0F;
constexpr float kRuntimeNgIdleDetailCutoff = 1.0F;
constexpr int kRuntimeNgIdleDelayMs = 180;
// Runtime mesh LOD must stay visually responsive when many external meshes are
// present. We therefore cap per-filter parallel chunk loads so one mesh cannot
// monopolize CPU/network resources. When interaction settles we can raise the
// cap to converge to the idle detail target faster.
constexpr size_t kRuntimeNgMaxTasksMultiplierInteraction = 4;
constexpr size_t kRuntimeNgMaxTasksMultiplierIdle = 8;

struct RuntimeNeuroglancerOpenResult
{
  uint64_t epoch = 0;
  QString error;
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source;
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest> manifest;
  std::vector<uint32_t> baseRows;
};

struct RuntimeNeuroglancerChunkLoadResult
{
  uint64_t epoch = 0;
  uint32_t row = 0;
  uint64_t encodedBytes = 0;
  bool cancelled = false;
  QString error;
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh> chunk;
};

template<class Fn>
void postToMeshFilterThread(Z3DMeshFilter* filter, Fn&& fn)
{
  CHECK(filter);
  (void)QMetaObject::invokeMethod(
    filter,
    [filter, fn = std::forward<Fn>(fn)]() mutable {
      try {
        fn(*filter);
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Unhandled exception in runtime Neuroglancer mesh callback: " << e.what();
      }
      catch (...) {
        LOG(ERROR) << "Unhandled non-std exception in runtime Neuroglancer mesh callback";
      }
    },
    Qt::QueuedConnection);
}

folly::coro::Task<void> wrapRuntimeNeuroglancerTask(folly::coro::Task<void> task, std::string label)
{
  try {
    co_await std::move(task);
  }
  catch (const ZCancellationException&) {
    co_return;
  }
  catch (const folly::OperationCancelled&) {
    co_return;
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Runtime Neuroglancer mesh coroutine failed" << (label.empty() ? "" : " label='")
               << (label.empty() ? "" : label) << (label.empty() ? "" : "'") << ": " << e.what();
    co_return;
  }
  catch (...) {
    LOG(ERROR) << "Runtime Neuroglancer mesh coroutine failed" << (label.empty() ? "" : " label='")
               << (label.empty() ? "" : label) << (label.empty() ? "" : "'");
    co_return;
  }
  co_return;
}

[[nodiscard]] bool sameOptionalArray(const std::optional<std::array<double, 3>>& lhs,
                                     const std::optional<std::array<double, 3>>& rhs)
{
  return lhs == rhs;
}

[[nodiscard]] bool sameOptionalArray(const std::optional<std::array<int64_t, 3>>& lhs,
                                     const std::optional<std::array<int64_t, 3>>& rhs)
{
  return lhs == rhs;
}

[[nodiscard]] std::vector<uint32_t>
collectRuntimeNeuroglancerBaseRows(const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest& manifest)
{
  std::vector<uint32_t> baseRows;
  const uint32_t coarsestLod = manifest.coarsestStoredLod().value_or(0U);
  for (uint32_t row = 0; row < manifest.octreeNodes.size(); ++row) {
    if (manifest.rowLods[row] != coarsestLod) {
      continue;
    }
    if (manifest.octreeNodes[row].empty()) {
      continue;
    }
    if (manifest.offsets[row + 1U] <= manifest.offsets[row]) {
      continue;
    }
    baseRows.push_back(row);
  }
  return baseRows;
}

[[nodiscard]] size_t runtimeNgWorkerThreadCount()
{
  return atlasBackgroundExecutorThreadCount();
}

[[nodiscard]] size_t runtimeNgMaxTasksInFlight(bool interactionActive)
{
  const size_t threads = runtimeNgWorkerThreadCount();
  const size_t multiplier =
    interactionActive ? kRuntimeNgMaxTasksMultiplierInteraction : kRuntimeNgMaxTasksMultiplierIdle;
  return std::max<size_t>(1, threads * multiplier);
}

[[nodiscard]] uint64_t runtimeNgMaxEncodedBytesInFlight(bool interactionActive)
{
  const size_t threads = runtimeNgWorkerThreadCount();
  const uint32_t mbPerThread = interactionActive
                                 ? absl::GetFlag(FLAGS_atlas_ng_mesh_runtime_lod_in_flight_mb_per_thread_interaction)
                                 : absl::GetFlag(FLAGS_atlas_ng_mesh_runtime_lod_in_flight_mb_per_thread_idle);
  if (mbPerThread == 0U) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(mbPerThread) * threads * 1024ULL * 1024ULL;
}

[[nodiscard]] uint64_t encodedBytesForManifestRow(const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest& manifest,
                                                  uint32_t row)
{
  if (row + 1U >= manifest.offsets.size()) {
    return 0;
  }
  const uint64_t begin = manifest.offsets[row];
  const uint64_t end = manifest.offsets[row + 1U];
  return end > begin ? (end - begin) : 0;
}

[[nodiscard]] bool hasImportedMeshGeometry(const std::vector<ZMesh*>& meshes)
{
  for (const ZMesh* mesh : meshes) {
    if (mesh != nullptr && !mesh->empty()) {
      return true;
    }
  }
  return false;
}

folly::coro::Task<RuntimeNeuroglancerOpenResult>
openRuntimeNeuroglancerSourceAsync(uint64_t epoch,
                                   ZNeuroglancerMeshExternalSourceKey key,
                                   std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext,
                                   folly::CancellationToken cancellationToken)
{
  RuntimeNeuroglancerOpenResult out;
  out.epoch = epoch;
  try {
    maybeCancel(cancellationToken);
    CHECK(key.baseResolutionNm);
    CHECK(key.baseVoxelOffset);
    const auto source = remoteContext
                          ? co_await folly::coro::co_withCancellation(
                              cancellationToken,
                              ZNeuroglancerPrecomputedMeshSource::openAsync(QUrl(key.meshSourceDirUrl),
                                                                            *key.baseResolutionNm,
                                                                            *key.baseVoxelOffset,
                                                                            std::move(remoteContext)))
                          : co_await folly::coro::co_withCancellation(
                              cancellationToken,
                              ZNeuroglancerPrecomputedMeshSource::openAsync(QUrl(key.meshSourceDirUrl),
                                                                            *key.baseResolutionNm,
                                                                            *key.baseVoxelOffset,
                                                                            std::chrono::milliseconds(30000),
                                                                            nullptr));
    maybeCancel(cancellationToken);
    CHECK(source);
    if (!source->supportsRuntimeLod()) {
      out.error = QStringLiteral("dataset does not support multiscale mesh LOD");
      co_return out;
    }

    out.source = source;
    out.manifest =
      co_await folly::coro::co_withCancellation(cancellationToken, source->loadManifestAsync(key.segmentId));
    maybeCancel(cancellationToken);
    CHECK(out.manifest);
    out.baseRows = collectRuntimeNeuroglancerBaseRows(*out.manifest);
  }
  catch (const ZCancellationException&) {
    out.error = QStringLiteral("cancelled");
  }
  catch (const folly::OperationCancelled&) {
    out.error = QStringLiteral("cancelled");
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
  }
  co_return out;
}

folly::coro::Task<RuntimeNeuroglancerChunkLoadResult>
loadRuntimeNeuroglancerChunkAsync(uint64_t epoch,
                                  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source,
                                  uint64_t segmentId,
                                  uint32_t row,
                                  ZNeuroglancerPrecomputedMeshSource::ChunkVertexSpace vertexSpace,
                                  uint64_t encodedBytes,
                                  folly::CancellationToken cancellationToken)
{
  RuntimeNeuroglancerChunkLoadResult out;
  out.epoch = epoch;
  out.row = row;
  out.encodedBytes = encodedBytes;
  CHECK(source);
  try {
    maybeCancel(cancellationToken);
    auto chunkMesh = co_await folly::coro::co_withCancellation(cancellationToken,
                                                               source->loadChunkMeshAsync(segmentId, row, vertexSpace));
    maybeCancel(cancellationToken);
    CHECK(chunkMesh);
    out.chunk = std::move(chunkMesh);
  }
  catch (const ZCancellationException&) {
    out.cancelled = true;
  }
  catch (const folly::OperationCancelled&) {
    out.cancelled = true;
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
  }
  co_return out;
}

[[nodiscard]] std::vector<RuntimeNeuroglancerChunkLoadResult>
loadRuntimeNeuroglancerRowsBlockingForExport(const std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource>& source,
                                             uint64_t segmentId,
                                             ZNeuroglancerPrecomputedMeshSource::ChunkVertexSpace vertexSpace,
                                             const std::vector<uint32_t>& rows,
                                             folly::CancellationToken cancellationToken)
{
  CHECK(source);

  std::vector<RuntimeNeuroglancerChunkLoadResult> out;
  if (rows.empty()) {
    return out;
  }

  maybeCancel(cancellationToken);

  std::vector<folly::coro::TaskWithExecutor<RuntimeNeuroglancerChunkLoadResult>> tasks;
  tasks.reserve(rows.size());
  for (const uint32_t row : rows) {
    tasks.push_back(folly::coro::co_withExecutor(
      getAtlasBackgroundExecutor(),
      loadRuntimeNeuroglancerChunkAsync(0U, source, segmentId, row, vertexSpace, 0U, cancellationToken)));
  }
  auto results =
    folly::coro::blockingWait(folly::coro::collectAllWindowed(std::move(tasks), runtimeNgMaxTasksInFlight(false)));
  maybeCancel(cancellationToken);
  return results;
}

} // namespace

Z3DMeshFilter::Z3DMeshFilter(Z3DGlobalParameters& globalParas, const RegionNode* regionNode, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_triangleListRenderer(m_rendererBase)
  , m_wireframeMode("Wireframe Option")
  , m_wireframeColor("Wireframe Color", glm::vec4(1), glm::vec4(0), glm::vec4(1))
  , m_colorMode("Color Mode")
  , m_singleColorForAllMesh("Mesh Color",
                            glm::vec4(ZRandom::instance().randReal<float>(),
                                      ZRandom::instance().randReal<float>(),
                                      ZRandom::instance().randReal<float>(),
                                      1.f))
  , m_glow("Glow", false)
  , m_glowMode("Glow Mode")
  , m_glowBlurRadius("Glow Blur Radius", 10, 2, 1000)
  , m_glowBlurScale("Glow Blur Scale", 1.f, 1.f, 5.f)
  , m_glowBlurStrength("Glow Blur Strength", .5f, 0.f, 1.f)
  , m_selectMeshEvent("Select Mesh", false)
  , m_pressedMesh(nullptr)
  , m_selectedMeshes(nullptr)
  , m_dataIsInvalid(false)
  , m_regionNode(regionNode)
{
  m_singleColorForAllMesh.setStyle("COLOR");
  connect(&m_singleColorForAllMesh, &ZVec4Parameter::valueChanged, this, &Z3DMeshFilter::prepareColor);

  // Color Mode
  m_colorMode.addOptions("Mesh Color", "Single Color");
  m_colorMode.select("Single Color");
  m_colorMode.setDescription(QStringLiteral(
    "Controls how mesh surface colors are chosen:\n"
    "- 'Single Color' (recommended when you want to control color) applies the 'Mesh Color' parameter below as a solid color for the whole mesh.\n"
    "- 'Mesh Color' reads color attributes embedded in the mesh file (per-vertex/face). In this mode you cannot override the color with parameters;\n"
    "  if the mesh lacks embedded colors, the fill color falls back to the contained/default color (often black). Choose 'Single Color' to force a specific color."));

  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::prepareColor);
  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::adjustWidgets);

  m_wireframeMode.addOptionsWithData(
    std::make_pair("No Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::NoWireframe)),
    std::make_pair("With Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::WithWireframe)),
    std::make_pair("Only Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::OnlyWireframe)));
  m_wireframeMode.select("No Wireframe");
  m_wireframeColor.setStyle("COLOR");
  updateWireframeMode();
  updateWireframeColor();
  connect(&m_wireframeMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::updateWireframeMode);
  connect(&m_wireframeColor, &ZVec4Parameter::valueChanged, this, &Z3DMeshFilter::updateWireframeColor);

  addParameter(m_colorMode);

  addParameter(m_singleColorForAllMesh);
  m_singleColorForAllMesh.setDescription(
    QStringLiteral("Solid RGBA color used when 'Color Mode' is set to 'Single Color'."));

  addParameter(m_wireframeMode);
  addParameter(m_wireframeColor);
  m_wireframeMode.setDescription(QStringLiteral("Render meshes with or without a wireframe overlay."));
  m_wireframeColor.setDescription(QStringLiteral("Wireframe line color (applies when wireframe is visible)."));

  connect(&m_glow, &ZBoolParameter::valueChanged, this, &Z3DMeshFilter::adjustWidgets);
  addParameter(m_glow);
  // Initialize glow parameter defaults to match previous behavior
  m_glowMode.clearOptions();
  m_glowMode.addOptionsWithData(
    std::make_pair(enumToQString(GlowMode::Additive), static_cast<int>(GlowMode::Additive)),
    std::make_pair(enumToQString(GlowMode::Screen), static_cast<int>(GlowMode::Screen)),
    std::make_pair(enumToQString(GlowMode::Softlight), static_cast<int>(GlowMode::Softlight)),
    std::make_pair(enumToQString(GlowMode::Glowmap), static_cast<int>(GlowMode::Glowmap)));
  m_glowMode.select(enumToQString(GlowMode::Screen));
  m_glowBlurScale.setSingleStep(0.5f);
  addParameter(m_glowMode);
  addParameter(m_glowBlurRadius);
  addParameter(m_glowBlurScale);
  addParameter(m_glowBlurStrength);
  m_glow.setDescription(QStringLiteral("Enable a post-processing glow around bright surfaces."));
  m_glowMode.setDescription(QStringLiteral("Blend mode for the glow pass."));
  m_glowBlurRadius.setDescription(QStringLiteral("Radius of the glow blur kernel (pixels)."));
  m_glowBlurScale.setDescription(QStringLiteral("Scale factor applied to the glow blur radius."));
  m_glowBlurStrength.setDescription(QStringLiteral("Opacity applied when compositing the glow."));

  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonDblClick);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonDblClick);
  m_selectMeshEvent.listenTo("append select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectMeshEvent.listenTo("append select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonRelease);
  connect(&m_selectMeshEvent, &ZEventListenerParameter::mouseEventTriggered, this, &Z3DMeshFilter::selectMesh);
  addEventListener(m_selectMeshEvent);

  m_runtimeNgIdleTimer.setSingleShot(true);
  connect(&m_runtimeNgIdleTimer, &QTimer::timeout, this, &Z3DMeshFilter::onRuntimeNeuroglancerIdleTimeout);
  connect(&m_globalParameters.camera,
          &Z3DCameraParameter::valueChanged,
          this,
          &Z3DMeshFilter::onRuntimeNeuroglancerCameraChanged);
  connect(this,
          &Z3DBoundedFilter::rendererCoordTransformChanged,
          this,
          &Z3DMeshFilter::markRuntimeNeuroglancerLodDirty);

  m_runtimeNgCancellationSource = std::make_shared<folly::CancellationSource>();

  adjustWidgets();
}

Z3DMeshFilter::~Z3DMeshFilter()
{
  if (m_runtimeNgCancellationSource) {
    m_runtimeNgCancellationSource->requestCancellation();
  }
  (void)cancelRuntimeNeuroglancerInFlightRows();
  folly::coro::blockingWait(m_runtimeNgTaskScope.joinAsync());
}

QString Z3DMeshFilter::regionName() const
{
  return m_regionNode ? m_regionNode->name : QString();
}

double Z3DMeshFilter::process(Z3DEye)
{
  syncRendererState();
  applyRuntimeNeuroglancerSelection();

  if (m_dataIsInvalid) {
    prepareData();
  }

  return 1.;
}

void Z3DMeshFilter::setProgressiveRenderingMode(bool v)
{
  m_runtimeNgProgressiveRendering = v;
}

void Z3DMeshFilter::spawnRuntimeNeuroglancerTask(folly::coro::Task<void> task, std::string_view debugLabel)
{
  m_runtimeNgTaskScope.add(
    folly::coro::co_withExecutor(getAtlasBackgroundExecutor(),
                                 wrapRuntimeNeuroglancerTask(std::move(task), std::string(debugLabel))));
}

void Z3DMeshFilter::beginExportMeshLod(const glm::uvec2& fullViewport, folly::CancellationToken cancellationToken)
{
  if (!m_runtimeNgSourceKey || fullViewport.x == 0U || fullViewport.y == 0U) {
    return;
  }

  maybeCancel(cancellationToken);

  m_runtimeNgIdleTimer.stop();
  m_runtimeNgInteractionActive = false;

  if (!m_runtimeNgSource || !m_runtimeNgManifest) {
    const RuntimeNeuroglancerOpenResult openResult =
      folly::coro::blockingWait(openRuntimeNeuroglancerSourceAsync(m_runtimeNgEpoch,
                                                                   *m_runtimeNgSourceKey,
                                                                   m_runtimeNgRemoteContext,
                                                                   cancellationToken));
    maybeCancel(cancellationToken);
    if (!openResult.error.isEmpty()) {
      LOG(WARNING) << fmt::format("Mesh export LOD preload skipped: {}", openResult.error);
      return;
    }
    m_runtimeNgSource = openResult.source;
    m_runtimeNgManifest = openResult.manifest;
    m_runtimeNgBaseRows = openResult.baseRows;
    m_runtimeNgBaseReady = m_runtimeNgBaseRows.empty();
  }

  CHECK(m_runtimeNgSource);
  CHECK(m_runtimeNgManifest);

  // Export owns the full-view preload barrier. Cancel any stale view-driven
  // refinement so it does not compete with export chunk loads for the same
  // background executor capacity.
  clearRuntimeNeuroglancerRequestFrontier();
  (void)cancelRuntimeNeuroglancerInFlightRows();

  m_runtimeNgFailedRows.clear();

  const glm::mat4 modelViewProjection = m_globalParameters.camera.get().projectionViewMatrix(MonoEye) *
                                        coordTransform() * m_runtimeNgSource->multiLodStoredToLocalVoxelTransform();
  const auto clippingPlanes = ZNeuroglancerPrecomputedMeshSource::getFrustumPlanes(modelViewProjection);
  const auto desiredRows = ZNeuroglancerPrecomputedMeshSource::desiredChunksForView(*m_runtimeNgManifest,
                                                                                    modelViewProjection,
                                                                                    clippingPlanes,
                                                                                    kRuntimeNgIdleDetailCutoff,
                                                                                    fullViewport.x,
                                                                                    fullViewport.y);

  std::vector<uint32_t> rowsToLoad = m_runtimeNgBaseRows;
  rowsToLoad.reserve(rowsToLoad.size() + desiredRows.size());
  for (const auto& desired : desiredRows) {
    if (desired.empty) {
      continue;
    }
    rowsToLoad.push_back(desired.row);
  }
  std::ranges::sort(rowsToLoad);
  rowsToLoad.erase(std::unique(rowsToLoad.begin(), rowsToLoad.end()), rowsToLoad.end());

  QString firstError;
  const auto vertexSpace = absl::GetFlag(FLAGS_atlas_ng_mesh_runtime_lod_gpu_transform)
                             ? ZNeuroglancerPrecomputedMeshSource::ChunkVertexSpace::Quantized
                             : ZNeuroglancerPrecomputedMeshSource::ChunkVertexSpace::LocalVoxel;
  std::vector<uint32_t> missingRows;
  missingRows.reserve(rowsToLoad.size());
  for (const uint32_t row : rowsToLoad) {
    if (!m_runtimeNgLoadedRows.contains(row)) {
      missingRows.push_back(row);
    }
  }

  if (!missingRows.empty() && m_runtimeNgSourceKey) {
    VLOG(1) << fmt::format(
      "Mesh export LOD preload start: segment={} mesh='{}' rows={} missingRows={} backgroundThreads={} "
      "dispatchPolicy=windowed_rows window={}",
      m_runtimeNgSourceKey->segmentId,
      m_runtimeNgSourceKey->meshSourceDirUrl,
      rowsToLoad.size(),
      missingRows.size(),
      runtimeNgWorkerThreadCount(),
      runtimeNgMaxTasksInFlight(false));
  }

  const std::vector<RuntimeNeuroglancerChunkLoadResult> preloadResults =
    loadRuntimeNeuroglancerRowsBlockingForExport(m_runtimeNgSource,
                                                 m_runtimeNgSourceKey->segmentId,
                                                 vertexSpace,
                                                 missingRows,
                                                 cancellationToken);
  maybeCancel(cancellationToken);
  for (auto result : preloadResults) {
    if (result.chunk) {
      m_runtimeNgLoadedRows[result.row] = std::move(result.chunk);
      continue;
    }
    m_runtimeNgFailedRows.insert(result.row);
    if (firstError.isEmpty() && !result.error.isEmpty()) {
      firstError = std::move(result.error);
    }
  }

  if (!missingRows.empty() && m_runtimeNgSourceKey) {
    VLOG(1) << fmt::format("Mesh export LOD preload complete: segment={} mesh='{}' loadedRows={} failedRows={}",
                           m_runtimeNgSourceKey->segmentId,
                           m_runtimeNgSourceKey->meshSourceDirUrl,
                           m_runtimeNgLoadedRows.size(),
                           m_runtimeNgFailedRows.size());
  }

  if (!firstError.isEmpty()) {
    LOG(WARNING) << fmt::format("Mesh export LOD preload had failures: {}", firstError);
  }

  m_runtimeNgBaseReady = std::ranges::all_of(m_runtimeNgBaseRows, [this](uint32_t row) {
    return m_runtimeNgLoadedRows.contains(row);
  });

  const auto drawChunks = ZNeuroglancerPrecomputedMeshSource::chunksToDrawForView(
    *m_runtimeNgManifest,
    modelViewProjection,
    clippingPlanes,
    kRuntimeNgIdleDetailCutoff,
    fullViewport.x,
    fullViewport.y,
    [this](uint32_t /*lod*/, uint32_t row, float /*renderScale*/) {
      return m_runtimeNgLoadedRows.contains(row);
    });

  std::vector<ZMesh*> frozenVisibleMeshes;
  std::vector<glm::mat4> frozenVisiblePosTransforms;
  std::vector<glm::mat3> frozenVisiblePosTransformNormalMatrices;
  const bool wantGpuTransform = absl::GetFlag(FLAGS_atlas_ng_mesh_runtime_lod_gpu_transform);
  for (const auto& drawChunk : drawChunks) {
    auto it = m_runtimeNgLoadedRows.find(drawChunk.row);
    if (it == m_runtimeNgLoadedRows.end()) {
      continue;
    }
    const auto& chunkMesh = it->second;
    CHECK(chunkMesh);
    if (wantGpuTransform) {
      CHECK(chunkMesh->hasVertexToLocalVoxelTransforms())
        << "Runtime NG GPU transform requires chunk meshes to supply vertex->local transforms";
      CHECK(chunkMesh->vertexToLocalVoxelTransforms.size() == chunkMesh->subMeshes.size());
      CHECK(chunkMesh->vertexToLocalVoxelNormalMatrices.size() == chunkMesh->subMeshes.size());
    }
    CHECK(drawChunk.subChunkBegin <= drawChunk.subChunkEnd);
    CHECK(drawChunk.subChunkEnd <= chunkMesh->subMeshes.size());
    for (uint32_t i = drawChunk.subChunkBegin; i < drawChunk.subChunkEnd; ++i) {
      const auto& subMesh = chunkMesh->subMeshes[i];
      if (subMesh && !subMesh->empty()) {
        frozenVisibleMeshes.push_back(subMesh.get());
        if (wantGpuTransform) {
          frozenVisiblePosTransforms.push_back(chunkMesh->vertexToLocalVoxelTransforms[i]);
          frozenVisiblePosTransformNormalMatrices.push_back(chunkMesh->vertexToLocalVoxelNormalMatrices[i]);
        }
      }
    }
  }

  m_runtimeNgFrozenVisibleMeshes = std::move(frozenVisibleMeshes);
  m_runtimeNgFrozenVisiblePosTransforms = std::move(frozenVisiblePosTransforms);
  m_runtimeNgFrozenVisiblePosTransformNormalMatrices = std::move(frozenVisiblePosTransformNormalMatrices);
  m_runtimeNgExportActive = true;
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::endExportMeshLod()
{
  if (!m_runtimeNgExportActive) {
    return;
  }

  m_runtimeNgExportActive = false;
  m_runtimeNgFrozenVisibleMeshes.clear();
  m_runtimeNgFrozenVisiblePosTransforms.clear();
  m_runtimeNgFrozenVisiblePosTransformNormalMatrices.clear();
  m_runtimeNgSelectionDirty = true;
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::setData(std::vector<ZMesh*>* meshList)
{
  m_origMeshList.clear();
  if (meshList) {
    m_origMeshList = *meshList;
    LOG(INFO) << className() << " read " << m_origMeshList.size() << " meshes.";
  }
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();
  initializeRotationCenterIfDefault();
}

void Z3DMeshFilter::setExternalSourceJson(json::value sourceJson)
{
  setExternalSourceState(std::move(sourceJson), nullptr);
}

void Z3DMeshFilter::setExternalSourceState(json::value sourceJson,
                                           std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext)
{
  const auto keyOpt = parseNeuroglancerMeshExternalSourceKey(sourceJson);
  if (!keyOpt || keyOpt->meshSourceDirUrl.isEmpty() || !keyOpt->baseResolutionNm || !keyOpt->baseVoxelOffset) {
    if (!m_runtimeNgSourceKey && m_externalSourceJson == sourceJson && m_runtimeNgRemoteContext == remoteContext) {
      return;
    }
    m_externalSourceJson = std::move(sourceJson);
    resetRuntimeNeuroglancerLodState();
    return;
  }

  if (isSameRuntimeNeuroglancerSource(*keyOpt) && m_externalSourceJson == sourceJson &&
      m_runtimeNgRemoteContext == remoteContext) {
    return;
  }

  m_externalSourceJson = std::move(sourceJson);
  resetRuntimeNeuroglancerLodState();
  m_runtimeNgRemoteContext = std::move(remoteContext);
  m_runtimeNgSourceKey = *keyOpt;
  applyRuntimeNeuroglancerVoxelAspectScale(*keyOpt);
  startRuntimeNeuroglancerOpen();
}

bool Z3DMeshFilter::isReady(Z3DEye eye) const
{
  if (!Z3DGeometryFilter::isReady(eye) || !m_visible.get()) {
    return false;
  }

  // Runtime Neuroglancer multi-LOD meshes may have no imported coarse mesh geometry.
  // Even when no vertices are currently visible (e.g. while opening/loading), the
  // filter must still be allowed to process() so it can drive async LOD streaming.
  if (m_runtimeNgSourceKey) {
    return true;
  }

  return !m_meshList.empty();
}

std::shared_ptr<ZWidgetsGroup> Z3DMeshFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Mesh", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_colorMode, 1);
    m_widgetsGroup->addChild(m_singleColorForAllMesh, 1);
    m_widgetsGroup->addChild(m_wireframeMode, 1);
    m_widgetsGroup->addChild(m_wireframeColor, 1);

    m_widgetsGroup->addChild(m_rendererParameters.coordTransform, 2);
    m_widgetsGroup->addChild(m_rendererParameters.opacity, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialAmbient, 7);
    m_widgetsGroup->addChild(m_rendererParameters.materialSpecular, 7);
    m_widgetsGroup->addChild(m_rendererParameters.materialShininess, 7);

    m_widgetsGroup->addChild(m_glow, 5);
    m_widgetsGroup->addChild(m_glowMode, 5);
    m_widgetsGroup->addChild(m_glowBlurRadius, 5);
    m_widgetsGroup->addChild(m_glowBlurScale, 5);
    m_widgetsGroup->addChild(m_glowBlurStrength, 5);

    m_widgetsGroup->addChild(m_xCut, 5);
    m_widgetsGroup->addChild(m_yCut, 5);
    m_widgetsGroup->addChild(m_zCut, 5);
    m_widgetsGroup->addChild(m_boundBoxMode, 5);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 5);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 5);
    m_widgetsGroup->addChild(m_selectionLineWidth, 7);
    m_widgetsGroup->addChild(m_selectionLineColor, 7);
    m_widgetsGroup->addChild(m_manipulatorSize, 7);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

std::shared_ptr<ZWidgetsGroup> Z3DMeshFilter::widgetsGroupForAnnotationFilter()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Mesh", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_singleColorForAllMesh, 1);
    m_widgetsGroup->addChild(m_wireframeMode, 1);
    m_widgetsGroup->addChild(m_wireframeColor, 1);

    m_widgetsGroup->addChild(m_rendererParameters.opacity, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialAmbient, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialSpecular, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialShininess, 5);
    m_widgetsGroup->addChild(m_xCut, 5);
    m_widgetsGroup->addChild(m_yCut, 5);
    m_widgetsGroup->addChild(m_zCut, 5);
    m_widgetsGroup->addChild(m_boundBoxMode, 5);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 5);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 5);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void Z3DMeshFilter::renderOpaque(Z3DEye eye)
{
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderVulkan(eye, m_triangleListRenderer);
  } else {
    m_rendererBase.render(eye, m_triangleListRenderer);
  }
  renderBoundBox(eye);
}

void Z3DMeshFilter::renderTransparent(Z3DEye eye)
{
  if (m_glow.get()) {
    // Compositor owns glow composition; only render bound box here
    renderBoundBox(eye);
  } else {
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderVulkan(eye, m_triangleListRenderer);
    } else {
      m_rendererBase.render(eye, m_triangleListRenderer);
    }
    renderBoundBox(eye);
  }
}

void Z3DMeshFilter::renderPicking(Z3DEye eye)
{
  if (!m_pickingObjectsRegistered) {
    registerPickingObjects();
  }
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderPickingVulkan(eye, m_triangleListRenderer);
  } else {
    m_rendererBase.renderPicking(eye, m_triangleListRenderer);
  }
}

void Z3DMeshFilter::switchRendererBackend(RenderBackend backendRequest)
{
  if (m_runtimeNgSourceKey && !m_runtimeNgExportActive) {
    // Backend switching should not trigger repaints or pipeline invalidations
    // from within the filter. We only request cancellation so in-flight row
    // loads will not post results back mid-switch, and then re-solve the
    // selection on the next process() call.
    m_runtimeNgSelectionDirty = true;
    (void)cancelRuntimeNeuroglancerInFlightRows();
  }

  Z3DBoundedFilter::switchRendererBackend(backendRequest);
}

void Z3DMeshFilter::setViewport(glm::uvec2 viewport)
{
  Z3DBoundedFilter::setViewport(viewport);
  // Runtime Neuroglancer mesh LOD uses the stable scene/output size propagated
  // from the rendering engine via updateSize(). The compositor temporarily
  // overrides per-filter viewports for intermediate passes; those must not
  // dirtify LOD.
}

void Z3DMeshFilter::setViewport(glm::uvec4 viewport)
{
  Z3DBoundedFilter::setViewport(viewport);
  // See setViewport(glm::uvec2).
}

void Z3DMeshFilter::updateSize(const glm::uvec2& targetSize)
{
  const bool sizeChanged = (targetSize != m_lastUpdateSize);
  Z3DGeometryFilter::updateSize(targetSize);
  if (!sizeChanged) {
    return;
  }

  // A real output size change should trigger a re-solve of the runtime NG LOD
  // selection (it is screen-space). This is intentionally keyed off the engine
  // output size update rather than compositor-internal per-pass viewports.
  if (!m_runtimeNgSourceKey || m_runtimeNgExportActive) {
    return;
  }
  m_runtimeNgFailedRows.clear();
  m_runtimeNgSelectionDirty = true;
}

void Z3DMeshFilter::prepareData()
{
  if (!m_dataIsInvalid) {
    return;
  }

  deregisterPickingObjects();

  m_triangleListRenderer.setData(&m_meshList);
  if (!m_meshPosTransforms.empty()) {
    CHECK(m_meshPosTransforms.size() == m_meshPosTransformNormalMatrices.size())
      << "Per-mesh transform arrays must match in size";
    CHECK(m_meshPosTransforms.size() == m_meshList.size()) << "Per-mesh transforms must match mesh list size";
    m_triangleListRenderer.setPerMeshPosTransforms(&m_meshPosTransforms, &m_meshPosTransformNormalMatrices);
  } else {
    m_triangleListRenderer.setPerMeshPosTransforms(nullptr, nullptr);
  }
  prepareColor();
  adjustWidgets();
  m_dataIsInvalid = false;
}

void Z3DMeshFilter::registerPickingObjects()
{
  if (!m_pickingObjectsRegistered) {
    for (auto mesh : m_meshList) {
      pickingManager().registerObject(mesh);
    }
    m_registeredMeshList = m_meshList;
    m_meshPickingColors.clear();
    for (auto mesh : m_meshList) {
      glm::col4 pickingColor = pickingManager().colorOfObject(mesh);
      glm::vec4 fPickingColor(pickingColor[0] / 255.f,
                              pickingColor[1] / 255.f,
                              pickingColor[2] / 255.f,
                              pickingColor[3] / 255.f);
      m_meshPickingColors.push_back(fPickingColor);
    }
    m_triangleListRenderer.setDataPickingColors(&m_meshPickingColors);
  }

  m_pickingObjectsRegistered = true;
}

void Z3DMeshFilter::deregisterPickingObjects()
{
  if (m_pickingObjectsRegistered) {
    for (auto mesh : m_registeredMeshList) {
      pickingManager().deregisterObject(mesh);
    }
    m_registeredMeshList.clear();
  }

  m_pickingObjectsRegistered = false;
}

ZBBox<glm::dvec3> Z3DMeshFilter::meshBound(ZMesh* p)
{
  auto it = m_meshBoundboxMapper.find(p);
  if (it != m_meshBoundboxMapper.end()) {
    return it->second;
  }

  ZBBox<glm::dvec3> result = p->boundBox(coordTransform());
  m_meshBoundboxMapper[p] = result;
  return result;
}

void Z3DMeshFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.reset();
  if (m_runtimeNgSourceKey && m_runtimeNgManifest && m_runtimeNgSource) {
    m_notTransformedBoundBox.expand(m_runtimeNgSource->multiLodClipBoundsLocalVoxel(*m_runtimeNgManifest));
  }
  for (auto& mesh : m_origMeshList) {
    m_notTransformedBoundBox.expand(mesh->boundBox());
  }
}

void Z3DMeshFilter::prepareColor()
{
  m_meshColors.clear();

  if (m_colorMode.isSelected("Single Color")) {
    for (size_t i = 0; i < m_meshList.size(); ++i) {
      m_meshColors.push_back(m_singleColorForAllMesh.get());
    }
    m_triangleListRenderer.setDataColors(&m_meshColors);
    m_triangleListRenderer.setColorSource(MeshColorSource::CustomColor);
  } else if (m_colorMode.isSelected("Mesh Color")) {
    m_triangleListRenderer.setColorSource(MeshColorSource::MeshColor);
  }
}

void Z3DMeshFilter::updateWireframeMode()
{
  m_triangleListRenderer.setWireframeMode(
    static_cast<Z3DMeshRenderer::WireframeMode>(m_wireframeMode.associatedData()));
  adjustWidgets();
}

void Z3DMeshFilter::updateWireframeColor()
{
  m_triangleListRenderer.setWireframeColor(m_wireframeColor.get());
}

void Z3DMeshFilter::adjustWidgets()
{
  m_singleColorForAllMesh.setVisible(m_colorMode.isSelected("Single Color"));

  m_glowMode.setVisible(m_glow.get());
  m_glowBlurRadius.setVisible(m_glow.get());
  m_glowBlurScale.setVisible(m_glow.get());
  m_glowBlurStrength.setVisible(m_glow.get());
  m_wireframeColor.setVisible(!m_wireframeMode.isSelected("No Wireframe"));
}

void Z3DMeshFilter::selectMesh(QMouseEvent* e, int /*w*/, int /*h*/)
{
  if (m_meshList.empty()) {
    return;
  }

  e->ignore();
  if (e->type() == QEvent::MouseButtonDblClick) {
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->position().x(), e->position().y()));
    bool appending = (e->modifiers() == Qt::ControlModifier);
    if (!obj && !appending && m_isSelected) {
      Q_EMIT objDeselected();
      return;
    }
    bool hit = contains(m_meshList, static_cast<const ZMesh*>(obj));
    if (hit) {
      Q_EMIT objSelected(appending);
      e->accept();
    }
    return;
  }

  e->ignore();
  // Mouse button pressend
  // can not accept the event in button press, because we don't know if it is a selection or interaction
  if (e->type() == QEvent::MouseButtonPress) {
    m_startCoord.x = e->position().x();
    m_startCoord.y = e->position().y();
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->position().x(), e->position().y()));
    if (!obj) {
      return;
    }

    // Check if any point was selected...
    for (auto m : m_meshList) {
      if (m == obj) {
        m_pressedMesh = m;
        break;
      }
    }
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    if (std::abs(e->position().x() - m_startCoord.x) < 2 && std::abs(m_startCoord.y - e->position().y()) < 2) {
      if (e->modifiers() == Qt::ControlModifier) {
        Q_EMIT meshSelected(m_pressedMesh, true);
      } else {
        Q_EMIT meshSelected(m_pressedMesh, false);
      }
      if (m_pressedMesh) {
        e->accept();
      }
    }
    m_pressedMesh = nullptr;
  }
}

void Z3DMeshFilter::onApplyTransform()
{
  VLOG(1) << m_rendererParameters.coordTransform.get();
}

void Z3DMeshFilter::updateMeshVisibleState()
{
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::resetRuntimeNeuroglancerLodState()
{
  if (m_runtimeNgRefinementActive) {
    logRuntimeNeuroglancerRefinementFinished("state reset");
    m_runtimeNgRefinementActive = false;
  }

  // Ensure any in-flight row loads stop promptly (e.g. when closing a scene or
  // quitting the app). Runtime NG mesh tasks are coroutine-backed and must
  // cooperate via cancellation tokens.
  if (m_runtimeNgCancellationSource) {
    m_runtimeNgCancellationSource->requestCancellation();
  }
  (void)cancelRuntimeNeuroglancerInFlightRows();
  m_runtimeNgCancellationSource = std::make_shared<folly::CancellationSource>();
  m_runtimeNgRowCancellationSources.clear();
  m_runtimeNgRowBytesInFlight.clear();
  m_runtimeNgBytesInFlight = 0;

  ++m_runtimeNgEpoch;
  m_runtimeNgIdleTimer.stop();
  m_runtimeNgSourceKey.reset();
  m_runtimeNgRemoteContext.reset();
  m_runtimeNgSource.reset();
  m_runtimeNgManifest.reset();
  m_runtimeNgBaseRows.clear();
  clearRuntimeNeuroglancerRequestFrontier();
  m_runtimeNgLoadedRows.clear();
  m_runtimeNgRowsInFlight.clear();
  m_runtimeNgFailedRows.clear();
  m_runtimeNgVisibleMeshes.clear();
  m_runtimeNgVisiblePosTransforms.clear();
  m_runtimeNgVisiblePosTransformNormalMatrices.clear();
  m_runtimeNgFrozenVisibleMeshes.clear();
  m_runtimeNgFrozenVisiblePosTransforms.clear();
  m_runtimeNgFrozenVisiblePosTransformNormalMatrices.clear();
  m_runtimeNgBaseReady = false;
  m_runtimeNgInteractionActive = false;
  m_runtimeNgSelectionDirty = false;
  m_runtimeNgExportActive = false;
  getVisibleData();
  updateBoundBox();
  initializeRotationCenterIfDefault();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::applyRuntimeNeuroglancerVoxelAspectScale(const ZNeuroglancerMeshExternalSourceKey& key)
{
  if (!key.baseResolutionNm) {
    return;
  }

  const auto& resolution = *key.baseResolutionNm;
  if (!std::isfinite(resolution[0]) || !std::isfinite(resolution[1]) || !std::isfinite(resolution[2]) ||
      resolution[0] <= 0.0 || resolution[1] <= 0.0 || resolution[2] <= 0.0) {
    return;
  }

  const double xy = std::max(resolution[0], resolution[1]);
  const double zOverXY = resolution[2] / xy;
  if (!std::isfinite(zOverXY) || zOverXY <= 0.0) {
    return;
  }

  const glm::vec3 currentScale = m_rendererParameters.coordTransform.scale();
  const bool isDefaultScale = glm::all(glm::epsilonEqual(currentScale, glm::vec3(1.f), 1e-6f));
  const bool isPreviousAutoScale = m_hasRuntimeNgAutoVoxelAspectScale &&
                                   glm::all(glm::epsilonEqual(currentScale, m_runtimeNgAutoVoxelAspectScale, 1e-6f));
  if (!isDefaultScale && !isPreviousAutoScale) {
    return;
  }

  const glm::vec3 suggestedScale(1.f, 1.f, static_cast<float>(zOverXY));
  m_hasRuntimeNgAutoVoxelAspectScale = true;
  m_runtimeNgAutoVoxelAspectScale = suggestedScale;
  if (glm::all(glm::epsilonEqual(currentScale, suggestedScale, 1e-6f))) {
    return;
  }

  m_rendererParameters.coordTransform.setScale(suggestedScale);
  LOG(INFO) << fmt::format("3D mesh: using Neuroglancer voxel-size aspect ratio for coordTransform scale: "
                           "baseResolutionNm=({:.6g},{:.6g},{:.6g}) -> scale=(1,1,{:.6g})",
                           resolution[0],
                           resolution[1],
                           resolution[2],
                           zOverXY);
}

void Z3DMeshFilter::clearRuntimeNeuroglancerRequestFrontier()
{
  m_runtimeNgRequestFrontierMode = RuntimeNeuroglancerRequestFrontierMode::None;
  m_runtimeNgRequestFrontierRows.clear();
  m_runtimeNgRequestFrontierRowSet.clear();
  updateRuntimeNeuroglancerRefinementState();
}

size_t Z3DMeshFilter::cancelRuntimeNeuroglancerInFlightRows(const std::unordered_set<uint32_t>* rowsToKeep)
{
  size_t cancelled = 0U;
  for (const auto& [row, source] : m_runtimeNgRowCancellationSources) {
    if (rowsToKeep && rowsToKeep->contains(row)) {
      continue;
    }
    if (source) {
      source->requestCancellation();
      ++cancelled;
    }
  }
  return cancelled;
}

void Z3DMeshFilter::setRuntimeNeuroglancerRequestFrontier(RuntimeNeuroglancerRequestFrontierMode mode,
                                                          std::vector<uint32_t> rows)
{
  CHECK(mode != RuntimeNeuroglancerRequestFrontierMode::None)
    << "Use clearRuntimeNeuroglancerRequestFrontier() to clear the frontier";

  m_runtimeNgRequestFrontierMode = mode;
  m_runtimeNgRequestFrontierRows = std::move(rows);
  m_runtimeNgRequestFrontierRowSet.clear();
  m_runtimeNgRequestFrontierRowSet.reserve(m_runtimeNgRequestFrontierRows.size());
  for (const uint32_t row : m_runtimeNgRequestFrontierRows) {
    CHECK(m_runtimeNgRequestFrontierRowSet.insert(row).second)
      << "Runtime Neuroglancer request frontier must not contain duplicate rows";
  }
  updateRuntimeNeuroglancerRefinementState();
}

void Z3DMeshFilter::markRuntimeNeuroglancerLodDirty()
{
  if (!m_runtimeNgSourceKey || m_runtimeNgExportActive) {
    return;
  }
  m_runtimeNgFailedRows.clear();
  m_runtimeNgSelectionDirty = true;
  invalidateResult();
}

void Z3DMeshFilter::updateRuntimeNeuroglancerRefinementState()
{
  if (m_runtimeNgRequestFrontierMode != RuntimeNeuroglancerRequestFrontierMode::ViewDriven) {
    if (m_runtimeNgRefinementActive) {
      logRuntimeNeuroglancerRefinementFinished("view-driven frontier cleared");
      m_runtimeNgRefinementActive = false;
    }
    return;
  }

  size_t remainingDesiredRows = 0;
  for (const uint32_t row : m_runtimeNgRequestFrontierRows) {
    if (m_runtimeNgLoadedRows.contains(row) || m_runtimeNgFailedRows.contains(row)) {
      continue;
    }
    ++remainingDesiredRows;
  }

  if (remainingDesiredRows > 0U) {
    if (!m_runtimeNgRefinementActive) {
      m_runtimeNgRefinementActive = true;
      logRuntimeNeuroglancerRefinementStarted(remainingDesiredRows);
    }
  } else if (m_runtimeNgRefinementActive) {
    logRuntimeNeuroglancerRefinementFinished("current view reached target detail");
    m_runtimeNgRefinementActive = false;
  }
}

void Z3DMeshFilter::onRuntimeNeuroglancerCameraChanged()
{
  if (!m_runtimeNgSourceKey || m_runtimeNgExportActive) {
    return;
  }
  const bool wasInteractionActive = m_runtimeNgInteractionActive;
  m_runtimeNgInteractionActive = true;
  m_runtimeNgIdleTimer.start(kRuntimeNgIdleDelayMs);
  const bool deferViewDrivenRefinementUntilIdle = m_runtimeNgBaseReady;
  if (deferViewDrivenRefinementUntilIdle && !wasInteractionActive) {
    clearRuntimeNeuroglancerRequestFrontier();
    (void)cancelRuntimeNeuroglancerInFlightRows();
  }
  markRuntimeNeuroglancerLodDirty();
}

void Z3DMeshFilter::onRuntimeNeuroglancerIdleTimeout()
{
  if (!m_runtimeNgInteractionActive || m_runtimeNgExportActive) {
    return;
  }
  m_runtimeNgInteractionActive = false;
  markRuntimeNeuroglancerLodDirty();
}

void Z3DMeshFilter::startRuntimeNeuroglancerOpen()
{
  CHECK(m_runtimeNgSourceKey);

  const uint64_t epoch = m_runtimeNgEpoch;
  const ZNeuroglancerMeshExternalSourceKey key = *m_runtimeNgSourceKey;
  const auto remoteContext = m_runtimeNgRemoteContext;
  const folly::CancellationToken cancellationToken =
    m_runtimeNgCancellationSource ? m_runtimeNgCancellationSource->getToken() : folly::CancellationToken();

  spawnRuntimeNeuroglancerTask(runRuntimeNeuroglancerOpenTask(epoch, key, remoteContext, cancellationToken),
                               "runtime_ng_mesh_open");
}

size_t Z3DMeshFilter::requestRuntimeNeuroglancerRows(const std::vector<uint32_t>& rows)
{
  if (!m_runtimeNgSourceKey || !m_runtimeNgSource || rows.empty()) {
    return 0U;
  }

  const bool interactionActive = m_runtimeNgInteractionActive;
  const size_t maxTasksInFlight = runtimeNgMaxTasksInFlight(interactionActive);
  const uint64_t maxEncodedBytesInFlight = runtimeNgMaxEncodedBytesInFlight(interactionActive);
  const bool byteBudgetEnabled = (maxEncodedBytesInFlight != std::numeric_limits<uint64_t>::max());

  const size_t availableTaskSlots =
    maxTasksInFlight > m_runtimeNgRowsInFlight.size() ? (maxTasksInFlight - m_runtimeNgRowsInFlight.size()) : 0U;
  if (availableTaskSlots == 0U) {
    return 0U;
  }

  struct RowLoadSpec
  {
    uint32_t row = 0;
    uint64_t encodedBytes = 0;
    std::shared_ptr<folly::CancellationSource> cancellationSource;
  };

  const auto manifest = m_runtimeNgManifest;
  std::vector<RowLoadSpec> rowsToLoad;
  rowsToLoad.reserve(std::min(rows.size(), availableTaskSlots));

  for (const uint32_t row : rows) {
    if (m_runtimeNgLoadedRows.contains(row) || m_runtimeNgRowsInFlight.contains(row) ||
        m_runtimeNgFailedRows.contains(row)) {
      continue;
    }

    const uint64_t rowBytes = manifest ? encodedBytesForManifestRow(*manifest, row) : 0;
    const bool atByteBudget = byteBudgetEnabled && rowBytes > 0 &&
                              m_runtimeNgBytesInFlight >= maxEncodedBytesInFlight && !m_runtimeNgRowsInFlight.empty();
    if (atByteBudget) {
      continue;
    }

    auto rowCancelSource = std::make_shared<folly::CancellationSource>();
    CHECK(rowCancelSource);

    m_runtimeNgRowsInFlight.insert(row);
    m_runtimeNgRowBytesInFlight[row] = rowBytes;
    if (std::numeric_limits<uint64_t>::max() - m_runtimeNgBytesInFlight < rowBytes) {
      m_runtimeNgBytesInFlight = std::numeric_limits<uint64_t>::max();
    } else {
      m_runtimeNgBytesInFlight += rowBytes;
    }
    m_runtimeNgRowCancellationSources[row] = rowCancelSource;

    rowsToLoad.push_back({
      .row = row,
      .encodedBytes = rowBytes,
      .cancellationSource = std::move(rowCancelSource),
    });

    if (rowsToLoad.size() >= availableTaskSlots) {
      break;
    }
  }
  if (rowsToLoad.empty()) {
    return 0U;
  }

  const uint64_t epoch = m_runtimeNgEpoch;
  const uint64_t segmentId = m_runtimeNgSourceKey->segmentId;
  const auto source = m_runtimeNgSource;
  CHECK(source);
  folly::CancellationToken viewCancellationToken;
  if (m_runtimeNgRequestFrontierMode == RuntimeNeuroglancerRequestFrontierMode::ViewDriven) {
    viewCancellationToken = m_globalParameters.currentMeshLodViewCancellationToken();
  }
  const auto vertexSpace = absl::GetFlag(FLAGS_atlas_ng_mesh_runtime_lod_gpu_transform)
                             ? ZNeuroglancerPrecomputedMeshSource::ChunkVertexSpace::Quantized
                             : ZNeuroglancerPrecomputedMeshSource::ChunkVertexSpace::LocalVoxel;

  for (const RowLoadSpec& spec : rowsToLoad) {
    const uint32_t row = spec.row;
    CHECK(spec.cancellationSource);
    CHECK(m_runtimeNgCancellationSource);
    const folly::CancellationToken cancellationToken =
      folly::cancellation_token_merge(m_runtimeNgCancellationSource->getToken(),
                                      spec.cancellationSource->getToken(),
                                      viewCancellationToken);
    spawnRuntimeNeuroglancerTask(runRuntimeNeuroglancerRowLoadTask(epoch,
                                                                   source,
                                                                   segmentId,
                                                                   row,
                                                                   vertexSpace,
                                                                   spec.encodedBytes,
                                                                   cancellationToken),
                                 "runtime_ng_mesh_row_load");
  }

  return rowsToLoad.size();
}

folly::coro::Task<void>
Z3DMeshFilter::runRuntimeNeuroglancerOpenTask(uint64_t epoch,
                                              ZNeuroglancerMeshExternalSourceKey key,
                                              std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext,
                                              folly::CancellationToken cancellationToken)
{
  RuntimeNeuroglancerOpenResult result =
    co_await openRuntimeNeuroglancerSourceAsync(epoch, std::move(key), std::move(remoteContext), cancellationToken);
  postToMeshFilterThread(this,
                         [epoch,
                          error = std::move(result.error),
                          source = std::move(result.source),
                          manifest = std::move(result.manifest),
                          baseRows = std::move(result.baseRows)](Z3DMeshFilter& filter) mutable {
                           filter.applyRuntimeNeuroglancerOpenResult(epoch,
                                                                     std::move(error),
                                                                     std::move(source),
                                                                     std::move(manifest),
                                                                     std::move(baseRows));
                         });
  co_return;
}

folly::coro::Task<void>
Z3DMeshFilter::runRuntimeNeuroglancerRowLoadTask(uint64_t epoch,
                                                 std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source,
                                                 uint64_t segmentId,
                                                 uint32_t row,
                                                 ZNeuroglancerPrecomputedMeshSource::ChunkVertexSpace vertexSpace,
                                                 uint64_t encodedBytes,
                                                 folly::CancellationToken cancellationToken)
{
  RuntimeNeuroglancerChunkLoadResult result = co_await loadRuntimeNeuroglancerChunkAsync(epoch,
                                                                                         std::move(source),
                                                                                         segmentId,
                                                                                         row,
                                                                                         vertexSpace,
                                                                                         encodedBytes,
                                                                                         cancellationToken);
  postToMeshFilterThread(this,
                         [epoch,
                          row = result.row,
                          encodedBytes = result.encodedBytes,
                          cancelled = result.cancelled,
                          error = std::move(result.error),
                          chunk = std::move(result.chunk)](Z3DMeshFilter& filter) mutable {
                           filter.applyRuntimeNeuroglancerChunkLoadResult(epoch,
                                                                          row,
                                                                          encodedBytes,
                                                                          cancelled,
                                                                          std::move(error),
                                                                          std::move(chunk));
                         });
  co_return;
}

void Z3DMeshFilter::applyRuntimeNeuroglancerOpenResult(
  uint64_t epoch,
  QString error,
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source,
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest> manifest,
  std::vector<uint32_t> baseRows)
{
  if (epoch != m_runtimeNgEpoch) {
    return;
  }
  if (!error.isEmpty()) {
    VLOG(1) << fmt::format("Runtime Neuroglancer mesh LOD disabled: {}", error);
    return;
  }

  m_runtimeNgSource = std::move(source);
  m_runtimeNgManifest = std::move(manifest);
  m_runtimeNgBaseRows = std::move(baseRows);
  m_runtimeNgBaseReady = m_runtimeNgBaseRows.empty();
  updateBoundBox();
  initializeRotationCenterIfDefault();
  if (!m_runtimeNgBaseReady) {
    // Hybrid policy: stream the full coarsest working set first while the
    // imported coarse mesh remains visible. Once base rows are complete we
    // switch to runtime chunks and start refining.
    if (m_runtimeNgSourceKey) {
      const size_t maxTasksInFlight = runtimeNgMaxTasksInFlight(m_runtimeNgInteractionActive);
      const uint64_t maxBytesInFlight = runtimeNgMaxEncodedBytesInFlight(m_runtimeNgInteractionActive);
      VLOG(1) << fmt::format(
        "Runtime Neuroglancer mesh base rows stream start: segment={} mesh='{}' baseRows={} maxTasksInFlight={} "
        "maxEncodedBytesInFlightMiB={}",
        m_runtimeNgSourceKey->segmentId,
        m_runtimeNgSourceKey->meshSourceDirUrl,
        m_runtimeNgBaseRows.size(),
        maxTasksInFlight,
        maxBytesInFlight / (1024ULL * 1024ULL));
    }
    setRuntimeNeuroglancerRequestFrontier(RuntimeNeuroglancerRequestFrontierMode::BaseWarmup, m_runtimeNgBaseRows);
    (void)pumpRuntimeNeuroglancerRequests();
  } else {
    clearRuntimeNeuroglancerRequestFrontier();
  }
  m_runtimeNgSelectionDirty = true;
  invalidateResult();
}

void Z3DMeshFilter::applyRuntimeNeuroglancerChunkLoadResult(
  uint64_t epoch,
  uint32_t row,
  uint64_t /*encodedBytes*/,
  bool cancelled,
  QString error,
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh> chunk)
{
  if (epoch != m_runtimeNgEpoch) {
    return;
  }

  // Cancellation is cooperative and may arrive after the worker finished.
  // Treat cancellation requested at completion time as "cancelled" so the
  // filter does not apply results or trigger invalidation from obsolete
  // tasks (e.g. during backend switches or view changes).
  bool cancellationRequested = false;
  if (auto it = m_runtimeNgRowCancellationSources.find(row); it != m_runtimeNgRowCancellationSources.end()) {
    cancellationRequested = it->second && it->second->getToken().isCancellationRequested();
  }
  const bool finalCancelled = cancelled || cancellationRequested;

  const bool baseWasReady = m_runtimeNgBaseReady;
  m_runtimeNgRowsInFlight.erase(row);
  if (auto it = m_runtimeNgRowBytesInFlight.find(row); it != m_runtimeNgRowBytesInFlight.end()) {
    const uint64_t bytes = it->second;
    if (m_runtimeNgBytesInFlight >= bytes) {
      m_runtimeNgBytesInFlight -= bytes;
    } else {
      m_runtimeNgBytesInFlight = 0;
    }
    m_runtimeNgRowBytesInFlight.erase(it);
  }
  m_runtimeNgRowCancellationSources.erase(row);

  if (finalCancelled) {
    updateRuntimeNeuroglancerRefinementState();
    (void)pumpRuntimeNeuroglancerRequests();
    if (VLOG_IS_ON(2)) {
      VLOG(2) << fmt::format("Runtime Neuroglancer mesh LOD chunk load cancelled: row={}", row);
    }
    return;
  } else if (chunk) {
    m_runtimeNgLoadedRows[row] = std::move(chunk);
  } else {
    m_runtimeNgFailedRows.insert(row);
    if (!error.isEmpty()) {
      VLOG(1) << fmt::format("Runtime Neuroglancer mesh LOD chunk load failed: {}", error);
    } else {
      VLOG(1) << "Runtime Neuroglancer mesh LOD chunk load failed with no error message.";
    }
  }

  if (!m_runtimeNgBaseReady) {
    m_runtimeNgBaseReady = std::ranges::all_of(m_runtimeNgBaseRows, [this](uint32_t rowValue) {
      return m_runtimeNgLoadedRows.contains(rowValue);
    });
  }

  updateRuntimeNeuroglancerRefinementState();
  (void)pumpRuntimeNeuroglancerRequests();

  if (!m_runtimeNgBaseReady) {
    // Keep streaming the coarsest working set without spending render
    // frames on intermediate partial coverage when an imported coarse mesh
    // is still visible. If the mesh was deferred (no imported geometry),
    // we still need to repaint as chunks arrive so the user can see
    // progressive fill.
    if (!hasImportedMeshGeometry(m_origMeshList)) {
      m_runtimeNgSelectionDirty = true;
      invalidateResult();
    }
    return;
  }
  if (!baseWasReady && m_runtimeNgBaseReady) {
    VLOG(1) << fmt::format("Runtime Neuroglancer mesh base rows complete: segment={} mesh='{}' baseRowsLoaded={}",
                           m_runtimeNgSourceKey ? m_runtimeNgSourceKey->segmentId : 0U,
                           m_runtimeNgSourceKey ? m_runtimeNgSourceKey->meshSourceDirUrl : QString(),
                           m_runtimeNgBaseRows.size());
  }
  m_runtimeNgSelectionDirty = true;
  invalidateResult();
}

size_t Z3DMeshFilter::pumpRuntimeNeuroglancerRequests()
{
  if (!m_runtimeNgSourceKey || !m_runtimeNgSource || m_runtimeNgExportActive) {
    return 0U;
  }
  if (m_runtimeNgRequestFrontierMode == RuntimeNeuroglancerRequestFrontierMode::None ||
      m_runtimeNgRequestFrontierRows.empty()) {
    return 0U;
  }

  // View-driven frontiers are only authoritative after the current selection
  // solve runs. If the filter is dirty, pumping the previous view's frontier
  // would re-enqueue obsolete work while the new selection is pending.
  if (m_runtimeNgRequestFrontierMode == RuntimeNeuroglancerRequestFrontierMode::ViewDriven &&
      m_runtimeNgSelectionDirty) {
    return 0U;
  }
  // After base coverage is ready, interaction renders from already loaded rows
  // only and defers new view-driven refinement until the idle debounce fires.
  if (m_runtimeNgBaseReady && m_runtimeNgInteractionActive &&
      m_runtimeNgRequestFrontierMode == RuntimeNeuroglancerRequestFrontierMode::ViewDriven) {
    return 0U;
  }

  return requestRuntimeNeuroglancerRows(m_runtimeNgRequestFrontierRows);
}

void Z3DMeshFilter::applyRuntimeNeuroglancerSelection()
{
  if (m_runtimeNgExportActive || !m_runtimeNgProgressiveRendering) {
    return;
  }

  if (!m_runtimeNgSource || !m_runtimeNgManifest || !m_runtimeNgSelectionDirty) {
    return;
  }

  // Runtime mesh LOD is driven by the stable scene/output viewport size pushed
  // in from the rendering engine via updateSize(). Compositor passes
  // temporarily override per-filter viewports for intermediate targets; those
  // must not affect LOD selection.
  const glm::uvec2 sceneViewport = m_lastUpdateSize;
  if (sceneViewport.x == 0U || sceneViewport.y == 0U) {
    return;
  }

  const bool wantGpuTransform = absl::GetFlag(FLAGS_atlas_ng_mesh_runtime_lod_gpu_transform);

  m_runtimeNgSelectionDirty = false;
  if (!m_runtimeNgBaseReady) {
    setRuntimeNeuroglancerRequestFrontier(RuntimeNeuroglancerRequestFrontierMode::BaseWarmup, m_runtimeNgBaseRows);
    (void)pumpRuntimeNeuroglancerRequests();
    if (hasImportedMeshGeometry(m_origMeshList)) {
      return;
    }

    // Mesh was deferred (no imported coarse mesh). Render whatever coarsest
    // chunks we have so far while we continue streaming the full base working
    // set in the background.
    const float detailCutoff = kRuntimeNgInteractionDetailCutoff;
    const glm::mat4 modelViewProjection = m_globalParameters.camera.get().projectionViewMatrix(MonoEye) *
                                          coordTransform() * m_runtimeNgSource->multiLodStoredToLocalVoxelTransform();
    const auto clippingPlanes = ZNeuroglancerPrecomputedMeshSource::getFrustumPlanes(modelViewProjection);

    const auto drawChunks = ZNeuroglancerPrecomputedMeshSource::chunksToDrawForView(
      *m_runtimeNgManifest,
      modelViewProjection,
      clippingPlanes,
      detailCutoff,
      sceneViewport.x,
      sceneViewport.y,
      [this](uint32_t /*lod*/, uint32_t row, float /*renderScale*/) {
        return m_runtimeNgLoadedRows.contains(row);
      });

    std::vector<ZMesh*> newVisibleMeshes;
    std::vector<glm::mat4> newVisiblePosTransforms;
    std::vector<glm::mat3> newVisiblePosTransformNormalMatrices;
    if (wantGpuTransform) {
      newVisiblePosTransforms.reserve(drawChunks.size());
      newVisiblePosTransformNormalMatrices.reserve(drawChunks.size());
    }
    for (const auto& drawChunk : drawChunks) {
      auto it = m_runtimeNgLoadedRows.find(drawChunk.row);
      if (it == m_runtimeNgLoadedRows.end()) {
        continue;
      }
      const auto& chunkMesh = it->second;
      CHECK(chunkMesh);
      if (wantGpuTransform) {
        CHECK(chunkMesh->hasVertexToLocalVoxelTransforms())
          << "Runtime NG GPU transform requires chunk meshes to supply vertex->local transforms";
        CHECK(chunkMesh->vertexToLocalVoxelTransforms.size() == chunkMesh->subMeshes.size());
        CHECK(chunkMesh->vertexToLocalVoxelNormalMatrices.size() == chunkMesh->subMeshes.size());
      }
      CHECK(drawChunk.subChunkBegin <= drawChunk.subChunkEnd);
      CHECK(drawChunk.subChunkEnd <= chunkMesh->subMeshes.size());
      for (uint32_t i = drawChunk.subChunkBegin; i < drawChunk.subChunkEnd; ++i) {
        const auto& subMesh = chunkMesh->subMeshes[i];
        if (subMesh && !subMesh->empty()) {
          newVisibleMeshes.push_back(subMesh.get());
          if (wantGpuTransform) {
            newVisiblePosTransforms.push_back(chunkMesh->vertexToLocalVoxelTransforms[i]);
            newVisiblePosTransformNormalMatrices.push_back(chunkMesh->vertexToLocalVoxelNormalMatrices[i]);
          }
        }
      }
    }

    const bool visibleChanged = (newVisibleMeshes != m_runtimeNgVisibleMeshes) ||
                                (newVisiblePosTransforms != m_runtimeNgVisiblePosTransforms) ||
                                (newVisiblePosTransformNormalMatrices != m_runtimeNgVisiblePosTransformNormalMatrices);
    if (visibleChanged) {
      m_runtimeNgVisibleMeshes = std::move(newVisibleMeshes);
      m_runtimeNgVisiblePosTransforms = std::move(newVisiblePosTransforms);
      m_runtimeNgVisiblePosTransformNormalMatrices = std::move(newVisiblePosTransformNormalMatrices);
      getVisibleData();
      m_dataIsInvalid = true;
      invalidateResult();
    }
    return;
  }

  const float detailCutoff =
    m_runtimeNgInteractionActive ? kRuntimeNgInteractionDetailCutoff : kRuntimeNgIdleDetailCutoff;
  const glm::mat4 modelViewProjection = m_globalParameters.camera.get().projectionViewMatrix(MonoEye) *
                                        coordTransform() * m_runtimeNgSource->multiLodStoredToLocalVoxelTransform();
  const auto clippingPlanes = ZNeuroglancerPrecomputedMeshSource::getFrustumPlanes(modelViewProjection);
  const bool deferViewDrivenRefinementUntilIdle = m_runtimeNgBaseReady && m_runtimeNgInteractionActive;

  size_t desiredRowCount = 0;
  size_t remainingDesiredRows = 0;
  size_t rowsToLoadNow = 0;
  if (deferViewDrivenRefinementUntilIdle) {
    if (m_runtimeNgRequestFrontierMode == RuntimeNeuroglancerRequestFrontierMode::ViewDriven) {
      clearRuntimeNeuroglancerRequestFrontier();
    }
  } else {
    const auto desiredRows = ZNeuroglancerPrecomputedMeshSource::desiredChunksForView(*m_runtimeNgManifest,
                                                                                      modelViewProjection,
                                                                                      clippingPlanes,
                                                                                      detailCutoff,
                                                                                      sceneViewport.x,
                                                                                      sceneViewport.y);
    desiredRowCount = desiredRows.size();
    std::vector<uint32_t> desiredFrontierRows;
    desiredFrontierRows.reserve(desiredRows.size());
    std::unordered_set<uint32_t> desiredRowSet;
    desiredRowSet.reserve(desiredRows.size());
    for (const auto& desired : desiredRows) {
      if (desired.empty) {
        continue;
      }
      if (!desiredRowSet.insert(desired.row).second) {
        continue;
      }
      desiredFrontierRows.push_back(desired.row);
      const bool loaded = m_runtimeNgLoadedRows.contains(desired.row);
      const bool failed = m_runtimeNgFailedRows.contains(desired.row);
      if (!loaded && !failed) {
        ++remainingDesiredRows;
      }
    }
    setRuntimeNeuroglancerRequestFrontier(RuntimeNeuroglancerRequestFrontierMode::ViewDriven,
                                          std::move(desiredFrontierRows));

    if (!m_runtimeNgRowsInFlight.empty()) {
      const size_t cancelled = cancelRuntimeNeuroglancerInFlightRows(&m_runtimeNgRequestFrontierRowSet);
      if (cancelled > 0U && VLOG_IS_ON(2) && m_runtimeNgSourceKey) {
        VLOG(2) << fmt::format(
          "Runtime Neuroglancer mesh LOD cancelled obsolete in-flight rows: segment={} mesh='{}' cancelledRows={} "
          "rowsInFlight={}",
          m_runtimeNgSourceKey->segmentId,
          m_runtimeNgSourceKey->meshSourceDirUrl,
          cancelled,
          m_runtimeNgRowsInFlight.size());
      }
    }
    rowsToLoadNow = pumpRuntimeNeuroglancerRequests();
  }

  const auto drawChunks = ZNeuroglancerPrecomputedMeshSource::chunksToDrawForView(
    *m_runtimeNgManifest,
    modelViewProjection,
    clippingPlanes,
    detailCutoff,
    sceneViewport.x,
    sceneViewport.y,
    [this](uint32_t /*lod*/, uint32_t row, float /*renderScale*/) {
      return m_runtimeNgLoadedRows.contains(row);
    });

  std::vector<ZMesh*> newVisibleMeshes;
  std::vector<glm::mat4> newVisiblePosTransforms;
  std::vector<glm::mat3> newVisiblePosTransformNormalMatrices;
  if (wantGpuTransform) {
    newVisiblePosTransforms.reserve(drawChunks.size());
    newVisiblePosTransformNormalMatrices.reserve(drawChunks.size());
  }
  for (const auto& drawChunk : drawChunks) {
    auto it = m_runtimeNgLoadedRows.find(drawChunk.row);
    if (it == m_runtimeNgLoadedRows.end()) {
      continue;
    }
    const auto& chunkMesh = it->second;
    CHECK(chunkMesh);
    if (wantGpuTransform) {
      CHECK(chunkMesh->hasVertexToLocalVoxelTransforms())
        << "Runtime NG GPU transform requires chunk meshes to supply vertex->local transforms";
      CHECK(chunkMesh->vertexToLocalVoxelTransforms.size() == chunkMesh->subMeshes.size());
      CHECK(chunkMesh->vertexToLocalVoxelNormalMatrices.size() == chunkMesh->subMeshes.size());
    }
    CHECK(drawChunk.subChunkBegin <= drawChunk.subChunkEnd);
    CHECK(drawChunk.subChunkEnd <= chunkMesh->subMeshes.size());
    for (uint32_t i = drawChunk.subChunkBegin; i < drawChunk.subChunkEnd; ++i) {
      const auto& subMesh = chunkMesh->subMeshes[i];
      if (subMesh && !subMesh->empty()) {
        newVisibleMeshes.push_back(subMesh.get());
        if (wantGpuTransform) {
          newVisiblePosTransforms.push_back(chunkMesh->vertexToLocalVoxelTransforms[i]);
          newVisiblePosTransformNormalMatrices.push_back(chunkMesh->vertexToLocalVoxelNormalMatrices[i]);
        }
      }
    }
  }

  const bool visibleChanged = (newVisibleMeshes != m_runtimeNgVisibleMeshes) ||
                              (newVisiblePosTransforms != m_runtimeNgVisiblePosTransforms) ||
                              (newVisiblePosTransformNormalMatrices != m_runtimeNgVisiblePosTransformNormalMatrices);
  if (VLOG_IS_ON(2) && m_runtimeNgSourceKey && (visibleChanged || rowsToLoadNow > 0U)) {
    const uint64_t maxBytesInFlight = runtimeNgMaxEncodedBytesInFlight(m_runtimeNgInteractionActive);
    const bool byteBudgetEnabled = (maxBytesInFlight != std::numeric_limits<uint64_t>::max());
    VLOG(2) << fmt::format(
      "Runtime Neuroglancer mesh LOD solve: segment={} mesh='{}' desiredRows={} remainingDesiredRows={} "
      "rowsToFetchNow={} loadedRows={} rowsInFlight={} failedRows={} encodedBytesInFlight={} "
      "maxEncodedBytesInFlight={} byteBudgetEnabled={} detailCutoff={} interactionActive={} visibleSubMeshes={}->{} "
      "gpuTransformEnabled={} visibleChanged={}",
      m_runtimeNgSourceKey->segmentId,
      m_runtimeNgSourceKey->meshSourceDirUrl,
      desiredRowCount,
      remainingDesiredRows,
      rowsToLoadNow,
      m_runtimeNgLoadedRows.size(),
      m_runtimeNgRowsInFlight.size(),
      m_runtimeNgFailedRows.size(),
      m_runtimeNgBytesInFlight,
      maxBytesInFlight,
      byteBudgetEnabled,
      detailCutoff,
      m_runtimeNgInteractionActive,
      m_runtimeNgVisibleMeshes.size(),
      newVisibleMeshes.size(),
      wantGpuTransform,
      visibleChanged);
  }

  if (visibleChanged) {
    m_runtimeNgVisibleMeshes = std::move(newVisibleMeshes);
    m_runtimeNgVisiblePosTransforms = std::move(newVisiblePosTransforms);
    m_runtimeNgVisiblePosTransformNormalMatrices = std::move(newVisiblePosTransformNormalMatrices);
    getVisibleData();
    m_dataIsInvalid = true;
    invalidateResult();
  }
}

bool Z3DMeshFilter::hasRuntimeNeuroglancerLod() const
{
  if (m_runtimeNgSource == nullptr || m_runtimeNgManifest == nullptr || m_runtimeNgVisibleMeshes.empty()) {
    return false;
  }

  if (m_runtimeNgBaseReady) {
    return true;
  }

  // When there is no imported coarse mesh (deferred scene-load), show partial
  // runtime chunks immediately instead of waiting for the full base working set.
  return !hasImportedMeshGeometry(m_origMeshList);
}

void Z3DMeshFilter::logRuntimeNeuroglancerRefinementStarted(size_t remainingDesiredRows)
{
  if (!m_runtimeNgSourceKey) {
    return;
  }
  VLOG(1) << fmt::format(
    "Runtime Neuroglancer mesh refinement started: segment={} mesh='{}' remainingDesiredRows={} loadedRows={} "
    "rowsInFlight={} failedRows={} visibleSubMeshes={}",
    m_runtimeNgSourceKey->segmentId,
    m_runtimeNgSourceKey->meshSourceDirUrl,
    remainingDesiredRows,
    m_runtimeNgLoadedRows.size(),
    m_runtimeNgRowsInFlight.size(),
    m_runtimeNgFailedRows.size(),
    m_runtimeNgVisibleMeshes.size());
}

void Z3DMeshFilter::logRuntimeNeuroglancerRefinementFinished(const char* reason)
{
  if (!m_runtimeNgSourceKey) {
    return;
  }
  VLOG(1) << fmt::format(
    "Runtime Neuroglancer mesh refinement finished: segment={} mesh='{}' reason='{}' loadedRows={} rowsInFlight={} "
    "failedRows={} visibleSubMeshes={}",
    m_runtimeNgSourceKey->segmentId,
    m_runtimeNgSourceKey->meshSourceDirUrl,
    reason,
    m_runtimeNgLoadedRows.size(),
    m_runtimeNgRowsInFlight.size(),
    m_runtimeNgFailedRows.size(),
    m_runtimeNgVisibleMeshes.size());
}

bool Z3DMeshFilter::isSameRuntimeNeuroglancerSource(const ZNeuroglancerMeshExternalSourceKey& key) const
{
  if (!m_runtimeNgSourceKey) {
    return false;
  }
  const auto& current = *m_runtimeNgSourceKey;
  return current.rootUrl == key.rootUrl && current.meshSourceDirUrl == key.meshSourceDirUrl &&
         current.segmentId == key.segmentId && sameOptionalArray(current.baseResolutionNm, key.baseResolutionNm) &&
         sameOptionalArray(current.baseVoxelOffset, key.baseVoxelOffset);
}

void Z3DMeshFilter::getVisibleData()
{
  m_meshPosTransforms.clear();
  m_meshPosTransformNormalMatrices.clear();

  if (m_runtimeNgExportActive) {
    m_meshList = m_runtimeNgFrozenVisibleMeshes;
    m_meshPosTransforms = m_runtimeNgFrozenVisiblePosTransforms;
    m_meshPosTransformNormalMatrices = m_runtimeNgFrozenVisiblePosTransformNormalMatrices;
    return;
  }
  if (hasRuntimeNeuroglancerLod()) {
    m_meshList = m_runtimeNgVisibleMeshes;
    m_meshPosTransforms = m_runtimeNgVisiblePosTransforms;
    m_meshPosTransformNormalMatrices = m_runtimeNgVisiblePosTransformNormalMatrices;
    return;
  }
  m_meshList = m_origMeshList;
}

} // namespace nim
