#include "z3drenderervulkanbackend.h"
#include "zcommandlineflags.h"

#include "z3drendererbase.h"
#include "z3drendercommands.h"
#include "z3dboundedfilter.h"
#include "z3dimg.h"
#include "z3dshaderprogram.h"
#include "zexception.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkanresidencymanager.h"
#include "zvulkancontext.h"
#include "zvulkantexture.h"
#include "z3dscratchresourcepool.h"
#include "zsysteminfo.h"
#include "zvulkanlinepipelinecontext.h"
#include "zvulkanmeshpipelinecontext.h"
#include "zvulkanellipsoidpipelinecontext.h"
#include "zvulkanspherepipelinecontext.h"
#include "zvulkanbackgroundpipelinecontext.h"
#include "zvulkanconepipelinecontext.h"
#include "zvulkantexturecopypipelinecontext.h"
#include "zvulkantextureblendpipelinecontext.h"
#include "zvulkantexturedualpeelpipelinecontext.h"
#include "zvulkantextureweightedaveragepipelinecontext.h"
#include "zvulkantextureweightedblendedpipelinecontext.h"
#include "zvulkantextureppllpipelinecontext.h"
#include "zvulkantextureglowpipelinecontext.h"
#include "zvulkanimgslicepipelinecontext.h"
#include "zvulkanimgraycasterpipelinecontext.h"
#include "z3dimgraycasterrenderer.h"
#include "z3dimgslicerenderer.h"
#include "zvulkanfontpipelinecontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanresourcemetadata.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanuniforms.h"
#include "z3drenderglobalstate.h"
#include "z3dscratchresourcepool.h"
#include "zvulkanbuffer.h"
#include "z3dperfcollector.h"
#include "zvulkanbindings.h"
#include "zvulkanbindlessdescriptorset.h"

#include <folly/OperationCancelled.h>
#include "zrenderthreadexecutor_tls.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdint>
#include <exception>

#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>

ABSL_FLAG(bool,
          vk_reserve_upload_slices,
          true,
          "Reserve per-draw upload arena capacity (precise) before suballocation to avoid mid-upload growth");
ABSL_DECLARE_FLAG(bool, atlas_perf_trace_calibrated);
ABSL_FLAG(bool,
          atlas_vk_ddp_indirect_count,
          true,
          "Use drawIndirectCount gating for Vulkan dual-depth peeling (device-side early stop)");
ABSL_FLAG(
  bool,
  atlas_vk_profile_pipeline_contexts,
  false,
  "If true, record additional CPU scopes attributing Vulkan batch processing time to individual pipeline contexts "
  "(mesh/line/sphere/etc). Adds overhead; intended for perf triage.");
ABSL_FLAG(
  bool,
  atlas_vk_cache_draw_secondaries,
  true,
  "If true, cache per-draw secondary command buffers for Vulkan raster pipeline contexts (currently sphere/cone). "
  "This avoids repeating expensive vkCmd recording work in steady state (camera-only changes) at the cost of "
  "extra memory for cached command buffers.");

ABSL_FLAG(int32_t,
          atlas_vk_bindless_texture2d_capacity,
          1408,
          "Bindless Vulkan sampled-image table capacity for texture2D entries (set=0 binding=0).");
ABSL_FLAG(int32_t,
          atlas_vk_bindless_texture2darray_capacity,
          256,
          "Bindless Vulkan sampled-image table capacity for texture2DArray entries (set=0 binding=1).");
ABSL_FLAG(int32_t,
          atlas_vk_bindless_texture3d_capacity,
          256,
          "Bindless Vulkan sampled-image table capacity for texture3D entries (set=0 binding=2).");
ABSL_FLAG(int32_t,
          atlas_vk_bindless_utexture2d_capacity,
          64,
          "Bindless Vulkan sampled-image table capacity for utexture2D entries (set=0 binding=3).");
ABSL_FLAG(int32_t,
          atlas_vk_bindless_utexture3d_capacity,
          64,
          "Bindless Vulkan sampled-image table capacity for utexture3D entries (set=0 binding=4).");

// Baseline capacity for the per-frame uniform arena (in KiB). This buffer backs
// all dynamic UBO bindings for the frame. The backend pre-sizes beyond this baseline
// based on a cheap estimate; mid-frame growth is disallowed.
static constexpr int kUniformArenaBaseKiB = 256;
static constexpr size_t kUploadArenaMinPageBytes = 1ull << 20;
static constexpr size_t kUploadArenaPreferredPageBytes = 32ull * 1024ull * 1024ull;
static constexpr size_t kUploadArenaStrictPageGranularityBytes = 1ull << 20;

namespace nim {

namespace {

size_t uploadArenaMaxPageBytes(const ZVulkanDevice& device)
{
  const vk::DeviceSize limit = device.maxMemoryAllocationSize();
  if (limit == 0 || limit >= static_cast<vk::DeviceSize>(std::numeric_limits<size_t>::max())) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(limit);
}

size_t roundUpToMultiple(size_t value, size_t multiple)
{
  CHECK_GT(multiple, 0u) << "roundUpToMultiple requires a non-zero multiple";
  const size_t remainder = value % multiple;
  if (remainder == 0u) {
    return value;
  }
  const size_t delta = multiple - remainder;
  CHECK_LE(value, std::numeric_limits<size_t>::max() - delta) << "roundUpToMultiple overflow";
  return value + delta;
}

size_t chooseUploadArenaPageCapacity(const ZVulkanDevice& device, size_t minCapacity)
{
  const size_t limit = uploadArenaMaxPageBytes(device);
  if (minCapacity > limit) {
    return 0;
  }

  if (device.residencyManager().strictBudgetActive()) {
    size_t capacity = std::max(minCapacity, kUploadArenaMinPageBytes);
    capacity = roundUpToMultiple(capacity, kUploadArenaStrictPageGranularityBytes);
    if (capacity > limit) {
      capacity = minCapacity;
    }
    return capacity;
  }

  const size_t preferred = std::max<size_t>(kUploadArenaMinPageBytes, std::min(kUploadArenaPreferredPageBytes, limit));
  size_t capacity = std::max(minCapacity, preferred);
  if (capacity > limit) {
    capacity = minCapacity;
  }
  return capacity;
}

std::string_view staticCacheOwnerName(Z3DRendererVulkanBackend::StaticCacheOwner owner)
{
  switch (owner) {
    case Z3DRendererVulkanBackend::StaticCacheOwner::Mesh:
      return "mesh";
    case Z3DRendererVulkanBackend::StaticCacheOwner::Line:
      return "line";
    case Z3DRendererVulkanBackend::StaticCacheOwner::Ellipsoid:
      return "ellipsoid";
    case Z3DRendererVulkanBackend::StaticCacheOwner::Sphere:
      return "sphere";
    case Z3DRendererVulkanBackend::StaticCacheOwner::Cone:
      return "cone";
  }
  return "<unknown>";
}

uint64_t staticCandidateOwnerDomainKey(Z3DRendererVulkanBackend::StaticCacheOwner owner,
                                       Z3DRendererVulkanBackend::StaticPressureDomain domain)
{
  return (static_cast<uint64_t>(owner) << 8u) | static_cast<uint64_t>(domain);
}

constexpr uint64_t kRecentStaticGeometryProtectedEpochs = 1u;
constexpr size_t kStaticEvictionTelemetryLimit = 512u;

Z3DRendererVulkanBackend::StaticCacheOwner staticCandidateOwner(uint64_t key)
{
  return static_cast<Z3DRendererVulkanBackend::StaticCacheOwner>((key >> 8u) & 0xffu);
}

Z3DRendererVulkanBackend::StaticPressureDomain staticCandidateDomain(uint64_t key)
{
  return static_cast<Z3DRendererVulkanBackend::StaticPressureDomain>(key & 0xffu);
}

} // namespace

namespace vulkan {
size_t UniformArenaBudgetTraits<ImgRaycasterPayload>::estimateAdditionalBytes(const ImgRaycasterPayload& payload,
                                                                              size_t uniformAlignment)
{
  // Raycaster allocates 32B dynamic UBO slices for the backend-shared image
  // indices descriptor set in planar stages, and a larger PageData std140 UBO
  // in progressive paging. Fast shaders may only consume the leading fields,
  // but the binding contract is the full 32B shared descriptor range.
  //
  // Keep this conservative: higher-level schedulers rely on it to pre-size the
  // per-frame uniform arena before recording begins.
  const bool planarGeometry = payload.planarGeometry;
  // Shared image indices descriptor set has a fixed 32B std140 range.
  const size_t szIndices = alignUp(sizeof(uint32_t) * 8u, uniformAlignment); // 32B std140 padded
  const size_t szRaySetup = alignUp(sizeof(ImgRaySetupUBOStd140), uniformAlignment);

  auto estimatePageDataBytes = [&]() -> size_t {
    if (!payload.image) {
      return 0u;
    }
    const size_t levelCount = static_cast<size_t>(payload.image->numLevels());
    if (levelCount == 0u) {
      return 0u;
    }
    const size_t bytes = 64u + 64u * levelCount; // matches buildPageDataBuffer packing contract
    return alignUp(bytes, uniformAlignment);
  };

  switch (payload.stage) {
    case ImgRaycasterPayload::Stage::FastDirect:
    case ImgRaycasterPayload::Stage::FastLayers:
      return planarGeometry ? szIndices : szRaySetup;
    case ImgRaycasterPayload::Stage::ProgressivePreviewLayers:
      return szRaySetup;
    case ImgRaycasterPayload::Stage::ProgressiveBlockId:
    case ImgRaycasterPayload::Stage::ProgressiveRaycast:
      return szRaySetup + estimatePageDataBytes();
    case ImgRaycasterPayload::Stage::ProgressiveCopyToLayers:
    case ImgRaycasterPayload::Stage::ProgressiveMerge:
      return 0u;
    default:
      return 0u;
  }
}

size_t UniformArenaBudgetTraits<ImgSlicePayload>::estimateAdditionalBytes(const ImgSlicePayload& payload,
                                                                          size_t uniformAlignment)
{
  // Slice renderer allocates:
  // - A 32B dynamic UBO slice for the backend-shared image indices descriptor
  //   set in fast draws. Fast shaders may only consume the leading fields, but
  //   the binding contract is the full 32B shared descriptor range.
  // - A paged bindless-indices UBO plus a larger PageData std140 UBO in paged
  //   slice rendering and block-ID discovery.
  // Shared image indices descriptor set has a fixed 32B std140 range.
  const size_t szFastIndices = alignUp(sizeof(uint32_t) * 8u, uniformAlignment); // 32B std140 padded
  const size_t szPagedIndices = alignUp(sizeof(uint32_t) * 8u, uniformAlignment); // 32B std140 padded

  auto estimatePageDataBytes = [&]() -> size_t {
    if (!payload.image) {
      return 0u;
    }
    const size_t levelCount = static_cast<size_t>(payload.image->numLevels());
    if (levelCount == 0u) {
      return 0u;
    }
    const size_t bytes = 64u + 64u * levelCount; // matches PageData packing contract
    return alignUp(bytes, uniformAlignment);
  };

  switch (payload.stage) {
    case ImgSlicePayload::Stage::DrawLayers: {
      // Paged draws only happen in round 1+ (round 0 uses fast preview + BlockIdDiscovery).
      const bool usePaging = (!payload.fastPathOnly && payload.image && payload.image->isVolumeDownsampled());
      const bool pagingDraw = (usePaging && payload.channelIndexRaw >= 0 && payload.roundIndexRaw > 0);
      return pagingDraw ? (estimatePageDataBytes() + szPagedIndices) : szFastIndices;
    }
    case ImgSlicePayload::Stage::BlockIdDiscovery:
      return estimatePageDataBytes() + szPagedIndices;
    default:
      return 0u;
  }
}
} // namespace vulkan

namespace {
constexpr uint32_t kMaxTimestampQueries = 64u;

float computeSphereBoxCorrection(float fovyDegrees)
{
  // Match the legacy sphere pipeline behaviour. This factor expands the
  // billboard quad slightly so the raycast sphere does not clip at the corners
  // under wide FOV. (Empirical fit preserved for GL parity.)
  if (fovyDegrees <= 90.0f) {
    return 1.0027f + 0.000111f * fovyDegrees + 0.000098f * fovyDegrees * fovyDegrees;
  }
  return 2.02082f - 0.033935f * fovyDegrees + 0.00037854f * fovyDegrees * fovyDegrees;
}
} // namespace

thread_local Z3DRendererVulkanBackend* Z3DRendererVulkanBackend::s_currentBackend = nullptr;

Z3DRendererVulkanBackend::Z3DRendererVulkanBackend()
  : m_lineContext(std::make_unique<ZVulkanLinePipelineContext>(*this))
  , m_meshContext(std::make_unique<ZVulkanMeshPipelineContext>(*this))
  , m_ellipsoidContext(std::make_unique<ZVulkanEllipsoidPipelineContext>(*this))
  , m_sphereContext(std::make_unique<ZVulkanSpherePipelineContext>(*this))
  , m_coneContext(std::make_unique<ZVulkanConePipelineContext>(*this))
  , m_backgroundContext(std::make_unique<ZVulkanBackgroundPipelineContext>(*this))
  , m_textureCopyContext(std::make_unique<ZVulkanTextureCopyPipelineContext>(*this))
  , m_textureBlendContext(std::make_unique<ZVulkanTextureBlendPipelineContext>(*this))
  , m_textureDualPeelContext(std::make_unique<ZVulkanTextureDualPeelPipelineContext>(*this))
  , m_textureWeightedAverageContext(std::make_unique<ZVulkanTextureWeightedAveragePipelineContext>(*this))
  , m_textureWeightedBlendedContext(std::make_unique<ZVulkanTextureWeightedBlendedPipelineContext>(*this))
  , m_texturePPLLContext(std::make_unique<ZVulkanTexturePPLLPipelineContext>(*this))
  , m_textureGlowContext(std::make_unique<ZVulkanTextureGlowPipelineContext>(*this))
  , m_imgSliceContext(std::make_unique<ZVulkanImgSlicePipelineContext>(*this))
  , m_imgRaycasterContext(std::make_unique<ZVulkanImgRaycasterPipelineContext>(*this))
  , m_fontContext(std::make_unique<ZVulkanFontPipelineContext>(*this))
{}

Z3DRendererVulkanBackend::~Z3DRendererVulkanBackend()
{
  if (m_aliveFlag) {
    *m_aliveFlag = false;
  }

  // Teardown invariant: do not drop fence-gated continuations (residency unpins,
  // per-frame deferred releases) while they still refer to in-flight submissions.
  // When the owning renderer/filter is destroyed, callers may still destroy
  // resources (e.g. Z3DImg) whose Vulkan cache textures are pinned per submission.
  // Ensure those pins are drained now.
  flushForTeardown("backend_dtor");

  // The scratch pool may still release outstanding Vulkan leases while the
  // engine is tearing down. Do not leave a scheduler closure capturing this
  // backend beyond its lifetime.
  uninstallMemoryBrokerProviders();
  Z3DRenderGlobalState::instance().scratchPool().setVulkanReleaseScheduler({});
  Z3DRenderGlobalState::instance().scratchPool().setVulkanMemoryPressureHandler({});
}

Z3DRendererVulkanBackend* Z3DRendererVulkanBackend::current()
{
  return s_currentBackend;
}

void Z3DRendererVulkanBackend::preBackendSwitch()
{
  // Ensure all in-flight submissions have completed and all fence-gated
  // continuations (residency unpins, deferred scratch-slot releases) are drained
  // before we clear frame resources and allow renderer-owned resources to be
  // destroyed.
  flushForTeardown("preBackendSwitch");

  // Switching away from Vulkan: drop the scratch-pool release scheduler that is
  // tied to this backend instance. After flushForTeardown(), immediate release
  // is safe.
  uninstallMemoryBrokerProviders();
  Z3DRenderGlobalState::instance().scratchPool().setVulkanReleaseScheduler({});
  Z3DRenderGlobalState::instance().scratchPool().setVulkanMemoryPressureHandler({});

  // Drop shared placeholders; they'll be recreated lazily on next use.
  m_defaultPlaceholder2D.reset();
  m_defaultSampler.reset();
  m_nearestClampSampler.reset();
  m_linearBorderZero3DSampler.reset();
  m_sharedDescriptorLayouts = {};
  // Finish any in-flight frame before we start tearing resources down.
  if (m_frameRecording && m_activeFrameHandle && m_activeFrameHandle->valid()) {
    try {
      m_activeFrameHandle->commandBuffer().end();
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan command buffer end during backend switch failed: " << e.what();
    }
    m_frameRecording = false;
  }
  m_activeFrameHandle.reset();
  m_activeFrame = nullptr;
  m_submissionResourcePinningOpen = false;

  // Global coordination (device waitIdle, scratch-pool reset) is handled by the
  // rendering engine during backend switches. Beyond clearing the scratch-pool
  // release scheduler above, avoid touching global state here to prevent
  // ordering conflicts with persistent lease release.
  if (m_sharedDevice) {
    for (auto& frame : m_frames) {
      collectFrameTimings(frame);
    }
  }

  // Try flushing closed perf tokens now that we've ingested outstanding timings.
  //
  // Important: do *not* force-flush here. During a global backend switch we
  // switch multiple Vulkan backends (one per filter) sequentially, and each
  // backend ingests its own per-submission timings during preBackendSwitch().
  // A forced flush can prune token state (startedSubmissions/closed) before
  // other Vulkan backends have a chance to ingest their submissions for the
  // same real-frame token, leading to CHECK failures when those late ingestions
  // arrive.
  Z3DPerfCollector::instance().maybeFlush(false);

  resetFrameResources();
}

void Z3DRendererVulkanBackend::beginPassScope(std::string_view label)
{
  m_passScope = {};
  m_passScope.active = true;
  m_passScope.label = std::string(label);
  m_passScope.start = std::chrono::steady_clock::now();
  if (m_activeFrame) {
    m_passScope.baseline.descriptorSetsAllocated = m_activeFrame->descriptorSetsAllocated;
    m_passScope.baseline.pipelinesBoundUnique = m_activeFrame->pipelinesBound.size();
    m_passScope.baseline.renderingSegmentsBegan = m_activeFrame->renderingSegmentsBegan;
    m_passScope.baseline.attachmentClears = m_activeFrame->attachmentClears;
    m_passScope.baseline.attachmentLoads = m_activeFrame->attachmentLoads;
    m_passScope.baseline.descriptorWritesWhileRecording = m_activeFrame->descriptorWritesWhileRecording;
    m_passScope.baseline.boundSetRewriteAttempts = m_activeFrame->boundSetRewriteAttempts;
    m_passScope.baseline.uploadHighWatermark = m_activeFrame->uploadArena.highWatermark;
    m_passScope.baseline.staticBytesStaged = m_activeFrame->staticBytesStaged;
    m_passScope.baseline.readbackBytesCopied = m_activeFrame->readbackBytesCopied;
    m_passScope.baseline.readbackSlotsInFlight = m_activeFrame->readbackSlotsInFlight;
  }
}

void Z3DRendererVulkanBackend::endPassScope()
{
  if (!m_passScope.active) {
    return;
  }
  const double ms =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_passScope.start).count();
  if (!m_passScope.label.empty()) {
    recordCpuScope(m_passScope.label, ms);
  }
  uint32_t dsets = 0, segs = 0, clr = 0, ld = 0, dwr = 0, rew = 0;
  size_t bound = 0;
  vk::DeviceSize uploadHi = 0;
  uint64_t staticStaged = 0, rb = 0;
  uint32_t rbinflight = 0;
  if (m_activeFrame) {
    dsets = m_activeFrame->descriptorSetsAllocated - m_passScope.baseline.descriptorSetsAllocated;
    bound = (m_activeFrame->pipelinesBound.size() >= m_passScope.baseline.pipelinesBoundUnique)
              ? (m_activeFrame->pipelinesBound.size() - m_passScope.baseline.pipelinesBoundUnique)
              : 0;
    segs = m_activeFrame->renderingSegmentsBegan - m_passScope.baseline.renderingSegmentsBegan;
    clr = m_activeFrame->attachmentClears - m_passScope.baseline.attachmentClears;
    ld = m_activeFrame->attachmentLoads - m_passScope.baseline.attachmentLoads;
    dwr = m_activeFrame->descriptorWritesWhileRecording - m_passScope.baseline.descriptorWritesWhileRecording;
    rew = m_activeFrame->boundSetRewriteAttempts - m_passScope.baseline.boundSetRewriteAttempts;
    uploadHi = (m_activeFrame->uploadArena.highWatermark >= m_passScope.baseline.uploadHighWatermark)
                 ? (m_activeFrame->uploadArena.highWatermark - m_passScope.baseline.uploadHighWatermark)
                 : 0;
    staticStaged = (m_activeFrame->staticBytesStaged >= m_passScope.baseline.staticBytesStaged)
                     ? (m_activeFrame->staticBytesStaged - m_passScope.baseline.staticBytesStaged)
                     : 0;
    rb = (m_activeFrame->readbackBytesCopied >= m_passScope.baseline.readbackBytesCopied)
           ? (m_activeFrame->readbackBytesCopied - m_passScope.baseline.readbackBytesCopied)
           : 0;
    rbinflight = (m_activeFrame->readbackSlotsInFlight >= m_passScope.baseline.readbackSlotsInFlight)
                   ? (m_activeFrame->readbackSlotsInFlight - m_passScope.baseline.readbackSlotsInFlight)
                   : 0;
  }
  VLOG(1) << fmt::format(
    "pass_end pass='{}' cpu={:.3f} ms draws={} segs={} clr={} ld={} dsets={} pipes_bound_delta={} dwr={} rew={} uploads_delta={}B static_delta={}B rb_delta={}B rbinflight_delta={} transitions={} noop={} buf_barriers={} buf_noop={}",
    m_passScope.label,
    ms,
    m_passScope.draws,
    segs,
    clr,
    ld,
    dsets,
    bound,
    dwr,
    rew,
    static_cast<uint64_t>(uploadHi),
    staticStaged,
    rb,
    rbinflight,
    m_passScope.layoutTransitions,
    m_passScope.layoutNoops,
    m_passScope.bufferBarriers,
    m_passScope.bufferBarrierNoops);
  m_passScope = {};
}

void Z3DRendererVulkanBackend::notifyDrawSubmitted()
{
  if (m_activeFrame) {
    m_activeFrame->drawsSubmitted++;
  }
  if (m_passScope.active) {
    m_passScope.draws++;
  }
}

void Z3DRendererVulkanBackend::notifyLayoutTransition(bool wasNoop)
{
  if (!m_passScope.active) {
    return;
  }
  if (wasNoop) {
    m_passScope.layoutNoops++;
  } else {
    m_passScope.layoutTransitions++;
  }
}

void Z3DRendererVulkanBackend::notifyBufferBarrier(bool wasNoop)
{
  if (!m_passScope.active) {
    return;
  }
  if (wasNoop) {
    m_passScope.bufferBarrierNoops++;
  } else {
    m_passScope.bufferBarriers++;
  }
}

void Z3DRendererVulkanBackend::setGlobalShaderParameters(Z3DRendererBase& renderer,
                                                         Z3DShaderProgram& shader,
                                                         Z3DEye eye)
{
  (void)renderer;
  (void)shader;
  (void)eye;
  CHECK(false) << "Vulkan backend does not provide GLSL shader parameter bindings";
}

std::string Z3DRendererVulkanBackend::generateHeader(const Z3DRendererBase& renderer) const
{
  (void)renderer;
  return std::string();
}

std::string Z3DRendererVulkanBackend::generateGeomHeader(const Z3DRendererBase& renderer) const
{
  (void)renderer;
  return std::string();
}

void Z3DRendererVulkanBackend::setPendingBeginRenderPreRecordActions(std::vector<BeginRenderPreRecordAction> actions,
                                                                     std::string_view debugLabel)
{
  CHECK(m_pendingBeginRenderPreRecordActions.empty())
    << "Pending beginRender pre-record actions already set (previous_label='" << m_pendingBeginRenderPreRecordLabel
    << "')";
  m_pendingBeginRenderPreRecordActions = std::move(actions);
  m_pendingBeginRenderPreRecordLabel = std::string(debugLabel);
}

void Z3DRendererVulkanBackend::setPendingBeginRenderScriptStats(BeginRenderScriptStats stats)
{
  CHECK(!m_pendingBeginRenderScriptStats.has_value()) << "Pending beginRender script stats already set";
  m_pendingBeginRenderScriptStats = std::move(stats);
}

void Z3DRendererVulkanBackend::beginRender(Z3DRendererBase& renderer)
{
  const auto beginRenderEntry = std::chrono::steady_clock::now();

  // Expose the thread-local "current backend" only after a frame slot is active.
  // This avoids call sites (debug validation, descriptor helpers) observing a
  // partially-initialised backend before ensureDevice/beginFrame.
  s_currentBackend = nullptr;
  m_submissionResourcePinningOpen = false;
  m_activeRenderer = &renderer;
  auto preRecordActions = std::move(m_pendingBeginRenderPreRecordActions);
  m_pendingBeginRenderPreRecordActions.clear();
  std::string pendingPreRecordLabel = std::move(m_pendingBeginRenderPreRecordLabel);
  m_pendingBeginRenderPreRecordLabel.clear();
  std::optional<BeginRenderScriptStats> scriptStats = std::move(m_pendingBeginRenderScriptStats);
  m_pendingBeginRenderScriptStats.reset();
  // Per-frame recording state
  m_externalBufferUseStates.clear();
  if (m_lineContext) {
    m_lineContext->resetFrame();
  }
  if (m_meshContext) {
    m_meshContext->resetFrame();
  }
  if (m_ellipsoidContext) {
    m_ellipsoidContext->resetFrame();
  }
  if (m_sphereContext) {
    m_sphereContext->resetFrame();
  }
  if (m_coneContext) {
    m_coneContext->resetFrame();
  }
  if (m_backgroundContext) {}
  if (m_textureCopyContext) {
    m_textureCopyContext->resetFrame();
  }
  if (m_textureBlendContext) {
    m_textureBlendContext->resetFrame();
  }
  if (m_textureDualPeelContext) {
    m_textureDualPeelContext->resetFrame();
  }
  if (m_textureWeightedAverageContext) {
    m_textureWeightedAverageContext->resetFrame();
  }
  if (m_textureWeightedBlendedContext) {
    m_textureWeightedBlendedContext->resetFrame();
  }
  if (m_texturePPLLContext) {
    m_texturePPLLContext->resetFrame();
  }
  if (m_textureGlowContext) {
    m_textureGlowContext->resetFrame();
  }
  if (m_imgSliceContext) {
    m_imgSliceContext->resetFrame();
  }
  if (m_imgRaycasterContext) {
    m_imgRaycasterContext->resetFrame();
  }
  if (m_fontContext) {
    m_fontContext->resetFrame();
  }
  ensureDevice();

  // Drain pending stream eviction requests. These are issued from outside the
  // render thread (e.g., primitive renderer destruction) and processed here to
  // keep all cache mutations on the rendering thread.
  if (m_sharedDevice != nullptr) {
    std::unordered_set<uint64_t> evictKeys;
    {
      std::scoped_lock lock(m_pendingEvictionsMutex);
      evictKeys.swap(m_pendingEvictStreamKeys);
    }
    if (!evictKeys.empty()) {
      VLOG(1) << fmt::format("VK static cache eviction: streams={}", evictKeys.size());
      for (uint64_t streamKey : evictKeys) {
        if (m_lineContext) {
          m_lineContext->evictStream(streamKey);
        }
        if (m_meshContext) {
          m_meshContext->evictStream(streamKey);
        }
        if (m_ellipsoidContext) {
          m_ellipsoidContext->evictStream(streamKey);
        }
        if (m_sphereContext) {
          m_sphereContext->evictStream(streamKey);
        }
        if (m_coneContext) {
          m_coneContext->evictStream(streamKey);
        }
        if (m_fontContext) {
          m_fontContext->evictStream(streamKey);
        }
      }
    }
  }

  const auto& viewport = renderer.frameState().viewport;
  const uint32_t width = viewport.z;
  const uint32_t height = viewport.w;
  const auto& surf = renderer.frameState().activeSurface;
  if (VLOG_IS_ON(2)) {
    uint64_t c0Handle = 0;
    uint32_t c0W = 0, c0H = 0;
    auto c0Fmt = enumOrUnderlying(vk::Format{}, 16);
    if (!surf.colorAttachments.empty() && surf.colorAttachments[0].handle.valid() &&
        surf.colorAttachments[0].handle.backend == RenderBackend::Vulkan) {
      auto* tex = reinterpret_cast<ZVulkanTexture*>(surf.colorAttachments[0].handle.id);
      if (tex) {
        c0Handle = reinterpret_cast<uint64_t>(tex);
        c0W = tex->width();
        c0H = tex->height();
        c0Fmt = enumOrUnderlying(tex->format(), 16);
      }
    }
    uint64_t dHandle = 0;
    uint32_t dW = 0, dH = 0;
    auto dFmt = enumOrUnderlying(vk::Format{}, 16);
    if (surf.depthAttachment && surf.depthAttachment->handle.valid() &&
        surf.depthAttachment->handle.backend == RenderBackend::Vulkan) {
      auto* dtex = reinterpret_cast<ZVulkanTexture*>(surf.depthAttachment->handle.id);
      if (dtex) {
        dHandle = reinterpret_cast<uint64_t>(dtex);
        dW = dtex->width();
        dH = dtex->height();
        dFmt = enumOrUnderlying(dtex->format(), 16);
      }
    }
    // Prefer the frame label here (per-frame), not per-pass label.
    std::string frameLabel = std::string(renderer.currentFrameLabel());
    if (frameLabel.empty()) {
      frameLabel = "<unlabeled-frame>";
    }
    VLOG(2) << fmt::format(
      "VK frameBegin: frame='{}' viewport={}x{} colors={} c0=0x{:x} fmt={} {}x{} depth={} d=0x{:x} fmt={} {}x{}",
      frameLabel,
      width,
      height,
      surf.colorAttachments.size(),
      c0Handle,
      c0Fmt,
      c0W,
      c0H,
      surf.depthAttachment.has_value(),
      dHandle,
      dFmt,
      dW,
      dH);
  }

  if (width == 0U || height == 0U) {
    m_activeFrameHandle.reset();
    m_activeFrame = nullptr;
    m_frameRecording = false;
    m_submissionResourcePinningOpen = false;
    m_activeRenderer = nullptr;
    m_activePPLLIndex.reset();
    s_currentBackend = nullptr;
    return;
  }

  m_activeFrameHandle = device().frameExecutor().beginFrame();
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    m_activeFrame = nullptr;
    m_frameRecording = false;
    m_submissionResourcePinningOpen = false;
    m_activeRenderer = nullptr;
    m_activePPLLIndex.reset();
    s_currentBackend = nullptr;
    return;
  }

  auto& frameResources = ensureFrameResourcesForKey(m_activeFrameHandle->key());
  CHECK(m_staticCacheEpoch < std::numeric_limits<uint64_t>::max()) << "Static cache epoch overflow";
  ++m_staticCacheEpoch;

  // Populate script stats (if provided by ZVulkanLinearScript) early so they're
  // available for per-frame logging even if later code returns early.
  frameResources.scriptUniformHintMs = scriptStats ? scriptStats->uniformHintMs : 0.0;
  frameResources.scriptUniformHintBytes = scriptStats ? scriptStats->uniformHintBytes : 0;
  frameResources.scriptNodeCount = scriptStats ? scriptStats->nodeCount : 0u;
  frameResources.scriptRasterNodeCount = scriptStats ? scriptStats->rasterNodeCount : 0u;
  frameResources.scriptReplayNodeCount = scriptStats ? scriptStats->replayNodeCount : 0u;
  frameResources.scriptCommandsNodeCount = scriptStats ? scriptStats->commandsNodeCount : 0u;
  frameResources.scriptPreRecordNodeCount = scriptStats ? scriptStats->preRecordNodeCount : 0u;
  frameResources.scriptBatchCount = scriptStats ? scriptStats->batchCount : 0u;

  // Stage 2: apply descriptor arena reset when reusing this in-flight frame.
  // Safe point: frame executor waited for the fence when acquiring the frame.
  applyPendingArenaReset(frameResources);
  // Run any fence-gated completion callbacks for frames that have already
  // finished, then opportunistically enter the frame completion safe point for
  // those slots as well. This reduces latency for safe-point-gated consumers
  // (paging compaction readbacks, deferred descriptor updates, scratch releases)
  // without weakening ordering guarantees after completion callbacks (e.g.
  // residency unpins).
  m_completedFrameKeysScratch.clear();
  device().frameExecutor().pollCompletions(&m_completedFrameKeysScratch);
  pumpFrameCompletionSafePoints(m_completedFrameKeysScratch);
  ensureArenaOnFrame(frameResources);
  ensureUniformArena(frameResources);
  ensurePersistentUniformArena(frameResources);
  ensureBindlessSampledImagesOnFrame(frameResources);
  ensureDDPGatingResources(frameResources);
  // Expose frame resources early so suballocateUniform can target this frame.
  m_activeFrame = &frameResources;
  m_submissionResourcePinningOpen = true;
  // Pre-record warmups can create/register residency-managed or externally
  // brokered resources that are later consumed by this submission. Publish the
  // active backend before those warmups so resource helpers can attach
  // completion-safe pins even though command recording has not begun yet.
  s_currentBackend = this;
  // Compute and publish the shared per-frame lighting UBO slice before descriptor priming.
  {
    const size_t align = uniformAlignment();
    LightingUBOStd140 lighting{};
    const auto& scene = renderer.sceneState();
    // lighting_enabled: globally enabled when at least one configured light is present
    const size_t availableLightsRaw = std::min({scene.lighting.positions.size(),
                                                scene.lighting.ambient.size(),
                                                scene.lighting.diffuse.size(),
                                                scene.lighting.specular.size(),
                                                scene.lighting.attenuation.size(),
                                                scene.lighting.spotCutoff.size(),
                                                scene.lighting.spotExponent.size(),
                                                scene.lighting.spotDirection.size()});
    const size_t clampedCount = std::min(availableLightsRaw, static_cast<size_t>(scene.lighting.lightCount));
    lighting.numLights = static_cast<int>(std::min(clampedCount, lighting.lights.size()));
    lighting.lighting_enabled = (lighting.numLights > 0) ? 1 : 0;
    // Global scene ambient
    lighting.scene_ambient = scene.sceneAmbient;
    // Viewport reciprocal
    const auto vp = renderer.frameState().viewport;
    if (vp.z > 0u && vp.w > 0u) {
      lighting.screen_dim_RCP = glm::vec2(1.0f / static_cast<float>(vp.z), 1.0f / static_cast<float>(vp.w));
    }
    // Fog
    lighting.fog_color_top = scene.fog.topColor;
    lighting.fog_color_bottom = scene.fog.bottomColor;
    lighting.fog_end = scene.fog.range.y;
    lighting.fog_scale =
      scene.fog.range.y > scene.fog.range.x ? 1.0f / std::max(scene.fog.range.y - scene.fog.range.x, 1e-6f) : 0.0f;
    constexpr float kLog2e = 1.44269504088896340735992468100189214f;
    lighting.fog_density_log2e = scene.fog.density * kLog2e;
    lighting.fog_density_density_log2e = scene.fog.density * scene.fog.density * kLog2e;
    // Weighted blended OIT global scale shared via lighting UBO
    lighting.weighted_blended_depth_scale = scene.weightedBlendedDepthScale;
    // Depth conversion constants (zw = a/ze + b) used by WB init shaders
    const float n = renderer.viewState().nearClip;
    const float f = renderer.viewState().farClip;
    const float denom = std::max(f - n, 1e-6f);
    lighting.ze_to_zw_a = (f * n) / denom;
    lighting.ze_to_zw_b = 0.5f * (f + n) / denom + 0.5f;

    for (int i = 0; i < lighting.numLights; ++i) {
      const size_t idx = static_cast<size_t>(i);
      lighting.lights[i].position = scene.lighting.positions[idx];
      lighting.lights[i].ambient = scene.lighting.ambient[idx];
      lighting.lights[i].diffuse = scene.lighting.diffuse[idx];
      lighting.lights[i].specular = scene.lighting.specular[idx];
      lighting.lights[i].attenuation = scene.lighting.attenuation[idx];
      lighting.lights[i].spotCutoff = scene.lighting.spotCutoff[idx];
      lighting.lights[i].spotExponent = scene.lighting.spotExponent[idx];
      lighting.lights[i].spotDirection = scene.lighting.spotDirection[idx];
    }
    auto slice = suballocateUniform(sizeof(LightingUBOStd140), align);
    if (slice.mapped) {
      std::memcpy(slice.mapped, &lighting, sizeof(lighting));
    }
    frameResources.lightingDynOffset = slice.offset;

    // Picking pass: match OpenGL picking shader parameters (no lighting, no fog).
    // Vulkan shares a frame-global lighting UBO, so provide a dedicated slice
    // with lighting disabled and bind it via dynamic offset when recording
    // picking draws.
    LightingUBOStd140 pickingLighting = lighting;
    pickingLighting.lighting_enabled = 0;
    pickingLighting.numLights = 0;
    auto pickingSlice = suballocateUniform(sizeof(LightingUBOStd140), align);
    if (pickingSlice.mapped) {
      std::memcpy(pickingSlice.mapped, &pickingLighting, sizeof(pickingLighting));
    }
    frameResources.pickingLightingDynOffset = pickingSlice.offset;
  }
  // Compute and publish per-eye frame transform UBO slices. These are shared
  // across all draw calls and are the only transforms expected to change during
  // camera-only motion (view/projection matrices).
  {
    const size_t align = uniformAlignment();
    frameResources.frameTransformsDynOffsetByEye = {0, 0, 0};
    frameResources.frameTransformsDynOffsetValidMask = 0u;

    const auto& viewState = renderer.viewState();
    CHECK(viewState.eyes.size() == frameResources.frameTransformsDynOffsetByEye.size())
      << "Frame transform UBO eye-count mismatch";

    for (size_t eyeIndex = 0; eyeIndex < viewState.eyes.size(); ++eyeIndex) {
      const auto& eyeState = viewState.eyes[eyeIndex];

      FrameTransformsUBOStd140 xf{};
      xf.projection_view_matrix = eyeState.projectionViewMatrix;
      xf.view_matrix = eyeState.viewMatrix;
      xf.projection_matrix = eyeState.projectionMatrix;
      xf.inverse_projection_matrix = eyeState.inverseProjectionMatrix;

      const float orthoFlag = eyeState.isPerspective ? 0.0f : 1.0f;
      const float boxCorrection = computeSphereBoxCorrection(glm::degrees(eyeState.fieldOfView));
      xf.parameters = glm::vec4(0.0f, orthoFlag, boxCorrection, 0.0f);

      auto slice = suballocateUniform(sizeof(FrameTransformsUBOStd140), align);
      if (slice.mapped) {
        std::memcpy(slice.mapped, &xf, sizeof(xf));
      }

      frameResources.frameTransformsDynOffsetByEye[eyeIndex] = slice.offset;
      frameResources.frameTransformsDynOffsetValidMask |= (1u << static_cast<uint32_t>(eyeIndex));
    }
  }
  // Reset per-submission descriptor set counters after entering the completion
  // safe point for this frame-slot. The safe point already ingested the
  // previous submission's timings (if any) into Z3DPerfCollector.
  frameResources.descriptorSetsAllocated = 0;
  frameResources.gpuScopes.clear();
  frameResources.cpuScopes.clear();
  frameResources.nextQuery = 0;
  // Capture the frame name from the renderer (if provided)
  frameResources.frameName = std::string(renderer.currentFrameLabel());
  frameResources.progressivePassHint = renderer.currentRenderPassIsProgressive();
  const auto now = std::chrono::steady_clock::now();
  frameResources.beginRenderPreambleMs = std::chrono::duration<double, std::milli>(now - beginRenderEntry).count();
  const auto perfStart = Z3DRenderGlobalState::instance().currentPerfFrameStartTime();
  if (perfStart.time_since_epoch().count() != 0) {
    frameResources.preCpuStartMs = std::chrono::duration<double, std::milli>(now - perfStart).count();
  } else {
    frameResources.preCpuStartMs.reset();
  }
  frameResources.cpuStart = now;
  frameResources.cpuEnd = {};
  // Tag this submission with the current real-frame token and a submission index
  frameResources.realFrameToken = Z3DRenderGlobalState::instance().currentPerfFrameToken();
  frameResources.submissionId =
    Z3DRenderGlobalState::instance().nextPerfFrameSubmissionId(frameResources.realFrameToken);
  if (frameResources.realFrameToken != 0) {
    Z3DPerfCollector::instance().noteSubmissionStarted(frameResources.realFrameToken, frameResources.submissionId);
  }
  // Reset Stage 3 instrumentation
  frameResources.renderingSegmentsBegan = 0;
  frameResources.attachmentClears = 0;
  frameResources.attachmentLoads = 0;
  frameResources.drawsSubmitted = 0;
  frameResources.drawSecondaryCacheAttempts = 0;
  frameResources.drawSecondaryCacheKeyFound = 0;
  frameResources.drawSecondaryCacheSignatureMismatches = 0;
  frameResources.drawSecondaryCacheSignatureMismatchMaskOr = 0;
  frameResources.drawSecondaryCacheHits = 0;
  frameResources.drawSecondaryCacheBuilds = 0;
  frameResources.drawSecondaryCacheExecutes = 0;
  frameResources.pipelinesCreated = 0;
  frameResources.pipelinesBound.clear();
  CHECK(frameResources.externalResidencyPinReleases.empty())
    << "External Vulkan residency pins survived the frame completion safe point";
  frameResources.residencyPinnedTextures.clear();
  frameResources.activeSegmentFormats.reset();
  frameResources.skippedBatchesFormatMismatch = 0;
  frameResources.descriptorWritesWhileRecording = 0;
  frameResources.boundSetRewriteAttempts = 0;
  // Reset Stage 4 (readback) bookkeeping
  frameResources.pendingColorReadbacks.clear();
  frameResources.pendingBufferReadbacks.clear();
  frameResources.forceFenceWaitForCompletionSafePoint = false;
  frameResources.readbackBytesCopied = 0;
  frameResources.readbackSlotsInFlight = 0;
  frameResources.allSamples = 0;
  frameResources.allMaxMs.reset();
  // Reset Stage 4.5 (static staging) bookkeeping
  frameResources.staticBytesStaged = 0;
  frameResources.staticStreamRestaged = 0;
  frameResources.linesBytesStaged = 0;
  frameResources.fontsBytesStaged = 0;
  frameResources.meshesBytesStaged = 0;
  frameResources.spheresBytesStaged = 0;
  frameResources.conesBytesStaged = 0;
  frameResources.ellipsoidsBytesStaged = 0;
  frameResources.scheduledCopies.clear();
  // Reset DDP device-args arena for this submission.
  frameResources.ddpArgsDevice.cursor = 0;
  frameResources.ddpArgsDevice.retiredBuffers.clear();
  trimUnusedUploadPages(frameResources.uploadArena);
  // Reset per-frame upload pages. Pages are kept per frame-slot so upload
  // slices remain valid until submission completion, then reused by rewinding
  // the compact active prefix at the next safe point.
  for (auto& page : frameResources.uploadArena.pages) {
    page.cursor = 0;
  }
  frameResources.uploadArena.activePageIndex = 0;
  frameResources.uploadArena.usedPageCount = 0;
  frameResources.uploadArena.usedBytes = 0;
  frameResources.uploadArena.highWatermark = 0;
  // Run any pending "pre-record" actions now that the frame slot, descriptor
  // arena, and thread-local renderer state are established, but before descriptor
  // priming and command-buffer recording begins.
  //
  // These actions are call-site defined (opaque to the script/backend) and are
  // intended for resource priming such as PPLL sizing/activation.
  if (!preRecordActions.empty()) {
    if (VLOG_IS_ON(1)) {
      const std::string frameLabel = std::string(renderer.currentFrameLabel());
      VLOG(1) << fmt::format("VK preRecord: frame='{}' actions={} pending_label='{}'",
                             frameLabel.empty() ? std::string("<unlabeled-frame>") : frameLabel,
                             preRecordActions.size(),
                             pendingPreRecordLabel.empty() ? std::string("<none>") : pendingPreRecordLabel);
    }
    for (auto& action : preRecordActions) {
      CHECK(action.fn) << "VK preRecord action missing function";
      if (VLOG_IS_ON(2)) {
        VLOG(2) << "VK preRecord action: " << (action.label.empty() ? std::string("<unlabeled>") : action.label);
      }
      action.fn(*this, renderer);
    }
  }

  // Expose frame resources to allow pre-record descriptor priming (already set above)

  // Prime the backend-shared per-frame-slot descriptor sets now that pre-record
  // actions (e.g. PPLL sizing / ring selection) have run. These sets are bound
  // across multiple pipeline contexts and must not be written during recording.
  ensureSharedDescriptorSetsOnFrame(frameResources);

  // Prime helper compute descriptor sets that are bound during recording. This
  // keeps vkUpdateDescriptorSets out of command buffer recording on all paths.
  {
    ensureDDPComputePipeline();
    CHECK(m_ddpCountSetLayout.has_value()) << "DDP count compute descriptor set layout missing";
    CHECK(m_activeFrame != nullptr) << "DDP compute priming requires an active frame";
    CHECK(m_activeFrame->ddpChangedFlag && m_activeFrame->ddpIndirectCount)
      << "DDP compute priming requires ddpChangedFlag + ddpIndirectCount buffers";
    if (!m_activeFrame->ddpCountComputeDescriptorSet) {
      m_activeFrame->ddpCountComputeDescriptorSet = allocatePersistentDescriptorSet(**m_ddpCountSetLayout);
      CHECK(m_activeFrame->ddpCountComputeDescriptorSet != nullptr)
        << "Failed to allocate DDP count compute descriptor set";
    }
    // binding 0 = changed flag SSBO, binding 1 = indirect count SSBO
    m_activeFrame->ddpCountComputeDescriptorSet->updateStorageBuffer(0, *m_activeFrame->ddpChangedFlag);
    m_activeFrame->ddpCountComputeDescriptorSet->updateStorageBuffer(1, *m_activeFrame->ddpIndirectCount);
  }

  // Shared descriptor sets (lighting/transforms/OIT + slice/raycaster helpers)
  // are primed in ensureSharedDescriptorSetsOnFrame(). Per-context pre-record
  // descriptor priming is intentionally avoided to keep all common descriptor
  // state centralized and stable across frames.

  vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  auto& cmdBuffer = m_activeFrameHandle->commandBuffer();
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "VK cmdBegin: flags=eOneTimeSubmit";
  }
  cmdBuffer.begin(beginInfo);
  if (timestampQueriesEnabled()) {
    cmdBuffer.resetQueryPool(*frameResources.queryPool, 0, kMaxTimestampQueries);
  }

  m_frameRecording = true;

  // Install scratch-pool deferred release scheduler for this backend.
  // (Used by RenderTargetLease to delay Vulkan slot reuse until the frame-slot
  // reaches the completion safe point.)
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  pool.setVulkanMemoryPressureHandler([this](Z3DScratchResourcePool::VulkanScratchReclaimMode mode) {
    reclaimTransientResourcesForMemoryPressure(mode, "scratch_pool");
  });
  pool.setVulkanReleaseScheduler([this](std::function<void()> fn) {
    if (!fn) {
      return;
    }

    // If we're already at a completion safe point, it is safe to release
    // immediately. Re-entrant safe-point scheduling is forbidden elsewhere.
    if (m_frameInCompletionSafePoint.load(std::memory_order_acquire) != nullptr) {
      fn();
      return;
    }

    if (!m_sharedDevice) {
      fn();
      return;
    }

    if (currentRenderThreadExecutorOrNull() == nullptr) {
      // Exception teardown can destroy persistent scratch leases after the engine
      // has cleared render-thread executor TLS. At that point we cannot register
      // a coroutine completion hook, so fall back to a blocking GPU-safe release.
      m_sharedDevice->frameExecutor().waitForAllInFlight();
      for (auto& frame : m_frames) {
        applyPendingArenaReset(frame);
      }
      fn();
      return;
    }

    // Persistent scratch leases can be released outside an active frame when
    // resizing between tiled-export regions. If all submitted work has already
    // reached its fence, the slot is GPU-safe now; releasing immediately lets
    // the following acquire retarget/reuse the same slot instead of allocating
    // a parallel full-size/edge-size variant.
    if (!m_activeFrame && !m_frameRecording && m_sharedDevice->frameExecutor().inFlightCount() == 0u) {
      fn();
      return;
    }

    // Common case: delay until the current frame-slot reaches the completion
    // safe point, but run before user readback/update hooks so completed
    // scratch backing is reclaimable under strict residency budgets.
    const std::string_view debugLabel = "scratch_pool_vulkan_release";
    if (m_activeFrame && m_activeFrameHandle && m_activeFrameHandle->valid()) {
      m_activeFrame->afterFrameCompletionHooks.registerHook(
        FrameHookSpot::AfterFrameResourceRelease,
        currentRenderThreadExecutorKeepAlive(debugLabel),
        [fn = std::move(fn)](Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
          fn();
          co_return;
        },
        debugLabel);
      return;
    }

    // If the lease is released after endRender() has cleared the active frame,
    // defer the release to the most recently submitted slot. This preserves
    // non-blocking submission while still preventing scratch-slot reuse until
    // the backend observes the completion safe point for that slot.
    if (currentRenderThreadExecutorOrNull() != nullptr && m_lastSubmittedFrameKey != nullptr &&
        m_frameDevice == m_sharedDevice) {
      auto it = m_frameResourceMap.find(m_lastSubmittedFrameKey);
      if (it != m_frameResourceMap.end()) {
        CHECK_LT(it->second, m_frames.size()) << "Last submitted frame index out of bounds";
        m_frames[it->second].afterFrameCompletionHooks.registerHook(
          FrameHookSpot::AfterFrameResourceRelease,
          currentRenderThreadExecutorKeepAlive(debugLabel),
          [fn = std::move(fn)](Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
            fn();
            co_return;
          },
          debugLabel);
        return;
      }
    }

    // Fallback: if we cannot tie the release to a known backend frame-slot,
    // drain GPU work and also drain any pending safe-point hooks so we do not
    // strand readback consumers that gate UI presentation.
    m_sharedDevice->frameExecutor().waitForAllInFlight();
    for (auto& frame : m_frames) {
      applyPendingArenaReset(frame);
    }
    fn();
  });

  // beginRender() ends here; command buffer recording continues in pipeline contexts.
}

void Z3DRendererVulkanBackend::endRender(Z3DRendererBase& renderer)
{
  (void)renderer;
  auto clearCurrentBackendGuard = folly::makeGuard([]() {
    s_currentBackend = nullptr;
  });
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    m_submissionResourcePinningOpen = false;
    return;
  }

  auto& frame = *m_activeFrame;
  auto& frameHandle = *m_activeFrameHandle;
  m_lastSubmittedFrameKey = frameHandle.key();

  auto resetActiveStateGuard = folly::makeGuard([this]() {
    m_frameRecording = false;
    m_submissionResourcePinningOpen = false;
    m_activeFrameHandle.reset();
    m_activeFrame = nullptr;
    m_activeRenderer = nullptr;
    m_activePPLLIndex.reset();
  });

  // Insert end-of-frame image->buffer copies for pending readbacks
  if (m_frameRecording && !frame.pendingColorReadbacks.empty()) {
    auto& cmd = frameHandle.commandBuffer();
    for (auto& pr : frame.pendingColorReadbacks) {
      if (pr.slotIndex < 0 || pr.src == nullptr) {
        continue;
      }
      try {
        CHECK(static_cast<size_t>(pr.slotIndex) < m_readbackSlots.size()) << "Readback slot index out of bounds";
        const auto originalLayout = pr.src->layout();
        const auto aspect = (pr.aspectMask == vk::ImageAspectFlags{}) ? vk::ImageAspectFlagBits::eColor : pr.aspectMask;
        pr.src->transitionLayout(cmd, originalLayout, vk::ImageLayout::eTransferSrcOptimal, aspect);
        vk::BufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = aspect;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = (pr.src->info().imageType == vk::ImageType::e3D) ? 0u : pr.arrayLayer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = vk::Offset3D{0, 0, 0};
        region.imageExtent = vk::Extent3D{pr.size.x, pr.size.y, 1u};
        const auto& slot = m_readbackSlots[static_cast<size_t>(pr.slotIndex)];
        CHECK(slot.mapped != nullptr) << "Readback slot missing mapped pointer";
        cmd.copyImageToBuffer(pr.src->image(), vk::ImageLayout::eTransferSrcOptimal, slot.buffer->buffer(), region);
        const vk::ImageLayout restore =
          (originalLayout == vk::ImageLayout::eUndefined) ? vk::ImageLayout::eGeneral : originalLayout;
        pr.src->transitionLayout(cmd, vk::ImageLayout::eTransferSrcOptimal, restore, aspect);
        frame.readbackBytesCopied += pr.bytes;
        frame.readbackSlotsInFlight++;
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Vulkan readback copy failed: " << e.what();
      }
    }
  }

  // Insert end-of-frame buffer->buffer copies for pending readbacks
  if (m_frameRecording && !frame.pendingBufferReadbacks.empty()) {
    auto& cmd = frameHandle.commandBuffer();
    for (auto& pr : frame.pendingBufferReadbacks) {
      if (pr.slotIndex < 0 || pr.src == nullptr || pr.bytes == 0) {
        continue;
      }
      CHECK(static_cast<size_t>(pr.slotIndex) < m_readbackSlots.size());
      try {
        // Conservative memory barrier: make all prior writes visible to transfer reads.
        vk::BufferMemoryBarrier2 barrier{};
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
        barrier.buffer = pr.src->buffer();
        barrier.offset = pr.srcOffset;
        barrier.size = pr.bytes;
        vk::DependencyInfo dep{};
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &barrier;
        cmd.pipelineBarrier2(dep);

        vk::BufferCopy region{};
        region.srcOffset = pr.srcOffset;
        region.dstOffset = 0;
        region.size = pr.bytes;
        const auto& slot = m_readbackSlots[static_cast<size_t>(pr.slotIndex)];
        CHECK(slot.mapped != nullptr) << "Buffer readback slot missing mapped pointer";
        cmd.copyBuffer(pr.src->buffer(), slot.buffer->buffer(), region);
        frame.readbackBytesCopied += pr.bytes;
        frame.readbackSlotsInFlight++;
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Vulkan buffer readback copy failed: " << e.what();
      }
    }
  }

  if (m_frameRecording) {
    try {
      frameHandle.commandBuffer().end();
      if (VLOG_IS_ON(2)) {
        VLOG(2) << "VK cmdEnd";
      }
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan command buffer end failed: " << e.what();
      return;
    }
  }

  m_frameRecording = false;

  frame.cpuEnd = std::chrono::steady_clock::now();

  auto& context = m_sharedDevice->context();
  auto& queue = context.graphicsQueue();
  vk::CommandBuffer rawCmd = *frameHandle.commandBuffer();
  vk::SubmitInfo submitInfo{};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &rawCmd;

  // Stage 1 (offscreen/no WSI): do not wait on an acquire semaphore.
  // These semaphores are reserved for swapchain integration; waiting here
  // without a prior signal leads to a dead wait and device loss on MoltenVK.
  // When WSI is wired, plumb an explicit flag to arm these sync points.

  // Optionally keep signalling the release semaphore for future presentation wiring.
  // For now, omit both waits and signals to avoid dangling sync dependencies.
  // Uncomment the following block once presentation path consumes the signal:
  // vk::Semaphore signalSemaphore = static_cast<vk::Semaphore>(*frameHandle.releaseSemaphore());
  // if (signalSemaphore) {
  //   submitInfo.signalSemaphoreCount = 1;
  //   submitInfo.pSignalSemaphores = &signalSemaphore;
  // }

  // Diagnostics: log submission intent with frame identity and wait conditions.
  const bool hasReadbacks = (m_activeFrame && (!m_activeFrame->pendingColorReadbacks.empty() ||
                                               !m_activeFrame->pendingBufferReadbacks.empty()));
  const bool progressivePassHint = frame.progressivePassHint;
  const bool waitForReadbacksPolicy = !progressivePassHint;
  const bool needFenceWait = (m_pumpFenceAfterFirstSubmit || frame.forceFenceWaitForCompletionSafePoint ||
                              (hasReadbacks && waitForReadbacksPolicy));
  if (VLOG_IS_ON(1)) {
    VLOG(1) << fmt::format(
      "VK queueSubmit: frame='{}' token={} submit#{} cmd=0x{:x} fence=0x{:x} has_readback={} progressive_pass_hint={} wait_for_readbacks_policy={} force_safe_point_wait={} will_wait={} pending_color_readbacks={} pending_buffer_readbacks={}",
      frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
      frame.realFrameToken,
      frame.submissionId,
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkCommandBuffer>(rawCmd))),
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkFence>(*frameHandle.fence()))),
      hasReadbacks,
      progressivePassHint,
      waitForReadbacksPolicy,
      frame.forceFenceWaitForCompletionSafePoint,
      needFenceWait,
      frame.pendingColorReadbacks.size(),
      frame.pendingBufferReadbacks.size());
  }

  // Keep submission-scoped pins attached to the backend frame record until the
  // frame completion safe point. applyPendingArenaReset() releases them before
  // user completion hooks run, which gives all residency classes the same
  // after-fence boundary and avoids extending pins into readback/update hooks.
  m_submissionResourcePinningOpen = false;

  bool submitted = false;
  try {
    queue.submit(submitInfo, *frameHandle.fence());
    device().frameExecutor().markSubmitted(frameHandle);
    submitted = true;
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Vulkan queue submit failed: " << e.what();
  }

  if (!submitted) {
    if (!frame.residencyPinnedTextures.empty()) {
      for (auto* tex : frame.residencyPinnedTextures) {
        device().residencyManager().unpinIfManaged(tex);
      }
      frame.residencyPinnedTextures.clear();
    }
    if (!frame.externalResidencyPinReleases.empty()) {
      for (auto& [_, release] : frame.externalResidencyPinReleases) {
        CHECK(release) << "External Vulkan residency release callback is empty";
        release();
      }
      frame.externalResidencyPinReleases.clear();
    }
    if (!frame.pinnedStaticSegments.empty()) {
      for (void* segVoid : frame.pinnedStaticSegments) {
        auto* seg = static_cast<StaticArena::Segment*>(segVoid);
        if (seg == nullptr) {
          continue;
        }
        CHECK_GT(seg->pinCount, 0u) << "Static segment pinCount underflow on failed submission";
        seg->pinCount--;
        if (seg->pinCount == 0) {
          flushPendingFreesAndMaybeTrimStaticSegment(seg);
        }
      }
      frame.pinnedStaticSegments.clear();
    }
  }

  // Collect this submission's CPU/GPU timings when the fence completes.
  //
  // Why: all Vulkan renderer backends share a single ZVulkanFrameExecutor ring
  // (command buffers + fences). Slot reuse (beginFrame) can therefore be driven
  // by *other* backends, which means relying solely on this backend re-entering
  // the completion safe point via beginRender() is not robust.
  //
  // If a backend never re-acquires the slot that recorded a given submission
  // (common when multiple backends interleave submissions), that submission is
  // never ingested into Z3DPerfCollector, and the per-frame perf summary stalls
  // because Z3DPerfCollector preserves token ordering.
  //
  // Ordering: readback-fence batons may already be registered on this frame.
  // The backend completion safe point releases residency/static pins before it
  // runs user hooks, then ingests timings after those hooks finish.
  if (frame.realFrameToken != 0 && frame.submissionId != 0) {
    if (submitted) {
      const std::weak_ptr<bool> alive = m_aliveFlag;
      void* frameKey = frameHandle.key();
      const uint64_t expectedToken = frame.realFrameToken;
      const uint32_t expectedSubmissionId = frame.submissionId;
      auto renderEx = currentRenderThreadExecutorKeepAlive("vk_collect_frame_timings");
      device().frameExecutor().scheduleAfterCompletion(
        frameHandle,
        [alive,
         renderEx = std::move(renderEx),
         backend = this,
         frameKey,
         expectedToken,
         expectedSubmissionId]() mutable {
          auto aliveStrong = alive.lock();
          if (!aliveStrong || !*aliveStrong) {
            return;
          }

          // If the frame executor observed completion on the render thread (the
          // common case), collect inline to minimize latency.
          if (currentRenderThreadExecutorOrNull() != nullptr) {
            backend->enterCompletionSafePointForKeyIfMatches(frameKey, expectedToken, expectedSubmissionId);
            return;
          }

          renderEx->add([alive, backend, frameKey, expectedToken, expectedSubmissionId]() mutable {
            auto aliveStrong2 = alive.lock();
            if (!aliveStrong2 || !*aliveStrong2) {
              return;
            }
            backend->enterCompletionSafePointForKeyIfMatches(frameKey, expectedToken, expectedSubmissionId);
          });
        });
    } else {
      // Avoid stalling the perf collector on a started submission that never
      // reaches the GPU. This is an external failure path, so we still emit the
      // CPU-side timing/log line best-effort (GPU scopes may be unavailable).
      collectFrameTimings(frame);
    }
  }

  // Stage 2: schedule exactly one descriptor arena reset for this frame.
  scheduleArenaReset(frame);

  if (needFenceWait) {
    // If the caller requested a synchronous safe-point boundary, do not rely on
    // the engine's opportunistic pollVulkanCompletionsOnce(). Instead, wait
    // here until the current submission reaches the backend's frame completion
    // safe point (applyPendingArenaReset), so hooks that gate progressive flow
    // (paging cache uploads, compaction readbacks, etc.) are observed before we
    // return to client code.
    //
    // Important cancellation invariant:
    // Some callers (notably ZVulkanLinearScript::readbackBufferTo) register
    // fence-gated completion hooks that copy GPU readback data into *caller*
    // memory (often stack-allocated vectors). Those APIs are documented to
    // return only after the completion safe point has executed, precisely so
    // that the destination pointer remains valid.
    //
    // Therefore, we must not throw cancellation while waiting for the safe
    // point. If cancellation is requested during this wait, defer propagation
    // until AFTER the waited submission reaches the safe point and all hooks
    // have run. Otherwise, we can unwind the caller stack and later scribble
    // into freed memory from a completion callback (heap corruption / malloc
    // checksum failures on macOS).
    if (submitted) {
      constexpr auto kPollInterval = std::chrono::milliseconds(1);
      const folly::CancellationToken cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
      void* waitedKey = frameHandle.key();
      bool waitedKeyCompleted = false;
      bool cancellationRequested = false;
      while (!waitedKeyCompleted) {
        cancellationRequested |= cancellationToken.isCancellationRequested();

        m_completedFrameKeysScratch.clear();
        device().frameExecutor().pollCompletions(&m_completedFrameKeysScratch);
        if (!m_completedFrameKeysScratch.empty()) {
          for (void* key : m_completedFrameKeysScratch) {
            if (key == waitedKey) {
              waitedKeyCompleted = true;
              break;
            }
          }
          pumpFrameCompletionSafePoints(m_completedFrameKeysScratch);
        }

        if (!waitedKeyCompleted) {
          std::this_thread::sleep_for(kPollInterval);
        }
      }

      // We reached the safe point for the active submission; opportunistically
      // drain any other completions as well.
      m_completedFrameKeysScratch.clear();
      device().frameExecutor().pollCompletions(&m_completedFrameKeysScratch);
      pumpFrameCompletionSafePoints(m_completedFrameKeysScratch);

      if (cancellationRequested) {
        throw ZCancellationException();
      }
    }

    // Fence signaled; safe-point work executed.
    if (m_pumpFenceAfterFirstSubmit) {
      VLOG(1) << "VK pumped first-frame fence: delivered readback callbacks";
      m_pumpFenceAfterFirstSubmit = false;
    }
  } else {
    // Poll completion callbacks; some fences may have signaled already.
    m_completedFrameKeysScratch.clear();
    device().frameExecutor().pollCompletions(&m_completedFrameKeysScratch);
    pumpFrameCompletionSafePoints(m_completedFrameKeysScratch);
  }

  // VLOG(1) frame recycling stats (descriptors and arena reset scheduling)
  vlogFrameRecyclingStats(frame);

  // Stage 3: VLOG instrumentation for dynamic rendering segments and pipeline stats
  VLOG(1) << fmt::format(
    "VK segments: frame='{}' token={} submit#{} began={} clears={} loads={} pipelines_created={} pipelines_bound_unique={} skipped_format_mismatch={} descriptor_writes_while_recording={} bound_set_rewrite_attempts={} readback_bytes_copied={} readback_slots_in_flight={} static_bytes_staged={} static_stream_restaged={} lines_bytes_staged={} fonts_bytes_staged={} meshes_bytes_staged={} spheres_bytes_staged={} cones_bytes_staged={} ellipsoids_bytes_staged={}",
    frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
    frame.realFrameToken,
    frame.submissionId,
    frame.renderingSegmentsBegan,
    frame.attachmentClears,
    frame.attachmentLoads,
    frame.pipelinesCreated,
    frame.pipelinesBound.size(),
    frame.skippedBatchesFormatMismatch,
    frame.descriptorWritesWhileRecording,
    frame.boundSetRewriteAttempts,
    frame.readbackBytesCopied,
    frame.readbackSlotsInFlight,
    frame.staticBytesStaged,
    frame.staticStreamRestaged,
    frame.linesBytesStaged,
    frame.fontsBytesStaged,
    frame.meshesBytesStaged,
    frame.spheresBytesStaged,
    frame.conesBytesStaged,
    frame.ellipsoidsBytesStaged);

  // Stage 5: VLOG(1) static/upload/uniform arena usage
  VLOG(1) << fmt::format(
    "VK arena: frame='{}' token={} submit#{} upload_high_watermark={}B uniform_high_watermark={}B uniform_capacity={}B static_vb_used={}B static_ib_used={}B",
    frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
    frame.realFrameToken,
    frame.submissionId,
    m_activeFrame ? m_activeFrame->uploadArena.highWatermark : 0,
    frame.uniformArena.highWatermark,
    frame.uniformArena.capacity,
    m_staticArena.vbHighWatermark,
    m_staticArena.ibHighWatermark);

  // Debug-only guardrail: if a caller supplied a "min capacity" hint for the
  // uniform arena, validate that actual usage did not exceed it. This catches
  // estimator drift (new/unaccounted suballocateUniform sites) even when the
  // base/carry capacity masks overflow.
  if (frame.uniformArena.minCapacityHint > 0) {
    DCHECK(frame.uniformArena.highWatermark <= frame.uniformArena.minCapacityHint)
      << fmt::format("VK uniform hint under-estimated: frame='{}' token={} submit#{} hint={}B used={}B",
                     frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
                     frame.realFrameToken,
                     frame.submissionId,
                     frame.uniformArena.minCapacityHint,
                     frame.uniformArena.highWatermark);
  }

  // No cross-frame uniform sizing heuristics; each frame is sized independently.
}

namespace {

std::string_view describeGeometry(const GeometryPayload& geometry)
{
  if (std::holds_alternative<std::monostate>(geometry)) {
    return "none";
  }
  if (std::holds_alternative<LinePayload>(geometry)) {
    return "line";
  }
  if (std::holds_alternative<MeshPayload>(geometry)) {
    return "mesh";
  }
  if (std::holds_alternative<SpherePayload>(geometry)) {
    return "sphere";
  }
  if (std::holds_alternative<BackgroundPayload>(geometry)) {
    return "background";
  }
  if (std::holds_alternative<TextureCopyPayload>(geometry)) {
    return "texture_copy";
  }
  if (std::holds_alternative<TextureBlendPayload>(geometry)) {
    return "texture_blend";
  }
  if (std::holds_alternative<TextureGlowPayload>(geometry)) {
    return "texture_glow";
  }
  if (std::holds_alternative<TextureDualPeelPayload>(geometry)) {
    return "texture_dual_peel";
  }
  if (std::holds_alternative<TextureWeightedAveragePayload>(geometry)) {
    return "texture_weighted_average";
  }
  if (std::holds_alternative<TextureWeightedBlendedPayload>(geometry)) {
    return "texture_weighted_blended";
  }
  if (std::holds_alternative<TexturePPLLResolvePayload>(geometry)) {
    return "texture_ppll_resolve";
  }
  if (std::holds_alternative<EllipsoidPayload>(geometry)) {
    return "ellipsoid";
  }
  if (std::holds_alternative<ConePayload>(geometry)) {
    return "cone";
  }
  if (std::holds_alternative<FontPayload>(geometry)) {
    return "font";
  }
  return "unknown";
}

} // namespace

void Z3DRendererVulkanBackend::processBatches(Z3DRendererBase& renderer, const RendererCPUState& state)
{
  if (!m_activeFrame || state.batches.empty()) {
    return;
  }

  const bool profilePipelineContexts = absl::GetFlag(FLAGS_atlas_vk_profile_pipeline_contexts);
  struct PipelineCpuTimes
  {
    double lineMs = 0.0;
    double meshMs = 0.0;
    double sphereMs = 0.0;
    double ellipsoidMs = 0.0;
    double coneMs = 0.0;
    double backgroundMs = 0.0;
    double imgSliceMs = 0.0;
    double imgRaycasterMs = 0.0;
    double fontMs = 0.0;
    double textureCopyMs = 0.0;
    double textureBlendMs = 0.0;
    double textureGlowMs = 0.0;
    double textureDualPeelMs = 0.0;
    double textureWeightedAverageMs = 0.0;
    double textureWeightedBlendedMs = 0.0;
    double texturePPLLResolveMs = 0.0;
  };
  PipelineCpuTimes cpuTimes{};

  auto& cmd = m_activeFrameHandle->commandBuffer();

  // Segment key for coalescing dynamic-rendering segments. This must include
  // every field that affects vkCmdBeginRendering state (render area, attachments,
  // load/store ops, clear values, and final-use contracts) so we never merge two
  // passes that require different begin-rendering semantics.
  struct SegmentAttachmentKey
  {
    uint64_t id = 0;
    uint32_t index = 0;
    LoadOp loadOp = LoadOp::Load;
    StoreOp storeOp = StoreOp::Store;
    AttachmentFinalUse finalUse = AttachmentFinalUse::Unspecified;
    ClearValue clearValue{};
    bool operator==(const SegmentAttachmentKey& o) const
    {
      const auto colorEqual = [](const glm::vec4& a, const glm::vec4& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
      };
      return id == o.id && index == o.index && loadOp == o.loadOp && storeOp == o.storeOp && finalUse == o.finalUse &&
             colorEqual(clearValue.color, o.clearValue.color) && clearValue.depth == o.clearValue.depth &&
             clearValue.stencil == o.clearValue.stencil;
    }
  };
  struct SegmentKey
  {
    vk::Rect2D renderArea{};
    // Attachment keys are built for every raster batch during steady-state
    // rendering. Most passes use a single color attachment (or a small handful),
    // so keep a small inline array to avoid per-batch heap allocations.
    enum : size_t
    {
      kInlineColorCapacity = 8
    };
    std::array<SegmentAttachmentKey, kInlineColorCapacity> inlineColors{};
    uint32_t inlineColorCount = 0;
    std::vector<SegmentAttachmentKey> overflowColors{};
    std::optional<SegmentAttachmentKey> depth;

    [[nodiscard]] std::span<const SegmentAttachmentKey> colors() const
    {
      if (!overflowColors.empty()) {
        return std::span<const SegmentAttachmentKey>(overflowColors.data(), overflowColors.size());
      }
      return std::span<const SegmentAttachmentKey>(inlineColors.data(), inlineColorCount);
    }

    bool operator==(const SegmentKey& o) const
    {
      if (renderArea.offset.x != o.renderArea.offset.x || renderArea.offset.y != o.renderArea.offset.y ||
          renderArea.extent.width != o.renderArea.extent.width ||
          renderArea.extent.height != o.renderArea.extent.height) {
        return false;
      }
      const auto a = colors();
      const auto b = o.colors();
      if (a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin())) {
        return false;
      }
      return depth == o.depth;
    }
  };
  auto buildKey = [](const RenderBatch& b, const vk::Rect2D& renderArea) {
    SegmentKey k;
    k.renderArea = renderArea;
    for (const auto& a : b.pass.colorAttachments) {
      if (!a.handle.valid()) {
        continue;
      }
      SegmentAttachmentKey key{.id = a.handle.id,
                               .index = a.handle.index,
                               .loadOp = a.loadOp,
                               .storeOp = a.storeOp,
                               .finalUse = a.finalUse,
                               .clearValue = a.clearValue};
      if (k.overflowColors.empty() && k.inlineColorCount < SegmentKey::kInlineColorCapacity) {
        k.inlineColors[k.inlineColorCount++] = key;
      } else {
        if (k.overflowColors.empty()) {
          k.overflowColors.reserve(b.pass.colorAttachments.size());
          k.overflowColors.insert(k.overflowColors.end(),
                                  k.inlineColors.begin(),
                                  k.inlineColors.begin() + k.inlineColorCount);
        }
        k.overflowColors.push_back(key);
      }
    }
    if (b.pass.depthAttachment && b.pass.depthAttachment->handle.valid()) {
      const auto& a = *b.pass.depthAttachment;
      k.depth = SegmentAttachmentKey{.id = a.handle.id,
                                     .index = a.handle.index,
                                     .loadOp = a.loadOp,
                                     .storeOp = a.storeOp,
                                     .finalUse = a.finalUse,
                                     .clearValue = a.clearValue};
    }
    return k;
  };

  // Begin a dynamic rendering segment for the batch's attachments via the recorder
  auto beginSegmentForBatch = [&](const RenderBatch& batch,
                                  ZVulkanPipelineCommandRecorder& recorder,
                                  ZVulkanPipelineCommandRecorder::RenderingSegmentSpec& outSpec,
                                  bool forceLoadOps) {
    outSpec = {};
    // Render area from pass viewport/scissor
    outSpec.renderArea = vulkan::toVkScissor(batch.pass);

    auto chooseAttachmentView =
      [](ZVulkanTexture& texture, uint32_t layerIndex, vk::ImageAspectFlags aspect) -> vk::ImageView {
      // AttachmentHandle.index is treated as the array layer for 2D array textures. This enables
      // segment-managed pipelines (backend-owned vkCmdBeginRendering) to target individual layers without
      // self-managed recording.
      const vk::ImageView view = texture.layerImageView(layerIndex, aspect);
      CHECK(static_cast<VkImageView>(view) != VK_NULL_HANDLE)
        << "Vulkan attachment view unavailable: layer=" << layerIndex << " layers=" << texture.arrayLayers();
      return view;
    };

    // Build color attachments
    outSpec.colorAttachments.reserve(batch.pass.colorAttachments.size());
    for (const auto& attachment : batch.pass.colorAttachments) {
      if (!attachment.handle.valid()) {
        continue;
      }
      auto& texture = vulkan::textureFromHandle(attachment.handle, device(), "renderer color attachment");
      CHECK(texture.resident()) << "Vulkan color attachment backing is not resident before recording";
      ZVulkanAttachmentInfo info{};
      info.image = texture.image();
      info.view = chooseAttachmentView(texture, attachment.handle.index, vk::ImageAspectFlagBits::eColor);
      info.format = texture.format();
      info.initialLayout = texture.layout();

      // Choose explicit final layout based on backend-neutral attachment usage metadata.
      // This avoids relying on shader-hook / label heuristics that are prone to state leakage.
      switch (attachment.finalUse) {
        case AttachmentFinalUse::Unspecified:
          CHECK(false) << "AttachmentDesc::finalUse must be specified for Vulkan passes (explicit metadata required)";
          info.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
          break;
        case AttachmentFinalUse::RenderTarget:
          info.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
          break;
        case AttachmentFinalUse::Sampled:
          info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
          break;
        case AttachmentFinalUse::TransferSrc:
          info.finalLayout = vk::ImageLayout::eTransferSrcOptimal;
          break;
        case AttachmentFinalUse::General:
          info.finalLayout = vk::ImageLayout::eGeneral;
          break;
      }
      info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
      if (forceLoadOps) {
        info.loadOp = vk::AttachmentLoadOp::eLoad;
      }
      info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
      info.aspect = vk::ImageAspectFlagBits::eColor;
      info.trackingTexture = &texture;
      vk::ClearValue clear{};
      clear.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                             attachment.clearValue.color.g,
                                                             attachment.clearValue.color.b,
                                                             attachment.clearValue.color.a});
      info.clearValue = clear;
      outSpec.colorAttachments.push_back(info);
    }

    // Optional depth attachment
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      const auto& attachment = *batch.pass.depthAttachment;
      auto& texture = vulkan::textureFromHandle(attachment.handle, device(), "renderer depth attachment");
      CHECK(texture.resident()) << "Vulkan depth attachment backing is not resident before recording";
      ZVulkanAttachmentInfo info{};
      info.image = texture.image();
      info.view = chooseAttachmentView(texture, attachment.handle.index, vk::ImageAspectFlagBits::eDepth);
      info.format = texture.format();
      info.initialLayout = texture.layout();
      switch (attachment.finalUse) {
        case AttachmentFinalUse::Unspecified:
          CHECK(false) << "AttachmentDesc::finalUse must be specified for Vulkan passes (explicit metadata required)";
          info.finalLayout = vk::ImageLayout::eDepthAttachmentOptimal;
          break;
        case AttachmentFinalUse::RenderTarget:
          info.finalLayout = vk::ImageLayout::eDepthAttachmentOptimal;
          break;
        case AttachmentFinalUse::Sampled:
          info.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
          break;
        case AttachmentFinalUse::TransferSrc:
          info.finalLayout = vk::ImageLayout::eTransferSrcOptimal;
          break;
        case AttachmentFinalUse::General:
          info.finalLayout = vk::ImageLayout::eGeneral;
          break;
      }
      info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
      if (forceLoadOps) {
        info.loadOp = vk::AttachmentLoadOp::eLoad;
      }
      info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
      // Treat all depth(-stencil) formats as depth aspect for rendering; recorder will bind stencil if needed
      info.aspect = vk::ImageAspectFlagBits::eDepth;
      info.trackingTexture = &texture;
      vk::ClearValue clear{};
      clear.depthStencil = vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
      info.clearValue = clear;
      outSpec.depthStencilAttachment = info;
    }

    // Skip empty segments
    if (outSpec.colorAttachments.empty() && !outSpec.depthStencilAttachment.has_value()) {
      if (VLOG_IS_ON(2)) {
        VLOG(2) << fmt::format("VK skip cmdBeginRendering: label='{}' colors=0 depth=0 renderArea=({},{} {}x{})",
                               renderer.currentPassLabel(),
                               outSpec.renderArea.offset.x,
                               outSpec.renderArea.offset.y,
                               outSpec.renderArea.extent.width,
                               outSpec.renderArea.extent.height);
      }
      return false;
    }

    // Instrumentation: log attachments and render area
    uint64_t firstColorHandle = 0;
    auto firstColorFmt = enumOrUnderlying(vk::Format{}, 16);
    uint32_t firstColorW = 0, firstColorH = 0;
    uint64_t depthHandle = 0;
    auto depthFmt = enumOrUnderlying(vk::Format{}, 16);
    uint32_t depthW = 0, depthH = 0;
    if (VLOG_IS_ON(3)) {
      if (!batch.pass.colorAttachments.empty()) {
        const auto& a = batch.pass.colorAttachments.front();
        if (a.handle.valid() && a.handle.backend == RenderBackend::Vulkan) {
          auto* tex = reinterpret_cast<ZVulkanTexture*>(a.handle.id);
          if (tex) {
            firstColorHandle = reinterpret_cast<uint64_t>(tex);
            firstColorFmt = enumOrUnderlying(tex->format(), 16);
            firstColorW = tex->width();
            firstColorH = tex->height();
          }
        }
      }
      if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid() &&
          batch.pass.depthAttachment->handle.backend == RenderBackend::Vulkan) {
        auto* dtex = reinterpret_cast<ZVulkanTexture*>(batch.pass.depthAttachment->handle.id);
        if (dtex) {
          depthHandle = reinterpret_cast<uint64_t>(dtex);
          depthFmt = enumOrUnderlying(dtex->format(), 16);
          depthW = dtex->width();
          depthH = dtex->height();
        }
      }
      std::string passLabel = std::string(renderer.currentPassLabel());
      if (passLabel.empty() && m_passScope.active) {
        passLabel = m_passScope.label;
      }
      if (passLabel.empty()) {
        passLabel = "<unnamed>";
      }
      outSpec.debugLabel = passLabel;
      VLOG(3) << fmt::format(
        "VK cmdBeginRendering: label='{}' colors={} c0=0x{:x} fmt={} {}x{} depth={} d=0x{:x} fmt={} {}x{} renderArea=({},{} {}x{})",
        passLabel,
        outSpec.colorAttachments.size(),
        firstColorHandle,
        firstColorFmt,
        firstColorW,
        firstColorH,
        static_cast<bool>(outSpec.depthStencilAttachment.has_value()),
        depthHandle,
        depthFmt,
        depthW,
        depthH,
        outSpec.renderArea.offset.x,
        outSpec.renderArea.offset.y,
        outSpec.renderArea.extent.width,
        outSpec.renderArea.extent.height);
    }

    recorder.beginRenderingSegment(outSpec);

    if (m_activeFrame) {
      m_activeFrame->renderingSegmentsBegan++;
      for (const auto& a : batch.pass.colorAttachments) {
        const LoadOp effective = forceLoadOps ? LoadOp::Load : a.loadOp;
        if (effective == LoadOp::Clear) {
          m_activeFrame->attachmentClears++;
        } else {
          m_activeFrame->attachmentLoads++;
        }
      }
      if (batch.pass.depthAttachment) {
        const LoadOp effective = forceLoadOps ? LoadOp::Load : batch.pass.depthAttachment->loadOp;
        if (effective == LoadOp::Clear) {
          m_activeFrame->attachmentClears++;
        } else {
          m_activeFrame->attachmentLoads++;
        }
      }
    }
    return true;
  };

  size_t batchIndex = 0;
  std::optional<SegmentKey> currentKey;
  bool segmentOpen = false;
  std::optional<ZVulkanPipelineCommandRecorder::RenderingSegmentSpec> openSpec{};
  ZVulkanPipelineCommandRecorder recorder(cmd);

  auto endRenderingSegmentIfOpen = [&]() {
    if (!segmentOpen) {
      return;
    }
    if (openSpec) {
      recorder.endRenderingSegment(*openSpec);
      openSpec.reset();
    }
    segmentOpen = false;
    currentKey.reset();
  };

  // Ensure we never leave a Vulkan recording session with an active dynamic-rendering
  // instance. Cancellation/exception paths can unwind out of a pipeline context's
  // record() call; without this guard we would hit:
  //   vkEndCommandBuffer(): invalid inside active vkCmdBeginRendering instance.
  auto segmentCloseGuard = folly::makeGuard([&]() {
    endRenderingSegmentIfOpen();
    if (m_activeFrame) {
      m_activeFrame->activeSegmentFormats.reset();
    }
  });

  auto openSegmentForBatch = [&](const RenderBatch& batch, const SegmentKey& key, bool forceLoadOps) -> bool {
    ZVulkanPipelineCommandRecorder::RenderingSegmentSpec spec;
    if (!beginSegmentForBatch(batch, recorder, spec, forceLoadOps)) {
      return false;
    }
    segmentOpen = true;
    currentKey = key;
    openSpec = spec;
    // Track active segment formats for validation.
    if (m_activeFrame) {
      m_activeFrame->activeSegmentFormats = vulkan::extractAttachmentFormats(batch);
    }
    return true;
  };

  auto timeContext = [&](double& accumulator, auto&& fn) {
    if (!profilePipelineContexts) {
      fn();
      return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    accumulator += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };

  for (const auto& batch : state.batches) {
    const auto geom = describeGeometry(batch.geometry);
    // Per-batch logs are extremely verbose for large scenes; keep them behind
    // VLOG(2) so --v=1 remains usable for performance investigations.
    VLOG(2) << fmt::format("VK batch[{}]: geom={}, colors={}, depth={} viewport=({},{} {}x{}) clip={} planes={}",
                           batchIndex++,
                           geom,
                           batch.pass.colorAttachments.size(),
                           batch.pass.depthAttachment.has_value(),
                           static_cast<int>(batch.pass.viewport.origin.x),
                           static_cast<int>(batch.pass.viewport.origin.y),
                           static_cast<int>(batch.pass.viewport.extent.x),
                           static_cast<int>(batch.pass.viewport.extent.y),
                           (batch.clipPlanes.captured && batch.clipPlanes.enabled) ? 1 : 0,
                           batch.clipPlanes.captured ? batch.clipPlanes.planeCount : 0u);
    if (VLOG_IS_ON(3) && batch.clipPlanes.captured && batch.clipPlanes.enabled && batch.clipPlanes.planeCount > 0u) {
      const auto& p0 = batch.clipPlanes.planes[0];
      VLOG(3) << fmt::format("VK batch clip planes: count={} plane0=({:.3f},{:.3f},{:.3f},{:.3f})",
                             batch.clipPlanes.planeCount,
                             p0.x,
                             p0.y,
                             p0.z,
                             p0.w);
    }
    if (batch.pass.kind == BackendPassDesc::Kind::Raster) {
      CHECK(batch.pass.viewport.extent.x > 0.0f && batch.pass.viewport.extent.y > 0.0f)
        << "Vulkan batch missing viewport extent: geom=" << geom;
      if (batch.pass.enableScissor) {
        CHECK(batch.pass.scissorRect.z > 0.0f && batch.pass.scissorRect.w > 0.0f)
          << "Vulkan batch enables scissor but has empty scissorRect: geom=" << geom;
      }
    }

    const auto vkViewport = vulkan::toVkViewport(batch.pass.viewport);
    const auto vkScissor = vulkan::toVkScissor(batch.pass);

    const bool isRaster = (batch.pass.kind == BackendPassDesc::Kind::Raster);
    const bool isCompute = (batch.pass.kind == BackendPassDesc::Kind::Compute);
    std::optional<SegmentKey> segmentKey;
    if (isRaster) {
      segmentKey = buildKey(batch, vkScissor);
    }

    // Segment boundaries must be established before emitting any barriers:
    // Vulkan forbids vkCmdPipelineBarrier2 (image/buffer barriers) inside a
    // dynamic-rendering instance unless dynamicRenderingLocalRead is enabled.
    if (isCompute) {
      endRenderingSegmentIfOpen();
    } else if (segmentOpen && (!currentKey || !segmentKey || *currentKey != *segmentKey)) {
      endRenderingSegmentIfOpen();
    }

    // If we had to temporarily close a segment to emit barriers for this batch
    // (layout transitions / buffer hazards), reopening must *not* re-clear
    // attachments. Force all attachment loadOps to LOAD on that restart.
    bool forceLoadOps = false;

    // Ensure any external image uses referenced by this batch are transitioned
    // to the required layouts. This is driven by explicit pass metadata
    // (BackendPassDesc::externalImageUses), not by payload/label heuristics.
    auto isPassAttachment = [&](const AttachmentHandle& handle) {
      if (!handle.valid()) {
        return false;
      }
      for (const auto& att : batch.pass.colorAttachments) {
        if (att.handle.id == handle.id) {
          return true;
        }
      }
      if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.id == handle.id) {
        return true;
      }
      if (batch.pass.resolveAttachment && batch.pass.resolveAttachment->handle.id == handle.id) {
        return true;
      }
      return false;
    };

    auto ensureExternalImageUse = [&](const ExternalImageUseDesc& use) {
      if (!use.handle.valid()) {
        return;
      }
      // Invariant: do not sample/copy from images that are also bound as attachments
      // in the same pass; this would create a read-while-write feedback loop.
      CHECK(!isPassAttachment(use.handle)) << "External image use references an active attachment (read-while-write)";

      auto& texture = vulkan::textureFromHandle(use.handle, device(), "renderer external image use");
      (void)device().residencyManager().ensureResidentIfManaged(&texture, "renderer_external_image_use");
      pinTextureForActiveSubmission(&texture);
      CHECK(texture.resident()) << "Vulkan external image backing is not resident before recording: handle=0x"
                                << std::hex << use.handle.id << std::dec << " kind=" << static_cast<int>(use.kind)
                                << " pass='"
                                << (renderer.currentPassLabel().empty() ? "<unlabeled-pass>"
                                                                        : renderer.currentPassLabel())
                                << "'";

      vk::ImageLayout desiredLayout = vk::ImageLayout::eGeneral;
      vk::ImageAspectFlags transitionAspect{};
      bool updateDescriptorLayout = false;

      const auto useInfo = vulkan::resolveExternalImageUse(use.kind, use.aspectHint);
      desiredLayout = useInfo.layout;
      transitionAspect = useInfo.transitionAspect;
      updateDescriptorLayout = useInfo.updateDescriptorLayout;

      const bool needsTransition = (texture.layout() != desiredLayout);
      if (needsTransition && VLOG_IS_ON(2)) {
        std::string passLabel = std::string(renderer.currentPassLabel());
        if (passLabel.empty()) {
          passLabel = "<unlabeled-pass>";
        }
        VLOG(2) << fmt::format("ensureExternalImageUse('{}'): handle=0x{:x} kind={} {} -> {} fmt={}",
                               passLabel,
                               use.handle.id,
                               static_cast<int>(use.kind),
                               enumOrUnderlying(texture.layout(), 16),
                               enumOrUnderlying(desiredLayout, 16),
                               enumOrUnderlying(texture.format(), 16));
      }

      if (auto* be = Z3DRendererVulkanBackend::current()) {
        be->notifyLayoutTransition(!needsTransition);
      }

      if (needsTransition) {
        texture.transitionLayout(cmd, texture.layout(), desiredLayout, transitionAspect);
      }
      if (updateDescriptorLayout) {
        texture.setDescriptorLayout(desiredLayout);
      }
    };

    auto externalBufferUseNeedsBarrier = [&](const ExternalBufferUseDesc& use) -> bool {
      if (!use.handle.valid()) {
        return false;
      }

      auto& buffer = vulkan::bufferFromHandle(use.handle, device(), "renderer external buffer use");
      const auto useInfo = vulkan::resolveExternalBufferUse(use.kind, buffer.usage());
      const vk::PipelineStageFlags2 desiredStage = useInfo.stage;
      const vk::AccessFlags2 desiredAccess = useInfo.access;

      const uint64_t bufferKey =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(buffer.buffer())));
      const auto it = m_externalBufferUseStates.find(bufferKey);
      const ExternalBufferUseState previous =
        (it == m_externalBufferUseStates.end()) ? ExternalBufferUseState{} : it->second;

      auto hasWrite = [](vk::AccessFlags2 access) {
        return static_cast<bool>(access & (vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
                                           vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eHostWrite));
      };

      return (previous.stage != desiredStage || previous.access != desiredAccess) &&
             (hasWrite(previous.access) || hasWrite(desiredAccess));
    };

    // When a segment is already open (same attachments), avoid emitting any
    // barriers inside dynamic rendering by restarting the segment if any
    // external use needs a transition/barrier.
    if (segmentOpen) {
      bool needsBarrierOutsideSegment = false;
      for (const auto& use : batch.pass.externalImageUses) {
        if (!use.handle.valid()) {
          continue;
        }
        auto& texture = vulkan::textureFromHandle(use.handle, device(), "renderer external image use");
        const auto useInfo = vulkan::resolveExternalImageUse(use.kind, use.aspectHint);
        if (texture.layout() != useInfo.layout) {
          needsBarrierOutsideSegment = true;
          break;
        }
      }
      if (!needsBarrierOutsideSegment) {
        for (const auto& use : batch.pass.externalBufferUses) {
          if (externalBufferUseNeedsBarrier(use)) {
            needsBarrierOutsideSegment = true;
            break;
          }
        }
      }
      if (needsBarrierOutsideSegment) {
        endRenderingSegmentIfOpen();
        forceLoadOps = true;
      }
    }

    for (const auto& use : batch.pass.externalImageUses) {
      ensureExternalImageUse(use);
    }

    // Ensure any external buffer uses referenced by this batch are synchronized
    // for the required access masks. Like external images, this is driven by
    // explicit metadata (BackendPassDesc::externalBufferUses).
    auto ensureExternalBufferUse = [&](const ExternalBufferUseDesc& use) {
      if (!use.handle.valid()) {
        return;
      }

      auto& buffer = vulkan::bufferFromHandle(use.handle, device(), "renderer external buffer use");

      const auto useInfo = vulkan::resolveExternalBufferUse(use.kind, buffer.usage());
      const vk::PipelineStageFlags2 desiredStage = useInfo.stage;
      const vk::AccessFlags2 desiredAccess = useInfo.access;

      const uint64_t bufferKey =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(buffer.buffer())));
      const auto it = m_externalBufferUseStates.find(bufferKey);
      const ExternalBufferUseState previous =
        (it == m_externalBufferUseStates.end()) ? ExternalBufferUseState{} : it->second;

      auto hasWrite = [](vk::AccessFlags2 access) {
        return static_cast<bool>(access & (vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
                                           vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eHostWrite));
      };

      const bool needsBarrier = (previous.stage != desiredStage || previous.access != desiredAccess) &&
                                (hasWrite(previous.access) || hasWrite(desiredAccess));

      if (auto* be = Z3DRendererVulkanBackend::current()) {
        be->notifyBufferBarrier(!needsBarrier);
      }

      if (needsBarrier) {
        vk::BufferMemoryBarrier2 barrier{.srcStageMask = previous.stage,
                                         .srcAccessMask = previous.access,
                                         .dstStageMask = desiredStage,
                                         .dstAccessMask = desiredAccess,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .buffer = buffer.buffer(),
                                         .offset = 0,
                                         .size = VK_WHOLE_SIZE};
        vk::DependencyInfo dep{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier};
        cmd.pipelineBarrier2(dep);
      }

      m_externalBufferUseStates[bufferKey] = ExternalBufferUseState{desiredStage, desiredAccess};
    };

    for (const auto& use : batch.pass.externalBufferUses) {
      ensureExternalBufferUse(use);
    }

    if (isCompute) {
      endRenderingSegmentIfOpen();
    } else if (isRaster && !segmentOpen) {
      CHECK(segmentKey.has_value());
      if (!openSegmentForBatch(batch, *segmentKey, forceLoadOps)) {
        continue;
      }
    }

    bool handled = false;
    if (const auto* line = std::get_if<LinePayload>(&batch.geometry)) {
      CHECK(m_lineContext != nullptr) << "Line pipeline context missing";
      timeContext(cpuTimes.lineMs, [&]() {
        m_lineContext->record(renderer, batch, *line, vkViewport, vkScissor, cmd);
      });
      handled = true;
    }
    if (!handled) {
      if (const auto* mesh = std::get_if<MeshPayload>(&batch.geometry)) {
        CHECK(m_meshContext != nullptr) << "Mesh pipeline context missing";
        timeContext(cpuTimes.meshMs, [&]() {
          m_meshContext->record(renderer, batch, *mesh, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* sphere = std::get_if<SpherePayload>(&batch.geometry)) {
        CHECK(m_sphereContext != nullptr) << "Sphere pipeline context missing";
        timeContext(cpuTimes.sphereMs, [&]() {
          m_sphereContext->record(renderer, batch, *sphere, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* background = std::get_if<BackgroundPayload>(&batch.geometry)) {
        CHECK(m_backgroundContext != nullptr) << "Background pipeline context missing";
        timeContext(cpuTimes.backgroundMs, [&]() {
          m_backgroundContext->record(renderer, batch, *background, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* slice = std::get_if<ImgSlicePayload>(&batch.geometry)) {
        CHECK(m_imgSliceContext != nullptr) << "ImgSlice pipeline context missing";
        timeContext(cpuTimes.imgSliceMs, [&]() {
          m_imgSliceContext->record(renderer, batch, *slice, vkViewport, vkScissor, cmd);
        });
        // If slices just completed a progressive round, finalize it now.
        if (auto fin = m_imgSliceContext->takePendingFinalization()) {
          if (fin->streamKey != 0) {
            finalizeImgSliceRoundByKey(renderer, fin->streamKey, fin->eye, fin->lastRound, fin->channelCount);
          }
        }
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* font = std::get_if<FontPayload>(&batch.geometry)) {
        CHECK(m_fontContext != nullptr) << "Font pipeline context missing";
        timeContext(cpuTimes.fontMs, [&]() {
          m_fontContext->record(renderer, batch, *font, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* textureCopy = std::get_if<TextureCopyPayload>(&batch.geometry)) {
        CHECK(m_textureCopyContext != nullptr) << "TextureCopy pipeline context missing";
        timeContext(cpuTimes.textureCopyMs, [&]() {
          m_textureCopyContext->record(renderer, batch, *textureCopy, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* textureBlend = std::get_if<TextureBlendPayload>(&batch.geometry)) {
        CHECK(m_textureBlendContext != nullptr) << "TextureBlend pipeline context missing";
        timeContext(cpuTimes.textureBlendMs, [&]() {
          m_textureBlendContext->record(renderer, batch, *textureBlend, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* textureGlow = std::get_if<TextureGlowPayload>(&batch.geometry)) {
        CHECK(m_textureGlowContext != nullptr) << "TextureGlow pipeline context missing";
        timeContext(cpuTimes.textureGlowMs, [&]() {
          m_textureGlowContext->record(renderer, batch, *textureGlow, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* dualPeel = std::get_if<TextureDualPeelPayload>(&batch.geometry)) {
        CHECK(m_textureDualPeelContext != nullptr) << "TextureDualPeel pipeline context missing";
        timeContext(cpuTimes.textureDualPeelMs, [&]() {
          m_textureDualPeelContext->record(renderer, batch, *dualPeel, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* weightedAverage = std::get_if<TextureWeightedAveragePayload>(&batch.geometry)) {
        CHECK(m_textureWeightedAverageContext != nullptr) << "TextureWeightedAverage pipeline context missing";
        timeContext(cpuTimes.textureWeightedAverageMs, [&]() {
          m_textureWeightedAverageContext->record(renderer, batch, *weightedAverage, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* weightedBlended = std::get_if<TextureWeightedBlendedPayload>(&batch.geometry)) {
        CHECK(m_textureWeightedBlendedContext != nullptr) << "TextureWeightedBlended pipeline context missing";
        timeContext(cpuTimes.textureWeightedBlendedMs, [&]() {
          m_textureWeightedBlendedContext->record(renderer, batch, *weightedBlended, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* ppllResolve = std::get_if<TexturePPLLResolvePayload>(&batch.geometry)) {
        CHECK(m_texturePPLLContext != nullptr) << "TexturePPLL pipeline context missing";
        timeContext(cpuTimes.texturePPLLResolveMs, [&]() {
          m_texturePPLLContext->record(renderer, batch, *ppllResolve, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* ellipsoid = std::get_if<EllipsoidPayload>(&batch.geometry)) {
        CHECK(m_ellipsoidContext != nullptr) << "Ellipsoid pipeline context missing";
        timeContext(cpuTimes.ellipsoidMs, [&]() {
          m_ellipsoidContext->record(renderer, batch, *ellipsoid, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* cone = std::get_if<ConePayload>(&batch.geometry)) {
        CHECK(m_coneContext != nullptr) << "Cone pipeline context missing";
        timeContext(cpuTimes.coneMs, [&]() {
          m_coneContext->record(renderer, batch, *cone, vkViewport, vkScissor, cmd);
        });
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* raycaster = std::get_if<ImgRaycasterPayload>(&batch.geometry)) {
        if (raycaster->image) {
          CHECK(m_imgRaycasterContext != nullptr) << "ImgRaycaster pipeline context missing";
          timeContext(cpuTimes.imgRaycasterMs, [&]() {
            m_imgRaycasterContext->record(renderer, batch, *raycaster, vkViewport, vkScissor, cmd);
          });
          // If the raycaster just completed a progressive round, finalize it now.
          if (auto fin = m_imgRaycasterContext->takePendingFinalization()) {
            if (fin->streamKey != 0) {
              finalizeImgRaycasterRoundByKey(renderer, fin->streamKey, fin->eye, fin->lastRound, fin->channelCount);
            }
          }
          handled = true;
        }
      }
    }
    if (!handled) {
      cmd.setViewport(0, vkViewport);
      cmd.setScissor(0, vkScissor);
      CHECK(false) << "Vulkan backend has not yet implemented draw emission for geometry type '"
                   << describeGeometry(batch.geometry) << "'.";
    }
  }

  if (profilePipelineContexts) {
    const std::string passLabel = m_passScope.active ? m_passScope.label : std::string("processBatches");
    auto emit = [&](std::string_view ctx, double ms) {
      if (ms <= 0.0) {
        return;
      }
      recordCpuScope(fmt::format("{} ctx={}", passLabel, ctx), ms);
    };
    emit("line", cpuTimes.lineMs);
    emit("mesh", cpuTimes.meshMs);
    emit("sphere", cpuTimes.sphereMs);
    emit("ellipsoid", cpuTimes.ellipsoidMs);
    emit("cone", cpuTimes.coneMs);
    emit("background", cpuTimes.backgroundMs);
    emit("img_slice", cpuTimes.imgSliceMs);
    emit("img_raycaster", cpuTimes.imgRaycasterMs);
    emit("font", cpuTimes.fontMs);
    emit("tex_copy", cpuTimes.textureCopyMs);
    emit("tex_blend", cpuTimes.textureBlendMs);
    emit("tex_glow", cpuTimes.textureGlowMs);
    emit("tex_ddp", cpuTimes.textureDualPeelMs);
    emit("tex_wa", cpuTimes.textureWeightedAverageMs);
    emit("tex_wb", cpuTimes.textureWeightedBlendedMs);
    emit("tex_ppll_resolve", cpuTimes.texturePPLLResolveMs);
  }

  // Recording-session sync point:
  // - no active dynamic rendering segment
  // - scheduled upload->static copies flushed
  endRenderingSegmentIfOpen();
  flushScheduledCopies(cmd);
  if (m_activeFrame) {
    m_activeFrame->activeSegmentFormats.reset();
  }
}

void Z3DRendererVulkanBackend::hintNextUniformArenaMinCapacity(size_t bytes)
{
  m_nextUniformMinCapacity = std::max(m_nextUniformMinCapacity, bytes);
  m_uniformMinCapacityCarry = std::max(m_uniformMinCapacityCarry, bytes);
}

size_t Z3DRendererVulkanBackend::uniformAlignmentForEstimates() const
{
  return uniformAlignment();
}

size_t Z3DRendererVulkanBackend::estimateFrameUniformOverheadBytes()
{
  // beginRender() always suballocates two lighting UBO slices (scene lighting +
  // picking lighting), plus one per-eye frame transform UBO slice.
  //
  // Provide a conservative (alignment-rounded) estimate so higher-level
  // schedulers can size the arena before opening a Vulkan frame.
  ensureDevice();
  const size_t align = uniformAlignment();
  const size_t szLighting = vulkan::alignUp(sizeof(LightingUBOStd140), align);
  const size_t szFrameTransforms = vulkan::alignUp(sizeof(FrameTransformsUBOStd140), align);
  return 2u * szLighting + 3u * szFrameTransforms;
}

size_t Z3DRendererVulkanBackend::estimateAdditionalUniformBytesForBatches(const RendererCPUState& state)
{
  ensureDevice();
  const size_t align = uniformAlignment();

  size_t total = 0;
  for (const auto& b : state.batches) {
    total += vulkan::estimateAdditionalUniformArenaBytesForBatch(b, align);
  }

  // Conservatively align to the device's dynamic UBO alignment.
  return vulkan::alignUp(total, align);
}

bool Z3DRendererVulkanBackend::supportsCommandLists() const
{
  return true;
}

RendererFrameState::ActiveSurface
Z3DRendererVulkanBackend::describeSurfaceFromLease(const Z3DScratchResourcePool::RenderTargetLease& lease)
{
  RendererFrameState::ActiveSurface surface;
  if (!lease) {
    return surface;
  }

  if (lease.backend != RenderBackend::Vulkan || !lease.hasVulkanImage()) {
    return surface;
  }

  const auto& descriptor = lease.descriptor;

  for (const auto& attachment : descriptor.attachments) {
    if (attachment.kind == ScratchAttachmentKind::Color) {
      if (auto* texture = lease.colorAttachment(attachment.index)) {
        AttachmentDesc desc;
        desc.handle.backend = RenderBackend::Vulkan;
        desc.handle.id = reinterpret_cast<uint64_t>(texture);
        desc.handle.index = attachment.index;
        desc.finalUse = AttachmentFinalUse::RenderTarget;
        surface.colorAttachments.push_back(desc);
      }
    } else if (attachment.kind == ScratchAttachmentKind::Depth) {
      if (auto* texture = lease.depthAttachmentTexture()) {
        AttachmentDesc desc;
        desc.handle.backend = RenderBackend::Vulkan;
        desc.handle.id = reinterpret_cast<uint64_t>(texture);
        desc.handle.index = attachment.index;
        desc.finalUse = AttachmentFinalUse::RenderTarget;
        surface.depthAttachment = desc;
      }
    }
  }

  return surface;
}

ZVulkanDevice& Z3DRendererVulkanBackend::device()
{
  ensureDevice();
  CHECK(m_sharedDevice != nullptr);
  return *m_sharedDevice;
}

const ZVulkanDevice& Z3DRendererVulkanBackend::device() const
{
  CHECK(m_sharedDevice != nullptr);
  return *m_sharedDevice;
}

void Z3DRendererVulkanBackend::ensureDefaultPlaceholders()
{
  ensureDevice();
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureDefaultPlaceholders";
  if (!m_defaultPlaceholder2D) {
    // 1x1 RGBA8 white texture for placeholder sampling
    m_defaultPlaceholder2D =
      m_sharedDevice->createTexture(1,
                                    1,
                                    vk::Format::eR8G8B8A8Unorm,
                                    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                    vk::MemoryPropertyFlagBits::eDeviceLocal);
    uint32_t pixel = 0xffffffffu;
    m_defaultPlaceholder2D->uploadData(&pixel, sizeof(pixel), vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  if (!m_defaultPlaceholder2DArray) {
    auto info =
      ZVulkanTexture::CreateInfo::make2DArray(1,
                                              1,
                                              1,
                                              vk::Format::eR8G8B8A8Unorm,
                                              vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                              vk::MemoryPropertyFlagBits::eDeviceLocal,
                                              1u,
                                              true,
                                              vk::ImageLayout::eShaderReadOnlyOptimal);
    m_defaultPlaceholder2DArray = m_sharedDevice->createTexture(info);
    uint32_t pixel = 0xffffffffu;
    m_defaultPlaceholder2DArray->uploadData(&pixel, sizeof(pixel), vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  if (!m_defaultPlaceholder3D) {
    auto info =
      ZVulkanTexture::CreateInfo::make3D(1,
                                         1,
                                         1,
                                         vk::Format::eR8Unorm,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal,
                                         1u,
                                         true,
                                         vk::ImageLayout::eShaderReadOnlyOptimal);
    m_defaultPlaceholder3D = m_sharedDevice->createTexture(info);
    const uint8_t zero = 0u;
    m_defaultPlaceholder3D->uploadData(&zero, sizeof(zero), vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  if (!m_defaultPlaceholderU2D) {
    auto info =
      ZVulkanTexture::CreateInfo::make2D(1,
                                         1,
                                         vk::Format::eR32G32B32A32Uint,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal,
                                         1u,
                                         true,
                                         vk::ImageLayout::eShaderReadOnlyOptimal);
    m_defaultPlaceholderU2D = m_sharedDevice->createTexture(info);
    std::array<uint32_t, 4> zero{0u, 0u, 0u, 0u};
    m_defaultPlaceholderU2D->uploadData(zero.data(), sizeof(zero), vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  if (!m_defaultPlaceholderU3D) {
    auto info =
      ZVulkanTexture::CreateInfo::make3D(1,
                                         1,
                                         1,
                                         vk::Format::eR32G32B32A32Uint,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal,
                                         1u,
                                         true,
                                         vk::ImageLayout::eShaderReadOnlyOptimal);
    m_defaultPlaceholderU3D = m_sharedDevice->createTexture(info);
    std::array<uint32_t, 4> zero{0u, 0u, 0u, 0u};
    m_defaultPlaceholderU3D->uploadData(zero.data(), sizeof(zero), vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  if (!m_defaultPlaceholderStorageBuffer) {
    // Tiny 4-byte storage buffer used to satisfy unused SSBO bindings.
    m_defaultPlaceholderStorageBuffer = m_sharedDevice->createBuffer(sizeof(uint32_t),
                                                                     vk::BufferUsageFlagBits::eStorageBuffer,
                                                                     vk::MemoryPropertyFlagBits::eHostVisible |
                                                                       vk::MemoryPropertyFlagBits::eHostCoherent);
    const uint32_t zero = 0u;
    m_defaultPlaceholderStorageBuffer->copyData(&zero, sizeof(zero));
  }
  ensureSharedSamplers();
}

ZVulkanTexture& Z3DRendererVulkanBackend::defaultPlaceholderTexture2D()
{
  ensureDefaultPlaceholders();
  return *m_defaultPlaceholder2D;
}

void Z3DRendererVulkanBackend::ensureSharedDescriptorLayouts()
{
  ensureDevice();
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureSharedDescriptorLayouts";

  auto& vkDevice = m_sharedDevice->context().device();

  const auto& caps = m_sharedDevice->context().effectiveBindlessSampledImageCapacities();
  const uint32_t cap2D = caps.texture2D;
  const uint32_t cap2DArray = caps.texture2DArray;
  const uint32_t cap3D = caps.texture3D;
  const uint32_t capU2D = caps.uTexture2D;
  const uint32_t capU3D = caps.uTexture3D;

  if (!m_sharedDescriptorLayouts.bindlessSampledImages) {
    const bool useUpdateAfterBind = m_sharedDevice->context().supportsDescriptorIndexingSampledImageUpdateAfterBind();

    const vk::ShaderStageFlags fragStage = vk::ShaderStageFlagBits::eFragment;
    const vk::ShaderStageFlags compStage = vk::ShaderStageFlagBits::eCompute;
    ensureSharedSamplers();
    CHECK(m_defaultSampler.has_value() && m_nearestClampSampler.has_value() && m_linearBorderZero3DSampler.has_value())
      << "Bindless set layout requires shared samplers to be initialized";
    const std::array<vk::Sampler, 1> linearSamplers{**m_defaultSampler};
    const std::array<vk::Sampler, 1> nearestSamplers{**m_nearestClampSampler};
    const std::array<vk::Sampler, 1> linearBorderZero3DSamplers{**m_linearBorderZero3DSampler};
    std::array<vk::DescriptorSetLayoutBinding, 8> bindings{
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessTexture2D,
                                     .descriptorType = vk::DescriptorType::eSampledImage,
                                     .descriptorCount = cap2D,
                                     .stageFlags = fragStage},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessTexture2DArray,
                                     .descriptorType = vk::DescriptorType::eSampledImage,
                                     .descriptorCount = cap2DArray,
                                     .stageFlags = fragStage},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessTexture3D,
                                     .descriptorType = vk::DescriptorType::eSampledImage,
                                     .descriptorCount = cap3D,
                                     .stageFlags = fragStage},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessUTexture2D,
                                     .descriptorType = vk::DescriptorType::eSampledImage,
                                     .descriptorCount = capU2D,
                                     .stageFlags = compStage},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessUTexture3D,
                                     .descriptorType = vk::DescriptorType::eSampledImage,
                                     .descriptorCount = capU3D,
                                     .stageFlags = fragStage},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessSamplerLinearClamp,
                                     .descriptorType = vk::DescriptorType::eSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = fragStage,
                                     .pImmutableSamplers = linearSamplers.data()},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessSamplerNearestClamp,
                                     .descriptorType = vk::DescriptorType::eSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = fragStage | compStage,
                                     .pImmutableSamplers = nearestSamplers.data()},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessSamplerLinearBorderZero3D,
                                     .descriptorType = vk::DescriptorType::eSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = fragStage,
                                     .pImmutableSamplers = linearBorderZero3DSamplers.data()},
    };

    // Enable descriptor indexing on these bindings:
    // - Partially bound: not all array elements must be populated.
    // - Update-after-bind: when enabled, large bindless arrays are accounted
    //   against descriptor indexing limits.
    std::array<vk::DescriptorBindingFlags, 8> bindingFlags{};
    for (size_t i = 0; i < 5; ++i) {
      vk::DescriptorBindingFlags f = vk::DescriptorBindingFlagBits::ePartiallyBound;
      if (useUpdateAfterBind) {
        f |= vk::DescriptorBindingFlagBits::eUpdateAfterBind;
      }
      bindingFlags[i] = f;
    }
    vk::DescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
      .bindingCount = static_cast<uint32_t>(bindingFlags.size()),
      .pBindingFlags = bindingFlags.data(),
    };

    vk::DescriptorSetLayoutCreateFlags layoutFlags{};
    if (useUpdateAfterBind) {
      layoutFlags |= vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
    }
    vk::DescriptorSetLayoutCreateInfo info{
      .pNext = &flagsInfo,
      .flags = layoutFlags,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
    };
    m_sharedDescriptorLayouts.bindlessSampledImages.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.lighting) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                           .descriptorCount = 1,
                                           .stageFlags =
                                             vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_sharedDescriptorLayouts.lighting.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.transforms) {
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 2,
                                     .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_sharedDescriptorLayouts.transforms.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.oitParams) {
    // OIT descriptor set (set=3):
    // - binding 0: OIT params SSBO (viewport, pixelCount) for exact per-pixel fragment list (PPLL/A-buffer)
    // - binding 1: DDP "changed" flag SSBO (preserved binding index for existing shaders)
    // - bindings 2..5: PPLL buffers (counts/offsets/cursors/fragments)
    std::array<vk::DescriptorSetLayoutBinding, 6> bindings{
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITParams,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITDDPFlag,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITPPLLCounts,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITPPLLOffsets,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITPPLLCursors,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITPPLLFragments,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
    };
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_sharedDescriptorLayouts.oitParams.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.empty) {
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 0, .pBindings = nullptr};
    m_sharedDescriptorLayouts.empty.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.imgRaySetup) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_sharedDescriptorLayouts.imgRaySetup.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.imgIndices) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_sharedDescriptorLayouts.imgIndices.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.imgPageData) {
    vk::DescriptorSetLayoutBinding binding{.binding = 2,
                                           .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_sharedDescriptorLayouts.imgPageData.emplace(vkDevice, info);
  }
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::bindlessSampledImageDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.bindlessSampledImages ? **m_sharedDescriptorLayouts.bindlessSampledImages
                                                         : vk::DescriptorSetLayout{};
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::lightingDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.lighting ? **m_sharedDescriptorLayouts.lighting : vk::DescriptorSetLayout{};
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::transformDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.transforms ? **m_sharedDescriptorLayouts.transforms : vk::DescriptorSetLayout{};
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::oitDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.oitParams ? **m_sharedDescriptorLayouts.oitParams : vk::DescriptorSetLayout{};
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::emptyDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.empty ? **m_sharedDescriptorLayouts.empty : vk::DescriptorSetLayout{};
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::imgRaySetupDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.imgRaySetup ? **m_sharedDescriptorLayouts.imgRaySetup : vk::DescriptorSetLayout{};
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::imgIndicesDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.imgIndices ? **m_sharedDescriptorLayouts.imgIndices : vk::DescriptorSetLayout{};
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::imgPageDataDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.imgPageData ? **m_sharedDescriptorLayouts.imgPageData : vk::DescriptorSetLayout{};
}

vk::DescriptorSet Z3DRendererVulkanBackend::bindlessSampledImageDescriptorSet() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(m_activeFrame->bindlessSampledImages != nullptr) << "Bindless descriptor set missing on active frame-slot";
  return m_activeFrame->bindlessSampledImages->descriptorSet();
}

uint64_t Z3DRendererVulkanBackend::bindlessSampledImageDescriptorSetGeneration() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return 0;
  }
  CHECK(m_activeFrame->bindlessSampledImages != nullptr) << "Bindless descriptor set missing on active frame-slot";
  return m_activeFrame->bindlessSampledImages->generation();
}

vk::DescriptorSet Z3DRendererVulkanBackend::sharedEmptyDescriptorSet() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(m_activeFrame->sharedEmpty != nullptr) << "Shared empty descriptor set missing on active frame-slot";
  return m_activeFrame->sharedEmpty->descriptorSet();
}

vk::DescriptorSet Z3DRendererVulkanBackend::sharedLightingDescriptorSet() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(m_activeFrame->sharedLighting != nullptr) << "Shared lighting descriptor set missing on active frame-slot";
  return m_activeFrame->sharedLighting->descriptorSet();
}

uint64_t Z3DRendererVulkanBackend::sharedLightingDescriptorSetGeneration() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return 0;
  }
  CHECK(m_activeFrame->sharedLighting != nullptr) << "Shared lighting descriptor set missing on active frame-slot";
  return m_activeFrame->sharedLighting->generation();
}

vk::DescriptorSet Z3DRendererVulkanBackend::sharedTransformsDescriptorSetUniform() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(m_activeFrame->sharedTransformsUniform != nullptr)
    << "Shared uniform transforms descriptor set missing on active frame-slot";
  return m_activeFrame->sharedTransformsUniform->descriptorSet();
}

uint64_t Z3DRendererVulkanBackend::sharedTransformsDescriptorSetUniformGeneration() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return 0;
  }
  CHECK(m_activeFrame->sharedTransformsUniform != nullptr)
    << "Shared uniform transforms descriptor set missing on active frame-slot";
  return m_activeFrame->sharedTransformsUniform->generation();
}

vk::DescriptorSet Z3DRendererVulkanBackend::sharedTransformsDescriptorSetPersistent() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(m_activeFrame->sharedTransformsPersistent != nullptr)
    << "Shared persistent transforms descriptor set missing on active frame-slot";
  return m_activeFrame->sharedTransformsPersistent->descriptorSet();
}

uint64_t Z3DRendererVulkanBackend::sharedTransformsDescriptorSetPersistentGeneration() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return 0;
  }
  CHECK(m_activeFrame->sharedTransformsPersistent != nullptr)
    << "Shared persistent transforms descriptor set missing on active frame-slot";
  return m_activeFrame->sharedTransformsPersistent->generation();
}

vk::DescriptorSet Z3DRendererVulkanBackend::sharedOITDescriptorSet() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(!m_activeFrame->sharedOITByRing.empty()) << "Shared OIT descriptor set(s) missing on active frame-slot";
  const uint32_t ringIndex = sharedOITDescriptorSetRingIndex();
  CHECK(static_cast<size_t>(ringIndex) < m_activeFrame->sharedOITByRing.size())
    << "Shared OIT descriptor set ring index out of range";
  CHECK(m_activeFrame->sharedOITByRing[ringIndex] != nullptr)
    << "Shared OIT descriptor set missing on active frame-slot for ring index " << ringIndex;
  return m_activeFrame->sharedOITByRing[ringIndex]->descriptorSet();
}

uint64_t Z3DRendererVulkanBackend::sharedOITDescriptorSetGeneration() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return 0;
  }
  CHECK(!m_activeFrame->sharedOITByRing.empty()) << "Shared OIT descriptor set(s) missing on active frame-slot";
  const uint32_t ringIndex = sharedOITDescriptorSetRingIndex();
  CHECK(static_cast<size_t>(ringIndex) < m_activeFrame->sharedOITByRing.size())
    << "Shared OIT descriptor set ring index out of range";
  CHECK(m_activeFrame->sharedOITByRing[ringIndex] != nullptr)
    << "Shared OIT descriptor set missing on active frame-slot for ring index " << ringIndex;
  return m_activeFrame->sharedOITByRing[ringIndex]->generation();
}

uint32_t Z3DRendererVulkanBackend::sharedOITDescriptorSetRingIndex() const noexcept
{
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return 0u;
  }
  if (!m_activePPLLIndex.has_value()) {
    return 0u;
  }
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size()) << "Active PPLL ring index out of range for OIT descriptor set";
  return static_cast<uint32_t>(*m_activePPLLIndex);
}

vk::DescriptorSet Z3DRendererVulkanBackend::sharedImgRaySetupDescriptorSet() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(m_activeFrame->sharedImgRaySetup != nullptr)
    << "Shared image ray-setup descriptor set missing on active frame-slot";
  return m_activeFrame->sharedImgRaySetup->descriptorSet();
}

vk::DescriptorSet Z3DRendererVulkanBackend::sharedImgIndicesDescriptorSet() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(m_activeFrame->sharedImgIndices != nullptr)
    << "Shared image indices descriptor set missing on active frame-slot";
  return m_activeFrame->sharedImgIndices->descriptorSet();
}

vk::DescriptorSet Z3DRendererVulkanBackend::sharedImgPageDataDescriptorSet() const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return vk::DescriptorSet{};
  }
  CHECK(m_activeFrame->sharedImgPageData != nullptr)
    << "Shared image page-data descriptor set missing on active frame-slot";
  return m_activeFrame->sharedImgPageData->descriptorSet();
}

namespace {
[[nodiscard]] bool isUnsignedIntegerSampledFormat(vk::Format fmt)
{
  // Atlas currently uses RGBA32UI for block-ID and page-table textures.
  // Extend this list if additional unsigned-integer sampled formats are introduced.
  switch (fmt) {
    case vk::Format::eR32G32B32A32Uint:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] ZVulkanBindlessDescriptorSet::Kind bindlessKindForTextureOrCrash(const ZVulkanTexture& texture)
{
  const bool uintFormat = isUnsignedIntegerSampledFormat(texture.format());
  const vk::ImageViewType viewType = texture.info().viewType;
  switch (viewType) {
    case vk::ImageViewType::e2D:
      return uintFormat ? ZVulkanBindlessDescriptorSet::Kind::UTexture2D
                        : ZVulkanBindlessDescriptorSet::Kind::Texture2D;
    case vk::ImageViewType::e2DArray:
      CHECK(!uintFormat) << "Bindless: unsigned-integer 2D-array textures are not supported (no utexture2DArray table)";
      return ZVulkanBindlessDescriptorSet::Kind::Texture2DArray;
    case vk::ImageViewType::e3D:
      return uintFormat ? ZVulkanBindlessDescriptorSet::Kind::UTexture3D
                        : ZVulkanBindlessDescriptorSet::Kind::Texture3D;
    default:
      CHECK(false) << "Bindless: unsupported image view type for sampled images: " << enumOrUnderlying(viewType, 16);
      return ZVulkanBindlessDescriptorSet::Kind::Texture2D;
  }
}

[[nodiscard]] const char* bindlessKindName(ZVulkanBindlessDescriptorSet::Kind kind)
{
  switch (kind) {
    case ZVulkanBindlessDescriptorSet::Kind::Texture2D:
      return "texture2D";
    case ZVulkanBindlessDescriptorSet::Kind::Texture2DArray:
      return "texture2DArray";
    case ZVulkanBindlessDescriptorSet::Kind::Texture3D:
      return "texture3D";
    case ZVulkanBindlessDescriptorSet::Kind::UTexture2D:
      return "utexture2D";
    case ZVulkanBindlessDescriptorSet::Kind::UTexture3D:
      return "utexture3D";
  }
  return "<unknown>";
}
} // namespace

uint32_t Z3DRendererVulkanBackend::bindlessRegisterSampledImageAuto(ZVulkanTexture& texture,
                                                                    std::string_view debugLabel)
{
  CHECK(m_activeFrame != nullptr) << "bindlessRegisterSampledImageAuto called without an active Vulkan frame";
  CHECK(!isRecording())
    << "bindlessRegisterSampledImageAuto called while recording; register bindless textures before cmd begin";

  ensureBindlessSampledImagesOnFrame(*m_activeFrame);
  CHECK(m_activeFrame->bindlessSampledImages != nullptr) << "Bindless descriptor set missing on active frame-slot";

  if (device().residencyManager().ensureResidentIfManaged(&texture,
                                                          debugLabel.empty() ? std::string_view("bindless_register")
                                                                             : debugLabel)) {
    pinTextureForActiveSubmission(&texture);
  }
  CHECK(texture.resident()) << "bindlessRegisterSampledImageAuto requires a resident texture"
                            << (debugLabel.empty() ? "" : " (") << std::string(debugLabel)
                            << (debugLabel.empty() ? "" : ")");

  ZVulkanBindlessDescriptorSet::RegisterRequest req{};
  req.kind = bindlessKindForTextureOrCrash(texture);
  req.texture = &texture;
  req.debugLabel = debugLabel;
  return m_activeFrame->bindlessSampledImages->registerTexture(req);
}

uint32_t Z3DRendererVulkanBackend::bindlessLookupSampledImageAutoOrCrash(ZVulkanTexture& texture,
                                                                         std::string_view debugLabel) const
{
  CHECK(m_activeFrame != nullptr) << "bindlessLookupSampledImageAutoOrCrash called without an active Vulkan frame";
  CHECK(m_activeFrame->bindlessSampledImages != nullptr) << "Bindless descriptor set missing on active frame-slot";

  ZVulkanBindlessDescriptorSet::RegisterRequest req{};
  req.kind = bindlessKindForTextureOrCrash(texture);
  req.texture = &texture;
  req.debugLabel = debugLabel;
  const auto idx = m_activeFrame->bindlessSampledImages->lookupTexture(req);
  CHECK(idx.has_value()) << "Missing bindless sampled-image entry; this texture must be registered before recording"
                         << " kind=" << bindlessKindName(req.kind) << (debugLabel.empty() ? "" : " (")
                         << std::string(debugLabel) << (debugLabel.empty() ? "" : ")");
  return *idx;
}

void Z3DRendererVulkanBackend::debugDumpBindlessSampledImageEntry(ZVulkanTexture& texture, std::string_view label) const
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    LOG(INFO) << fmt::format("VK bindless debug dump skipped (no active frame) label='{}'",
                             label.empty() ? std::string("<none>") : std::string(label));
    return;
  }
  CHECK(m_activeFrame->bindlessSampledImages != nullptr) << "Bindless descriptor set missing on active frame-slot";

  ZVulkanBindlessDescriptorSet::RegisterRequest req{};
  req.kind = bindlessKindForTextureOrCrash(texture);
  req.texture = &texture;
  req.debugLabel = label;

  const auto state = m_activeFrame->bindlessSampledImages->debugEntryState(req);

  const vk::DescriptorImageInfo curInfo = texture.descriptorInfo();
  const uint64_t curGen = texture.imageGeneration();

  const bool viewMatches = (state.found && state.entryValid && state.entryInfo.imageView == curInfo.imageView);
  const bool layoutMatches = (state.found && state.entryValid && state.entryInfo.imageLayout == curInfo.imageLayout &&
                              state.entryInfo.imageLayout == texture.descriptorLayout());
  const bool genMatches = (state.found && state.entryValid && state.entryImageGeneration == curGen);

  const std::string kindName = std::string(bindlessKindName(req.kind));

  LOG(INFO) << fmt::format(
    "VK bindless entry dump: label='{}' kind={} idx={} tex={} fmt={} viewType={} extent=({},{},{}) layers={} "
    "layout(cur={}) descLayout={} aspect=0x{:x} texGen={} entry(valid={} gen={} layout={} viewOk={} layoutOk={} "
    "genOk={})",
    label.empty() ? std::string("<none>") : std::string(label),
    kindName,
    state.found ? static_cast<int>(state.index) : -1,
    fmt::ptr(static_cast<const void*>(&texture)),
    enumOrUnderlying(texture.format(), 16),
    enumOrUnderlying(texture.info().viewType, 16),
    texture.width(),
    texture.height(),
    texture.depth(),
    texture.arrayLayers(),
    enumOrUnderlying(texture.layout(), 16),
    enumOrUnderlying(texture.descriptorLayout(), 16),
    static_cast<uint32_t>(texture.descriptorAspect()),
    curGen,
    state.entryValid ? 1 : 0,
    state.entryImageGeneration,
    enumOrUnderlying(state.entryInfo.imageLayout, 16),
    viewMatches ? 1 : 0,
    layoutMatches ? 1 : 0,
    genMatches ? 1 : 0);

  if (state.found && state.entryValid && (!viewMatches || !layoutMatches || !genMatches)) {
    LOG(ERROR) << fmt::format(
      "VK bindless entry mismatch (stale/incorrect): label='{}' kind={} idx={} texGen={} entryGen={} "
      "viewMatches={} layoutMatches={} genMatches={}",
      label.empty() ? std::string("<none>") : std::string(label),
      kindName,
      state.index,
      curGen,
      state.entryImageGeneration,
      viewMatches ? 1 : 0,
      layoutMatches ? 1 : 0,
      genMatches ? 1 : 0);
  }
}

void Z3DRendererVulkanBackend::bindlessPreRegisterExternalSampledImageUses(std::span<const ExternalImageUseDesc> uses,
                                                                           std::string_view debugLabel)
{
  if (uses.empty()) {
    return;
  }

  CHECK(m_activeFrame != nullptr) << "bindlessPreRegisterExternalSampledImageUses called without an active Vulkan frame"
                                  << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");
  CHECK(!isRecording()) << "bindlessPreRegisterExternalSampledImageUses must run before command recording begins"
                        << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");

  size_t registered = 0;
  for (const auto& use : uses) {
    if (!use.handle.valid() || use.handle.backend != RenderBackend::Vulkan) {
      continue;
    }
    if (use.kind != ExternalImageUseKind::SampledRead) {
      continue;
    }

    auto& texture = vulkan::textureFromHandle(use.handle, device(), "bindless pre-register sampled image use");
    const auto useInfo = vulkan::resolveExternalImageUse(use.kind, use.aspectHint);
    if (useInfo.updateDescriptorLayout) {
      texture.setDescriptorLayout(useInfo.layout);
      if (useInfo.descriptorAspect != vk::ImageAspectFlags{}) {
        texture.setDescriptorAspect(useInfo.descriptorAspect);
      }
    }

    // Ensure the texture is present in the bindless tables for this frame-slot.
    (void)bindlessRegisterSampledImageAuto(texture, debugLabel);
    registered++;
  }

  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("VK bindless pre-register: uses={} registered={} label='{}'",
                           uses.size(),
                           registered,
                           debugLabel.empty() ? std::string("<none>") : std::string(debugLabel));
  }
}

void Z3DRendererVulkanBackend::bindlessPreRegisterFontAtlasPixels(std::span<const BindlessFontAtlasPixelsDesc> atlases,
                                                                  std::string_view debugLabel)
{
  if (atlases.empty()) {
    return;
  }

  CHECK(m_fontContext != nullptr) << "bindlessPreRegisterFontAtlasPixels requires a font pipeline context";
  CHECK(m_activeFrame != nullptr) << "bindlessPreRegisterFontAtlasPixels called without an active Vulkan frame"
                                  << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");
  CHECK(!isRecording()) << "bindlessPreRegisterFontAtlasPixels must run before command recording begins"
                        << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");

  size_t registered = 0;
  for (const auto& atlas : atlases) {
    if (atlas.pixelsBGRA8 == nullptr || atlas.width == 0u || atlas.height == 0u) {
      continue;
    }

    auto& texture = m_fontContext->ensureAtlasFromCpuPixelsOrCrash(atlas.pixelsBGRA8, atlas.width, atlas.height);
    (void)bindlessRegisterSampledImageAuto(texture, debugLabel);
    registered++;
  }

  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("VK bindless pre-register font atlases: atlases={} registered={} label='{}'",
                           atlases.size(),
                           registered,
                           debugLabel.empty() ? std::string("<none>") : std::string(debugLabel));
  }
}

void Z3DRendererVulkanBackend::bindlessPreWarmupImgRaycaster(Z3DImg* image,
                                                             const std::vector<Z3DTransferFunction*>* transferFunctions,
                                                             std::span<const size_t> channels,
                                                             bool wants2D,
                                                             bool wantsVolume3D,
                                                             bool wantsPaging,
                                                             std::string_view debugLabel)
{
  CHECK(m_imgRaycasterContext != nullptr) << "bindlessPreWarmupImgRaycaster requires an img raycaster pipeline context";
  CHECK(m_activeFrame != nullptr) << "bindlessPreWarmupImgRaycaster called without an active Vulkan frame"
                                  << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");
  CHECK(!isRecording()) << "bindlessPreWarmupImgRaycaster must run before command recording begins"
                        << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");

  ZVulkanImgRaycasterPipelineContext::BindlessWarmupDesc desc{};
  desc.image = image;
  desc.transferFunctions = transferFunctions;
  desc.channels.assign(channels.begin(), channels.end());
  desc.wants2D = wants2D;
  desc.wantsVolume3D = wantsVolume3D;
  desc.wantsPaging = wantsPaging;
  m_imgRaycasterContext->preRecordBindlessWarmup(desc);
}

void Z3DRendererVulkanBackend::bindlessPreWarmupImgSlice(Z3DImg* image,
                                                         const std::vector<const ZColorMap*>* colormaps,
                                                         std::span<const size_t> channels,
                                                         bool wantsVolume3D,
                                                         bool wantsColormap,
                                                         bool wantsPaging,
                                                         std::string_view debugLabel)
{
  CHECK(m_imgSliceContext != nullptr) << "bindlessPreWarmupImgSlice requires an img slice pipeline context";
  CHECK(m_activeFrame != nullptr) << "bindlessPreWarmupImgSlice called without an active Vulkan frame"
                                  << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");
  CHECK(!isRecording()) << "bindlessPreWarmupImgSlice must run before command recording begins"
                        << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");

  ZVulkanImgSlicePipelineContext::BindlessWarmupDesc desc{};
  desc.image = image;
  desc.colormaps = colormaps;
  desc.channels.assign(channels.begin(), channels.end());
  desc.wantsVolume3D = wantsVolume3D;
  desc.wantsColormap = wantsColormap;
  desc.wantsPaging = wantsPaging;
  m_imgSliceContext->preRecordBindlessWarmup(desc);
}

void Z3DRendererVulkanBackend::bindlessPrePrimeImgRaycasterBlockIdCompaction(
  const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& blockIdLease,
  uint32_t effectiveAttachmentCount,
  uint32_t maxBlockId,
  std::string_view debugLabel)
{
  CHECK(m_imgRaycasterContext != nullptr)
    << "bindlessPrePrimeImgRaycasterBlockIdCompaction requires an img raycaster pipeline context";
  CHECK(m_activeFrame != nullptr)
    << "bindlessPrePrimeImgRaycasterBlockIdCompaction called without an active Vulkan frame"
    << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");
  CHECK(!isRecording()) << "bindlessPrePrimeImgRaycasterBlockIdCompaction must run before command recording begins"
                        << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");

  m_imgRaycasterContext->preRecordPrimeBlockIdCompaction(blockIdLease, effectiveAttachmentCount, maxBlockId);
}

void Z3DRendererVulkanBackend::bindlessPrePrimeImgSliceBlockIdCompaction(
  const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& blockIdLease,
  uint32_t sliceCount,
  uint32_t sliceIndex,
  uint32_t maxBlockId,
  std::string_view debugLabel)
{
  CHECK(m_imgSliceContext != nullptr)
    << "bindlessPrePrimeImgSliceBlockIdCompaction requires an img slice pipeline context";
  CHECK(m_activeFrame != nullptr) << "bindlessPrePrimeImgSliceBlockIdCompaction called without an active Vulkan frame"
                                  << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");
  CHECK(!isRecording()) << "bindlessPrePrimeImgSliceBlockIdCompaction must run before command recording begins"
                        << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");

  m_imgSliceContext->preRecordPrimeBlockIdCompaction(blockIdLease, sliceCount, sliceIndex, maxBlockId);
}

void Z3DRendererVulkanBackend::ensureSharedSamplers()
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureSharedSamplers";
  auto& vkDevice = m_sharedDevice->context().device();
  if (!m_defaultSampler) {
    vk::SamplerCreateInfo samplerInfo{.magFilter = vk::Filter::eLinear,
                                      .minFilter = vk::Filter::eLinear,
                                      .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                      .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                      .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                      .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                      .borderColor = vk::BorderColor::eFloatOpaqueWhite};
    m_defaultSampler.emplace(vkDevice, samplerInfo);
  }
  if (!m_nearestClampSampler) {
    vk::SamplerCreateInfo nearestInfo{.magFilter = vk::Filter::eNearest,
                                      .minFilter = vk::Filter::eNearest,
                                      .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                      .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                      .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                      .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                      .borderColor = vk::BorderColor::eFloatOpaqueWhite};
    m_nearestClampSampler.emplace(vkDevice, nearestInfo);
  }
  if (!m_linearBorderZero3DSampler) {
    vk::SamplerCreateInfo linearBorderInfo{.magFilter = vk::Filter::eLinear,
                                           .minFilter = vk::Filter::eLinear,
                                           .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                           .addressModeU = vk::SamplerAddressMode::eClampToBorder,
                                           .addressModeV = vk::SamplerAddressMode::eClampToBorder,
                                           .addressModeW = vk::SamplerAddressMode::eClampToBorder,
                                           .borderColor = vk::BorderColor::eFloatTransparentBlack};
    m_linearBorderZero3DSampler.emplace(vkDevice, linearBorderInfo);
  }
}

Z3DRendererVulkanBackend::FrameResources::UploadArena::Page*
Z3DRendererVulkanBackend::activateUploadPage(FrameResources::UploadArena& arena,
                                             size_t minCapacity,
                                             std::string_view debugLabel)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in activateUploadPage";

  const size_t pageCapacity = chooseUploadArenaPageCapacity(*m_sharedDevice, minCapacity);
  if (pageCapacity == 0) {
    LOG(ERROR) << fmt::format("Upload arena request exceeds Vulkan maxMemoryAllocationSize: need={}B max={}B",
                              minCapacity,
                              static_cast<uint64_t>(uploadArenaMaxPageBytes(*m_sharedDevice)));
    return nullptr;
  }

  const size_t targetIndex = arena.usedPageCount;
  CHECK_LE(targetIndex, arena.pages.size())
    << fmt::format("Upload arena usedPageCount exceeds page count: used={} pages={} ({})",
                   arena.usedPageCount,
                   arena.pages.size(),
                   debugLabel);
  size_t bestReuseIndex = arena.pages.size();
  for (size_t i = targetIndex; i < arena.pages.size(); ++i) {
    if (arena.pages[i].capacity < minCapacity) {
      continue;
    }
    if (bestReuseIndex == arena.pages.size() || arena.pages[i].capacity < arena.pages[bestReuseIndex].capacity) {
      bestReuseIndex = i;
    }
  }

  if (bestReuseIndex < arena.pages.size()) {
    if (bestReuseIndex != targetIndex) {
      std::swap(arena.pages[targetIndex], arena.pages[bestReuseIndex]);
    }
    auto& page = arena.pages[targetIndex];
    CHECK(page.cursor == 0u) << "Reused upload page cursor was not reset";
    arena.activePageIndex = targetIndex;
    arena.usedPageCount = targetIndex + 1;
    VLOG(1) << fmt::format("Upload arena reused page: index={} size={}B request={}B spare_pages={} ({})",
                           arena.activePageIndex,
                           page.capacity,
                           minCapacity,
                           arena.pages.size() - arena.usedPageCount,
                           debugLabel);
    return &page;
  }

  const size_t newIndex = arena.pages.size();
  FrameResources::UploadArena::Page newPage{};
  try {
    newPage.buffer = m_sharedDevice->createBufferInPool(
      pageCapacity,
      vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
        vk::BufferUsageFlagBits::eTransferSrc,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
      m_sharedDevice->uploadTransientPool());
    CHECK(newPage.buffer != nullptr) << "Upload arena page allocation returned null buffer";
    newPage.capacity = pageCapacity;
    newPage.mapped = newPage.buffer->map(0, pageCapacity);
    newPage.cursor = 0;
  }
  catch (const std::exception& e) {
    if (m_sharedDevice->residencyManager().strictBudgetActive()) {
      throw;
    }
    LOG(ERROR) << fmt::format("Upload arena page allocation failed: request={}B size={}B ({}) error={}",
                              minCapacity,
                              pageCapacity,
                              debugLabel,
                              e.what());
    return nullptr;
  }
  catch (...) {
    if (m_sharedDevice->residencyManager().strictBudgetActive()) {
      throw;
    }
    LOG(ERROR) << fmt::format("Upload arena page allocation failed: request={}B size={}B ({}) error=<unknown>",
                              minCapacity,
                              pageCapacity,
                              debugLabel);
    return nullptr;
  }
  CHECK_EQ(arena.pages.size(), newIndex) << fmt::format(
    "Upload arena page count changed during allocation: before={} after={} ({})",
    newIndex,
    arena.pages.size(),
    debugLabel);
  arena.pages.emplace_back(std::move(newPage));
  if (newIndex != targetIndex) {
    std::swap(arena.pages[targetIndex], arena.pages.back());
  }
  auto& page = arena.pages[targetIndex];
  arena.activePageIndex = targetIndex;
  arena.usedPageCount = targetIndex + 1;
  VLOG(1) << fmt::format("Upload arena added page: index={} size={}B request={}B total_pages={} ({})",
                         arena.activePageIndex,
                         pageCapacity,
                         minCapacity,
                         arena.pages.size(),
                         debugLabel);
  return &page;
}

void Z3DRendererVulkanBackend::trimUnusedUploadPages(FrameResources::UploadArena& arena)
{
  if (arena.pages.size() <= arena.usedPageCount) {
    return;
  }

  size_t trimmedBytes = 0;
  for (size_t i = arena.usedPageCount; i < arena.pages.size(); ++i) {
    trimmedBytes += arena.pages[i].capacity;
  }
  const size_t keptPages = arena.usedPageCount;
  const size_t trimmedPages = arena.pages.size() - keptPages;
  arena.pages.resize(keptPages);
  VLOG(1) << fmt::format("Upload arena trimmed unused tail: kept_pages={} trimmed_pages={} trimmed_bytes={}B",
                         keptPages,
                         trimmedPages,
                         trimmedBytes);
}

Z3DRendererVulkanBackend::UploadSlice Z3DRendererVulkanBackend::suballocateUpload(size_t bytes, size_t alignment)
{
  UploadSlice slice{};
  if (!m_activeFrame) {
    VLOG(2) << "suballocateUpload: inactive frame; returning null slice for " << bytes << " bytes";
    return slice;
  }
  if (bytes == 0) {
    return slice;
  }
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in suballocateUpload";

  auto& arena = m_activeFrame->uploadArena;

  auto ensureActivePage = [&](size_t minBytes,
                              size_t align) -> Z3DRendererVulkanBackend::FrameResources::UploadArena::Page* {
    if (arena.usedPageCount > 0 && arena.activePageIndex < arena.usedPageCount) {
      auto& page = arena.pages[arena.activePageIndex];
      const size_t alignedCursor = vulkan::alignUp(page.cursor, std::max<size_t>(align, 1));
      if (alignedCursor + minBytes <= page.capacity) {
        return &page;
      }
    }
    return activateUploadPage(arena, minBytes, "suballocateUpload");
  };

  auto* page = ensureActivePage(bytes, alignment);
  if (page == nullptr) {
    return slice;
  }

  const size_t alignedCursor = vulkan::alignUp(page->cursor, std::max<size_t>(alignment, 1));
  CHECK(alignedCursor + bytes <= page->capacity)
    << fmt::format("Upload arena page overflow: need={}B have={}B page={} cursor={}B align={}B",
                   bytes,
                   page->capacity,
                   arena.activePageIndex,
                   page->cursor,
                   alignment);

  const size_t consumed = (alignedCursor - page->cursor) + bytes;
  page->cursor = alignedCursor + bytes;
  arena.usedBytes += consumed;
  arena.highWatermark = std::max(arena.highWatermark, arena.usedBytes);

  slice.buffer = page->buffer->buffer();
  slice.offset = static_cast<vk::DeviceSize>(alignedCursor);
  slice.mapped = page->mapped ? static_cast<uint8_t*>(page->mapped) + alignedCursor : nullptr;
  slice.size = bytes;
  VLOG(2) << "suballocateUpload: request bytes=" << bytes << " align=" << alignment << " page=" << arena.activePageIndex
          << " pageCap=" << page->capacity << " off=" << slice.offset << " size=" << slice.size
          << " mapped=" << (slice.mapped != nullptr) << " used=" << arena.usedBytes;
  return slice;
}

void Z3DRendererVulkanBackend::reserveUploadSlices(std::initializer_list<std::pair<size_t, size_t>> slices)
{
  if (!absl::GetFlag(FLAGS_vk_reserve_upload_slices)) {
    return;
  }
  if (!m_activeFrame) {
    return;
  }
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in reserveUploadSlices";
  auto& arena = m_activeFrame->uploadArena;
  size_t cursor = 0;
  for (const auto& s : slices) {
    const size_t bytes = s.first;
    const size_t align = s.second ? s.second : 1;
    if (bytes == 0) {
      continue;
    }
    const size_t aligned = vulkan::alignUp(cursor, align);
    cursor = aligned + bytes;
  }
  const size_t required = cursor;
  if (required == 0) {
    return;
  }

  const size_t maxPageBytes = uploadArenaMaxPageBytes(*m_sharedDevice);
  if (required > maxPageBytes) {
    VLOG(1) << fmt::format(
      "reserveUploadSlices: sequence {}B exceeds max page {}B; suballocations may span multiple pages",
      required,
      maxPageBytes);
    return;
  }

  if (arena.usedPageCount > 0 && arena.activePageIndex < arena.usedPageCount) {
    const auto& page = arena.pages[arena.activePageIndex];
    size_t pageCursor = page.cursor;
    for (const auto& s : slices) {
      const size_t bytes = s.first;
      const size_t align = s.second ? s.second : 1;
      if (bytes == 0) {
        continue;
      }
      const size_t aligned = vulkan::alignUp(pageCursor, align);
      pageCursor = aligned + bytes;
    }
    if (pageCursor <= page.capacity) {
      return;
    }
  }

  auto* page = activateUploadPage(arena, required, "reserveUploadSlices");
  if (page == nullptr) {
    VLOG(1) << fmt::format("reserveUploadSlices: failed to prepare page for {} slices ({}B)", slices.size(), required);
    return;
  }
  VLOG(2) << fmt::format("reserveUploadSlices: prepared page={} size={}B for {} slices ({}B)",
                         arena.activePageIndex,
                         page->capacity,
                         slices.size(),
                         required);
}

Z3DRendererVulkanBackend::StaticArena::Segment::~Segment()
{
  if (block) {
    vmaDestroyVirtualBlock(block);
    block = nullptr;
  }
}

std::unique_ptr<Z3DRendererVulkanBackend::StaticArena::Segment>
Z3DRendererVulkanBackend::createStaticArenaSegment(StaticArena::Kind kind, size_t capacityBytes)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in createStaticArenaSegment";
  CHECK_GT(capacityBytes, 0u) << "createStaticArenaSegment requires non-zero capacity";

  vk::BufferUsageFlags usage{};
  switch (kind) {
    case StaticArena::Kind::Vertex:
      usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
      break;
    case StaticArena::Kind::Index:
      usage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
      break;
  }

  auto seg = std::make_unique<StaticArena::Segment>();
  seg->kind = kind;
  seg->id = m_staticArena.nextSegmentId++;
  seg->capacity = capacityBytes;
  try {
    seg->buffer = m_sharedDevice->createBuffer(capacityBytes, usage, vk::MemoryPropertyFlagBits::eDeviceLocal);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << fmt::format("Failed to allocate static arena segment: kind={} capacity={}B error={}",
                              kind == StaticArena::Kind::Vertex ? "VB" : "IB",
                              capacityBytes,
                              e.what());
    return {};
  }
  catch (...) {
    LOG(ERROR) << fmt::format("Failed to allocate static arena segment: kind={} capacity={}B error=<unknown>",
                              kind == StaticArena::Kind::Vertex ? "VB" : "IB",
                              capacityBytes);
    return {};
  }
  if (!seg->buffer) {
    LOG(ERROR) << fmt::format("Failed to allocate static arena segment: kind={} capacity={}B",
                              kind == StaticArena::Kind::Vertex ? "VB" : "IB",
                              capacityBytes);
    return {};
  }

  VmaVirtualBlockCreateInfo vinfo{};
  vinfo.size = capacityBytes;
  if (vmaCreateVirtualBlock(&vinfo, &seg->block) != VK_SUCCESS) {
    LOG(ERROR) << fmt::format("Failed to create VMA virtual block for static arena: kind={} capacity={}B",
                              kind == StaticArena::Kind::Vertex ? "VB" : "IB",
                              capacityBytes);
    seg->buffer.reset();
    seg->block = nullptr;
    return {};
  }

  const VkBuffer raw = static_cast<VkBuffer>(seg->buffer->buffer());
  CHECK(raw != VK_NULL_HANDLE) << "Static arena segment created with null VkBuffer";
  const auto [_, inserted] = m_staticArena.segmentByBuffer.emplace(raw, seg.get());
  CHECK(inserted) << "Static arena segmentByBuffer already contained buffer 0x"
                  << fmt::format("{:x}", reinterpret_cast<uint64_t>(raw));

  return seg;
}

void Z3DRendererVulkanBackend::flushPendingFreesAndMaybeTrimStaticSegment(StaticArena::Segment* segment)
{
  if (segment == nullptr) {
    return;
  }
  if (segment->pinCount != 0) {
    return;
  }

  if (!segment->pendingFrees.empty()) {
    CHECK(segment->block != nullptr) << "Static arena segment missing virtual block";
    for (const auto& pf : segment->pendingFrees) {
      if (pf.allocation == nullptr) {
        continue;
      }
      vmaVirtualFree(segment->block, pf.allocation);
    }
    segment->pendingFrees.clear();
  }

  if (segment->allocCount != 0) {
    return;
  }

  CHECK_EQ(segment->usedBytes, 0u) << "Static arena segment allocCount==0 but usedBytes != 0";
  CHECK(segment->pendingFrees.empty()) << "Static arena segment has pending frees while unpinned";

  const size_t capacityBytes = segment->capacity;
  const StaticArena::Kind kind = segment->kind;
  VkBuffer raw = VK_NULL_HANDLE;
  if (segment->buffer) {
    raw = static_cast<VkBuffer>(segment->buffer->buffer());
  }

  if (raw != VK_NULL_HANDLE) {
    auto it = m_staticArena.segmentByBuffer.find(raw);
    if (it != m_staticArena.segmentByBuffer.end() && it->second == segment) {
      m_staticArena.segmentByBuffer.erase(it);
    }
  }

  auto& vec = (kind == StaticArena::Kind::Vertex) ? m_staticArena.vb : m_staticArena.ib;
  auto itVec = std::find_if(vec.begin(), vec.end(), [&](const std::unique_ptr<StaticArena::Segment>& p) {
    return p.get() == segment;
  });
  CHECK(itVec != vec.end()) << "Static arena segment not found for trimming";
  vec.erase(itVec);

  VLOG(1) << fmt::format("VK static arena trimmed: kind={} capacity={}B remaining_segments={}",
                         kind == StaticArena::Kind::Vertex ? "VB" : "IB",
                         capacityBytes,
                         vec.size());
}

void Z3DRendererVulkanBackend::ensureStaticArenas()
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureStaticArenas";
  // Create on first use with conservative capacities. We grow by appending
  // additional segments (never reallocating in place) so previously returned
  // vk::Buffer handles remain valid for the lifetime of the backend.
  //
  // We also suballocate inside each segment using VMA virtual blocks so per-stream
  // eviction can reclaim memory without arbitrary caps.
  constexpr size_t kDefaultVB = static_cast<size_t>(32) * 1024 * 1024; // 32 MiB
  constexpr size_t kDefaultIB = static_cast<size_t>(8) * 1024 * 1024; // 8 MiB

  if (m_staticArena.vb.empty()) {
    auto seg = createStaticArenaSegment(StaticArena::Kind::Vertex, kDefaultVB);
    if (seg) {
      m_staticArena.vb.push_back(std::move(seg));
      size_t total = 0;
      for (const auto& s : m_staticArena.vb) {
        total += s->capacity;
      }
      m_staticArena.vbHighWatermark = std::max(m_staticArena.vbHighWatermark, total);
    }
  }

  if (m_staticArena.ib.empty()) {
    auto seg = createStaticArenaSegment(StaticArena::Kind::Index, kDefaultIB);
    if (seg) {
      m_staticArena.ib.push_back(std::move(seg));
      size_t total = 0;
      for (const auto& s : m_staticArena.ib) {
        total += s->capacity;
      }
      m_staticArena.ibHighWatermark = std::max(m_staticArena.ibHighWatermark, total);
    }
  }
}

void Z3DRendererVulkanBackend::ensureUniformArena(FrameResources& frame)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureUniformArena";
  // Track the scheduler-provided "min capacity" hint for debug validation at
  // endRender(). This captures the caller's request before base/carry capacity
  // heuristics are applied.
  frame.uniformArena.minCapacityHint = m_nextUniformMinCapacity;
  // Create if missing
  const size_t baseRequested = static_cast<size_t>(std::max(64, kUniformArenaBaseKiB)) * 1024ull;
  size_t requested = std::max({baseRequested, m_nextUniformMinCapacity, m_uniformMinCapacityCarry});
  if (!frame.uniformArena.buffer) {
    const size_t cap = requested;
    frame.uniformArena.buffer = m_sharedDevice->createBuffer(cap,
                                                             vk::BufferUsageFlagBits::eUniformBuffer,
                                                             vk::MemoryPropertyFlagBits::eHostVisible |
                                                               vk::MemoryPropertyFlagBits::eHostCoherent);
    frame.uniformArena.capacity = cap;
    frame.uniformArena.cursor = 0;
    frame.uniformArena.highWatermark = 0;
    frame.uniformArena.mapped = frame.uniformArena.buffer->map(0, cap);
  } else {
    // If an existing arena is smaller than the requested capacity, recreate it now (safe point).
    if (frame.uniformArena.capacity < requested) {
      frame.uniformArena.buffer.reset();
      frame.uniformArena.buffer = m_sharedDevice->createBuffer(requested,
                                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
      frame.uniformArena.capacity = requested;
      frame.uniformArena.mapped = frame.uniformArena.buffer->map(0, requested);
    }
    // Reset bump pointer for new frame
    frame.uniformArena.cursor = 0;
    frame.uniformArena.highWatermark = 0;
  }
  // Consume the hint once
  m_nextUniformMinCapacity = 0;
}

void Z3DRendererVulkanBackend::ensurePersistentUniformArena(FrameResources& frame)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensurePersistentUniformArena";

  // Persistent uniforms are used for dynamic-UBO slices whose offsets must stay
  // stable across frames (e.g., cached command buffers). Because descriptor set
  // writes during recording are forbidden for persistent sets, this arena must
  // be sized conservatively up front and must not need to grow mid-recording.
  constexpr size_t kBase = static_cast<size_t>(32) * 1024ull * 1024ull; // 32 MiB

  // If the arena already exists, keep its current capacity unless we detect an
  // invariant violation (cursor beyond capacity). Growth is expected to be rare
  // in steady state.
  size_t requested = frame.persistentUniformArena.buffer ? frame.persistentUniformArena.capacity : kBase;
  requested = std::max(requested, kBase);
  requested = std::max(requested, frame.persistentUniformArena.cursor);

  if (!frame.persistentUniformArena.buffer) {
    frame.persistentUniformArena.buffer = m_sharedDevice->createBuffer(requested,
                                                                       vk::BufferUsageFlagBits::eUniformBuffer,
                                                                       vk::MemoryPropertyFlagBits::eHostVisible |
                                                                         vk::MemoryPropertyFlagBits::eHostCoherent);
    frame.persistentUniformArena.capacity = requested;
    frame.persistentUniformArena.cursor = 0;
    frame.persistentUniformArena.highWatermark = 0;
    frame.persistentUniformArena.mapped = frame.persistentUniformArena.buffer->map(0, requested);
    return;
  }

  // Sanity: persistent cursor must always fit. If this triggers, something
  // corrupted cursor bookkeeping.
  CHECK(frame.persistentUniformArena.capacity >= frame.persistentUniformArena.cursor)
    << fmt::format("Persistent uniform arena cursor out of bounds: cursor={}B capacity={}B",
                   frame.persistentUniformArena.cursor,
                   frame.persistentUniformArena.capacity);
}

void Z3DRendererVulkanBackend::ensureDDPGatingResources(FrameResources& frame)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureDDPGatingResources";
  if (!frame.ddpChangedFlag) {
    frame.ddpChangedFlag =
      m_sharedDevice->createBuffer(4,
                                   vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                                     vk::BufferUsageFlagBits::eTransferDst,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
  }
  if (!frame.ddpIndirectCount) {
    frame.ddpIndirectCount =
      m_sharedDevice->createBuffer(4,
                                   vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                                     vk::BufferUsageFlagBits::eTransferDst,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
  }
  if (!frame.ddpArgsDevice.buffer) {
    // Device-local args buffer for indirect draws; contents populated by copy from upload arena.
    const size_t cap = 64 * sizeof(VkDrawIndexedIndirectCommand);
    frame.ddpArgsDevice.buffer =
      m_sharedDevice->createBuffer(cap,
                                   vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    frame.ddpArgsDevice.capacity = cap;
    frame.ddpArgsDevice.cursor = 0;
  }
}

namespace {

struct PPLLParamsStd430
{
  glm::uvec4 viewport{0u}; // x,y,w,h
  uint32_t pixelCount = 0;
  uint32_t blockCount = 0;
  uint32_t blockSize = 0;
  uint32_t fragmentCapacity = 0;
};

static_assert(sizeof(PPLLParamsStd430) == 32, "PPLL params must match std430 layout (32B)");

constexpr size_t kPPLLFragmentStrideBytes = 32; // vec4 + float (+ padding) in std430

} // namespace

void Z3DRendererVulkanBackend::ensurePPLLResources(const glm::uvec4& viewport, uint64_t requestedFragments)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensurePPLLResources";
  CHECK(m_activePPLLIndex.has_value()) << "ensurePPLLResources called without an active PPLL ring slot";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size()) << "Active PPLL ring index out of range";

  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  ppll.viewport = viewport;

  const uint64_t pixelCount64 = static_cast<uint64_t>(viewport.z) * static_cast<uint64_t>(viewport.w);
  CHECK(pixelCount64 <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    << fmt::format("PPLL viewport pixel count overflow: {}x{} = {}", viewport.z, viewport.w, pixelCount64);

  ppll.pixelCount = static_cast<uint32_t>(pixelCount64);
  ppll.blockCount =
    (ppll.pixelCount == 0u) ? 0u : ((ppll.pixelCount + PPLLResources::kBlockSize - 1u) / PPLLResources::kBlockSize);
  ppll.requestedFragmentCount = requestedFragments;
  const uint64_t fragCount = std::max<uint64_t>(requestedFragments, 1u);
  CHECK(fragCount <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    << fmt::format("PPLL fragment capacity overflow: fragments={}", fragCount);

  // Params SSBO (host-visible). Keep mapped across frames.
  const size_t paramsBytes = sizeof(PPLLParamsStd430);
  if (!ppll.params || ppll.paramsCapacity < paramsBytes) {
    if (ppll.params) {
      // Replacing an OIT buffer invalidates cached secondaries that were
      // recorded against the old VkBuffer handles.
      ++m_oitResourcesRevision;
    }
    ppll.params.reset();
    ppll.paramsMapped = nullptr;
    ppll.params = m_sharedDevice->createBuffer(paramsBytes,
                                               vk::BufferUsageFlagBits::eStorageBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
    ppll.paramsCapacity = paramsBytes;
    ppll.paramsMapped = ppll.params->map(0, paramsBytes);
  }
  if (ppll.paramsMapped) {
    PPLLParamsStd430 params{};
    params.viewport = viewport;
    params.pixelCount = ppll.pixelCount;
    params.blockCount = ppll.blockCount;
    params.blockSize = PPLLResources::kBlockSize;
    params.fragmentCapacity = static_cast<uint32_t>(fragCount);
    std::memcpy(ppll.paramsMapped, &params, sizeof(params));
  }

  auto ensureDeviceLocal =
    [&](std::unique_ptr<ZVulkanBuffer>& buf, size_t& capacityBytes, size_t requiredBytes, vk::BufferUsageFlags usage) {
      const size_t req = std::max<size_t>(requiredBytes, sizeof(uint32_t));
      if (!buf || capacityBytes < req) {
        if (buf) {
          // Replacing an OIT buffer invalidates cached secondaries that were
          // recorded against the old VkBuffer handles.
          ++m_oitResourcesRevision;
        }
        buf.reset();
        buf = m_sharedDevice->createBuffer(req, usage, vk::MemoryPropertyFlagBits::eDeviceLocal);
        capacityBytes = req;
      }
    };

  const size_t perPixelBytes = static_cast<size_t>(ppll.pixelCount) * sizeof(uint32_t);
  ensureDeviceLocal(ppll.counts,
                    ppll.countsCapacityBytes,
                    perPixelBytes,
                    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst);
  ensureDeviceLocal(ppll.offsets, ppll.offsetsCapacityBytes, perPixelBytes, vk::BufferUsageFlagBits::eStorageBuffer);
  ensureDeviceLocal(ppll.cursors,
                    ppll.cursorsCapacityBytes,
                    perPixelBytes,
                    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst);

  const size_t blocksBytes = static_cast<size_t>(ppll.blockCount) * sizeof(uint32_t);
  ensureDeviceLocal(ppll.blockSums,
                    ppll.blockSumsCapacityBytes,
                    blocksBytes,
                    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc);

  const size_t prefixesBytes = std::max<size_t>(blocksBytes, sizeof(uint32_t));
  if (!ppll.blockPrefixes || ppll.blockPrefixesCapacityBytes < prefixesBytes) {
    if (ppll.blockPrefixes) {
      ++m_oitResourcesRevision;
    }
    ppll.blockPrefixes.reset();
    ppll.blockPrefixesMapped = nullptr;
    ppll.blockPrefixes = m_sharedDevice->createBuffer(prefixesBytes,
                                                      vk::BufferUsageFlagBits::eStorageBuffer,
                                                      vk::MemoryPropertyFlagBits::eHostVisible |
                                                        vk::MemoryPropertyFlagBits::eHostCoherent);
    ppll.blockPrefixesCapacityBytes = prefixesBytes;
    ppll.blockPrefixesMapped = ppll.blockPrefixes->map(0, prefixesBytes);
  }

  // Fragment storage: grow to requestedFragments (in elements), but never shrink.
  const uint64_t fragBytes64 = fragCount * static_cast<uint64_t>(kPPLLFragmentStrideBytes);
  CHECK(fragBytes64 <= static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    << fmt::format("PPLL fragment buffer size overflow: fragments={} stride={}B bytes={}",
                   fragCount,
                   kPPLLFragmentStrideBytes,
                   fragBytes64);
  ensureDeviceLocal(ppll.fragments,
                    ppll.fragmentsCapacityBytes,
                    static_cast<size_t>(fragBytes64),
                    vk::BufferUsageFlagBits::eStorageBuffer);
}

void Z3DRendererVulkanBackend::ensureDDPComputePipeline()
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureDDPComputePipeline";
  if (m_ddpCountPipeline) {
    return;
  }
  auto& device = m_sharedDevice->context().device();
  // Compute helper shaders reserve set 0 but do not access bindless sampled
  // images. Use an empty set layout as set 0 to reduce per-stage descriptor
  // accounting pressure (especially on MoltenVK) while preserving shader set
  // numbering (the shader uses set = 1).
  const vk::DescriptorSetLayout emptyLayout = emptyDescriptorSetLayout();
  CHECK(emptyLayout != vk::DescriptorSetLayout{}) << "Empty descriptor set layout missing in ensureDDPComputePipeline";
  // Descriptor set layout: binding0 = flag SSBO (read), binding1 = count SSBO (write)
  std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
    vk::DescriptorSetLayoutBinding{.binding = 0,
                                   .descriptorType = vk::DescriptorType::eStorageBuffer,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eCompute},
    vk::DescriptorSetLayoutBinding{.binding = 1,
                                   .descriptorType = vk::DescriptorType::eStorageBuffer,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eCompute}
  };
  m_ddpCountSetLayout.emplace(device,
                              vk::DescriptorSetLayoutCreateInfo{.bindingCount = 2, .pBindings = bindings.data()});
  const std::array<vk::DescriptorSetLayout, 2> setLayouts{emptyLayout, **m_ddpCountSetLayout};
  m_ddpCountPipelineLayout.emplace(
    device,
    vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                 .pSetLayouts = setLayouts.data()});
  // Load SPIR-V
  const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";
  const std::string compPath = shaderBase + "ddp_count.comp.spv";
  std::ifstream file(compPath, std::ios::ate | std::ios::binary);
  if (!file) {
    throw ZException(fmt::format("Failed to open SPIR-V file: {}", compPath));
  }
  const size_t size = static_cast<size_t>(file.tellg());
  std::vector<uint32_t> code(size / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(code.data()), size);
  vk::raii::ShaderModule compModule(device, vk::ShaderModuleCreateInfo{.codeSize = size, .pCode = code.data()});
  vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                          .module = *compModule,
                                          .pName = "main"};
  m_ddpCountPipeline.emplace(device,
                             nullptr,
                             vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_ddpCountPipelineLayout});
}

void Z3DRendererVulkanBackend::ensurePPLLComputePipelines()
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensurePPLLComputePipelines";
  if (m_ppllScanLocalPipeline && m_ppllScanAddPipeline) {
    return;
  }
  auto& device = m_sharedDevice->context().device();
  // Compute helper shaders reserve set 0 but do not access bindless sampled
  // images. Use an empty set layout as set 0 to preserve shader set numbering
  // (the shaders use set = 1) while keeping bindless descriptors out of compute
  // stage resource accounting.
  const vk::DescriptorSetLayout emptyLayout = emptyDescriptorSetLayout();
  CHECK(emptyLayout != vk::DescriptorSetLayout{})
    << "Empty descriptor set layout missing in ensurePPLLComputePipelines";

  auto loadModule = [&](const std::string& spvName) -> vk::raii::ShaderModule {
    const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";
    const std::string path = shaderBase + spvName;
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
      throw ZException(fmt::format("Failed to open SPIR-V file: {}", path));
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    return vk::raii::ShaderModule(device, vk::ShaderModuleCreateInfo{.codeSize = size, .pCode = code.data()});
  };

  if (!m_ppllScanLocalPipeline) {
    std::array<vk::DescriptorSetLayoutBinding, 4> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 2,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 3,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute}
    };
    m_ppllScanLocalSetLayout.emplace(
      device,
      vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                        .pBindings = bindings.data()});
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{emptyLayout, **m_ppllScanLocalSetLayout};
    m_ppllScanLocalPipelineLayout.emplace(
      device,
      vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                   .pSetLayouts = setLayouts.data()});

    auto module = loadModule("ppll_scan_local.comp.spv");
    vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                            .module = *module,
                                            .pName = "main"};
    m_ppllScanLocalPipeline.emplace(
      device,
      nullptr,
      vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_ppllScanLocalPipelineLayout});
  }

  if (!m_ppllScanAddPipeline) {
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 2,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute}
    };
    m_ppllScanAddSetLayout.emplace(
      device,
      vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                        .pBindings = bindings.data()});
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{emptyLayout, **m_ppllScanAddSetLayout};
    m_ppllScanAddPipelineLayout.emplace(
      device,
      vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                   .pSetLayouts = setLayouts.data()});

    auto module = loadModule("ppll_scan_add.comp.spv");
    vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                            .module = *module,
                                            .pName = "main"};
    m_ppllScanAddPipeline.emplace(
      device,
      nullptr,
      vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_ppllScanAddPipelineLayout});
  }
}

bool Z3DRendererVulkanBackend::ddpIndirectCountEnabled() const
{
  return absl::GetFlag(FLAGS_atlas_vk_ddp_indirect_count) && m_supportsDrawIndirectCount &&
         m_supportsFragStoresAndAtomics;
}

void Z3DRendererVulkanBackend::primePPLLForCountPass(const glm::uvec4& viewport)
{
  primePPLLForStorePass(viewport, 0);
}

void Z3DRendererVulkanBackend::primePPLLForStorePass(const glm::uvec4& viewport, uint64_t requestedFragments)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in primePPLLForStorePass";
  CHECK(!isRecording()) << "primePPLLForStorePass must run before command recording begins";
  CHECK(m_activeFrame != nullptr) << "primePPLLForStorePass requires an active frame-slot";
  CHECK(m_supportsFragStoresAndAtomics) << "PPLL requires fragment storage buffer atomics (fragmentStoresAndAtomics)";

  const size_t desiredRing = std::max<size_t>(1, static_cast<size_t>(m_maxFramesInFlight));
  if (m_ppllFrameRing.size() != desiredRing) {
    // Resizing the PPLL ring can destroy buffers for dropped slots. Make sure
    // cached secondaries rebuild before executing any command buffer recorded
    // against now-destroyed OIT resources.
    ++m_oitResourcesRevision;
    m_ppllFrameRing.resize(desiredRing);
  }
  const size_t ringSize = m_ppllFrameRing.empty() ? 1 : m_ppllFrameRing.size();
  const uint64_t realFrameToken = Z3DRenderGlobalState::instance().currentPerfFrameToken();
  m_activePPLLIndex = static_cast<size_t>(realFrameToken % ringSize);

  ensurePPLLResources(viewport, requestedFragments);

  // Prime compute helper descriptor sets (scan_local/scan_add) now that the
  // per-frame-ring PPLL buffers are sized. These are bound during recording.
  ensurePPLLComputePipelines();
  CHECK(m_ppllScanLocalSetLayout.has_value()) << "PPLL scan_local descriptor set layout missing";
  CHECK(m_ppllScanAddSetLayout.has_value()) << "PPLL scan_add descriptor set layout missing";
  CHECK(m_activePPLLIndex.has_value()) << "PPLL priming called without an active ring slot";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size()) << "PPLL priming ring index out of range";
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  CHECK(ppll.params && ppll.counts && ppll.offsets && ppll.blockSums && ppll.blockPrefixes)
    << "PPLL priming missing required buffers";

  if (!m_activeFrame->ppllScanLocalComputeDescriptorSet) {
    m_activeFrame->ppllScanLocalComputeDescriptorSet = allocatePersistentDescriptorSet(**m_ppllScanLocalSetLayout);
    CHECK(m_activeFrame->ppllScanLocalComputeDescriptorSet != nullptr)
      << "Failed to allocate PPLL scan_local descriptor set";
  }
  // binding 0 = params, 1 = counts, 2 = offsets, 3 = blockSums
  m_activeFrame->ppllScanLocalComputeDescriptorSet->updateStorageBuffer(0, *ppll.params);
  m_activeFrame->ppllScanLocalComputeDescriptorSet->updateStorageBuffer(1, *ppll.counts);
  m_activeFrame->ppllScanLocalComputeDescriptorSet->updateStorageBuffer(2, *ppll.offsets);
  m_activeFrame->ppllScanLocalComputeDescriptorSet->updateStorageBuffer(3, *ppll.blockSums);

  if (!m_activeFrame->ppllScanAddComputeDescriptorSet) {
    m_activeFrame->ppllScanAddComputeDescriptorSet = allocatePersistentDescriptorSet(**m_ppllScanAddSetLayout);
    CHECK(m_activeFrame->ppllScanAddComputeDescriptorSet != nullptr)
      << "Failed to allocate PPLL scan_add descriptor set";
  }
  // binding 0 = params, 1 = offsets, 2 = blockPrefixes
  m_activeFrame->ppllScanAddComputeDescriptorSet->updateStorageBuffer(0, *ppll.params);
  m_activeFrame->ppllScanAddComputeDescriptorSet->updateStorageBuffer(1, *ppll.offsets);
  m_activeFrame->ppllScanAddComputeDescriptorSet->updateStorageBuffer(2, *ppll.blockPrefixes);
}

uint32_t Z3DRendererVulkanBackend::ppllPixelCount() const
{
  if (!m_activePPLLIndex.has_value() || *m_activePPLLIndex >= m_ppllFrameRing.size()) {
    return 0;
  }
  return m_ppllFrameRing[*m_activePPLLIndex].pixelCount;
}

uint32_t Z3DRendererVulkanBackend::ppllBlockCount() const
{
  if (!m_activePPLLIndex.has_value() || *m_activePPLLIndex >= m_ppllFrameRing.size()) {
    return 0;
  }
  return m_ppllFrameRing[*m_activePPLLIndex].blockCount;
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ppllParamsBufferObj()
{
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return nullptr;
  }
  ensureDefaultPlaceholders();
  if (!m_activePPLLIndex.has_value() || *m_activePPLLIndex >= m_ppllFrameRing.size()) {
    return m_defaultPlaceholderStorageBuffer.get();
  }
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  return ppll.params ? ppll.params.get() : m_defaultPlaceholderStorageBuffer.get();
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ppllCountsBufferObj()
{
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return nullptr;
  }
  ensureDefaultPlaceholders();
  if (!m_activePPLLIndex.has_value() || *m_activePPLLIndex >= m_ppllFrameRing.size()) {
    return m_defaultPlaceholderStorageBuffer.get();
  }
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  return ppll.counts ? ppll.counts.get() : m_defaultPlaceholderStorageBuffer.get();
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ppllOffsetsBufferObj()
{
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return nullptr;
  }
  ensureDefaultPlaceholders();
  if (!m_activePPLLIndex.has_value() || *m_activePPLLIndex >= m_ppllFrameRing.size()) {
    return m_defaultPlaceholderStorageBuffer.get();
  }
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  return ppll.offsets ? ppll.offsets.get() : m_defaultPlaceholderStorageBuffer.get();
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ppllCursorsBufferObj()
{
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return nullptr;
  }
  ensureDefaultPlaceholders();
  if (!m_activePPLLIndex.has_value() || *m_activePPLLIndex >= m_ppllFrameRing.size()) {
    return m_defaultPlaceholderStorageBuffer.get();
  }
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  return ppll.cursors ? ppll.cursors.get() : m_defaultPlaceholderStorageBuffer.get();
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ppllFragmentsBufferObj()
{
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return nullptr;
  }
  ensureDefaultPlaceholders();
  if (!m_activePPLLIndex.has_value() || *m_activePPLLIndex >= m_ppllFrameRing.size()) {
    return m_defaultPlaceholderStorageBuffer.get();
  }
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  return ppll.fragments ? ppll.fragments.get() : m_defaultPlaceholderStorageBuffer.get();
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ppllBlockSumsBufferObj()
{
  CHECK(m_activePPLLIndex.has_value()) << "ppllBlockSumsBufferObj called without active PPLL resources";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size());
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  return ppll.blockSums.get();
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ppllBlockPrefixesBufferObj()
{
  CHECK(m_activePPLLIndex.has_value()) << "ppllBlockPrefixesBufferObj called without active PPLL resources";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size());
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  return ppll.blockPrefixes.get();
}

void Z3DRendererVulkanBackend::ppllWriteBlockPrefixes(const uint32_t* prefixes, size_t count)
{
  CHECK(m_activePPLLIndex.has_value()) << "ppllWriteBlockPrefixes called without active PPLL resources";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size());
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  CHECK(ppll.blockPrefixesMapped != nullptr) << "PPLL blockPrefixes buffer is not mapped";

  if (count == 0u) {
    return;
  }
  CHECK(prefixes != nullptr) << "ppllWriteBlockPrefixes called with null prefixes pointer";

  const size_t bytes = count * sizeof(uint32_t);
  CHECK(bytes <= ppll.blockPrefixesCapacityBytes)
    << fmt::format("PPLL blockPrefixes write out of bounds: count={} bytes={} capacity={}",
                   count,
                   bytes,
                   ppll.blockPrefixesCapacityBytes);
  std::memcpy(ppll.blockPrefixesMapped, prefixes, bytes);
}

void Z3DRendererVulkanBackend::primeOITDescriptorSet(ZVulkanDescriptorSet& set)
{
  // Do not write descriptors during recording; callers must prime in beginRender().
  CHECK(!isRecording()) << "primeOITDescriptorSet called while recording";
  if (auto* buf = ppllParamsBufferObj()) {
    set.updateStorageBuffer(vkbind::kBindingOITParams, *buf);
  }
  if (auto* buf = ddpChangedFlagBufferObj()) {
    set.updateStorageBuffer(vkbind::kBindingOITDDPFlag, *buf);
  }
  if (auto* buf = ppllCountsBufferObj()) {
    set.updateStorageBuffer(vkbind::kBindingOITPPLLCounts, *buf);
  }
  if (auto* buf = ppllOffsetsBufferObj()) {
    set.updateStorageBuffer(vkbind::kBindingOITPPLLOffsets, *buf);
  }
  if (auto* buf = ppllCursorsBufferObj()) {
    set.updateStorageBuffer(vkbind::kBindingOITPPLLCursors, *buf);
  }
  if (auto* buf = ppllFragmentsBufferObj()) {
    set.updateStorageBuffer(vkbind::kBindingOITPPLLFragments, *buf);
  }
}

vk::Buffer Z3DRendererVulkanBackend::ddpChangedFlagBuffer()
{
  if (!m_activeFrame) {
    return {};
  }
  ensureDDPGatingResources(*m_activeFrame);
  return m_activeFrame->ddpChangedFlag ? m_activeFrame->ddpChangedFlag->buffer() : vk::Buffer{};
}

vk::Buffer Z3DRendererVulkanBackend::ddpIndirectCountBuffer()
{
  if (!m_activeFrame) {
    return {};
  }
  ensureDDPGatingResources(*m_activeFrame);
  return m_activeFrame->ddpIndirectCount ? m_activeFrame->ddpIndirectCount->buffer() : vk::Buffer{};
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ddpChangedFlagBufferObj()
{
  if (!m_activeFrame) {
    return nullptr;
  }
  ensureDDPGatingResources(*m_activeFrame);
  return m_activeFrame->ddpChangedFlag.get();
}

ZVulkanBuffer* Z3DRendererVulkanBackend::ddpIndirectCountBufferObj()
{
  if (!m_activeFrame) {
    return nullptr;
  }
  ensureDDPGatingResources(*m_activeFrame);
  return m_activeFrame->ddpIndirectCount.get();
}

vk::Buffer Z3DRendererVulkanBackend::ddpDeviceArgsBuffer()
{
  if (!m_activeFrame) {
    return vk::Buffer{};
  }
  ensureDDPGatingResources(*m_activeFrame);
  return m_activeFrame->ddpArgsDevice.buffer ? m_activeFrame->ddpArgsDevice.buffer->buffer() : vk::Buffer{};
}

vk::DeviceSize Z3DRendererVulkanBackend::ddpAllocDeviceArgsSlot(size_t bytes)
{
  if (!m_activeFrame) {
    return 0;
  }
  ensureDDPGatingResources(*m_activeFrame);
  auto& arena = m_activeFrame->ddpArgsDevice;
  // Align to 16 bytes to satisfy both VkDraw(Indexed)IndirectCommand requirements
  const size_t align = 16;
  const size_t off = vulkan::alignUp(arena.cursor, align);
  if (off + bytes > arena.capacity) {
    // Grow device-local args arena; keep old buffer alive until frame end
    size_t newCap = arena.capacity ? arena.capacity : (64 * sizeof(VkDrawIndexedIndirectCommand));
    while (newCap < off + bytes) {
      newCap *= 2;
    }
    if (arena.buffer) {
      arena.retiredBuffers.push_back(std::move(arena.buffer));
    }
    arena.buffer =
      m_sharedDevice->createBuffer(newCap,
                                   vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    arena.capacity = newCap;
    arena.cursor = 0;
    return ddpAllocDeviceArgsSlot(bytes);
  }
  m_activeFrame->ddpArgsDevice.cursor = off + bytes;
  return static_cast<vk::DeviceSize>(off);
}

// Host-visible args path removed; device-local indirect args only

void Z3DRendererVulkanBackend::ddpResetForPass(vk::raii::CommandBuffer& cmd, bool firstPass)
{
  if (!m_activeFrame) {
    return;
  }
  ensureDDPGatingResources(*m_activeFrame);
  // Indirect-count gating is a "next-pass" signal: ddp_count.comp writes the
  // count after a peel pass based on ddpChangedFlag. Do not clobber it at the
  // start of subsequent passes; only seed the very first peel so it executes.
  if (firstPass) {
    cmd.fillBuffer(ddpIndirectCountBuffer(), 0, 4, 1u);
  }
  cmd.fillBuffer(ddpChangedFlagBuffer(), 0, 4, 0u);
}

void Z3DRendererVulkanBackend::ppllResetCounts(vk::raii::CommandBuffer& cmd)
{
  CHECK(m_activePPLLIndex.has_value()) << "ppllResetCounts called without active PPLL resources";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size());
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  if (ppll.pixelCount == 0u || !ppll.counts) {
    return;
  }
  const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(static_cast<size_t>(ppll.pixelCount) * sizeof(uint32_t));
  cmd.fillBuffer(ppll.counts->buffer(), 0, bytes, 0u);
}

void Z3DRendererVulkanBackend::ppllResetCursors(vk::raii::CommandBuffer& cmd)
{
  CHECK(m_activePPLLIndex.has_value()) << "ppllResetCursors called without active PPLL resources";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size());
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  if (ppll.pixelCount == 0u || !ppll.cursors) {
    return;
  }
  const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(static_cast<size_t>(ppll.pixelCount) * sizeof(uint32_t));
  cmd.fillBuffer(ppll.cursors->buffer(), 0, bytes, 0u);
}

void Z3DRendererVulkanBackend::ddpBarrierTransferToFrag(vk::raii::CommandBuffer& cmd)
{
  vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                        .dstStageMask =
                          vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eDrawIndirect,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                                         vk::AccessFlagBits2::eIndirectCommandRead};
  vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mb};
  cmd.pipelineBarrier2(dep);
}

void Z3DRendererVulkanBackend::ppllBarrierTransferToFrag(vk::raii::CommandBuffer& cmd)
{
  vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
  vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mb};
  cmd.pipelineBarrier2(dep);
}

void Z3DRendererVulkanBackend::ddpBarrierFragToCompute(vk::raii::CommandBuffer& cmd)
{
  vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
  vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mb};
  cmd.pipelineBarrier2(dep);
}

void Z3DRendererVulkanBackend::ddpBarrierComputeToTransfer(vk::raii::CommandBuffer& cmd)
{
  vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
  vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mb};
  cmd.pipelineBarrier2(dep);
}

void Z3DRendererVulkanBackend::ppllBarrierFragToCompute(vk::raii::CommandBuffer& cmd)
{
  vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
  vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mb};
  cmd.pipelineBarrier2(dep);
}

void Z3DRendererVulkanBackend::ddpBarrierComputeToIndirect(vk::raii::CommandBuffer& cmd)
{
  vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
                        .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead};
  vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mb};
  cmd.pipelineBarrier2(dep);
}

void Z3DRendererVulkanBackend::ppllBarrierComputeToFrag(vk::raii::CommandBuffer& cmd)
{
  vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
  vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mb};
  cmd.pipelineBarrier2(dep);
}

void Z3DRendererVulkanBackend::ppllBarrierFragToFrag(vk::raii::CommandBuffer& cmd)
{
  vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
  vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mb};
  cmd.pipelineBarrier2(dep);
}

void Z3DRendererVulkanBackend::ddpDispatchCountCompute(vk::raii::CommandBuffer& cmd)
{
  ensureDDPComputePipeline();
  CHECK(m_ddpCountPipeline.has_value()) << "DDP count compute pipeline missing";
  CHECK(m_ddpCountPipelineLayout.has_value()) << "DDP count compute pipeline layout missing";
  CHECK(m_activeFrame != nullptr) << "ddpDispatchCountCompute requires an active frame";
  CHECK(m_activeFrame->ddpCountComputeDescriptorSet != nullptr)
    << "ddpDispatchCountCompute requires a pre-record primed descriptor set";
  // Dispatch (1,1,1)
  cmd.bindPipeline(vk::PipelineBindPoint::eCompute, **m_ddpCountPipeline);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                         **m_ddpCountPipelineLayout,
                         1,
                         {m_activeFrame->ddpCountComputeDescriptorSet->descriptorSet()},
                         {});
  cmd.dispatch(1, 1, 1);
}

void Z3DRendererVulkanBackend::ppllDispatchScanLocal(vk::raii::CommandBuffer& cmd)
{
  CHECK(m_activePPLLIndex.has_value()) << "ppllDispatchScanLocal called without active PPLL resources";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size());
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  if (ppll.blockCount == 0u) {
    return;
  }
  ensurePPLLComputePipelines();
  CHECK(m_ppllScanLocalPipeline.has_value());
  CHECK(m_ppllScanLocalPipelineLayout.has_value());
  CHECK(m_activeFrame != nullptr) << "ppllDispatchScanLocal requires an active frame";
  CHECK(m_activeFrame->ppllScanLocalComputeDescriptorSet != nullptr)
    << "ppllDispatchScanLocal requires a pre-record primed descriptor set";

  cmd.bindPipeline(vk::PipelineBindPoint::eCompute, **m_ppllScanLocalPipeline);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                         **m_ppllScanLocalPipelineLayout,
                         1,
                         {m_activeFrame->ppllScanLocalComputeDescriptorSet->descriptorSet()},
                         {});
  cmd.dispatch(ppll.blockCount, 1, 1);
}

void Z3DRendererVulkanBackend::ppllDispatchScanAdd(vk::raii::CommandBuffer& cmd)
{
  CHECK(m_activePPLLIndex.has_value()) << "ppllDispatchScanAdd called without active PPLL resources";
  CHECK(*m_activePPLLIndex < m_ppllFrameRing.size());
  auto& ppll = m_ppllFrameRing[*m_activePPLLIndex];
  if (ppll.blockCount == 0u) {
    return;
  }
  ensurePPLLComputePipelines();
  CHECK(m_ppllScanAddPipeline.has_value());
  CHECK(m_ppllScanAddPipelineLayout.has_value());
  CHECK(m_activeFrame != nullptr) << "ppllDispatchScanAdd requires an active frame";
  CHECK(m_activeFrame->ppllScanAddComputeDescriptorSet != nullptr)
    << "ppllDispatchScanAdd requires a pre-record primed descriptor set";

  cmd.bindPipeline(vk::PipelineBindPoint::eCompute, **m_ppllScanAddPipeline);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                         **m_ppllScanAddPipelineLayout,
                         1,
                         {m_activeFrame->ppllScanAddComputeDescriptorSet->descriptorSet()},
                         {});
  cmd.dispatch(ppll.blockCount, 1, 1);
}

void Z3DRendererVulkanBackend::scheduleStaticCopyIndirect(vk::Buffer dst,
                                                          vk::DeviceSize dstOffset,
                                                          const UploadSlice& src)
{
  if (!m_activeFrame) {
    return;
  }
  // Reuse scheduled copy mechanism but ensure barrier targets DrawIndirect stage
  FrameResources::ScheduledCopy sc{};
  sc.dst = dst;
  sc.dstOffset = dstOffset;
  sc.src = src;
  sc.usage = FrameResources::ScheduledCopy::Usage::Indirect;
  m_activeFrame->scheduledCopies.push_back(sc);
}

size_t Z3DRendererVulkanBackend::uniformAlignment() const
{
  if (m_cachedUniformAlignment != 0) {
    return m_cachedUniformAlignment;
  }

  // uniformAlignment is used in a few debug-only validation sites that run
  // under the "current backend" thread-local pointer. Be robust to call order:
  // if ensureDevice() has not yet cached m_sharedDevice, query the shared
  // device directly from the scratch pool.
  auto* dev = m_sharedDevice;
  if (!dev) {
    dev = Z3DRenderGlobalState::instance().scratchPool().vulkanDevice();
  }
  CHECK(dev != nullptr) << "Shared Vulkan device missing in uniformAlignment";
  auto limits = dev->context().physicalDevice().getProperties().limits;
  size_t align = static_cast<size_t>(limits.minUniformBufferOffsetAlignment);
  if (!align) {
    align = 256;
  }
  m_cachedUniformAlignment = align;
  return align;
}

Z3DRendererVulkanBackend::UniformSlice Z3DRendererVulkanBackend::suballocateUniform(size_t bytes, size_t alignment)
{
  UniformSlice slice{};
  CHECK(m_activeFrame != nullptr) << "suballocateUniform called without an active frame";
  auto& arena = m_activeFrame->uniformArena;
  // Do not allocate here; fail fast to surface ordering/estimation bugs.
  CHECK(arena.buffer) << "Uniform arena not initialised before suballocateUniform";
  const size_t align = std::max(uniformAlignment(), alignment ? alignment : static_cast<size_t>(1));
  const size_t aligned = vulkan::alignUp(arena.cursor, align);
  if (aligned + bytes > arena.capacity) {
    CHECK(false) << fmt::format(
      "Uniform arena capacity exceeded within frame: need={}B have={}B (cursor={}B, align={}B).",
      aligned + bytes,
      arena.capacity,
      arena.cursor,
      align);
  }
  const size_t off = vulkan::alignUp(arena.cursor, align);
  slice.buffer = arena.buffer->buffer();
  slice.offset = static_cast<vk::DeviceSize>(off);
  slice.mapped = static_cast<char*>(arena.mapped) + off;
  slice.size = bytes;
  arena.cursor = off + bytes;
  arena.highWatermark = std::max(arena.highWatermark, arena.cursor);
  return slice;
}

Z3DRendererVulkanBackend::UniformSlice Z3DRendererVulkanBackend::suballocatePersistentUniform(size_t bytes,
                                                                                              size_t alignment)
{
  UniformSlice slice{};
  CHECK(m_activeFrame != nullptr) << "suballocatePersistentUniform called without an active frame";
  auto& arena = m_activeFrame->persistentUniformArena;
  CHECK(arena.buffer) << "Persistent uniform arena not initialised before suballocatePersistentUniform";
  const size_t align = std::max(uniformAlignment(), alignment ? alignment : static_cast<size_t>(1));
  const size_t aligned = vulkan::alignUp(arena.cursor, align);
  if (aligned + bytes > arena.capacity) {
    CHECK(false) << fmt::format(
      "Persistent uniform arena capacity exceeded: need={}B have={}B (cursor={}B, align={}B).",
      aligned + bytes,
      arena.capacity,
      arena.cursor,
      align);
  }
  const size_t off = vulkan::alignUp(arena.cursor, align);
  slice.buffer = arena.buffer->buffer();
  slice.offset = static_cast<vk::DeviceSize>(off);
  slice.mapped = static_cast<char*>(arena.mapped) + off;
  slice.size = bytes;
  arena.cursor = off + bytes;
  arena.highWatermark = std::max(arena.highWatermark, arena.cursor);
  return slice;
}

class ZVulkanBuffer& Z3DRendererVulkanBackend::uniformArenaBuffer()
{
  CHECK(m_activeFrame && m_activeFrame->uniformArena.buffer) << "Uniform arena not initialised";
  return *m_activeFrame->uniformArena.buffer;
}

class ZVulkanBuffer& Z3DRendererVulkanBackend::persistentUniformArenaBuffer()
{
  CHECK(m_activeFrame && m_activeFrame->persistentUniformArena.buffer) << "Persistent uniform arena not initialised";
  return *m_activeFrame->persistentUniformArena.buffer;
}

void* Z3DRendererVulkanBackend::persistentUniformMappedAt(vk::DeviceSize offset, size_t bytes)
{
  CHECK(m_activeFrame && m_activeFrame->persistentUniformArena.buffer) << "Persistent uniform arena not initialised";
  auto& arena = m_activeFrame->persistentUniformArena;
  CHECK(arena.mapped != nullptr) << "Persistent uniform arena missing mapped pointer";
  const size_t off = static_cast<size_t>(offset);
  CHECK(off + bytes <= arena.capacity)
    << fmt::format("Persistent uniform arena mappedAt OOB: off={}B bytes={}B cap={}B", off, bytes, arena.capacity);
  return static_cast<char*>(arena.mapped) + off;
}

uint32_t Z3DRendererVulkanBackend::activeSubmissionId() const
{
  CHECK(m_activeFrame != nullptr) << "activeSubmissionId called without an active frame";
  CHECK_NE(m_activeFrame->submissionId, 0u) << "activeSubmissionId called before beginRender assigned a submission";
  return m_activeFrame->submissionId;
}

bool Z3DRendererVulkanBackend::prepareStaticPromotionBudget(StaticPressureDomain domain,
                                                            size_t promotionBytes,
                                                            std::string_view reason)
{
  if (promotionBytes == 0u || m_sharedDevice == nullptr) {
    return true;
  }

  const uint64_t requestedBytes = promotionBytes > std::numeric_limits<uint64_t>::max()
                                    ? std::numeric_limits<uint64_t>::max()
                                    : static_cast<uint64_t>(promotionBytes);
  auto& residency = m_sharedDevice->residencyManager();
  const auto pressure = residency.allocationPressureFor(requestedBytes);
  if (!pressure.needsReclaim()) {
    return true;
  }

  const auto reclaimStats = residency.reclaimMemory(
    ZVulkanResidencyManager::ReclaimRequest{.requestClass = ZVulkanResidencyManager::ResourceClass::StaticGeometry,
                                            .requestedBytes = pressure.reclaimBytes,
                                            .force = false,
                                            .reason = reason});
  (void)reclaimStats;
  const auto retryPressure = residency.allocationPressureFor(requestedBytes);
  if (!retryPressure.needsReclaim()) {
    return true;
  }

  VLOG(1) << fmt::format(
    "Static {} promotion skipped under Vulkan memory budget: request={}B target={}B usage={}B budget={}B reason='{}'",
    domain == StaticPressureDomain::Vertex ? "VB" : "IB",
    promotionBytes,
    retryPressure.reclaimBytes,
    retryPressure.usageBytes,
    retryPressure.budgetBytes,
    reason.empty() ? "<unspecified>" : std::string(reason));
  return false;
}

Z3DRendererVulkanBackend::StaticSlice Z3DRendererVulkanBackend::allocateStaticVB(size_t bytes, size_t alignment)
{
  StaticSlice slice{};
  if (bytes == 0) {
    return slice;
  }
  if (!prepareStaticPromotionBudget(StaticPressureDomain::Vertex, bytes, "static_vb_promotion_preallocate")) {
    return {};
  }
  ensureStaticArenas();

  const size_t align = std::max<size_t>(1, alignment);
  VmaVirtualAllocationCreateInfo ainfo{};
  ainfo.size = bytes;
  ainfo.alignment = align;

  auto tryAlloc = [&](StaticArena::Segment* seg) -> bool {
    CHECK(seg != nullptr);
    CHECK(seg->kind == StaticArena::Kind::Vertex);
    CHECK(seg->buffer) << "Static VB segment missing buffer";
    CHECK(seg->block != nullptr) << "Static VB segment missing VMA virtual block";

    VmaVirtualAllocation alloc = nullptr;
    VkDeviceSize vOffset = 0;
    if (vmaVirtualAllocate(seg->block, &ainfo, &alloc, &vOffset) != VK_SUCCESS) {
      return false;
    }

    slice.buffer = seg->buffer->buffer();
    slice.offset = static_cast<vk::DeviceSize>(vOffset);
    slice.size = bytes;
    slice.alloc.segment = seg;
    slice.alloc.allocation = alloc;
    slice.alloc.size = bytes;
    slice.alloc.isIndexBuffer = false;

    CHECK(seg->usedBytes <= std::numeric_limits<size_t>::max() - bytes) << "Static VB usedBytes overflow";
    seg->usedBytes += bytes;
    seg->allocCount++;
    return true;
  };

  size_t requiredBytes = std::max(bytes, align);
  auto tryPressureEviction = [&](bool force) -> bool {
    if (evictColdStaticCacheForPressure(StaticPressureDomain::Vertex,
                                        requiredBytes,
                                        force,
                                        ZVulkanResidencyManager::ReclaimScope::PressureLadder,
                                        force ? "static_vb_forced_reuse" : "static_vb_reuse") == 0) {
      return false;
    }
    for (const auto& seg : m_staticArena.vb) {
      if (tryAlloc(seg.get())) {
        return true;
      }
    }
    return false;
  };

  // First try existing segments (reusing freed ranges).
  for (const auto& seg : m_staticArena.vb) {
    if (tryAlloc(seg.get())) {
      return slice;
    }
  }

  if (tryPressureEviction(false)) {
    return slice;
  }

  // Need a new segment. Size it to fit this allocation and grow by powers of two
  // to avoid pathological churn while preserving correctness.
  constexpr size_t kDefaultSegment = static_cast<size_t>(32) * 1024 * 1024; // 32 MiB
  size_t cap = kDefaultSegment;
  while (cap < requiredBytes) {
    CHECK(cap <= std::numeric_limits<size_t>::max() / 2) << "Static VB segment capacity overflow";
    cap *= 2;
  }

  if (!prepareStaticPromotionBudget(StaticPressureDomain::Vertex, cap, "static_vb_segment_preallocate")) {
    return {};
  }

  auto newSeg = createStaticArenaSegment(StaticArena::Kind::Vertex, cap);
  if (!newSeg) {
    if (tryPressureEviction(true)) {
      return slice;
    }
    return {};
  }
  StaticArena::Segment* newSegPtr = newSeg.get();
  m_staticArena.vb.push_back(std::move(newSeg));
  size_t totalCapacity = 0;
  for (const auto& s : m_staticArena.vb) {
    totalCapacity += s->capacity;
  }
  m_staticArena.vbHighWatermark = std::max(m_staticArena.vbHighWatermark, totalCapacity);
  VLOG(1) << fmt::format("Static VB arena grew: segments={} added={}B total_capacity={}B",
                         m_staticArena.vb.size(),
                         cap,
                         totalCapacity);

  if (!tryAlloc(newSegPtr)) {
    LOG(ERROR) << "Static VB virtual allocation failed after segment growth";
    flushPendingFreesAndMaybeTrimStaticSegment(newSegPtr);
    return {};
  }

  return slice;
}

Z3DRendererVulkanBackend::StaticSlice Z3DRendererVulkanBackend::allocateStaticIB(size_t bytes, size_t alignment)
{
  StaticSlice slice{};
  if (bytes == 0) {
    return slice;
  }
  if (!prepareStaticPromotionBudget(StaticPressureDomain::Index, bytes, "static_ib_promotion_preallocate")) {
    return {};
  }
  ensureStaticArenas();

  const size_t align = std::max<size_t>(1, alignment);
  VmaVirtualAllocationCreateInfo ainfo{};
  ainfo.size = bytes;
  ainfo.alignment = align;

  auto tryAlloc = [&](StaticArena::Segment* seg) -> bool {
    CHECK(seg != nullptr);
    CHECK(seg->kind == StaticArena::Kind::Index);
    CHECK(seg->buffer) << "Static IB segment missing buffer";
    CHECK(seg->block != nullptr) << "Static IB segment missing VMA virtual block";

    VmaVirtualAllocation alloc = nullptr;
    VkDeviceSize vOffset = 0;
    if (vmaVirtualAllocate(seg->block, &ainfo, &alloc, &vOffset) != VK_SUCCESS) {
      return false;
    }

    slice.buffer = seg->buffer->buffer();
    slice.offset = static_cast<vk::DeviceSize>(vOffset);
    slice.size = bytes;
    slice.alloc.segment = seg;
    slice.alloc.allocation = alloc;
    slice.alloc.size = bytes;
    slice.alloc.isIndexBuffer = true;

    CHECK(seg->usedBytes <= std::numeric_limits<size_t>::max() - bytes) << "Static IB usedBytes overflow";
    seg->usedBytes += bytes;
    seg->allocCount++;
    return true;
  };

  size_t requiredBytes = std::max(bytes, align);
  auto tryPressureEviction = [&](bool force) -> bool {
    if (evictColdStaticCacheForPressure(StaticPressureDomain::Index,
                                        requiredBytes,
                                        force,
                                        ZVulkanResidencyManager::ReclaimScope::PressureLadder,
                                        force ? "static_ib_forced_reuse" : "static_ib_reuse") == 0) {
      return false;
    }
    for (const auto& seg : m_staticArena.ib) {
      if (tryAlloc(seg.get())) {
        return true;
      }
    }
    return false;
  };

  for (const auto& seg : m_staticArena.ib) {
    if (tryAlloc(seg.get())) {
      return slice;
    }
  }

  if (tryPressureEviction(false)) {
    return slice;
  }

  constexpr size_t kDefaultSegment = static_cast<size_t>(8) * 1024 * 1024; // 8 MiB
  size_t cap = kDefaultSegment;
  while (cap < requiredBytes) {
    CHECK(cap <= std::numeric_limits<size_t>::max() / 2) << "Static IB segment capacity overflow";
    cap *= 2;
  }

  if (!prepareStaticPromotionBudget(StaticPressureDomain::Index, cap, "static_ib_segment_preallocate")) {
    return {};
  }

  auto newSeg = createStaticArenaSegment(StaticArena::Kind::Index, cap);
  if (!newSeg) {
    if (tryPressureEviction(true)) {
      return slice;
    }
    return {};
  }
  StaticArena::Segment* newSegPtr = newSeg.get();
  m_staticArena.ib.push_back(std::move(newSeg));
  size_t totalCapacity = 0;
  for (const auto& s : m_staticArena.ib) {
    totalCapacity += s->capacity;
  }
  m_staticArena.ibHighWatermark = std::max(m_staticArena.ibHighWatermark, totalCapacity);
  VLOG(1) << fmt::format("Static IB arena grew: segments={} added={}B total_capacity={}B",
                         m_staticArena.ib.size(),
                         cap,
                         totalCapacity);

  if (!tryAlloc(newSegPtr)) {
    LOG(ERROR) << "Static IB virtual allocation failed after segment growth";
    flushPendingFreesAndMaybeTrimStaticSegment(newSegPtr);
    return {};
  }

  return slice;
}

void Z3DRendererVulkanBackend::releaseStaticSlice(StaticSlice& slice)
{
  if (!slice.alloc) {
    slice = {};
    return;
  }

  auto* segment = static_cast<StaticArena::Segment*>(slice.alloc.segment);
  CHECK(segment != nullptr) << "releaseStaticSlice called with null segment";
  CHECK(segment->block != nullptr) << "releaseStaticSlice called with a segment missing VMA block";
  CHECK(slice.alloc.allocation != nullptr) << "releaseStaticSlice called with null allocation handle";
  CHECK_GT(slice.alloc.size, 0u) << "releaseStaticSlice called with zero size";
  CHECK_GT(segment->allocCount, 0u) << "Static segment allocCount underflow on release";
  CHECK_GE(segment->usedBytes, slice.alloc.size) << "Static segment usedBytes underflow on release";

  segment->allocCount--;
  segment->usedBytes -= slice.alloc.size;

  if (segment->pinCount != 0) {
    StaticArena::Segment::PendingFree pf{};
    pf.allocation = slice.alloc.allocation;
    pf.size = slice.alloc.size;
    segment->pendingFrees.push_back(pf);
  } else {
    vmaVirtualFree(segment->block, slice.alloc.allocation);
    flushPendingFreesAndMaybeTrimStaticSegment(segment);
  }

  slice = {};
}

void Z3DRendererVulkanBackend::pinStaticSliceForActiveSubmission(const StaticSlice& slice)
{
  if (!slice.alloc) {
    return;
  }
  if (!m_activeFrame || !m_submissionResourcePinningOpen) {
    return;
  }
  auto* segment = static_cast<StaticArena::Segment*>(slice.alloc.segment);
  CHECK(segment != nullptr) << "pinStaticSliceForActiveSubmission called with null segment";

  const auto [_, inserted] = m_activeFrame->pinnedStaticSegments.insert(segment);
  if (inserted) {
    CHECK(segment->pinCount < std::numeric_limits<uint32_t>::max()) << "Static segment pinCount overflow";
    segment->pinCount++;
  }
}

uint64_t Z3DRendererVulkanBackend::staticArenaSegmentIdForBuffer(vk::Buffer buffer) const
{
  if (!buffer) {
    return 0;
  }
  const VkBuffer raw = static_cast<VkBuffer>(buffer);
  auto it = m_staticArena.segmentByBuffer.find(raw);
  if (it == m_staticArena.segmentByBuffer.end() || it->second == nullptr) {
    return 0;
  }
  return it->second->id;
}

void Z3DRendererVulkanBackend::requestEvictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }
  std::scoped_lock lock(m_pendingEvictionsMutex);
  m_pendingEvictStreamKeys.insert(streamKey);
}

void Z3DRendererVulkanBackend::noteStaticStreamPromoted(StaticCacheOwner owner,
                                                        uint64_t streamKey,
                                                        size_t stagedBytes,
                                                        std::string_view reason)
{
  if (streamKey == 0u || stagedBytes == 0u || m_recentStaticEvictions.empty()) {
    return;
  }

  for (size_t index = m_recentStaticEvictions.size(); index-- > 0u;) {
    const auto& eviction = m_recentStaticEvictions[index];
    if (eviction.owner != owner || eviction.streamKey != streamKey) {
      continue;
    }

    const uint64_t reuseEpochs =
      m_staticCacheEpoch >= eviction.evictionEpoch ? (m_staticCacheEpoch - eviction.evictionEpoch) : 0u;
    VLOG(1) << fmt::format(
      "VK static restore after eviction: owner={} streamKey={} reuse_epochs={} staged={}B evicted_resident={}B evicted_virtual={}B evict_reason='{}' promote_reason='{}'",
      staticCacheOwnerName(owner),
      streamKey,
      reuseEpochs,
      stagedBytes,
      eviction.residentBytes,
      eviction.virtualBytes,
      eviction.reason.empty() ? "<unspecified>" : eviction.reason,
      reason.empty() ? "<unspecified>" : std::string(reason));
    m_recentStaticEvictions.erase(m_recentStaticEvictions.begin() + static_cast<std::ptrdiff_t>(index));
    return;
  }
}

void Z3DRendererVulkanBackend::recordStaticStreamEviction(StaticCacheOwner owner,
                                                          uint64_t streamKey,
                                                          uint64_t residentBytes,
                                                          uint64_t virtualBytes,
                                                          std::string_view reason)
{
  if (streamKey == 0u || (residentBytes == 0u && virtualBytes == 0u)) {
    return;
  }

  for (auto& eviction : m_recentStaticEvictions) {
    if (eviction.owner != owner || eviction.streamKey != streamKey) {
      continue;
    }
    eviction.evictionEpoch = m_staticCacheEpoch;
    eviction.residentBytes = residentBytes;
    eviction.virtualBytes = virtualBytes;
    eviction.reason = std::string(reason);
    return;
  }

  m_recentStaticEvictions.push_back(StaticEvictionTelemetry{.owner = owner,
                                                            .streamKey = streamKey,
                                                            .evictionEpoch = m_staticCacheEpoch,
                                                            .residentBytes = residentBytes,
                                                            .virtualBytes = virtualBytes,
                                                            .reason = std::string(reason)});
  if (m_recentStaticEvictions.size() > kStaticEvictionTelemetryLimit) {
    m_recentStaticEvictions.erase(
      m_recentStaticEvictions.begin(),
      m_recentStaticEvictions.begin() +
        static_cast<std::ptrdiff_t>(m_recentStaticEvictions.size() - kStaticEvictionTelemetryLimit));
  }
}

uint64_t
Z3DRendererVulkanBackend::staticGeometryProtectedEpochForRequest(bool force,
                                                                 ZVulkanResidencyManager::ReclaimScope scope) const
{
  uint64_t protectedEpoch = m_submissionResourcePinningOpen ? m_staticCacheEpoch : std::numeric_limits<uint64_t>::max();

  const bool protectRecentStatic = !force && scope != ZVulkanResidencyManager::ReclaimScope::RequestedClassOnly &&
                                   m_sharedDevice != nullptr &&
                                   !m_sharedDevice->residencyManager().strictBudgetActive();
  if (!protectRecentStatic) {
    return protectedEpoch;
  }

  const uint64_t recentProtectedEpoch = m_staticCacheEpoch > kRecentStaticGeometryProtectedEpochs
                                          ? (m_staticCacheEpoch - kRecentStaticGeometryProtectedEpochs)
                                          : 0u;
  return std::min(protectedEpoch, recentProtectedEpoch);
}

size_t Z3DRendererVulkanBackend::evictColdStaticCacheForPressure(StaticPressureDomain domain,
                                                                 size_t growthBytes,
                                                                 bool force,
                                                                 ZVulkanResidencyManager::ReclaimScope scope,
                                                                 std::string_view reason)
{
  if (growthBytes == 0 || m_sharedDevice == nullptr || m_staticCacheEpoch == 0) {
    return 0;
  }

  const auto budget = m_sharedDevice->deviceLocalBudget();
  const uint64_t cappedGrowthBytes = growthBytes > std::numeric_limits<uint64_t>::max()
                                       ? std::numeric_limits<uint64_t>::max()
                                       : static_cast<uint64_t>(growthBytes);
  const uint64_t projectedUsage = budget.usageBytes > std::numeric_limits<uint64_t>::max() - cappedGrowthBytes
                                    ? std::numeric_limits<uint64_t>::max()
                                    : budget.usageBytes + cappedGrowthBytes;
  const uint64_t budgetBytes = m_sharedDevice->residencyManager().effectiveBrokerBudgetBytes();
  if (!force) {
    if (budgetBytes == 0 || projectedUsage <= budgetBytes) {
      return 0;
    }
  }

  const uint64_t pressureBytes64 =
    (budgetBytes != 0 && projectedUsage > budgetBytes) ? (projectedUsage - budgetBytes) : 0;
  const size_t pressureBytes = pressureBytes64 >= std::numeric_limits<size_t>::max()
                                 ? std::numeric_limits<size_t>::max()
                                 : static_cast<size_t>(pressureBytes64);
  const size_t targetBytes = std::max(growthBytes, pressureBytes);
  const uint64_t protectedEpoch = staticGeometryProtectedEpochForRequest(force, scope);

  size_t releasedBytes = 0;
  size_t evictedStreams = 0;
  while (releasedBytes < targetBytes) {
    std::vector<StaticPressureEvictionCandidate> candidates;
    if (m_meshContext) {
      if (auto candidate = m_meshContext->oldestEvictableStaticStream(domain, protectedEpoch)) {
        candidates.push_back(*candidate);
      }
    }
    if (m_lineContext) {
      if (auto candidate = m_lineContext->oldestEvictableStaticStream(domain, protectedEpoch)) {
        candidates.push_back(*candidate);
      }
    }
    if (m_ellipsoidContext) {
      if (auto candidate = m_ellipsoidContext->oldestEvictableStaticStream(domain, protectedEpoch)) {
        candidates.push_back(*candidate);
      }
    }
    if (m_sphereContext) {
      if (auto candidate = m_sphereContext->oldestEvictableStaticStream(domain, protectedEpoch)) {
        candidates.push_back(*candidate);
      }
    }
    if (m_coneContext) {
      if (auto candidate = m_coneContext->oldestEvictableStaticStream(domain, protectedEpoch)) {
        candidates.push_back(*candidate);
      }
    }
    if (candidates.empty()) {
      break;
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
      if (a.lastUsedEpoch != b.lastUsedEpoch) {
        return a.lastUsedEpoch < b.lastUsedEpoch;
      }
      if (a.bytes != b.bytes) {
        return a.bytes > b.bytes;
      }
      return a.streamKey < b.streamKey;
    });

    const auto candidate = candidates.front();
    size_t bytesFreed = 0;
    switch (candidate.owner) {
      case StaticCacheOwner::Mesh:
        CHECK(m_meshContext != nullptr);
        bytesFreed = m_meshContext->evictStaticStreamForPressure(candidate.streamKey);
        break;
      case StaticCacheOwner::Line:
        CHECK(m_lineContext != nullptr);
        bytesFreed = m_lineContext->evictStaticStreamForPressure(candidate.streamKey);
        break;
      case StaticCacheOwner::Ellipsoid:
        CHECK(m_ellipsoidContext != nullptr);
        bytesFreed = m_ellipsoidContext->evictStaticStreamForPressure(candidate.streamKey);
        break;
      case StaticCacheOwner::Sphere:
        CHECK(m_sphereContext != nullptr);
        bytesFreed = m_sphereContext->evictStaticStreamForPressure(candidate.streamKey);
        break;
      case StaticCacheOwner::Cone:
        CHECK(m_coneContext != nullptr);
        bytesFreed = m_coneContext->evictStaticStreamForPressure(candidate.streamKey);
        break;
    }
    if (bytesFreed == 0) {
      break;
    }
    releasedBytes += bytesFreed;
    ++evictedStreams;
    recordStaticStreamEviction(candidate.owner, candidate.streamKey, 0u, bytesFreed, reason);
    const uint64_t age =
      m_staticCacheEpoch >= candidate.lastUsedEpoch ? (m_staticCacheEpoch - candidate.lastUsedEpoch) : 0u;
    VLOG(1) << fmt::format(
      "VK static pressure eviction: owner={} domain={} streamKey={} age={} released={}B estimated={}B progress={}/{}B usage={}B budget={}B reason='{}'",
      staticCacheOwnerName(candidate.owner),
      domain == StaticPressureDomain::Vertex ? "VB" : "IB",
      candidate.streamKey,
      age,
      bytesFreed,
      candidate.bytes,
      releasedBytes,
      targetBytes,
      budget.usageBytes,
      budgetBytes,
      reason.empty() ? "<unspecified>" : std::string(reason));
  }

  if (releasedBytes > 0) {
    VLOG(1) << fmt::format("VK static pressure eviction summary: domain={} streams={} released={}B target={}B force={}",
                           domain == StaticPressureDomain::Vertex ? "VB" : "IB",
                           evictedStreams,
                           releasedBytes,
                           targetBytes,
                           force);
  }
  return releasedBytes;
}

void Z3DRendererVulkanBackend::installMemoryBrokerProviders()
{
  if (m_sharedDevice == nullptr || !m_memoryBrokerProviderIds.empty()) {
    return;
  }

  auto& broker = m_sharedDevice->residencyManager();
  auto registerProvider = [&](ZVulkanResidencyManager::ResourceProvider provider) {
    if (provider.owner == nullptr) {
      provider.owner = this;
    }
    m_memoryBrokerProviderIds.push_back(broker.registerResourceProvider(std::move(provider)));
  };
  constexpr uint32_t kBrokerPriorityUploadPages = 0;
  constexpr uint32_t kBrokerPriorityScratchBacking = 10;
  constexpr uint32_t kBrokerPriorityStaticGeometry = 20;
  constexpr uint32_t kBrokerPriorityReadbackStaging = 30;

  registerProvider(ZVulkanResidencyManager::ResourceProvider{
    .resourceClass = ZVulkanResidencyManager::ResourceClass::TransientUploadPage,
    .priority = kBrokerPriorityUploadPages,
    .label = "renderer_upload_pages",
    .reclaim =
      [this](const ZVulkanResidencyManager::ReclaimRequest& request) {
        if (currentRenderThreadExecutorOrNull() == nullptr) {
          return ZVulkanResidencyManager::ReclaimStats{};
        }
        return reclaimCompletedUploadPagesForMemoryPressure(request.reason);
      },
    .report =
      [this]() {
        if (currentRenderThreadExecutorOrNull() == nullptr) {
          ZVulkanResidencyManager::ResourceReport report{};
          report.resourceClass = ZVulkanResidencyManager::ResourceClass::TransientUploadPage;
          report.label = "renderer_upload_pages";
          return report;
        }
        return uploadPageMemoryReport();
      },
  });

  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();
  registerProvider(ZVulkanResidencyManager::ResourceProvider{
    .resourceClass = ZVulkanResidencyManager::ResourceClass::ScratchBacking,
    .priority = kBrokerPriorityScratchBacking,
    .owner = &scratchPool,
    .label = "scratch_pool_backing",
    .reclaim =
      [](const ZVulkanResidencyManager::ReclaimRequest& request) {
        if (currentRenderThreadExecutorOrNull() == nullptr) {
          return ZVulkanResidencyManager::ReclaimStats{};
        }
        auto& pool = Z3DRenderGlobalState::instance().scratchPool();
        auto stats = pool.reclaimFreeVulkanScratchBacking(request.reason, request.requestedBytes);
        return ZVulkanResidencyManager::ReclaimStats{.resourcesReleased = stats.slotsEvicted,
                                                     .bytesReleased = stats.bytesReleased};
      },
    .collectCandidates =
      [](const ZVulkanResidencyManager::ReclaimRequest& request) {
        std::vector<ZVulkanResidencyManager::EvictionCandidate> out;
        if (currentRenderThreadExecutorOrNull() == nullptr) {
          return out;
        }
        auto& pool = Z3DRenderGlobalState::instance().scratchPool();
        const bool includeLeasedScratchBacking = request.force;
        const auto scratchCandidates = pool.vulkanScratchBackingCandidates(includeLeasedScratchBacking);
        out.reserve(scratchCandidates.size());
        for (const auto& candidate : scratchCandidates) {
          out.push_back(ZVulkanResidencyManager::EvictionCandidate{
            .resourceClass = ZVulkanResidencyManager::ResourceClass::ScratchBacking,
            .priority = candidate.inUse ? 1u : 0u,
            .residentBytes = candidate.residentBytes,
            .lastUsedEpoch = candidate.lastUseTick,
            .pinCount = candidate.pinCount,
            .restoreAvailable = true,
            .userKey0 = static_cast<uint64_t>(candidate.usage),
            .userKey1 = static_cast<uint64_t>(candidate.slotIndex),
            .label = candidate.label});
        }
        return out;
      },
    .evictCandidate =
      [](const ZVulkanResidencyManager::EvictionCandidate& candidate,
         const ZVulkanResidencyManager::ReclaimRequest& request) {
        if (currentRenderThreadExecutorOrNull() == nullptr) {
          return ZVulkanResidencyManager::ReclaimStats{};
        }
        const size_t usageIndex = static_cast<size_t>(candidate.userKey0);
        if (usageIndex >= kScratchUsageCount || candidate.userKey1 > std::numeric_limits<size_t>::max()) {
          return ZVulkanResidencyManager::ReclaimStats{};
        }
        auto& pool = Z3DRenderGlobalState::instance().scratchPool();
        const auto stats = pool.reclaimVulkanScratchBackingCandidate(static_cast<ScratchImageUsage>(usageIndex),
                                                                     static_cast<size_t>(candidate.userKey1),
                                                                     request.reason);
        return ZVulkanResidencyManager::ReclaimStats{.resourcesReleased = stats.slotsEvicted,
                                                     .bytesReleased = stats.bytesReleased};
      },
    .report =
      [this]() {
        ZVulkanResidencyManager::ResourceReport report{};
        report.resourceClass = ZVulkanResidencyManager::ResourceClass::ScratchBacking;
        report.label = "scratch_pool_backing";
        if (currentRenderThreadExecutorOrNull() == nullptr) {
          return report;
        }
        const auto scratchReport = Z3DRenderGlobalState::instance().scratchPool().vulkanScratchBackingReport();
        const bool inFlight = m_sharedDevice != nullptr && m_sharedDevice->frameExecutor().inFlightCount() != 0u;
        report.residentObjects = scratchReport.residentSlots;
        report.pinnedObjects =
          scratchReport.releasePendingSlots + scratchReport.protectedSlots + (inFlight ? scratchReport.inUseSlots : 0u);
        report.residentBytes = scratchReport.residentBytes;
        return report;
      },
  });

  registerProvider(ZVulkanResidencyManager::ResourceProvider{
    .resourceClass = ZVulkanResidencyManager::ResourceClass::StaticGeometry,
    .priority = kBrokerPriorityStaticGeometry,
    .label = "renderer_static_geometry",
    .reclaim =
      [this](const ZVulkanResidencyManager::ReclaimRequest& request) {
        return reclaimStaticGeometryForMemoryPressure(request);
      },
    .collectCandidates =
      [this](const ZVulkanResidencyManager::ReclaimRequest& request) {
        return staticGeometryEvictionCandidates(request);
      },
    .evictCandidate =
      [this](const ZVulkanResidencyManager::EvictionCandidate& candidate,
             const ZVulkanResidencyManager::ReclaimRequest& request) {
        return evictStaticGeometryCandidate(candidate, request.reason);
      },
    .report =
      [this]() {
        return staticGeometryMemoryReport();
      },
  });

  registerProvider(ZVulkanResidencyManager::ResourceProvider{
    .resourceClass = ZVulkanResidencyManager::ResourceClass::ReadbackStaging,
    .priority = kBrokerPriorityReadbackStaging,
    .label = "renderer_readback_staging",
    .reclaim =
      [this](const ZVulkanResidencyManager::ReclaimRequest& request) {
        if (currentRenderThreadExecutorOrNull() == nullptr) {
          return ZVulkanResidencyManager::ReclaimStats{};
        }
        return reclaimReadbackStagingForMemoryPressure(request.reason);
      },
    .report =
      [this]() {
        if (currentRenderThreadExecutorOrNull() == nullptr) {
          ZVulkanResidencyManager::ResourceReport report{};
          report.resourceClass = ZVulkanResidencyManager::ResourceClass::ReadbackStaging;
          report.label = "renderer_readback_staging";
          return report;
        }
        return readbackStagingMemoryReport();
      },
  });
}

void Z3DRendererVulkanBackend::uninstallMemoryBrokerProviders()
{
  if (m_sharedDevice == nullptr || m_memoryBrokerProviderIds.empty()) {
    m_memoryBrokerProviderIds.clear();
    return;
  }
  auto& broker = m_sharedDevice->residencyManager();
  for (const auto id : m_memoryBrokerProviderIds) {
    broker.unregisterResourceProvider(id);
  }
  m_memoryBrokerProviderIds.clear();
}

ZVulkanResidencyManager::ReclaimStats
Z3DRendererVulkanBackend::reclaimCompletedUploadPagesForMemoryPressure(std::string_view reason)
{
  ZVulkanResidencyManager::ReclaimStats stats{};
  if (m_sharedDevice == nullptr || m_sharedDevice->frameExecutor().inFlightCount() != 0u) {
    return stats;
  }

  for (auto& frame : m_frames) {
    auto& arena = frame.uploadArena;
    const bool activeFrame = (&frame == m_activeFrame);
    const size_t keepPages = activeFrame ? std::min(arena.usedPageCount, arena.pages.size()) : 0u;
    for (size_t pageIndex = keepPages; pageIndex < arena.pages.size(); ++pageIndex) {
      const auto& page = arena.pages[pageIndex];
      if (page.capacity == 0u) {
        continue;
      }
      stats.bytesReleased += page.capacity;
      stats.resourcesReleased++;
    }
    arena.pages.resize(keepPages);
    arena.usedPageCount = keepPages;
    if (keepPages == 0u) {
      arena.activePageIndex = 0;
      arena.usedBytes = 0;
      arena.highWatermark = 0;
    } else if (arena.activePageIndex >= keepPages) {
      arena.activePageIndex = keepPages - 1u;
    }
  }

  if (stats.resourcesReleased > 0u) {
    VLOG(1) << fmt::format("VK upload-page broker reclaim: pages={} bytes={}B reason='{}'",
                           stats.resourcesReleased,
                           stats.bytesReleased,
                           reason.empty() ? "<unspecified>" : std::string(reason));
  }
  return stats;
}

std::vector<ZVulkanResidencyManager::EvictionCandidate>
Z3DRendererVulkanBackend::staticGeometryEvictionCandidates(const ZVulkanResidencyManager::ReclaimRequest& request) const
{
  std::vector<ZVulkanResidencyManager::EvictionCandidate> candidates;
  if (m_sharedDevice == nullptr || m_staticCacheEpoch == 0) {
    return candidates;
  }

  const uint64_t protectedEpoch = staticGeometryProtectedEpochForRequest(request.force, request.scope);
  auto addCandidate = [&](std::optional<StaticPressureEvictionCandidate> candidate, StaticPressureDomain domain) {
    if (!candidate || candidate->bytes == 0u) {
      return;
    }
    const uint64_t age =
      m_staticCacheEpoch >= candidate->lastUsedEpoch ? (m_staticCacheEpoch - candidate->lastUsedEpoch) : 0u;
    candidates.push_back(ZVulkanResidencyManager::EvictionCandidate{
      .resourceClass = ZVulkanResidencyManager::ResourceClass::StaticGeometry,
      .priority = 0,
      .residentBytes = static_cast<uint64_t>(candidate->bytes),
      .lastUsedEpoch = candidate->lastUsedEpoch,
      .pinCount = 0,
      .restoreAvailable = true,
      .userKey0 = candidate->streamKey,
      .userKey1 = staticCandidateOwnerDomainKey(candidate->owner, domain),
      .label = fmt::format("static_{}_{} stream={}",
                           staticCacheOwnerName(candidate->owner),
                           domain == StaticPressureDomain::Vertex ? "vb" : "ib",
                           candidate->streamKey)});
    VLOG(2) << fmt::format("VK static broker candidate: owner={} domain={} streamKey={} age={} bytes={}B reason='{}'",
                           staticCacheOwnerName(candidate->owner),
                           domain == StaticPressureDomain::Vertex ? "VB" : "IB",
                           candidate->streamKey,
                           age,
                           candidate->bytes,
                           request.reason.empty() ? "<unspecified>" : std::string(request.reason));
  };

  for (StaticPressureDomain domain : {StaticPressureDomain::Vertex, StaticPressureDomain::Index}) {
    if (m_meshContext) {
      addCandidate(m_meshContext->oldestEvictableStaticStream(domain, protectedEpoch), domain);
    }
    if (m_lineContext) {
      addCandidate(m_lineContext->oldestEvictableStaticStream(domain, protectedEpoch), domain);
    }
    if (m_ellipsoidContext) {
      addCandidate(m_ellipsoidContext->oldestEvictableStaticStream(domain, protectedEpoch), domain);
    }
    if (m_sphereContext) {
      addCandidate(m_sphereContext->oldestEvictableStaticStream(domain, protectedEpoch), domain);
    }
    if (m_coneContext) {
      addCandidate(m_coneContext->oldestEvictableStaticStream(domain, protectedEpoch), domain);
    }
  }

  return candidates;
}

ZVulkanResidencyManager::ReclaimStats
Z3DRendererVulkanBackend::evictStaticGeometryCandidate(const ZVulkanResidencyManager::EvictionCandidate& candidate,
                                                       std::string_view reason)
{
  ZVulkanResidencyManager::ReclaimStats stats{};
  if (candidate.resourceClass != ZVulkanResidencyManager::ResourceClass::StaticGeometry || candidate.userKey0 == 0u) {
    return stats;
  }

  const uint64_t streamKey = candidate.userKey0;
  const StaticCacheOwner owner = staticCandidateOwner(candidate.userKey1);
  const StaticPressureDomain domain = staticCandidateDomain(candidate.userKey1);
  const uint64_t residentBytesBefore = staticGeometryMemoryReport().residentBytes;
  size_t virtualBytesFreed = 0;
  switch (owner) {
    case StaticCacheOwner::Mesh:
      if (m_meshContext) {
        virtualBytesFreed = m_meshContext->evictStaticStreamForPressure(streamKey);
      }
      break;
    case StaticCacheOwner::Line:
      if (m_lineContext) {
        virtualBytesFreed = m_lineContext->evictStaticStreamForPressure(streamKey);
      }
      break;
    case StaticCacheOwner::Ellipsoid:
      if (m_ellipsoidContext) {
        virtualBytesFreed = m_ellipsoidContext->evictStaticStreamForPressure(streamKey);
      }
      break;
    case StaticCacheOwner::Sphere:
      if (m_sphereContext) {
        virtualBytesFreed = m_sphereContext->evictStaticStreamForPressure(streamKey);
      }
      break;
    case StaticCacheOwner::Cone:
      if (m_coneContext) {
        virtualBytesFreed = m_coneContext->evictStaticStreamForPressure(streamKey);
      }
      break;
  }

  const uint64_t residentBytesAfter = staticGeometryMemoryReport().residentBytes;
  const uint64_t residentBytesFreed =
    residentBytesBefore > residentBytesAfter ? (residentBytesBefore - residentBytesAfter) : 0u;
  if (virtualBytesFreed == 0u && residentBytesFreed == 0u) {
    return stats;
  }
  stats.resourcesReleased = 1u;
  stats.bytesReleased = residentBytesFreed;
  const uint64_t age =
    m_staticCacheEpoch >= candidate.lastUsedEpoch ? (m_staticCacheEpoch - candidate.lastUsedEpoch) : 0u;
  recordStaticStreamEviction(owner, streamKey, residentBytesFreed, virtualBytesFreed, reason);
  VLOG(1) << fmt::format(
    "VK static broker candidate evict: owner={} domain={} streamKey={} age={} resident_released={}B virtual_released={}B reason='{}'",
    staticCacheOwnerName(owner),
    domain == StaticPressureDomain::Vertex ? "VB" : "IB",
    streamKey,
    age,
    residentBytesFreed,
    virtualBytesFreed,
    reason.empty() ? "<unspecified>" : std::string(reason));
  return stats;
}

ZVulkanResidencyManager::ReclaimStats
Z3DRendererVulkanBackend::reclaimStaticGeometryForMemoryPressure(const ZVulkanResidencyManager::ReclaimRequest& request)
{
  const size_t maxSize = std::numeric_limits<size_t>::max();
  const size_t target = request.requestedBytes == 0u || request.requestedBytes > static_cast<uint64_t>(maxSize)
                          ? maxSize
                          : static_cast<size_t>(request.requestedBytes);

  ZVulkanResidencyManager::ReclaimStats stats{};
  const uint64_t residentBytesBefore = staticGeometryMemoryReport().residentBytes;
  const size_t vbReleased =
    evictColdStaticCacheForPressure(StaticPressureDomain::Vertex, target, request.force, request.scope, request.reason);
  if (vbReleased > 0u) {
    stats.resourcesReleased++;
  }

  size_t remaining = maxSize;
  if (target != maxSize) {
    remaining = vbReleased >= target ? 0u : (target - vbReleased);
  }
  if (remaining > 0u) {
    const size_t ibReleased = evictColdStaticCacheForPressure(StaticPressureDomain::Index,
                                                              remaining,
                                                              request.force,
                                                              request.scope,
                                                              request.reason);
    if (ibReleased > 0u) {
      stats.resourcesReleased++;
    }
  }

  const uint64_t residentBytesAfter = staticGeometryMemoryReport().residentBytes;
  stats.bytesReleased = residentBytesBefore > residentBytesAfter ? (residentBytesBefore - residentBytesAfter) : 0u;
  if (stats.bytesReleased > 0u) {
    VLOG(1) << fmt::format("VK static-geometry broker reclaim: bytes={}B target={}B reason='{}'",
                           stats.bytesReleased,
                           target == maxSize ? 0u : target,
                           request.reason.empty() ? "<unspecified>" : std::string(request.reason));
  }
  return stats;
}

ZVulkanResidencyManager::ReclaimStats
Z3DRendererVulkanBackend::reclaimReadbackStagingForMemoryPressure(std::string_view reason)
{
  ZVulkanResidencyManager::ReclaimStats stats{};
  for (auto& slot : m_readbackSlots) {
    if (slot.inUse || !slot.buffer) {
      continue;
    }
    stats.bytesReleased += slot.capacity;
    stats.resourcesReleased++;
    if (slot.mapped != nullptr) {
      slot.buffer->unmap();
      slot.mapped = nullptr;
    }
    slot.buffer.reset();
    slot.capacity = 0;
  }
  if (stats.resourcesReleased > 0u) {
    VLOG(1) << fmt::format("VK readback-staging broker reclaim: slots={} bytes={}B reason='{}'",
                           stats.resourcesReleased,
                           stats.bytesReleased,
                           reason.empty() ? "<unspecified>" : std::string(reason));
  }
  return stats;
}

ZVulkanResidencyManager::ResourceReport Z3DRendererVulkanBackend::uploadPageMemoryReport() const
{
  ZVulkanResidencyManager::ResourceReport report{};
  report.resourceClass = ZVulkanResidencyManager::ResourceClass::TransientUploadPage;
  report.label = "renderer_upload_pages";
  const bool inFlight = m_sharedDevice != nullptr && m_sharedDevice->frameExecutor().inFlightCount() != 0u;
  for (const auto& frame : m_frames) {
    for (const auto& page : frame.uploadArena.pages) {
      if (!page.buffer || page.capacity == 0u) {
        continue;
      }
      report.residentObjects++;
      report.residentBytes += page.capacity;
    }
    if (inFlight || &frame == m_activeFrame) {
      report.pinnedObjects += static_cast<uint32_t>(
        std::min<size_t>(frame.uploadArena.usedPageCount, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    }
  }
  return report;
}

ZVulkanResidencyManager::ResourceReport Z3DRendererVulkanBackend::scratchBackingMemoryReport() const
{
  ZVulkanResidencyManager::ResourceReport report{};
  report.resourceClass = ZVulkanResidencyManager::ResourceClass::ScratchBacking;
  report.label = "scratch_pool_backing";
  if (currentRenderThreadExecutorOrNull() == nullptr) {
    return report;
  }
  const auto scratchReport = Z3DRenderGlobalState::instance().scratchPool().vulkanScratchBackingReport();
  const bool inFlight = m_sharedDevice != nullptr && m_sharedDevice->frameExecutor().inFlightCount() != 0u;
  report.residentObjects = scratchReport.residentSlots;
  report.pinnedObjects =
    scratchReport.releasePendingSlots + scratchReport.protectedSlots + (inFlight ? scratchReport.inUseSlots : 0u);
  report.residentBytes = scratchReport.residentBytes;
  return report;
}

ZVulkanResidencyManager::ResourceReport Z3DRendererVulkanBackend::staticGeometryMemoryReport() const
{
  ZVulkanResidencyManager::ResourceReport report{};
  report.resourceClass = ZVulkanResidencyManager::ResourceClass::StaticGeometry;
  report.label = "renderer_static_geometry";
  auto addSegments = [&](const auto& segments) {
    for (const auto& segment : segments) {
      if (!segment || !segment->buffer || segment->capacity == 0u) {
        continue;
      }
      report.residentObjects++;
      report.residentBytes += segment->capacity;
      if (segment->pinCount > 0u) {
        report.pinnedObjects++;
      }
    }
  };
  addSegments(m_staticArena.vb);
  addSegments(m_staticArena.ib);
  return report;
}

ZVulkanResidencyManager::ResourceReport Z3DRendererVulkanBackend::readbackStagingMemoryReport() const
{
  ZVulkanResidencyManager::ResourceReport report{};
  report.resourceClass = ZVulkanResidencyManager::ResourceClass::ReadbackStaging;
  report.label = "renderer_readback_staging";
  for (const auto& slot : m_readbackSlots) {
    if (!slot.buffer || slot.capacity == 0u) {
      continue;
    }
    report.residentObjects++;
    report.residentBytes += slot.capacity;
    if (slot.inUse) {
      report.pinnedObjects++;
    }
  }
  return report;
}

void Z3DRendererVulkanBackend::stageCopy(vk::Buffer dst,
                                         vk::DeviceSize dstOffset,
                                         const UploadSlice& src,
                                         bool isIndexBuffer)
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid() || !dst || !src.buffer || src.size == 0) {
    return;
  }
  auto& cmd = m_activeFrameHandle->commandBuffer();
  vk::BufferCopy region{.srcOffset = src.offset, .dstOffset = dstOffset, .size = static_cast<vk::DeviceSize>(src.size)};
  std::array<vk::BufferCopy, 1> regions{region};
  cmd.copyBuffer(src.buffer, dst, regions);
  if (m_activeFrame) {
    m_activeFrame->staticBytesStaged += src.size;
    m_activeFrame->staticStreamRestaged++;
  }

  // Barrier to make the transfer writes visible to vertex input stage.
  // We rely on synchronization2 (1.3 or KHR) already enabled by the context.
  vk::BufferMemoryBarrier2 barrier{};
  barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  barrier.dstStageMask = vk::PipelineStageFlagBits2::eVertexInput;
  barrier.dstAccessMask = isIndexBuffer ? vk::AccessFlagBits2::eIndexRead : vk::AccessFlagBits2::eVertexAttributeRead;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = dst;
  barrier.offset = dstOffset;
  barrier.size = region.size;
  vk::DependencyInfo dep{};
  dep.bufferMemoryBarrierCount = 1;
  dep.pBufferMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);
}

std::unique_ptr<ZVulkanDescriptorSet>
Z3DRendererVulkanBackend::allocateFrameDescriptorSet(vk::DescriptorSetLayout layout)
{
  if (!m_activeFrame) {
    VLOG(1) << "allocateFrameDescriptorSet called with no active frame";
    return {};
  }
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in allocateFrameDescriptorSet";
  ensureArenaOnFrame(*m_activeFrame);
  if (!m_activeFrame->descriptorPool) {
    VLOG(1) << "Descriptor arena missing; allocation skipped";
    return {};
  }
  auto set = m_sharedDevice->createDescriptorSet(*m_activeFrame->descriptorPool, layout);
  if (set) {
    m_activeFrame->descriptorSetsAllocated++;
  }
  return set;
}

std::unique_ptr<ZVulkanDescriptorSet>
Z3DRendererVulkanBackend::allocatePersistentDescriptorSet(vk::DescriptorSetLayout layout)
{
  if (!m_activeFrame) {
    VLOG(1) << "allocatePersistentDescriptorSet called with no active frame";
    return {};
  }
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in allocatePersistentDescriptorSet";
  ensurePersistentArenaOnFrame(*m_activeFrame);
  if (!m_activeFrame->persistentDescriptorPool) {
    VLOG(1) << "Persistent descriptor arena missing; allocation skipped";
    return {};
  }
  auto set = m_sharedDevice->createDescriptorSet(*m_activeFrame->persistentDescriptorPool, layout);
  if (set) {
    m_activeFrame->descriptorSetsAllocated++;
  }
  return set;
}

void Z3DRendererVulkanBackend::flushForTeardown(std::string_view reason)
{
  if (!m_sharedDevice) {
    return;
  }

  if (VLOG_IS_ON(1)) {
    VLOG(1) << fmt::format("VK flushForTeardown begin reason='{}' inFlight={}",
                           reason.empty() ? "<unspecified>" : std::string(reason),
                           m_sharedDevice->frameExecutor().hasInFlightFrames());
  }

  auto unpinUnsubmittedResidency = [&]() {
    if (!m_activeFrame || m_activeFrame->residencyPinnedTextures.empty()) {
      return;
    }
    for (auto* tex : m_activeFrame->residencyPinnedTextures) {
      m_sharedDevice->residencyManager().unpinIfManaged(tex);
    }
    m_activeFrame->residencyPinnedTextures.clear();
  };

  auto unpinUnsubmittedStaticSegments = [&]() {
    if (!m_activeFrame || m_activeFrame->pinnedStaticSegments.empty()) {
      return;
    }
    for (void* segVoid : m_activeFrame->pinnedStaticSegments) {
      auto* seg = static_cast<StaticArena::Segment*>(segVoid);
      CHECK(seg != nullptr) << "Active frame carried a null static segment pin during teardown";
      CHECK_GT(seg->pinCount, 0u) << "Static segment pinCount underflow during teardown";
      seg->pinCount--;
      if (seg->pinCount == 0u) {
        flushPendingFreesAndMaybeTrimStaticSegment(seg);
      }
    }
    m_activeFrame->pinnedStaticSegments.clear();
  };

  auto releaseUnsubmittedExternalResidency = [&]() {
    if (!m_activeFrame || m_activeFrame->externalResidencyPinReleases.empty()) {
      return;
    }
    for (auto& [_, release] : m_activeFrame->externalResidencyPinReleases) {
      CHECK(release) << "External Vulkan residency release callback is empty during teardown";
      release();
    }
    m_activeFrame->externalResidencyPinReleases.clear();
  };

  // If beginRender()/recording failed before endRender() submitted the command
  // buffer, any pins on m_activeFrame are CPU-only reservations. Drop them here
  // so resource-limit exceptions can unwind without leaving owner-destruction
  // callbacks to observe pinned managed textures.
  if (m_activeFrame) {
    unpinUnsubmittedResidency();
    releaseUnsubmittedExternalResidency();
    unpinUnsubmittedStaticSegments();
    m_frameRecording = false;
    m_submissionResourcePinningOpen = false;
    m_activeFrameHandle.reset();
    m_activeFrame = nullptr;
    m_activeRenderer = nullptr;
    m_activePPLLIndex.reset();
    if (s_currentBackend == this) {
      s_currentBackend = nullptr;
    }
  }

  // Wait for all submitted frames to finish so fence-gated callbacks can run safely.
  m_sharedDevice->frameExecutor().waitForAllInFlight();

  // After GPU idle and after all frame-executor completion callbacks ran, it is
  // safe to drain any backend-managed per-frame deferred work (descriptor-pool
  // resets, deferred releases, and coroutines awaiting the frame completion
  // safe point).
  for (auto& frame : m_frames) {
    applyPendingArenaReset(frame);
  }

  // Drain tracked detached tasks (debug readbacks, async post-fence consumers, etc.)
  // so nothing outlives the backend/device during teardown or backend switching.
  if (!m_detachedTaskScopeJoined) {
    folly::coro::blockingWait(m_detachedTaskScope.joinAsync());
    m_detachedTaskScopeJoined = true;
  }

  if (VLOG_IS_ON(1)) {
    VLOG(1) << fmt::format("VK flushForTeardown end reason='{}'",
                           reason.empty() ? "<unspecified>" : std::string(reason));
  }
}

void Z3DRendererVulkanBackend::requireCompletionSafePointWaitForActiveSubmission(std::string_view debugLabel)
{
  CHECK(currentRenderThreadExecutorOrNull() != nullptr)
    << "requireCompletionSafePointWaitForActiveSubmission must be called on the rendering thread"
    << (debugLabel.empty() ? "" : " (") << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : ")");
  CHECK(m_activeFrame != nullptr) << "requireCompletionSafePointWaitForActiveSubmission requires an active frame";
  m_activeFrame->forceFenceWaitForCompletionSafePoint = true;
}

void Z3DRendererVulkanBackend::pollCompletionsAndPumpSafePoints()
{
  CHECK(currentRenderThreadExecutorOrNull() != nullptr)
    << "pollCompletionsAndPumpSafePoints must be called on the rendering thread";
  if (!m_sharedDevice) {
    return;
  }

  m_completedFrameKeysScratch.clear();
  device().frameExecutor().pollCompletions(&m_completedFrameKeysScratch);
  pumpFrameCompletionSafePoints(m_completedFrameKeysScratch);
}

void Z3DRendererVulkanBackend::reclaimTransientResourcesForMemoryPressure(
  Z3DScratchResourcePool::VulkanScratchReclaimMode mode,
  std::string_view reason)
{
  CHECK(currentRenderThreadExecutorOrNull() != nullptr)
    << "reclaimTransientResourcesForMemoryPressure must be called on the rendering thread";
  if (!m_sharedDevice) {
    return;
  }

  if (mode == Z3DScratchResourcePool::VulkanScratchReclaimMode::PollCompleted || m_frameRecording || m_activeFrame) {
    pollCompletionsAndPumpSafePoints();
    return;
  }

  VLOG(1) << fmt::format("VK memory-pressure reclaim: mode={} reason='{}' in_flight={}",
                         static_cast<int>(mode),
                         reason,
                         m_sharedDevice->frameExecutor().inFlightCount());
  m_sharedDevice->frameExecutor().waitForAllInFlight();
  for (auto& frame : m_frames) {
    applyPendingArenaReset(frame);
  }

  m_completedFrameKeysScratch.clear();
  device().frameExecutor().pollCompletions(&m_completedFrameKeysScratch);
  pumpFrameCompletionSafePoints(m_completedFrameKeysScratch);
}

bool Z3DRendererVulkanBackend::hasInFlightFrames() const
{
  return m_sharedDevice != nullptr && m_sharedDevice->frameExecutor().hasInFlightFrames();
}

uint32_t Z3DRendererVulkanBackend::inFlightCount() const
{
  return m_sharedDevice != nullptr ? m_sharedDevice->frameExecutor().inFlightCount() : 0u;
}

uint32_t Z3DRendererVulkanBackend::maxFramesInFlight() const
{
  return m_sharedDevice != nullptr ? m_sharedDevice->frameExecutor().maxFramesInFlight() : 0u;
}

size_t Z3DRendererVulkanBackend::maxMonolithicGeometryStreamBytes() const
{
  // Mesh/geometry segmentation can query this before beginRender() has latched
  // m_sharedDevice on the backend instance. Fall back to the shared scratch-pool
  // device so the hard guard still reflects the real Vulkan limit.
  auto* dev = m_sharedDevice;
  if (dev == nullptr) {
    dev = Z3DRenderGlobalState::instance().scratchPool().vulkanDevice();
  }
  CHECK(dev != nullptr) << "Shared Vulkan device missing in maxMonolithicGeometryStreamBytes";

  const vk::DeviceSize limit = dev->maxMemoryAllocationSize();
  if (limit == 0 || limit >= static_cast<vk::DeviceSize>(std::numeric_limits<size_t>::max())) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(limit);
}

Z3DRendererVulkanBackend::ActiveSubmissionFenceAwaiter
Z3DRendererVulkanBackend::awaitActiveSubmissionFence(std::string_view debugLabel)
{
  (void)debugLabel;
  CHECK(m_sharedDevice != nullptr) << "awaitActiveSubmissionFence requires an initialized Vulkan device";

  // Capture the active frame handle at call time (this function executes on the
  // render thread during recording/submission). The returned awaiter must not
  // depend on mutable backend state (m_activeFrameHandle) because callers may
  // await later from queued coroutines after endRender() resets it.
  auto state = std::make_shared<ActiveSubmissionFenceAwaiter::State>(false);

  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    // No active submission; conservatively wait for all in-flight submissions
    // right now so the awaiter is immediately ready.
    m_sharedDevice->frameExecutor().waitForAllInFlight();
    state->baton.post();
    return ActiveSubmissionFenceAwaiter(std::move(state));
  }

  // Tie completion to the frame-executor slot (not the raw VkFence) so fence
  // reuse/reset remains safe: the executor runs callbacks before reusing a slot.
  device().frameExecutor().scheduleAfterCompletion(*m_activeFrameHandle, [state]() {
    state->baton.post();
  });

  return ActiveSubmissionFenceAwaiter(std::move(state));
}

folly::coro::Task<void> Z3DRendererVulkanBackend::waitActiveSubmissionFence(ActiveSubmissionFenceAwaiter fence)
{
  co_await std::move(fence);

  // folly::coro::Baton resumes awaiters inline on the thread that calls post().
  // Restore executor affinity before returning to user code.
  auto* executor = co_await folly::coro::co_current_executor;
  CHECK(executor != nullptr) << "waitActiveSubmissionFence requires a coroutine with an executor";
  co_await folly::coro::co_reschedule_on_current_executor;
  co_return;
}

void Z3DRendererVulkanBackend::registerAfterCurrentFrameCompletionHook(folly::Executor::KeepAlive<> ex,
                                                                       AfterFrameCompletionHook hook,
                                                                       std::string_view debugLabel)
{
  CHECK(currentRenderThreadExecutorOrNull() != nullptr)
    << "registerAfterCurrentFrameCompletionHook must be called on the rendering thread"
    << (debugLabel.empty() ? "" : " (") << debugLabel << (debugLabel.empty() ? "" : ")");
  CHECK(m_sharedDevice != nullptr) << "registerAfterCurrentFrameCompletionHook requires an initialized Vulkan device";

  CHECK(m_frameInCompletionSafePoint.load(std::memory_order_acquire) == nullptr)
    << "registerAfterCurrentFrameCompletionHook must not be called during the frame completion safe point"
    << " (re-entrant safe-point scheduling is forbidden)" << (debugLabel.empty() ? "" : " label='")
    << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : "'");

  CHECK(m_activeFrame && m_activeFrameHandle && m_activeFrameHandle->valid())
    << "registerAfterCurrentFrameCompletionHook requires an active frame-slot recording context"
    << (debugLabel.empty() ? "" : " label='") << (debugLabel.empty() ? "" : debugLabel)
    << (debugLabel.empty() ? "" : "'");

  m_activeFrame->afterFrameCompletionHooks.registerHook(FrameHookSpot::AfterFrameCompletionSafePoint,
                                                        std::move(ex),
                                                        std::move(hook),
                                                        debugLabel);
}

void Z3DRendererVulkanBackend::spawnDetachedTask(folly::Executor::KeepAlive<> ex,
                                                 folly::coro::Task<void> task,
                                                 std::string_view debugLabel)
{
  CHECK(ex) << "spawnDetachedTask requires a valid executor" << (debugLabel.empty() ? "" : " (")
            << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : ")");
  CHECK(!m_detachedTaskScopeJoined) << "spawnDetachedTask called after flushForTeardown drained the detached task scope"
                                    << (debugLabel.empty() ? "" : " label='") << (debugLabel.empty() ? "" : debugLabel)
                                    << (debugLabel.empty() ? "" : "'");

  std::string label(debugLabel);
  auto wrapperTask =
    folly::coro::co_invoke([label = std::move(label), task = std::move(task)]() mutable -> folly::coro::Task<void> {
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
        LOG(FATAL) << "Detached coroutine task failed" << (label.empty() ? "" : " label='")
                   << (label.empty() ? "" : label) << (label.empty() ? "" : "'") << ": " << e.what();
      }
      catch (...) {
        LOG(FATAL) << "Detached coroutine task failed" << (label.empty() ? "" : " label='")
                   << (label.empty() ? "" : label) << (label.empty() ? "" : "'");
      }
      co_return;
    });

  m_detachedTaskScope.add(folly::coro::co_withExecutor(std::move(ex), std::move(wrapperTask)));
}

void Z3DRendererVulkanBackend::pinTextureForActiveSubmission(ZVulkanTexture* texture)
{
  if (!texture || !m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid() ||
      !m_submissionResourcePinningOpen) {
    return;
  }
  // Deduplicate per submission: we unpin once per unique texture, so only pin
  // on the first insertion. This also avoids pin-count under/overflows.
  const auto [_, inserted] = m_activeFrame->residencyPinnedTextures.insert(texture);
  if (!inserted) {
    return;
  }

  // Only managed textures participate in pinning. If this isn't managed, drop
  // it from the set so we don't carry dead entries.
  if (!device().residencyManager().pinIfManaged(texture)) {
    m_activeFrame->residencyPinnedTextures.erase(texture);
  }
}

void Z3DRendererVulkanBackend::prepareTextureForCommandUse(ZVulkanTexture& texture,
                                                           TextureCommandUse use,
                                                           std::string_view debugLabel)
{
  CHECK(m_sharedDevice != nullptr) << "prepareTextureForCommandUse requires an initialized Vulkan device";
  CHECK(m_activeFrame && m_activeFrameHandle && m_activeFrameHandle->valid())
    << "prepareTextureForCommandUse requires an active frame";
  CHECK(m_frameRecording) << "prepareTextureForCommandUse requires command-buffer recording";

  const std::string_view reason = debugLabel.empty() ? std::string_view("command_texture_use") : debugLabel;
  const bool contentsRequired = use == TextureCommandUse::ReadExistingContents;

  auto& residency = device().residencyManager();
  if (residency.ensureResidentIfManaged(&texture, reason)) {
    pinTextureForActiveSubmission(&texture);
  }

  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();
  const std::array scratchUses{
    Z3DScratchResourcePool::VulkanScratchTextureUse{.texture = &texture, .contentsRequired = contentsRequired}
  };
  scratchPool.prepareVulkanScratchTexturesForPass(
    std::span<const Z3DScratchResourcePool::VulkanScratchTextureUse>(scratchUses.data(), scratchUses.size()),
    reason);

  const std::array scratchTextures{&texture};
  auto scratchProtection = scratchPool.protectVulkanScratchTextures(
    std::span<ZVulkanTexture* const>(scratchTextures.data(), scratchTextures.size()));
  if (scratchProtection.active()) {
    auto sharedProtection =
      std::make_shared<Z3DScratchResourcePool::VulkanScratchProtectionScope>(std::move(scratchProtection));
    (void)pinExternalResidencyResourceForActiveSubmission(static_cast<const void*>(&texture),
                                                          [sharedProtection = std::move(sharedProtection)]() mutable {
                                                            sharedProtection.reset();
                                                          });
  }

  CHECK(texture.resident()) << "Vulkan command texture is not resident after backend preparation"
                            << (debugLabel.empty() ? "" : " label='") << (debugLabel.empty() ? "" : debugLabel)
                            << (debugLabel.empty() ? "" : "'");
}

void Z3DRendererVulkanBackend::clearColorTextureToShaderReadOnly(ZVulkanTexture& texture,
                                                                 const vk::ClearColorValue& clear,
                                                                 const vk::ImageSubresourceRange& range,
                                                                 std::string_view debugLabel)
{
  prepareTextureForCommandUse(texture, TextureCommandUse::DiscardAndWrite, debugLabel);

  auto& cmd = commandBuffer();
  texture.transitionLayout(cmd, texture.layout(), vk::ImageLayout::eTransferDstOptimal, range.aspectMask);
  cmd.clearColorImage(texture.image(), vk::ImageLayout::eTransferDstOptimal, clear, range);
  texture.transitionLayout(cmd,
                           vk::ImageLayout::eTransferDstOptimal,
                           vk::ImageLayout::eShaderReadOnlyOptimal,
                           range.aspectMask);
  texture.setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
}

bool Z3DRendererVulkanBackend::pinExternalResidencyResourceForActiveSubmission(const void* key,
                                                                               std::function<void()> release)
{
  if (key == nullptr || !m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid() ||
      !m_submissionResourcePinningOpen) {
    return false;
  }
  CHECK(release) << "External Vulkan residency pin requires a release callback";

  const auto [_, inserted] = m_activeFrame->externalResidencyPinReleases.emplace(key, std::move(release));
  return inserted;
}

vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer()
{
  CHECK(m_activeFrameHandle && m_activeFrameHandle->valid()) << "Command buffer requested outside active frame";
  CHECK(m_frameRecording) << "Command buffer requested while not recording";
  return m_activeFrameHandle->commandBuffer();
}

const vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer() const
{
  CHECK(m_activeFrameHandle && m_activeFrameHandle->valid()) << "Command buffer requested outside active frame";
  CHECK(m_frameRecording) << "Command buffer requested while not recording";
  return m_activeFrameHandle->commandBuffer();
}

ZVulkanBuffer& Z3DRendererVulkanBackend::fullscreenQuadVertexBuffer()
{
  ensureFullscreenQuad();
  return *m_fullscreenQuadVbo;
}

void Z3DRendererVulkanBackend::ensureDevice()
{
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  auto* dev = pool.vulkanDevice();
  CHECK(dev != nullptr) << "Shared Vulkan device not injected into scratch pool";
  if (m_sharedDevice != dev) {
    uninstallMemoryBrokerProviders();
    m_sharedDescriptorLayouts = {};
    m_sharedDevice = dev;
    m_deviceRevision++;
    m_cachedUniformAlignment = 0;
    resetFrameResources();
    // Cache device feature gates for DDP and fragment SSBO writes
    try {
      auto features2 = m_sharedDevice->context()
                         .physicalDevice()
                         .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
      const auto& f1 = features2.get<vk::PhysicalDeviceFeatures2>().features;
      const auto& f12 = features2.get<vk::PhysicalDeviceVulkan12Features>();
      m_supportsFragStoresAndAtomics = (f1.fragmentStoresAndAtomics == VK_TRUE);
      m_supportsDrawIndirectCount = (f12.drawIndirectCount == VK_TRUE);
      VLOG(1) << fmt::format("VK device gates: fragStoresAndAtomics={} drawIndirectCount={}",
                             m_supportsFragStoresAndAtomics,
                             m_supportsDrawIndirectCount);
    }
    catch (...) {
      m_supportsFragStoresAndAtomics = false;
      m_supportsDrawIndirectCount = false;
    }
    // Refresh timestamp period from the new physical device (ns per tick)
    try {
      auto& phys = m_sharedDevice->context().physicalDevice();
      auto props = phys.getProperties();
      m_timestampPeriod = props.limits.timestampPeriod; // nanoseconds per tick
      if (m_timestampPeriod <= 0.0f) {
        m_timestampPeriod = 1.0f;
      }
    }
    catch (...) {
      m_timestampPeriod = 1.0f;
    }
    installMemoryBrokerProviders();
  }
  installMemoryBrokerProviders();
  // Keep local ring sizes aligned with the device executor setting.
  m_maxFramesInFlight = m_sharedDevice->frameExecutor().maxFramesInFlight();
  if (m_completedFrameKeysScratch.capacity() < m_maxFramesInFlight) {
    m_completedFrameKeysScratch.reserve(m_maxFramesInFlight);
  }
}

void* Z3DRendererVulkanBackend::activeFrameKey() const
{
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return nullptr;
  }
  return m_activeFrameHandle->key();
}

void Z3DRendererVulkanBackend::resetFrameResources()
{
  m_activeFrameHandle.reset();
  m_activeFrame = nullptr;
  m_lastSubmittedFrameKey = nullptr;
  m_frameRecording = false;
  m_submissionResourcePinningOpen = false;
  m_frames.clear();
  m_frameResourceMap.clear();
  m_frameDevice = nullptr;
}

Z3DRendererVulkanBackend::FrameResources& Z3DRendererVulkanBackend::ensureFrameResourcesForKey(void* key)
{
  ensureDevice();
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing";

  if (m_frameDevice != m_sharedDevice) {
    m_frames.clear();
    m_frameResourceMap.clear();
    m_frameDevice = m_sharedDevice;
  }

  auto iter = m_frameResourceMap.find(key);
  if (iter != m_frameResourceMap.end()) {
    return m_frames[iter->second];
  }

  m_frames.emplace_back();
  const size_t index = m_frames.size() - 1;
  m_frameResourceMap.emplace(key, index);

  auto& frame = m_frames.back();
  auto& vkDevice = m_sharedDevice->context().device();
  vk::QueryPoolCreateInfo queryInfo{.queryType = vk::QueryType::eTimestamp, .queryCount = kMaxTimestampQueries};
  frame.queryPool = vk::raii::QueryPool(vkDevice, queryInfo);
  frame.descriptorPool = m_sharedDevice->createDescriptorPool();
  frame.persistentDescriptorPool = m_sharedDevice->createDescriptorPool();
  return frame;
}

void Z3DRendererVulkanBackend::ensureArenaOnFrame(FrameResources& frame)
{
  if (!frame.descriptorPool) {
    frame.descriptorPool = m_sharedDevice->createDescriptorPool();
  }
}

void Z3DRendererVulkanBackend::ensurePersistentArenaOnFrame(FrameResources& frame)
{
  if (!frame.persistentDescriptorPool) {
    frame.persistentDescriptorPool = m_sharedDevice->createDescriptorPool();
  }
}

void Z3DRendererVulkanBackend::ensureBindlessSampledImagesOnFrame(FrameResources& frame)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureBindlessSampledImagesOnFrame";
  ensureSharedDescriptorLayouts();
  ensurePersistentArenaOnFrame(frame);

  if (frame.bindlessSampledImages) {
    return;
  }

  const vk::DescriptorSetLayout layout = bindlessSampledImageDescriptorSetLayout();
  CHECK(static_cast<VkDescriptorSetLayout>(layout) != VK_NULL_HANDLE) << "Bindless descriptor set layout missing";
  CHECK(frame.persistentDescriptorPool) << "Persistent descriptor pool missing for bindless allocation";
  vk::DescriptorSet raw = frame.persistentDescriptorPool->allocateDescriptorSet(layout);
  CHECK(static_cast<VkDescriptorSet>(raw) != VK_NULL_HANDLE) << "Failed to allocate bindless descriptor set";

  const auto& caps = m_sharedDevice->context().effectiveBindlessSampledImageCapacities();
  frame.bindlessSampledImages = std::make_unique<ZVulkanBindlessDescriptorSet>(*m_sharedDevice,
                                                                               raw,
                                                                               caps.texture2D,
                                                                               caps.texture2DArray,
                                                                               caps.texture3D,
                                                                               caps.uTexture2D,
                                                                               caps.uTexture3D);

  // Reserve index 0 in every bindless table for well-defined placeholder
  // sampling. This allows shaders to use index=0 as "no texture" without
  // relying on partially-bound behavior for out-of-range indices.
  ensureDefaultPlaceholders();

  {
    ZVulkanBindlessDescriptorSet::RegisterRequest req{};
    req.kind = ZVulkanBindlessDescriptorSet::Kind::Texture2D;
    req.texture = m_defaultPlaceholder2D.get();
    req.debugLabel = "bindless_placeholder_2d";
    const uint32_t idx = frame.bindlessSampledImages->registerTexture(req);
    CHECK(idx == 0u) << "Bindless placeholder 2D expected to reserve index 0 (got " << idx << ")";
  }
  {
    ZVulkanBindlessDescriptorSet::RegisterRequest req{};
    req.kind = ZVulkanBindlessDescriptorSet::Kind::Texture2DArray;
    req.texture = m_defaultPlaceholder2DArray.get();
    req.debugLabel = "bindless_placeholder_2darray";
    const uint32_t idx = frame.bindlessSampledImages->registerTexture(req);
    CHECK(idx == 0u) << "Bindless placeholder 2DArray expected to reserve index 0 (got " << idx << ")";
  }
  {
    ZVulkanBindlessDescriptorSet::RegisterRequest req{};
    req.kind = ZVulkanBindlessDescriptorSet::Kind::Texture3D;
    req.texture = m_defaultPlaceholder3D.get();
    req.debugLabel = "bindless_placeholder_3d";
    const uint32_t idx = frame.bindlessSampledImages->registerTexture(req);
    CHECK(idx == 0u) << "Bindless placeholder 3D expected to reserve index 0 (got " << idx << ")";
  }
  {
    ZVulkanBindlessDescriptorSet::RegisterRequest req{};
    req.kind = ZVulkanBindlessDescriptorSet::Kind::UTexture2D;
    req.texture = m_defaultPlaceholderU2D.get();
    req.debugLabel = "bindless_placeholder_u2d";
    const uint32_t idx = frame.bindlessSampledImages->registerTexture(req);
    CHECK(idx == 0u) << "Bindless placeholder u2D expected to reserve index 0 (got " << idx << ")";
  }
  {
    ZVulkanBindlessDescriptorSet::RegisterRequest req{};
    req.kind = ZVulkanBindlessDescriptorSet::Kind::UTexture3D;
    req.texture = m_defaultPlaceholderU3D.get();
    req.debugLabel = "bindless_placeholder_u3d";
    const uint32_t idx = frame.bindlessSampledImages->registerTexture(req);
    CHECK(idx == 0u) << "Bindless placeholder u3D expected to reserve index 0 (got " << idx << ")";
  }
}

void Z3DRendererVulkanBackend::ensureSharedDescriptorSetsOnFrame(FrameResources& frame)
{
  CHECK(!isRecording()) << "ensureSharedDescriptorSetsOnFrame called while recording";
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureSharedDescriptorSetsOnFrame";
  ensureSharedDescriptorLayouts();
  ensurePersistentArenaOnFrame(frame);

  const vk::DescriptorSetLayout emptyLayout = emptyDescriptorSetLayout();
  CHECK(emptyLayout) << "Shared empty descriptor set layout missing";
  if (!frame.sharedEmpty) {
    frame.sharedEmpty = allocatePersistentDescriptorSet(emptyLayout);
    CHECK(frame.sharedEmpty != nullptr) << "Failed to allocate shared empty descriptor set";
  }

  const vk::DescriptorSetLayout lightingLayout = lightingDescriptorSetLayout();
  CHECK(lightingLayout) << "Shared lighting descriptor set layout missing";
  if (!frame.sharedLighting) {
    frame.sharedLighting = allocatePersistentDescriptorSet(lightingLayout);
    CHECK(frame.sharedLighting != nullptr) << "Failed to allocate shared lighting descriptor set";
  }

  const vk::DescriptorSetLayout transformsLayout = transformDescriptorSetLayout();
  CHECK(transformsLayout) << "Shared transforms descriptor set layout missing";
  if (!frame.sharedTransformsUniform) {
    frame.sharedTransformsUniform = allocatePersistentDescriptorSet(transformsLayout);
    CHECK(frame.sharedTransformsUniform != nullptr) << "Failed to allocate shared uniform transforms descriptor set";
  }
  if (!frame.sharedTransformsPersistent) {
    frame.sharedTransformsPersistent = allocatePersistentDescriptorSet(transformsLayout);
    CHECK(frame.sharedTransformsPersistent != nullptr)
      << "Failed to allocate shared persistent transforms descriptor set";
  }

  const vk::DescriptorSetLayout oitLayout = oitDescriptorSetLayout();
  CHECK(oitLayout) << "Shared OIT descriptor set layout missing";
  const size_t oitRingSize = std::max<size_t>(1, m_ppllFrameRing.size());
  if (frame.sharedOITByRing.size() != oitRingSize) {
    frame.sharedOITByRing.resize(oitRingSize);
  }
  for (size_t i = 0; i < frame.sharedOITByRing.size(); ++i) {
    if (frame.sharedOITByRing[i]) {
      continue;
    }
    frame.sharedOITByRing[i] = allocatePersistentDescriptorSet(oitLayout);
    CHECK(frame.sharedOITByRing[i] != nullptr) << "Failed to allocate shared OIT descriptor set";
  }

  const vk::DescriptorSetLayout imgRaySetupLayout = imgRaySetupDescriptorSetLayout();
  CHECK(imgRaySetupLayout) << "Shared image ray-setup descriptor set layout missing";
  if (!frame.sharedImgRaySetup) {
    frame.sharedImgRaySetup = allocatePersistentDescriptorSet(imgRaySetupLayout);
    CHECK(frame.sharedImgRaySetup != nullptr) << "Failed to allocate shared image ray-setup descriptor set";
  }

  const vk::DescriptorSetLayout imgIndicesLayout = imgIndicesDescriptorSetLayout();
  CHECK(imgIndicesLayout) << "Shared image indices descriptor set layout missing";
  if (!frame.sharedImgIndices) {
    frame.sharedImgIndices = allocatePersistentDescriptorSet(imgIndicesLayout);
    CHECK(frame.sharedImgIndices != nullptr) << "Failed to allocate shared image indices descriptor set";
  }

  const vk::DescriptorSetLayout imgPageDataLayout = imgPageDataDescriptorSetLayout();
  CHECK(imgPageDataLayout) << "Shared image page-data descriptor set layout missing";
  if (!frame.sharedImgPageData) {
    frame.sharedImgPageData = allocatePersistentDescriptorSet(imgPageDataLayout);
    CHECK(frame.sharedImgPageData != nullptr) << "Failed to allocate shared image page-data descriptor set";
  }

  // Update shared dynamic UBO bindings to point at the current arena buffers.
  // These updates are no-ops when the underlying VkBuffer + range are unchanged.
  CHECK(m_activeFrame != nullptr) << "Shared descriptor set priming requires an active frame";

  // Lighting: binding 0 = per-frame lighting UBO.
  frame.sharedLighting->updateUniformBufferDynamic(0, uniformArenaBuffer(), sizeof(LightingUBOStd140));

  // Transforms (uniform variant): bindings {0,1,2} all point at the per-frame uniform arena.
  frame.sharedTransformsUniform->updateUniformBufferDynamic(0, uniformArenaBuffer(), sizeof(FrameTransformsUBOStd140));
  frame.sharedTransformsUniform->updateUniformBufferDynamic(1, uniformArenaBuffer(), sizeof(ObjectTransformsUBOStd140));
  frame.sharedTransformsUniform->updateUniformBufferDynamic(2, uniformArenaBuffer(), sizeof(MaterialUBOStd140));

  // Transforms (persistent variant): binding 0 uses per-frame arena; bindings 1/2
  // use the persistent uniform arena for stable offsets across frames.
  frame.sharedTransformsPersistent->updateUniformBufferDynamic(0,
                                                               uniformArenaBuffer(),
                                                               sizeof(FrameTransformsUBOStd140));
  frame.sharedTransformsPersistent->updateUniformBufferDynamic(1,
                                                               persistentUniformArenaBuffer(),
                                                               sizeof(ObjectTransformsUBOStd140));
  frame.sharedTransformsPersistent->updateUniformBufferDynamic(2,
                                                               persistentUniformArenaBuffer(),
                                                               sizeof(MaterialUBOStd140));

  // OIT: bind to the current backend-owned SSBO set (PPLL/DDP). Buffers may
  // change across frames (ring slots / resizing), so update at the safe point.
  const uint32_t oitRingIndex = sharedOITDescriptorSetRingIndex();
  CHECK(!frame.sharedOITByRing.empty()) << "Shared OIT descriptor set(s) missing on active frame-slot";
  CHECK(static_cast<size_t>(oitRingIndex) < frame.sharedOITByRing.size())
    << "Shared OIT descriptor set ring index out of range while priming";
  primeOITDescriptorSet(*frame.sharedOITByRing[oitRingIndex]);

  // Image helpers: indices + page-data UBO views over the uniform arena. All
  // users of the backend-shared image-indices descriptor bind through one 32B
  // std140 dynamic UBO range. Some fast shaders only read the leading fields,
  // but suballocations and budgets must still satisfy the full 32B range.
  frame.sharedImgRaySetup->updateUniformBufferDynamic(0, uniformArenaBuffer(), sizeof(ImgRaySetupUBOStd140));

  constexpr vk::DeviceSize kIndicesRange = sizeof(uint32_t) * 8u; // std140 padded (32B)
  frame.sharedImgIndices->updateUniformBufferDynamic(0, uniformArenaBuffer(), kIndicesRange);

  const auto limits = device().context().physicalDevice().getProperties().limits;
  const size_t arenaBytes = uniformArenaBuffer().size();
  const size_t maxUbo = static_cast<size_t>(limits.maxUniformBufferRange);
  const vk::DeviceSize pageRange = static_cast<vk::DeviceSize>(std::min(arenaBytes, maxUbo));
  CHECK_GT(pageRange, 0u) << "Shared image page-data UBO range computed as zero";
  frame.sharedImgPageData->updateUniformBufferDynamic(2, uniformArenaBuffer(), pageRange);
}

void Z3DRendererVulkanBackend::applyPendingArenaReset(FrameResources& frame)
{
  // We are at the "frame completion safe point" for this frame-slot: the
  // backend has observed that the slot's previous submission fence is complete
  // (either because the executor waited for the fence before reusing the slot,
  // because the backend performed an explicit wait-for-completion, or because
  // pollCompletions() observed the fence as signaled). In all cases, it is now
  // safe to run work that must observe the end of the previous submission and
  // then start a new generation for this slot.

  if (frame.arenaResetScheduled) {
    CHECK(frame.descriptorPool) << "Descriptor pool missing while reset was scheduled";
    frame.descriptorPool->reset();
    frame.arenaResetScheduled = false;
    frame.arenaResetsPerformed++;
  }

  // Safe point cleanup: drop any retired DDP device-args buffers that were kept
  // alive until the previous submission fence completed.
  frame.ddpArgsDevice.retiredBuffers.clear();

  // Completion safe point: the frame slot's fence is complete, so submission
  // pins are safe to release before user completion hooks allocate readback,
  // staging, restored textures, or promoted geometry.
  if (!frame.residencyPinnedTextures.empty()) {
    CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing while releasing residency pins";
    for (auto* tex : frame.residencyPinnedTextures) {
      m_sharedDevice->residencyManager().unpinIfManaged(tex);
    }
    VLOG(1) << fmt::format("VK completion safe point released texture pins: count={}",
                           frame.residencyPinnedTextures.size());
    frame.residencyPinnedTextures.clear();
  }

  if (!frame.externalResidencyPinReleases.empty()) {
    const size_t releaseCount = frame.externalResidencyPinReleases.size();
    for (auto& [_, release] : frame.externalResidencyPinReleases) {
      CHECK(release) << "External Vulkan residency release callback is empty";
      release();
    }
    VLOG(1) << fmt::format("VK completion safe point released external residency pins: count={}", releaseCount);
    frame.externalResidencyPinReleases.clear();
  }

  if (!frame.pinnedStaticSegments.empty()) {
    for (void* segVoid : frame.pinnedStaticSegments) {
      auto* seg = static_cast<StaticArena::Segment*>(segVoid);
      CHECK(seg != nullptr) << "Static segment pin carried a null segment";
      CHECK_GT(seg->pinCount, 0u) << "Static segment pinCount underflow";
      seg->pinCount--;
      if (seg->pinCount == 0u) {
        flushPendingFreesAndMaybeTrimStaticSegment(seg);
      }
    }
    VLOG(1) << fmt::format("VK completion safe point released static pins: count={}",
                           frame.pinnedStaticSegments.size());
    frame.pinnedStaticSegments.clear();
  }

  // Barrier: execute backend resource-release hooks first, then user completion
  // hooks for this frame-slot completion safe point.
  //
  // We capture and rethrow after waking awaiters so cancellation/errors do not
  // strand coroutines waiting on the frame completion signal.
  FrameResources* previousSafePointFrame = m_frameInCompletionSafePoint.exchange(&frame, std::memory_order_acq_rel);

  std::exception_ptr deferredException;
  auto reachSpot = [&](FrameHookSpot spot) {
    try {
      folly::coro::blockingWait(frame.afterFrameCompletionHooks.reach(spot, *this));
    }
    catch (...) {
      if (!deferredException) {
        deferredException = std::current_exception();
      }
    }
  };
  reachSpot(FrameHookSpot::AfterFrameResourceRelease);
  reachSpot(FrameHookSpot::AfterFrameCompletionSafePoint);

  m_frameInCompletionSafePoint.store(previousSafePointFrame, std::memory_order_release);

  // Timing ingestion: once we've reached the completion safe point barrier,
  // any fence-gated consumers (notably compositor readback publish + end-to-end
  // `all` latency measurement) have finished, and it is now safe to ingest this
  // submission into Z3DPerfCollector.
  //
  // Important: do this even if a hook threw (we rethrow below) so the perf
  // collector doesn't stall ordered summaries on missing submissions.
  collectFrameTimings(frame);

  if (deferredException) {
    std::rethrow_exception(deferredException);
  }
}

void Z3DRendererVulkanBackend::recordAllMsForCompletionSafePoint(double milliseconds)
{
  FrameResources* frame = m_frameInCompletionSafePoint.load(std::memory_order_acquire);
  CHECK(frame != nullptr) << "recordAllMsForCompletionSafePoint must be called during the completion safe point";
  if (milliseconds < 0.0) {
    return;
  }

  frame->allSamples++;
  if (!frame->allMaxMs.has_value() || milliseconds > *frame->allMaxMs) {
    frame->allMaxMs = milliseconds;
  }
}

void Z3DRendererVulkanBackend::pumpFrameCompletionSafePoints(const std::vector<void*>& completedFrameKeys)
{
  if (completedFrameKeys.empty()) {
    return;
  }

  // pollCompletions() operates on the device-owned frame-executor slots. Atlas'
  // backend resources are keyed by the slot key (ActiveFrame::key()). Some
  // Vulkan subsystems may use the shared executor without participating in the
  // backend's per-frame resource map, so treat unknown keys as benign and skip.
  for (void* key : completedFrameKeys) {
    auto it = m_frameResourceMap.find(key);
    if (it == m_frameResourceMap.end()) {
      continue;
    }
    CHECK_LT(it->second, m_frames.size()) << "Frame resource map index out of bounds";
    applyPendingArenaReset(m_frames[it->second]);
  }
}

void Z3DRendererVulkanBackend::scheduleArenaReset(FrameResources& frame)
{
  // Debug guard: should schedule exactly once per frame
  CHECK(!frame.arenaResetScheduled) << "Descriptor arena reset scheduled more than once for the same frame";
  frame.arenaResetScheduled = true;
}

void Z3DRendererVulkanBackend::vlogFrameRecyclingStats(const FrameResources& frame) const
{
  VLOG(1) << fmt::format("VK frame: frame='{}' token={} submit#{} descriptor_sets={} arena_resets={}",
                         frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
                         frame.realFrameToken,
                         frame.submissionId,
                         frame.descriptorSetsAllocated,
                         frame.arenaResetsPerformed);
}

const std::optional<vulkan::AttachmentFormats>& Z3DRendererVulkanBackend::currentSegmentFormats() const
{
  static const std::optional<vulkan::AttachmentFormats> kNone;
  if (!m_activeFrame) {
    return kNone;
  }
  return m_activeFrame->activeSegmentFormats;
}

void Z3DRendererVulkanBackend::validateFormatsOrCrash(const vulkan::AttachmentFormats& pipelineFormats,
                                                      /*nullable*/ const char* contextTag)
{
  if (!m_activeFrame) {
    return;
  }
  const auto& seg = m_activeFrame->activeSegmentFormats;
  if (!seg) {
    return;
  }
  const bool depthMatch = seg->depthFormat == pipelineFormats.depthFormat;
  const bool colorMatch = seg->colorFormats == pipelineFormats.colorFormats;
  if (depthMatch && colorMatch) {
    return;
  }
  m_activeFrame->skippedBatchesFormatMismatch++;
  const std::string ctx = (contextTag && *contextTag) ? fmt::format(" [{}]", contextTag) : std::string();
  LOG(ERROR) << fmt::format("VK: format mismatch{}: seg colors={} depth={}, pipeline colors={} depth={}",
                            ctx,
                            seg->colorFormats.size(),
                            seg->depthFormat.has_value(),
                            pipelineFormats.colorFormats.size(),
                            pipelineFormats.depthFormat.has_value());
  CHECK(false) << "Vulkan dynamic rendering segment/pipeline format mismatch (fatal).";
}

void Z3DRendererVulkanBackend::ensureFullscreenQuad()
{
  if (m_fullscreenQuadVbo) {
    return;
  }
  ensureDevice();
  auto& dev = device();
  struct QuadVertex
  {
    glm::vec3 pos;
  };
  // Use far-plane depth for fullscreen passes that don't explicitly disable
  // depth writes, so scene geometry isn't occluded by the background.
  constexpr float z = 1.0f - 1e-5f;
  const std::array<QuadVertex, 4> vertices{QuadVertex{glm::vec3(-1.0f, 1.0f, z)},
                                           QuadVertex{glm::vec3(-1.0f, -1.0f, z)},
                                           QuadVertex{glm::vec3(1.0f, 1.0f, z)},
                                           QuadVertex{glm::vec3(1.0f, -1.0f, z)}};
  const size_t bytes = vertices.size() * sizeof(QuadVertex);
  m_fullscreenQuadVbo =
    dev.createBuffer(bytes,
                     vk::BufferUsageFlagBits::eVertexBuffer,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_fullscreenQuadVbo->copyData(vertices.data(), bytes);
}

void Z3DRendererVulkanBackend::ensureDummyVertexBuffer()
{
  if (m_dummyVertexBuffer) {
    return;
  }
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureDummyVertexBuffer";
  struct Dummy
  {
    uint32_t x;
  } dummy{0u};
  const size_t bytes = sizeof(Dummy);
  m_dummyVertexBuffer =
    m_sharedDevice->createBuffer(bytes,
                                 vk::BufferUsageFlagBits::eVertexBuffer,
                                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_dummyVertexBuffer->copyData(&dummy, bytes);
}

vk::Buffer Z3DRendererVulkanBackend::dummyVertexBuffer()
{
  ensureDummyVertexBuffer();
  return m_dummyVertexBuffer ? m_dummyVertexBuffer->buffer() : VK_NULL_HANDLE;
}

void Z3DRendererVulkanBackend::ensureReadbackSlots(size_t minBytes, uint32_t minSlots)
{
  const uint32_t desired = std::max<uint32_t>(minSlots, std::max<uint32_t>(m_maxFramesInFlight, 2));
  if (m_readbackSlots.size() < desired) {
    m_readbackSlots.resize(desired);
  }
  for (auto& slot : m_readbackSlots) {
    // Never replace a buffer that is currently in use. Callers may legally
    // retain the mapped pointer (zero-copy display path) across multiple frames.
    if (slot.inUse) {
      continue;
    }
    if (!slot.buffer || slot.capacity < minBytes) {
      auto newBuffer = device().createBufferInPool(minBytes,
                                                   vk::BufferUsageFlagBits::eTransferDst,
                                                   vk::MemoryPropertyFlagBits::eHostVisible |
                                                     vk::MemoryPropertyFlagBits::eHostCoherent |
                                                     vk::MemoryPropertyFlagBits::eHostCached,
                                                   device().readbackStagingPool());
      void* newMapped = newBuffer->map(0, minBytes);
      if (slot.buffer && slot.mapped != nullptr) {
        slot.buffer->unmap();
      }
      slot.buffer = std::move(newBuffer);
      slot.capacity = minBytes;
      slot.mapped = newMapped;
      slot.inUse = false;
      slot.tag = "color";
    }
  }
}

int Z3DRendererVulkanBackend::acquireReadbackSlot(size_t requiredBytes)
{
  // Ensure baseline capacity: at least 3 slots (previous pinned + current color + picking)
  if (m_readbackSlots.empty()) {
    ensureReadbackSlots(requiredBytes,
                        std::max<uint32_t>(3u, std::max<uint32_t>(m_maxFramesInFlight + 1u, static_cast<uint32_t>(2))));
  } else {
    ensureReadbackSlots(requiredBytes, static_cast<uint32_t>(m_readbackSlots.size()));
  }
  auto tryAcquire = [&](size_t n) -> int {
    for (size_t i = 0; i < n; ++i) {
      const size_t idx = (m_readbackCursor + i) % n;
      if (!m_readbackSlots[idx].inUse && m_readbackSlots[idx].capacity >= requiredBytes) {
        m_readbackSlots[idx].inUse = true;
        m_readbackCursor = static_cast<uint32_t>((idx + 1) % n);
        return static_cast<int>(idx);
      }
    }
    return -1;
  };

  size_t n = m_readbackSlots.size();
  int idx = tryAcquire(n);
  if (idx >= 0) {
    return idx;
  }
  // All slots busy; grow by one and retry.
  ensureReadbackSlots(requiredBytes, static_cast<uint32_t>(n + 1));
  n = m_readbackSlots.size();
  idx = tryAcquire(n);
  return idx;
}

void Z3DRendererVulkanBackend::releaseReadbackSlot(int index)
{
  if (index < 0) {
    return;
  }
  const size_t idx = static_cast<size_t>(index);
  if (idx >= m_readbackSlots.size()) {
    return;
  }
  m_readbackSlots[idx].inUse = false;
}

namespace {
size_t bytesPerPixelForReadback(vk::Format f)
{
  switch (f) {
    case vk::Format::eR8Unorm:
    case vk::Format::eR8Snorm:
      return 1u;
    case vk::Format::eR8G8Unorm:
    case vk::Format::eR8G8Snorm:
    case vk::Format::eR16Unorm:
    case vk::Format::eR16Snorm:
    case vk::Format::eR16Sfloat:
    case vk::Format::eD16Unorm:
      return 2u;
    case vk::Format::eR8G8B8Unorm:
    case vk::Format::eR8G8B8Srgb:
    case vk::Format::eR8G8B8Snorm:
    case vk::Format::eB8G8R8Unorm:
    case vk::Format::eB8G8R8Srgb:
      return 3u;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eR8G8B8A8Snorm:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eR32Sfloat:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eX8D24UnormPack32:
      return 4u;
    case vk::Format::eR16G16Unorm:
    case vk::Format::eR16G16Snorm:
    case vk::Format::eR16G16Sfloat:
      return 4u;
    case vk::Format::eR16G16B16Unorm:
    case vk::Format::eR16G16B16Snorm:
      return 6u;
    case vk::Format::eR16G16B16A16Unorm:
    case vk::Format::eR16G16B16A16Snorm:
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32Sfloat:
      return 8u;
    case vk::Format::eR32G32B32Sfloat:
      return 12u;
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eR32G32B32A32Uint:
      return 16u;
    default:
      CHECK(false) << "Unsupported Vulkan readback format: " << enumOrUnderlying(f, 16);
      return 0u;
  }
}
} // namespace

Z3DRendererVulkanBackend::EndOfFrameColorReadbackTicket
Z3DRendererVulkanBackend::requestEndOfFrameColorReadbackTicket(ZVulkanTexture& src,
                                                               Z3DEye eye,
                                                               std::string_view debugLabel)
{
  return requestEndOfFrameImageReadbackTicket(src,
                                              eye,
                                              /*arrayLayer=*/0u,
                                              vk::ImageAspectFlagBits::eColor,
                                              debugLabel);
}

Z3DRendererVulkanBackend::EndOfFrameColorReadbackTicket
Z3DRendererVulkanBackend::requestEndOfFrameImageReadbackTicket(ZVulkanTexture& src,
                                                               Z3DEye eye,
                                                               uint32_t arrayLayer,
                                                               vk::ImageAspectFlags aspectMask,
                                                               std::string_view debugLabel)
{
  CHECK(currentRenderThreadExecutorOrNull() != nullptr)
    << "requestEndOfFrameImageReadbackTicket must be called on the rendering thread" << (debugLabel.empty() ? "" : " (")
    << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : ")");
  CHECK(m_activeFrame && m_activeFrameHandle && m_activeFrameHandle->valid())
    << "VK image readback requested outside of an active frame";
  prepareTextureForCommandUse(src,
                              TextureCommandUse::ReadExistingContents,
                              debugLabel.empty() ? std::string_view("vk_image_readback") : debugLabel);

  const glm::uvec2 size{src.width(), src.height()};
  const vk::Format fmt = src.format();
  const size_t bytes = static_cast<size_t>(size.x) * size.y * bytesPerPixelForReadback(fmt);
  const int slotIndex = acquireReadbackSlot(bytes);
  CHECK(slotIndex >= 0) << "VK readback slot unavailable (bytes=" << bytes << ")";
  CHECK(static_cast<size_t>(slotIndex) < m_readbackSlots.size());

  auto& slot = m_readbackSlots[static_cast<size_t>(slotIndex)];
  slot.tag = "image";
  const void* mapped = slot.mapped;
  CHECK(mapped != nullptr) << "VK readback mapped pointer is null";

  // Resolve array layer. For 3D images, Vulkan expects baseArrayLayer=0.
  const uint32_t resolvedLayer = (src.info().imageType == vk::ImageType::e3D)
                                   ? 0u
                                   : std::min<uint32_t>(arrayLayer, std::max<uint32_t>(1u, src.arrayLayers()) - 1u);

  VLOG(1) << fmt::format(
    "VK image readback enqueue src=0x{:x} size={}x{} bytes={} slot={} eye={} layer={} aspect=0x{:x}",
    reinterpret_cast<uint64_t>(&src),
    size.x,
    size.y,
    bytes,
    slotIndex,
    static_cast<int>(eye),
    resolvedLayer,
    static_cast<uint32_t>(aspectMask));

  FrameResources::PendingReadback pr{};
  pr.src = &src;
  pr.eye = eye;
  pr.size = size;
  pr.format = fmt;
  pr.aspectMask = aspectMask;
  pr.arrayLayer = resolvedLayer;
  pr.bytes = bytes;
  pr.slotIndex = slotIndex;
  m_activeFrame->pendingColorReadbacks.emplace_back(std::move(pr));

  // Release must be safe from arbitrary threads. Route it back to the render
  // thread and guard backend lifetime (backend switches destroy/recreate the
  // Vulkan backend).
  const std::weak_ptr<bool> alive = m_aliveFlag;
  auto releaseEx = currentRenderThreadExecutorKeepAlive("vk_readback_release");
  std::function<void()> releaseSlot = [alive, releaseEx = std::move(releaseEx), backend = this, slotIndex]() mutable {
    auto aliveStrong = alive.lock();
    if (!aliveStrong || !*aliveStrong) {
      return;
    }
    releaseEx->add([alive, backend, slotIndex]() mutable {
      auto aliveStrong2 = alive.lock();
      if (!aliveStrong2 || !*aliveStrong2) {
        return;
      }
      backend->releaseReadbackSlot(slotIndex);
    });
  };

  EndOfFrameColorReadbackTicket ticket{};
  ticket.fence = awaitActiveSubmissionFence(debugLabel.empty() ? "vk_image_readback" : debugLabel);
  ticket.mapped = mapped;
  ticket.bytes = bytes;
  ticket.format = fmt;
  ticket.size = size;
  ticket.aspectMask = aspectMask;
  ticket.arrayLayer = resolvedLayer;
  ticket.releaseSlot = std::move(releaseSlot);
  return ticket;
}

Z3DRendererVulkanBackend::EndOfFrameHostImageReadbackTicket
Z3DRendererVulkanBackend::requestEndOfFrameImageReadbackToHostTicket(ZVulkanTexture& src,
                                                                     Z3DEye eye,
                                                                     uint32_t arrayLayer,
                                                                     vk::ImageAspectFlags aspectMask,
                                                                     std::string_view debugLabel)
{
  return requestEndOfFrameImageReadbackToHostTicket(src,
                                                    eye,
                                                    arrayLayer,
                                                    aspectMask,
                                                    std::shared_ptr<void>{},
                                                    debugLabel);
}

Z3DRendererVulkanBackend::EndOfFrameHostImageReadbackTicket
Z3DRendererVulkanBackend::requestEndOfFrameImageReadbackToHostTicket(ZVulkanTexture& src,
                                                                     Z3DEye eye,
                                                                     uint32_t arrayLayer,
                                                                     vk::ImageAspectFlags aspectMask,
                                                                     std::shared_ptr<void> keepAlive,
                                                                     std::string_view debugLabel)
{
  CHECK(currentRenderThreadExecutorOrNull() != nullptr)
    << "requestEndOfFrameImageReadbackToHostTicket must be called on the rendering thread"
    << (debugLabel.empty() ? "" : " (") << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : ")");

  // Reuse the core staging-slot machinery (image->buffer copy at endRender),
  // but do not expose the mapped pointer to the call site.
  auto stagingTicket = requestEndOfFrameImageReadbackTicket(src, eye, arrayLayer, aspectMask, debugLabel);

  CHECK_GT(stagingTicket.bytes, 0u) << "requestEndOfFrameImageReadbackToHostTicket computed 0 bytes";
  auto hostBytes = std::make_shared<std::vector<uint8_t>>();
  hostBytes->resize(stagingTicket.bytes);

  EndOfFrameHostImageReadbackTicket ticket{};
  ticket.format = stagingTicket.format;
  ticket.size = stagingTicket.size;
  ticket.aspectMask = stagingTicket.aspectMask;
  ticket.arrayLayer = stagingTicket.arrayLayer;
  ticket.m_hostBytes = hostBytes;
  ticket.m_keepAlive = std::move(keepAlive);
  ticket.m_stagingTicket = std::move(stagingTicket);

  return ticket;
}

Z3DRendererVulkanBackend::EndOfFrameBufferReadbackTicket
Z3DRendererVulkanBackend::requestEndOfFrameBufferReadbackTicket(ZVulkanBuffer& src,
                                                                vk::DeviceSize srcOffset,
                                                                size_t bytes,
                                                                std::string_view debugLabel)
{
  CHECK(currentRenderThreadExecutorOrNull() != nullptr)
    << "requestEndOfFrameBufferReadbackTicket must be called on the rendering thread"
    << (debugLabel.empty() ? "" : " (") << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : ")");
  CHECK(m_activeFrame && m_activeFrameHandle && m_activeFrameHandle->valid())
    << "VK buffer readback requested outside of an active frame";
  CHECK_GT(bytes, 0u) << "VK buffer readback requested with 0 bytes";
  const size_t offset = static_cast<size_t>(srcOffset);
  CHECK_LE(offset + bytes, src.size()) << "VK buffer readback range out of bounds";
  CHECK((src.usage() & vk::BufferUsageFlagBits::eTransferSrc) != vk::BufferUsageFlags{})
    << "VK buffer readback requires src buffer with eTransferSrc usage";

  const int slotIndex = acquireReadbackSlot(bytes);
  CHECK(slotIndex >= 0) << "VK readback slot unavailable (bytes=" << bytes << ")";
  CHECK(static_cast<size_t>(slotIndex) < m_readbackSlots.size());

  auto& slot = m_readbackSlots[static_cast<size_t>(slotIndex)];
  slot.tag = "buffer";
  const void* mapped = slot.mapped;
  CHECK(mapped != nullptr) << "VK buffer readback mapped pointer is null";

  VLOG(2) << fmt::format("VK buffer readback enqueue src=0x{:x} offset={} bytes={} slot={}",
                         reinterpret_cast<uint64_t>(&src),
                         offset,
                         bytes,
                         slotIndex);

  FrameResources::PendingBufferReadback pr{};
  pr.src = &src;
  pr.srcOffset = srcOffset;
  pr.bytes = bytes;
  pr.slotIndex = slotIndex;
  m_activeFrame->pendingBufferReadbacks.emplace_back(std::move(pr));

  const std::weak_ptr<bool> alive = m_aliveFlag;
  auto releaseEx = currentRenderThreadExecutorKeepAlive("vk_buffer_readback_release");
  std::function<void()> releaseSlot = [alive, releaseEx = std::move(releaseEx), backend = this, slotIndex]() mutable {
    auto aliveStrong = alive.lock();
    if (!aliveStrong || !*aliveStrong) {
      return;
    }
    releaseEx->add([alive, backend, slotIndex]() mutable {
      auto aliveStrong2 = alive.lock();
      if (!aliveStrong2 || !*aliveStrong2) {
        return;
      }
      backend->releaseReadbackSlot(slotIndex);
    });
  };

  EndOfFrameBufferReadbackTicket ticket{};
  ticket.m_fence = awaitActiveSubmissionFence(debugLabel.empty() ? "vk_buffer_readback" : debugLabel);
  ticket.m_mapped = mapped;
  ticket.m_bytes = bytes;
  ticket.m_releaseSlot = std::move(releaseSlot);
  return ticket;
}

std::vector<uint8_t> Z3DRendererVulkanBackend::readBufferRangeAfterCompletion(ZVulkanBuffer& src,
                                                                              vk::DeviceSize srcOffset,
                                                                              size_t bytes,
                                                                              std::string_view debugLabel)
{
  CHECK(currentRenderThreadExecutorOrNull() != nullptr)
    << "readBufferRangeAfterCompletion must be called on the rendering thread" << (debugLabel.empty() ? "" : " (")
    << (debugLabel.empty() ? "" : debugLabel) << (debugLabel.empty() ? "" : ")");
  CHECK_GT(bytes, 0u) << "readBufferRangeAfterCompletion requires bytes > 0";
  const size_t offset = static_cast<size_t>(srcOffset);
  CHECK_LE(offset, src.size()) << "readBufferRangeAfterCompletion offset out of bounds";
  CHECK_LE(bytes, src.size() - offset) << "readBufferRangeAfterCompletion range out of bounds";
  CHECK((src.usage() & vk::BufferUsageFlagBits::eTransferSrc) != vk::BufferUsageFlags{})
    << "readBufferRangeAfterCompletion requires a transfer-src buffer";

  auto staging =
    device().createBufferInPool(bytes,
                                vk::BufferUsageFlagBits::eTransferDst,
                                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent |
                                  vk::MemoryPropertyFlagBits::eHostCached,
                                device().readbackStagingPool());
  CHECK(staging != nullptr) << "readBufferRangeAfterCompletion failed to allocate staging buffer";
  void* mapped = staging->map(0, bytes);
  CHECK(mapped != nullptr) << "readBufferRangeAfterCompletion staging buffer is not mapped";

  device().frameExecutor().executeImmediate(
    [&](vk::raii::CommandBuffer& cmd) {
      vk::BufferMemoryBarrier2 barrier{};
      barrier.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
      barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
      barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
      barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
      barrier.buffer = src.buffer();
      barrier.offset = srcOffset;
      barrier.size = bytes;
      vk::DependencyInfo dep{};
      dep.bufferMemoryBarrierCount = 1;
      dep.pBufferMemoryBarriers = &barrier;
      cmd.pipelineBarrier2(dep);

      const vk::BufferCopy region{.srcOffset = srcOffset, .dstOffset = 0, .size = bytes};
      cmd.copyBuffer(src.buffer(), staging->buffer(), region);
    },
    debugLabel.empty() ? "buffer_range_readback_after_completion" : debugLabel);

  std::vector<uint8_t> out(bytes);
  std::memcpy(out.data(), mapped, bytes);
  staging->unmap();
  return out;
}

folly::coro::Task<void> Z3DRendererVulkanBackend::EndOfFrameHostImageReadbackTicket::awaitReady()
{
  CHECK(m_hostBytes != nullptr) << "EndOfFrameHostImageReadbackTicket used without host bytes";
  if (m_ready) {
    co_return;
  }

  CHECK(m_stagingTicket.mapped != nullptr) << "EndOfFrameHostImageReadbackTicket missing staging mapped pointer";
  CHECK_GT(m_stagingTicket.bytes, 0u) << "EndOfFrameHostImageReadbackTicket used with 0 bytes";
  CHECK(m_stagingTicket.releaseSlot) << "EndOfFrameHostImageReadbackTicket already consumed (release slot missing)";

  co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(std::move(m_stagingTicket.fence));

  m_hostBytes->resize(m_stagingTicket.bytes);
  std::memcpy(m_hostBytes->data(), m_stagingTicket.mapped, m_stagingTicket.bytes);

  m_stagingTicket.releaseSlot();
  m_stagingTicket.releaseSlot = {};

  // Drop the keep-alive once the GPU read and host copy are complete.
  m_keepAlive.reset();
  m_ready = true;
  co_return;
}

folly::coro::Task<std::vector<uint8_t>> Z3DRendererVulkanBackend::EndOfFrameColorReadbackTicket::awaitOwnedBytes()
{
  CHECK(mapped != nullptr) << "EndOfFrameColorReadbackTicket used without a mapped pointer";
  CHECK_GT(bytes, 0u) << "EndOfFrameColorReadbackTicket used with 0 bytes";
  CHECK(releaseSlot) << "EndOfFrameColorReadbackTicket already consumed (release slot missing)";

  co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(fence);

  std::vector<uint8_t> out;
  out.resize(bytes);
  std::memcpy(out.data(), mapped, bytes);

  releaseSlot();
  releaseSlot = {};
  co_return out;
}

folly::coro::Task<std::vector<uint8_t>> Z3DRendererVulkanBackend::EndOfFrameBufferReadbackTicket::awaitOwnedBytes()
{
  CHECK(m_mapped != nullptr) << "EndOfFrameBufferReadbackTicket used without a mapped pointer";
  CHECK_GT(m_bytes, 0u) << "EndOfFrameBufferReadbackTicket used with 0 bytes";
  CHECK(m_releaseSlot) << "EndOfFrameBufferReadbackTicket already consumed (release slot missing)";

  co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(m_fence);

  std::vector<uint8_t> out;
  out.resize(m_bytes);
  std::memcpy(out.data(), m_mapped, m_bytes);

  m_releaseSlot();
  m_releaseSlot = {};
  co_return out;
}

folly::coro::Task<void> Z3DRendererVulkanBackend::EndOfFrameBufferReadbackTicket::awaitCopyTo(void* dst,
                                                                                              size_t dstBytes)
{
  CHECK(dst != nullptr) << "EndOfFrameBufferReadbackTicket::awaitCopyTo requires dst";
  CHECK(m_mapped != nullptr) << "EndOfFrameBufferReadbackTicket used without a mapped pointer";
  CHECK_GT(m_bytes, 0u) << "EndOfFrameBufferReadbackTicket used with 0 bytes";
  CHECK_EQ(dstBytes, m_bytes) << "EndOfFrameBufferReadbackTicket::awaitCopyTo size mismatch";
  CHECK(m_releaseSlot) << "EndOfFrameBufferReadbackTicket already consumed (release slot missing)";

  co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(m_fence);

  std::memcpy(dst, m_mapped, m_bytes);
  m_releaseSlot();
  m_releaseSlot = {};
  co_return;
}

folly::coro::Task<void> Z3DRendererVulkanBackend::EndOfFrameBufferReadbackTicket::awaitAndDiscard()
{
  CHECK(m_mapped != nullptr) << "EndOfFrameBufferReadbackTicket used without a mapped pointer";
  CHECK_GT(m_bytes, 0u) << "EndOfFrameBufferReadbackTicket used with 0 bytes";
  CHECK(m_releaseSlot) << "EndOfFrameBufferReadbackTicket already consumed (release slot missing)";

  co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(m_fence);
  m_releaseSlot();
  m_releaseSlot = {};
  co_return;
}

void Z3DRendererVulkanBackend::notifyPipelineCreated()
{
  if (m_activeFrame) {
    m_activeFrame->pipelinesCreated++;
  }
}

void Z3DRendererVulkanBackend::notifyPipelineBound(vk::Pipeline pipeline)
{
  if (!m_activeFrame) {
    return;
  }
  // Track unique pipeline bindings for per-frame instrumentation
  VkPipeline raw = static_cast<VkPipeline>(pipeline);
  m_activeFrame->pipelinesBound.insert(reinterpret_cast<uint64_t>(raw));
}

void Z3DRendererVulkanBackend::notifyDrawSecondaryCacheAttempt()
{
  if (m_activeFrame) {
    m_activeFrame->drawSecondaryCacheAttempts++;
  }
}

void Z3DRendererVulkanBackend::notifyDrawSecondaryCacheKeyFound()
{
  if (m_activeFrame) {
    m_activeFrame->drawSecondaryCacheKeyFound++;
  }
}

void Z3DRendererVulkanBackend::notifyDrawSecondaryCacheSignatureMismatch()
{
  notifyDrawSecondaryCacheSignatureMismatchMask(0);
}

void Z3DRendererVulkanBackend::notifyDrawSecondaryCacheSignatureMismatchMask(uint32_t mask)
{
  if (!m_activeFrame) {
    return;
  }
  m_activeFrame->drawSecondaryCacheSignatureMismatches++;
  m_activeFrame->drawSecondaryCacheSignatureMismatchMaskOr |= mask;
}

void Z3DRendererVulkanBackend::notifyDrawSecondaryCacheHit()
{
  if (m_activeFrame) {
    m_activeFrame->drawSecondaryCacheHits++;
  }
}

void Z3DRendererVulkanBackend::notifyDrawSecondaryCacheBuild()
{
  if (m_activeFrame) {
    m_activeFrame->drawSecondaryCacheBuilds++;
  }
}

void Z3DRendererVulkanBackend::notifyDrawSecondaryCacheExecute()
{
  if (m_activeFrame) {
    m_activeFrame->drawSecondaryCacheExecutes++;
  }
}

// Queue a static copy to be performed outside dynamic rendering in this frame
void Z3DRendererVulkanBackend::scheduleStaticCopy(vk::Buffer dst,
                                                  vk::DeviceSize dstOffset,
                                                  const UploadSlice& src,
                                                  bool isIndexBuffer)
{
  if (!m_activeFrame) {
    return;
  }
  // Pin the destination static segment for this submission so that any evictions
  // cannot reuse its suballocated range until the fence signals.
  if (dst && m_submissionResourcePinningOpen) {
    const VkBuffer raw = static_cast<VkBuffer>(dst);
    auto it = m_staticArena.segmentByBuffer.find(raw);
    if (it != m_staticArena.segmentByBuffer.end() && it->second != nullptr) {
      const auto [_, inserted] = m_activeFrame->pinnedStaticSegments.insert(it->second);
      if (inserted) {
        CHECK(it->second->pinCount < std::numeric_limits<uint32_t>::max()) << "Static segment pinCount overflow";
        it->second->pinCount++;
      }
    }
  }
  FrameResources::ScheduledCopy sc{};
  sc.dst = dst;
  sc.dstOffset = dstOffset;
  sc.src = src;
  sc.usage = isIndexBuffer ? FrameResources::ScheduledCopy::Usage::Index : FrameResources::ScheduledCopy::Usage::Vertex;
  m_activeFrame->scheduledCopies.push_back(sc);
}

// Execute queued upload->static copies after all dynamic rendering segments end
void Z3DRendererVulkanBackend::flushScheduledCopies(vk::raii::CommandBuffer& cmd)
{
  if (!m_activeFrame || m_activeFrame->scheduledCopies.empty()) {
    return;
  }
  const size_t queued = m_activeFrame->scheduledCopies.size();
  size_t flushed = 0;
  size_t bytes = 0;
  for (const auto& sc : m_activeFrame->scheduledCopies) {
    if (!sc.dst || !sc.src.buffer || sc.src.size == 0) {
      continue;
    }
    vk::BufferCopy region{.srcOffset = sc.src.offset,
                          .dstOffset = sc.dstOffset,
                          .size = static_cast<vk::DeviceSize>(sc.src.size)};
    cmd.copyBuffer(sc.src.buffer, sc.dst, region);

    // Make transfer writes visible to the consumer stage for next usage
    vk::BufferMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    // Respect scheduled usage tag
    auto usage = sc.usage;
    if (usage == FrameResources::ScheduledCopy::Usage::Indirect) {
      barrier.dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect;
      barrier.dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead;
    } else if (usage == FrameResources::ScheduledCopy::Usage::Index) {
      barrier.dstStageMask = vk::PipelineStageFlagBits2::eVertexInput;
      barrier.dstAccessMask = vk::AccessFlagBits2::eIndexRead;
    } else {
      barrier.dstStageMask = vk::PipelineStageFlagBits2::eVertexInput;
      barrier.dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead;
    }
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = sc.dst;
    barrier.offset = sc.dstOffset;
    barrier.size = region.size;
    vk::DependencyInfo dep{};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &barrier;
    cmd.pipelineBarrier2(dep);

    m_activeFrame->staticBytesStaged += sc.src.size;
    flushed++;
    bytes += sc.src.size;
  }
  VLOG(1) << fmt::format("VK flush copies: queued={} flushed={} bytes={}", queued, flushed, bytes);
  m_activeFrame->scheduledCopies.clear();
}

void Z3DRendererVulkanBackend::enterCompletionSafePointForKeyIfMatches(void* key,
                                                                       uint64_t expectedRealFrameToken,
                                                                       uint32_t expectedSubmissionId)
{
  if (!m_sharedDevice || key == nullptr || expectedRealFrameToken == 0 || expectedSubmissionId == 0) {
    return;
  }

  auto it = m_frameResourceMap.find(key);
  if (it == m_frameResourceMap.end()) {
    return;
  }
  CHECK_LT(it->second, m_frames.size()) << "Frame resource map index out of bounds";
  FrameResources& frame = m_frames[it->second];
  if (frame.realFrameToken != expectedRealFrameToken || frame.submissionId != expectedSubmissionId) {
    return;
  }
  // Enter the "frame completion safe point" (after-fence). This executes all
  // registered safe-point hooks (notably final readback publish + end-to-end
  // `all` latency measurement in Z3DCompositor) and then ingests the completed
  // submission into Z3DPerfCollector.
  applyPendingArenaReset(frame);
}

void Z3DRendererVulkanBackend::collectFrameTimings(FrameResources& frame)
{
  if (frame.cpuStart.time_since_epoch().count() == 0 || frame.cpuEnd.time_since_epoch().count() == 0) {
    frame.gpuScopes.clear();
    frame.cpuScopes.clear();
    frame.nextQuery = 0;
    frame.cpuStart = {};
    frame.cpuEnd = {};
    return;
  }

  const double cpuMs = std::chrono::duration<double, std::milli>(frame.cpuEnd - frame.cpuStart).count();
  std::string message;
  if (frame.realFrameToken != 0) {
    message = fmt::format("VK batches [frame#{} sub#{}] '{}' CPU {:.3f} ms",
                          frame.realFrameToken,
                          frame.submissionId,
                          frame.frameName,
                          cpuMs);
  } else {
    message = fmt::format("VK batches '{}' CPU {:.3f} ms", frame.frameName, cpuMs);
  }

  // Calibration state (used for trace timestamp alignment)
  bool haveCalibration = false;
  uint64_t deviceCalibTicks = 0;
  uint64_t hostCalibNs = 0;
  const bool collectGpuTimestamps = timestampQueriesEnabled();

  if (collectGpuTimestamps && frame.nextQuery > 0 && !frame.gpuScopes.empty()) {
    // Optional calibration: map GPU ticks to CPU nanoseconds for trace alignment
    if (absl::GetFlag(FLAGS_atlas_perf_trace_calibrated)) {
      try {
        // Check time domain support
        auto& phys = m_sharedDevice->context().physicalDevice();
        auto domains = phys.getCalibrateableTimeDomainsEXT();
        bool haveDevice = false, haveMono = false;
        for (auto d : domains) {
          if (d == vk::TimeDomainEXT::eDevice) {
            haveDevice = true;
          }
          if (d == vk::TimeDomainEXT::eClockMonotonic) {
            haveMono = true;
          }
        }
        if (haveDevice && haveMono) {
          vk::CalibratedTimestampInfoKHR infoDev{};
          infoDev.timeDomain = vk::TimeDomainKHR::eDevice;
          vk::CalibratedTimestampInfoKHR infoMono{};
          infoMono.timeDomain = vk::TimeDomainKHR::eClockMonotonic;
          std::array<vk::CalibratedTimestampInfoKHR, 2> infos{infoDev, infoMono};
          auto& dev = m_sharedDevice->context().device();
          // Vulkan-Hpp returns {timestamps, maxDeviation} here and throws on failure.
          auto [timestamps, maxDeviation] = dev.getCalibratedTimestampsEXT(infos);
          (void)maxDeviation; // not used currently
          if (timestamps.size() >= 2) {
            deviceCalibTicks = timestamps[0];
            hostCalibNs = timestamps[1];
            haveCalibration = true;
            if (VLOG_IS_ON(2)) {
              VLOG(2) << "VK calibration ok: deviceTicks=" << deviceCalibTicks << " hostNs=" << hostCalibNs
                      << " maxDeviationNs=" << maxDeviation << " tickPeriodNs=" << m_timestampPeriod;
            }
          } else {
            if (VLOG_IS_ON(2)) {
              VLOG(2) << "VK calibration returned insufficient timestamps: count=" << timestamps.size();
            }
          }
        } else {
          if (VLOG_IS_ON(2)) {
            VLOG(2) << "VK calibration skipped: required time domains not both supported (device=" << haveDevice
                    << ", monotonic=" << haveMono << ")";
          }
        }
      }
      catch (...) {
        haveCalibration = false;
        if (VLOG_IS_ON(2)) {
          VLOG(2) << "VK calibration threw; falling back to uncalibrated timestamps";
        }
      }
    }
    const auto [result, queryData] = frame.queryPool.getResults<uint64_t>(0,
                                                                          frame.nextQuery,
                                                                          frame.nextQuery * sizeof(uint64_t),
                                                                          sizeof(uint64_t),
                                                                          vk::QueryResultFlagBits::e64);

    if (result == vk::Result::eSuccess) {
      for (const auto& scope : frame.gpuScopes) {
        if (scope.endQuery >= queryData.size() || scope.startQuery >= queryData.size()) {
          continue;
        }
        const uint64_t endTicks = queryData[scope.endQuery];
        const uint64_t startTicks = queryData[scope.startQuery];
        if (endTicks <= startTicks) {
          continue;
        }
        const double ns = static_cast<double>(endTicks - startTicks) * static_cast<double>(m_timestampPeriod);
        const double ms = ns * 1e-6;
        message += fmt::format(" | gpu:{} {:.3f} ms", scope.label, ms);
      }
    } else {
      VLOG(1) << "Vulkan query results unavailable: " << vk::to_string(result);
    }
  } else if (collectGpuTimestamps) {
    VLOG(1) << "No GPU timestamp scopes recorded this frame";
  }

  // Report end-to-end (perf-frame-start → host-ready) latency separately from
  // CPU/GPU scopes. This keeps GPU totals/percentages scoped to timestamped GPU
  // work while still exposing a user-visible "frame done" metric in a single line.
  if (frame.allMaxMs.has_value() && *frame.allMaxMs >= 0.0) {
    message += fmt::format(" | all {:.3f} ms", *frame.allMaxMs);
  }

  for (const auto& cpuScope : frame.cpuScopes) {
    message += fmt::format(" | cpu:{} {:.3f} ms", cpuScope.label, cpuScope.milliseconds);
  }

  // Append concise per-submission stats (counts and resource deltas)
  message += fmt::format(
    " | draws={} dsets={} pipes+={} bound={} segs={} clr={} ld={} dwr={} rew={} upload_hi={}B ubo_hi={}B static={}B rb={}B rbinflight={} sec2=a{} f{} m{} h{} b{} e{} mask=0x{:x}",
    frame.drawsSubmitted,
    frame.descriptorSetsAllocated,
    frame.pipelinesCreated,
    frame.pipelinesBound.size(),
    frame.renderingSegmentsBegan,
    frame.attachmentClears,
    frame.attachmentLoads,
    frame.descriptorWritesWhileRecording,
    frame.boundSetRewriteAttempts,
    frame.uploadArena.highWatermark,
    frame.uniformArena.highWatermark,
    frame.staticBytesStaged,
    frame.readbackBytesCopied,
    frame.readbackSlotsInFlight,
    frame.drawSecondaryCacheAttempts,
    frame.drawSecondaryCacheKeyFound,
    frame.drawSecondaryCacheSignatureMismatches,
    frame.drawSecondaryCacheHits,
    frame.drawSecondaryCacheBuilds,
    frame.drawSecondaryCacheExecutes,
    frame.drawSecondaryCacheSignatureMismatchMaskOr);

  LOG(INFO) << message;

  // Feed the perf collector with per-submission scopes for aggregation under the real-frame token.
  std::vector<Z3DPerfCollector::Scope> gpuScopesForCollector;
  std::vector<Z3DPerfCollector::Scope> cpuScopesForCollector;

  // Re-fetch timestamp query data to compute ms for GPU scopes (we already have message; reconstruct ms values)
  if (collectGpuTimestamps && frame.nextQuery > 0 && !frame.gpuScopes.empty()) {
    const auto [result, queryData] = frame.queryPool.getResults<uint64_t>(0,
                                                                          frame.nextQuery,
                                                                          frame.nextQuery * sizeof(uint64_t),
                                                                          sizeof(uint64_t),
                                                                          vk::QueryResultFlagBits::e64);
    if (result == vk::Result::eSuccess) {
      for (const auto& scope : frame.gpuScopes) {
        if (scope.endQuery >= queryData.size() || scope.startQuery >= queryData.size()) {
          continue;
        }
        const uint64_t endTicks = queryData[scope.endQuery];
        const uint64_t startTicks = queryData[scope.startQuery];
        if (endTicks <= startTicks) {
          continue;
        }
        const double ns = static_cast<double>(endTicks - startTicks) * static_cast<double>(m_timestampPeriod);
        const double ms = ns * 1e-6;
        Z3DPerfCollector::Scope sc{scope.label, ms};
        if (haveCalibration) {
          const double startNsCpu = static_cast<double>(hostCalibNs) +
                                    (static_cast<double>(startTicks) - static_cast<double>(deviceCalibTicks)) *
                                      static_cast<double>(m_timestampPeriod);
          sc.tsUs = startNsCpu * 1e-3;
        }
        sc.isPassScope = scope.isPassScope;
        gpuScopesForCollector.push_back(std::move(sc));
      }
    }
  }

  for (const auto& cs : frame.cpuScopes) {
    cpuScopesForCollector.push_back(Z3DPerfCollector::Scope{cs.label, cs.milliseconds});
  }

  if (frame.realFrameToken != 0) {
    Z3DPerfCollector::Stats stats{};
    stats.drawsSubmitted = frame.drawsSubmitted;
    stats.descriptorSetsAllocated = frame.descriptorSetsAllocated;
    stats.pipelinesCreated = frame.pipelinesCreated;
    stats.pipelinesBoundCount = static_cast<uint32_t>(frame.pipelinesBound.size());
    stats.renderingSegmentsBegan = frame.renderingSegmentsBegan;
    stats.attachmentClears = frame.attachmentClears;
    stats.attachmentLoads = frame.attachmentLoads;
    stats.descriptorWritesWhileRecording = frame.descriptorWritesWhileRecording;
    stats.boundSetRewriteAttempts = frame.boundSetRewriteAttempts;
    stats.uploadHighWatermarkBytes = frame.uploadArena.highWatermark;
    stats.uniformHighWatermarkBytes = frame.uniformArena.highWatermark;
    stats.staticBytesStaged = frame.staticBytesStaged;
    stats.linesBytesStaged = frame.linesBytesStaged;
    stats.fontsBytesStaged = frame.fontsBytesStaged;
    stats.meshesBytesStaged = frame.meshesBytesStaged;
    stats.spheresBytesStaged = frame.spheresBytesStaged;
    stats.conesBytesStaged = frame.conesBytesStaged;
    stats.ellipsoidsBytesStaged = frame.ellipsoidsBytesStaged;
    stats.readbackBytesCopied = frame.readbackBytesCopied;
    stats.readbackSlotsInFlight = frame.readbackSlotsInFlight;
    if (frame.preCpuStartMs.has_value() && *frame.preCpuStartMs >= 0.0) {
      stats.preCpuStartSamples = 1;
      stats.preCpuStartMs = *frame.preCpuStartMs;
    }
    stats.allSamples = frame.allSamples;
    stats.allMaxMs = frame.allMaxMs.value_or(0.0);
    stats.drawSecondaryCacheAttempts = frame.drawSecondaryCacheAttempts;
    stats.drawSecondaryCacheKeyFound = frame.drawSecondaryCacheKeyFound;
    stats.drawSecondaryCacheSignatureMismatches = frame.drawSecondaryCacheSignatureMismatches;
    stats.drawSecondaryCacheSignatureMismatchMaskOr = frame.drawSecondaryCacheSignatureMismatchMaskOr;
    stats.drawSecondaryCacheHits = frame.drawSecondaryCacheHits;
    stats.drawSecondaryCacheBuilds = frame.drawSecondaryCacheBuilds;
    stats.drawSecondaryCacheExecutes = frame.drawSecondaryCacheExecutes;

    // Pre_cpu attribution helpers
    stats.vkBeginRenderPreambleMs = frame.beginRenderPreambleMs;
    stats.scriptUniformHintMs = frame.scriptUniformHintMs;
    stats.scriptUniformHintBytes = frame.scriptUniformHintBytes;
    stats.scriptNodeCount = frame.scriptNodeCount;
    stats.scriptRasterNodeCount = frame.scriptRasterNodeCount;
    stats.scriptReplayNodeCount = frame.scriptReplayNodeCount;
    stats.scriptCommandsNodeCount = frame.scriptCommandsNodeCount;
    stats.scriptPreRecordNodeCount = frame.scriptPreRecordNodeCount;
    stats.scriptBatchCount = frame.scriptBatchCount;

    Z3DPerfCollector::instance().addSubmission(frame.realFrameToken,
                                               frame.submissionId,
                                               cpuMs,
                                               std::move(gpuScopesForCollector),
                                               std::move(cpuScopesForCollector),
                                               stats);
  }

  frame.gpuScopes.clear();
  frame.cpuScopes.clear();
  frame.nextQuery = 0;
  frame.cpuStart = {};
  frame.cpuEnd = {};
  frame.preCpuStartMs.reset();
}

bool Z3DRendererVulkanBackend::timestampQueriesEnabled() const
{
  if (!m_sharedDevice) {
    return false;
  }
  // Strict residency-budget runs are for proving Atlas-owned resources stay
  // within a hard cap. GPU timestamp writes are optional instrumentation, and
  // some Vulkan portability stacks allocate hidden query backing at submit time.
  return !m_sharedDevice->residencyManager().strictBudgetActive();
}

std::optional<size_t> Z3DRendererVulkanBackend::beginGpuScope(std::string_view label)
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid() || label.empty() || !m_frameRecording) {
    return std::nullopt;
  }
  if (!timestampQueriesEnabled()) {
    return std::nullopt;
  }
  auto& frame = *m_activeFrame;
  if (frame.nextQuery + 2 > kMaxTimestampQueries) {
    VLOG(1) << "Vulkan timestamp query budget exceeded";
    return std::nullopt;
  }
  const uint32_t startIndex = frame.nextQuery++;
  m_activeFrameHandle->commandBuffer().writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,
                                                      *frame.queryPool,
                                                      startIndex);
  const uint32_t endIndex = frame.nextQuery++;
  const bool isPassScope = (m_passScope.active && (m_passScope.label == label));
  frame.gpuScopes.push_back(GpuScopeRecord{std::string(label), startIndex, endIndex, isPassScope});
  return frame.gpuScopes.size() - 1;
}

void Z3DRendererVulkanBackend::endGpuScope(size_t token)
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid() || !m_frameRecording) {
    return;
  }
  auto& frame = *m_activeFrame;
  if (token >= frame.gpuScopes.size()) {
    return;
  }
  const uint32_t endIndex = frame.gpuScopes[token].endQuery;
  m_activeFrameHandle->commandBuffer().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                      *frame.queryPool,
                                                      endIndex);
}

void Z3DRendererVulkanBackend::recordCpuScope(std::string_view label, double milliseconds)
{
  if (!m_activeFrame || label.empty()) {
    return;
  }
  m_activeFrame->cpuScopes.push_back(CpuScopeRecord{std::string(label), milliseconds});
}

std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend()
{
  return std::unique_ptr<Z3DRendererBackend>(std::make_unique<Z3DRendererVulkanBackend>().release());
}

} // namespace nim
