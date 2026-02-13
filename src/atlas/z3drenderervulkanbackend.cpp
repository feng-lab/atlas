#include "z3drenderervulkanbackend.h"

#include "z3drendererbase.h"
#include "z3drendercommands.h"
#include "z3dboundedfilter.h"
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
#include <folly/coro/Task.h>

DEFINE_bool(vk_reserve_upload_slices,
            true,
            "Reserve per-draw upload arena capacity (precise) before suballocation to avoid mid-upload growth");
DECLARE_string(atlas_perf_mode);
DECLARE_bool(atlas_perf_trace_calibrated);
DEFINE_bool(atlas_vk_ddp_indirect_count,
            true,
            "Use drawIndirectCount gating for Vulkan dual-depth peeling (device-side early stop)");
DEFINE_bool(
  atlas_vk_profile_pipeline_contexts,
  false,
  "If true, record additional CPU scopes attributing Vulkan batch processing time to individual pipeline contexts "
  "(mesh/line/sphere/etc). Adds overhead; intended for perf triage.");
DEFINE_bool(
  atlas_vk_cache_draw_secondaries,
  true,
  "If true, cache per-draw secondary command buffers for Vulkan raster pipeline contexts (currently sphere/cone). "
  "This avoids repeating expensive vkCmd recording work in steady state (camera-only changes) at the cost of "
  "extra memory for cached command buffers.");

// Baseline capacity for the per-frame uniform arena (in KiB). This buffer backs
// all dynamic UBO bindings for the frame. The backend pre-sizes beyond this baseline
// based on a cheap estimate; mid-frame growth is disallowed.
static constexpr int kUniformArenaBaseKiB = 256;

namespace nim {

namespace {
constexpr uint32_t kMaxTimestampQueries = 64u;
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
  Z3DRenderGlobalState::instance().scratchPool().setVulkanReleaseScheduler({});
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
  Z3DRenderGlobalState::instance().scratchPool().setVulkanReleaseScheduler({});

  // Drop shared placeholders; they'll be recreated lazily on next use.
  m_defaultPlaceholder2D.reset();
  m_defaultSampler.reset();
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
    m_passScope.baseline.overrideSetsAllocated = m_activeFrame->overrideSetsAllocated;
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
  uint32_t dsets = 0, ovsets = 0, segs = 0, clr = 0, ld = 0, dwr = 0, rew = 0;
  size_t bound = 0;
  vk::DeviceSize uploadHi = 0;
  uint64_t staticStaged = 0, rb = 0;
  uint32_t rbinflight = 0;
  if (m_activeFrame) {
    dsets = m_activeFrame->descriptorSetsAllocated - m_passScope.baseline.descriptorSetsAllocated;
    ovsets = m_activeFrame->overrideSetsAllocated - m_passScope.baseline.overrideSetsAllocated;
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
    "pass_end pass='{}' cpu={:.3f} ms draws={} segs={} clr={} ld={} dsets={} ovsets={} pipes_bound_delta={} dwr={} rew={} uploads_delta={}B static_delta={}B rb_delta={}B rbinflight_delta={} transitions={} noop={} buf_barriers={} buf_noop={}",
    m_passScope.label,
    ms,
    m_passScope.draws,
    segs,
    clr,
    ld,
    dsets,
    ovsets,
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

void Z3DRendererVulkanBackend::beginRender(Z3DRendererBase& renderer)
{
  // Only expose the thread-local "current backend" while actively recording a
  // Vulkan frame. This avoids call sites (debug validation, descriptor helpers)
  // observing a partially-initialised backend before ensureDevice/beginFrame.
  s_currentBackend = nullptr;
  m_activeRenderer = &renderer;
  auto preRecordActions = std::move(m_pendingBeginRenderPreRecordActions);
  m_pendingBeginRenderPreRecordActions.clear();
  std::string pendingPreRecordLabel = std::move(m_pendingBeginRenderPreRecordLabel);
  m_pendingBeginRenderPreRecordLabel.clear();
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
      std::lock_guard<std::mutex> lock(m_pendingEvictionsMutex);
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
    m_activeRenderer = nullptr;
    m_activePPLLIndex.reset();
    s_currentBackend = nullptr;
    return;
  }

  m_activeFrameHandle = device().frameExecutor().beginFrame();
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    m_activeFrame = nullptr;
    m_frameRecording = false;
    m_activeRenderer = nullptr;
    m_activePPLLIndex.reset();
    s_currentBackend = nullptr;
    return;
  }

  auto& frameResources = ensureFrameResourcesForKey(m_activeFrameHandle->key());

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
  // Expose frame resources early so suballocateUniform can target this frame.
  m_activeFrame = &frameResources;
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
  collectFrameTimings(frameResources);
  // Reset per-submission descriptor set counters after reporting the previous
  // frame-slot use (collectFrameTimings reads the counters for logging and the
  // perf collector).
  frameResources.descriptorSetsAllocated = 0;
  frameResources.gpuScopes.clear();
  frameResources.cpuScopes.clear();
  frameResources.nextQuery = 0;
  // Capture the frame name from the renderer (if provided)
  frameResources.frameName = std::string(renderer.currentFrameLabel());
  frameResources.progressivePassHint = renderer.currentRenderPassIsProgressive();
  frameResources.cpuStart = std::chrono::steady_clock::now();
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
  frameResources.residencyPinnedTextures.clear();
  frameResources.transientOverrideSets.clear();
  frameResources.overrideSetsAllocated = 0;
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
  frameResources.scheduledCopies.clear();
  // Reset per-frame upload arena virtual block; free retired resources
  if (frameResources.uploadArena.block != nullptr) {
    vmaClearVirtualBlock(frameResources.uploadArena.block);
  }
  frameResources.uploadArena.highWatermark = 0;
  // Release any retired upload buffers from a previous use of this frame.
  for (auto& r : frameResources.uploadArena.retiredBuffers) {
    if (r.block != nullptr) {
      vmaDestroyVirtualBlock(r.block);
      r.block = nullptr;
    }
    r.buffer.reset();
  }
  frameResources.uploadArena.retiredBuffers.clear();
  // Ensure static arenas exist once a device is present
  ensureStaticArenas();

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

  // Prime persistent descriptor sets before command buffer recording begins.
  // This allows UBO/image bindings to be established without violating the
  // no-descriptor-writes-during-recording invariant.
  if (m_backgroundContext) {}
  if (m_lineContext) {
    m_lineContext->ensureDescriptorSets(renderer);
  }
  if (m_meshContext) {
    m_meshContext->ensureDescriptorSets();
  }
  if (m_ellipsoidContext) {
    m_ellipsoidContext->ensureDescriptorSets();
  }
  if (m_sphereContext) {
    m_sphereContext->ensureDescriptorSets();
  }
  if (m_coneContext) {
    m_coneContext->ensureDescriptorSets();
  }
  if (m_textureDualPeelContext) {
    m_textureDualPeelContext->ensureOITResources();
  }
  if (m_textureCopyContext) {
    m_textureCopyContext->ensureDescriptorLayout();
    m_textureCopyContext->ensureLightingResources();
    m_textureCopyContext->ensureOITResources();
  }
  if (m_textureWeightedAverageContext) {
    m_textureWeightedAverageContext->ensureDescriptorLayout();
  }
  if (m_textureWeightedBlendedContext) {
    m_textureWeightedBlendedContext->ensureDescriptorLayout();
    m_textureWeightedBlendedContext->ensureOITResources();
  }
  if (m_texturePPLLContext) {
    m_texturePPLLContext->ensureOITResources();
  }

  vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  auto& cmdBuffer = m_activeFrameHandle->commandBuffer();
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "VK cmdBegin: flags=eOneTimeSubmit";
  }
  cmdBuffer.begin(beginInfo);
  cmdBuffer.resetQueryPool(*frameResources.queryPool, 0, kMaxTimestampQueries);

  m_frameRecording = true;
  s_currentBackend = this;

  // Install scratch-pool deferred release scheduler for this backend.
  // (Used by RenderTargetLease to delay Vulkan slot reuse until the frame-slot
  // reaches the completion safe point.)
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
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

    // Common case: delay until the current frame-slot reaches the completion
    // safe point (applyPendingArenaReset).
    const std::string_view debugLabel = "scratch_pool_vulkan_release";
    if (m_activeFrame && m_activeFrameHandle && m_activeFrameHandle->valid()) {
      registerAfterCurrentFrameCompletionHook(
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
          FrameHookSpot::AfterFrameCompletionSafePoint,
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
    return;
  }

  auto& frame = *m_activeFrame;
  auto& frameHandle = *m_activeFrameHandle;
  m_lastSubmittedFrameKey = frameHandle.key();

  auto resetActiveStateGuard = folly::makeGuard([this]() {
    m_frameRecording = false;
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

  // Move submission-scoped residency pins out of the frame record so we can
  // unpin deterministically after submission, even if the frame record is reused.
  auto residencyPins = std::move(frame.residencyPinnedTextures);
  frame.residencyPinnedTextures.clear();

  // Move submission-scoped static-geometry segment pins out of the frame record
  // so we can unpin deterministically after submission, even if the frame record
  // is reused.
  auto staticSegmentPins = std::move(frame.pinnedStaticSegments);
  frame.pinnedStaticSegments.clear();

  bool submitted = false;
  try {
    queue.submit(submitInfo, *frameHandle.fence());
    device().frameExecutor().markSubmitted(frameHandle);
    submitted = true;
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Vulkan queue submit failed: " << e.what();
  }

  // Release residency pins after the GPU has finished executing this submission.
  // If submission failed, unpin immediately to avoid leaking pins indefinitely.
  if (!residencyPins.empty()) {
    if (submitted) {
      ZVulkanDevice* submitDevice = m_sharedDevice;
      device().frameExecutor().scheduleAfterCompletion(frameHandle,
                                                       [submitDevice, pins = std::move(residencyPins)]() mutable {
                                                         if (!submitDevice) {
                                                           return;
                                                         }
                                                         for (auto* tex : pins) {
                                                           submitDevice->residencyManager().unpinIfManaged(tex);
                                                         }
                                                       });
    } else {
      for (auto* tex : residencyPins) {
        device().residencyManager().unpinIfManaged(tex);
      }
    }
  }

  // Release static-geometry segment pins after the GPU has finished executing
  // this submission. This ensures deferred frees (per-stream eviction) can't
  // reuse the same suballocated ranges while the GPU is still reading them.
  if (!staticSegmentPins.empty()) {
    if (submitted) {
      Z3DRendererVulkanBackend* submitBackend = this;
      device().frameExecutor().scheduleAfterCompletion(
        frameHandle,
        [submitBackend, pins = std::move(staticSegmentPins)]() mutable {
          if (!submitBackend) {
            return;
          }
          for (void* segVoid : pins) {
            auto* seg = static_cast<StaticArena::Segment*>(segVoid);
            if (seg == nullptr) {
              continue;
            }
            CHECK_GT(seg->pinCount, 0u) << "Static segment pinCount underflow in completion callback";
            seg->pinCount--;
            if (seg->pinCount == 0) {
              submitBackend->flushPendingFreesAndMaybeTrimStaticSegment(seg);
            }
          }
        });
    } else {
      for (void* segVoid : staticSegmentPins) {
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
    if (submitted) {
      constexpr auto kPollInterval = std::chrono::milliseconds(1);
      const folly::CancellationToken cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
      void* waitedKey = frameHandle.key();
      bool waitedKeyCompleted = false;
      while (!waitedKeyCompleted) {
        if (cancellationToken.isCancellationRequested()) {
          throw ZCancellationException();
        }

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
    "VK segments: frame='{}' token={} submit#{} began={} clears={} loads={} pipelines_created={} pipelines_bound_unique={} overrides={} skipped_format_mismatch={} descriptor_writes_while_recording={} bound_set_rewrite_attempts={} readback_bytes_copied={} readback_slots_in_flight={} static_bytes_staged={} static_stream_restaged={} lines_bytes_staged={} fonts_bytes_staged={} meshes_bytes_staged={} spheres_bytes_staged={}",
    frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
    frame.realFrameToken,
    frame.submissionId,
    frame.renderingSegmentsBegan,
    frame.attachmentClears,
    frame.attachmentLoads,
    frame.pipelinesCreated,
    frame.pipelinesBound.size(),
    frame.overrideSetsAllocated,
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
    frame.spheresBytesStaged);

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

  const bool profilePipelineContexts = FLAGS_atlas_vk_profile_pipeline_contexts;
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

size_t Z3DRendererVulkanBackend::estimateFrameUniformOverheadBytes()
{
  // beginRender() always suballocates two lighting UBO slices (scene lighting +
  // picking lighting). Provide a conservative (alignment-rounded) estimate so
  // higher-level schedulers can size the arena before opening a Vulkan frame.
  ensureDevice();
  const size_t align = uniformAlignment();
  const size_t szLighting = vulkan::alignUp(sizeof(LightingUBOStd140), align);
  return 2u * szLighting;
}

size_t Z3DRendererVulkanBackend::estimateAdditionalUniformBytesForBatches(const RendererCPUState& state)
{
  ensureDevice();
  const size_t align = uniformAlignment();

  size_t total = 0;
  for (const auto& b : state.batches) {
    total += std::visit(
      [&](const auto& payload) -> size_t {
        using PayloadT = std::remove_cvref_t<decltype(payload)>;
        if constexpr (vulkan::kHasUniformArenaBudgetTraits<PayloadT>) {
          return vulkan::UniformArenaBudgetTraits<PayloadT>::estimateAdditionalBytes(payload, align);
        }
        return 0u;
      },
      b.geometry);
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

vk::Sampler Z3DRendererVulkanBackend::defaultSampler()
{
  ensureDefaultPlaceholders();
  return **m_defaultSampler;
}

vk::Sampler Z3DRendererVulkanBackend::nearestClampSampler()
{
  ensureDefaultPlaceholders();
  ensureSharedSamplers();
  return **m_nearestClampSampler;
}

void Z3DRendererVulkanBackend::ensureSharedDescriptorLayouts()
{
  ensureDevice();
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in ensureSharedDescriptorLayouts";

  auto& vkDevice = m_sharedDevice->context().device();

  if (!m_sharedDescriptorLayouts.meshTextures) {
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 2,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 3,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 4,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_sharedDescriptorLayouts.meshTextures.emplace(vkDevice, info);
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
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
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
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITDDPFlag,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment                        },
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITPPLLCounts,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITPPLLOffsets,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITPPLLCursors,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingOITPPLLFragments,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute}
    };
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_sharedDescriptorLayouts.oitParams.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.dualTexturePlaceholder) {
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_sharedDescriptorLayouts.dualTexturePlaceholder.emplace(vkDevice, info);
  }

  if (!m_sharedDescriptorLayouts.empty) {
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 0, .pBindings = nullptr};
    m_sharedDescriptorLayouts.empty.emplace(vkDevice, info);
  }
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::meshTextureDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.meshTextures ? **m_sharedDescriptorLayouts.meshTextures : vk::DescriptorSetLayout{};
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

vk::DescriptorSetLayout Z3DRendererVulkanBackend::dualTexturePlaceholderDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.dualTexturePlaceholder ? **m_sharedDescriptorLayouts.dualTexturePlaceholder
                                                          : vk::DescriptorSetLayout{};
}

vk::DescriptorSetLayout Z3DRendererVulkanBackend::emptyDescriptorSetLayout()
{
  ensureSharedDescriptorLayouts();
  return m_sharedDescriptorLayouts.empty ? **m_sharedDescriptorLayouts.empty : vk::DescriptorSetLayout{};
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
}

Z3DRendererVulkanBackend::UploadSlice Z3DRendererVulkanBackend::suballocateUpload(size_t bytes, size_t alignment)
{
  UploadSlice slice{};
  if (!m_activeFrame) {
    VLOG(2) << "suballocateUpload: inactive frame; returning null slice for " << bytes << " bytes";
    return slice;
  }
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in suballocateUpload";

  auto& arena = m_activeFrame->uploadArena;

  const size_t required = bytes; // virtual allocator handles alignment and placement

  auto ensureCapacity = [&](size_t minCapacity) {
    if (arena.buffer && arena.mapped && arena.capacity >= minCapacity) {
      return;
    }
    size_t newCapacity = std::max<size_t>(static_cast<size_t>(1) << 20, arena.capacity);
    while (newCapacity < minCapacity) {
      newCapacity = newCapacity ? (newCapacity * 2) : (static_cast<size_t>(1) << 20);
    }
    // Move the previous buffer to the retired list so any already-returned
    // mapped pointers remain valid for the rest of the frame.
    if (arena.buffer) {
      arena.retiredBuffers.push_back({std::move(arena.buffer), arena.block});
      arena.block = nullptr;
    }
    // Upload arena buffers act as sources for GPU copies into device-local
    // static buffers. They must carry TRANSFER_SRC usage (not DST).
    arena.buffer = m_sharedDevice->createBufferInPool(
      newCapacity,
      vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
        vk::BufferUsageFlagBits::eTransferSrc,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
      m_sharedDevice->uploadTransientPool());
    arena.capacity = newCapacity;
    // Persistently map the entire buffer
    arena.mapped = arena.buffer->map(0, newCapacity);
    // Create a fresh virtual block spanning [0, capacity)
    VmaVirtualBlockCreateInfo vinfo{};
    vinfo.size = newCapacity;
    if (vmaCreateVirtualBlock(&vinfo, &arena.block) != VK_SUCCESS) {
      arena.block = nullptr;
      LOG(ERROR) << "Failed to create VMA virtual block for upload arena";
    }
  };

  VLOG(2) << "suballocateUpload: request bytes=" << bytes << " align=" << alignment << " required=" << required
          << " cap=" << arena.capacity;
  const size_t prevCap = arena.capacity;
  void* prevMapped = arena.mapped;
  ensureCapacity(required);
  if (arena.capacity != prevCap || arena.mapped != prevMapped) {
    VLOG(2) << "suballocateUpload: after ensureCapacity cap=" << arena.capacity
            << " mapped=" << (arena.mapped != nullptr);
  }

  // Allocate from the virtual block
  VmaVirtualAllocationCreateInfo ainfo{};
  ainfo.size = bytes;
  ainfo.alignment = std::max<size_t>(1, alignment);
  VmaVirtualAllocation alloc = nullptr;
  VkDeviceSize vOffset = 0;
  if (arena.block == nullptr || vmaVirtualAllocate(arena.block, &ainfo, &alloc, &vOffset) != VK_SUCCESS) {
    VLOG(1) << "suballocateUpload: virtual allocate failed; attempting to grow arena";
    ensureCapacity(arena.capacity + required);
    if (arena.block == nullptr || vmaVirtualAllocate(arena.block, &ainfo, &alloc, &vOffset) != VK_SUCCESS) {
      VLOG(1) << "suballocateUpload: virtual allocate failed after growth";
      return slice;
    }
  }
  slice.buffer = arena.buffer->buffer();
  slice.offset = static_cast<vk::DeviceSize>(vOffset);
  slice.mapped = arena.mapped ? static_cast<uint8_t*>(arena.mapped) + static_cast<size_t>(vOffset) : nullptr;
  slice.size = bytes;
  arena.highWatermark = std::max<vk::DeviceSize>(arena.highWatermark, vOffset + static_cast<VkDeviceSize>(bytes));
  VLOG(2) << "suballocateUpload: slice buf=" << static_cast<bool>(slice.buffer) << " off=" << slice.offset
          << " size=" << slice.size << " mapped=" << (slice.mapped != nullptr);
  return slice;
}

void Z3DRendererVulkanBackend::reserveUploadSlices(std::initializer_list<std::pair<size_t, size_t>> slices)
{
  if (!FLAGS_vk_reserve_upload_slices) {
    return;
  }
  if (!m_activeFrame) {
    return;
  }
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in reserveUploadSlices";
  auto& arena = m_activeFrame->uploadArena;
  size_t cursor = 0; // virtual block will handle placement; we just size the buffer
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
  if (arena.buffer && arena.mapped && arena.capacity >= required) {
    return;
  }
  size_t newCapacity = std::max<size_t>(static_cast<size_t>(1) << 20, arena.capacity);
  while (newCapacity < required) {
    newCapacity = newCapacity ? (newCapacity * 2) : (static_cast<size_t>(1) << 20);
  }
  if (arena.buffer) {
    arena.retiredBuffers.push_back({std::move(arena.buffer), arena.block});
    arena.block = nullptr;
  }
  // Upload arena buffers act as the source of GPU copies into device-local
  // static buffers. They must be created with TRANSFER_SRC usage (not DST).
  arena.buffer = m_sharedDevice->createBufferInPool(
    newCapacity,
    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
      vk::BufferUsageFlagBits::eTransferSrc,
    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
    m_sharedDevice->uploadTransientPool());
  arena.capacity = newCapacity;
  arena.mapped = arena.buffer->map(0, newCapacity);
  // Create a fresh virtual block spanning [0, capacity)
  VmaVirtualBlockCreateInfo vinfo{};
  vinfo.size = newCapacity;
  if (vmaCreateVirtualBlock(&vinfo, &arena.block) != VK_SUCCESS) {
    arena.block = nullptr;
    LOG(ERROR) << "Failed to create VMA virtual block for upload arena (reserve)";
  }
  VLOG(2) << fmt::format("reserveUploadSlices: grew arena to {} bytes for {} slices", newCapacity, slices.size());
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
  seg->buffer = m_sharedDevice->createBuffer(capacityBytes, usage, vk::MemoryPropertyFlagBits::eDeviceLocal);
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
  } else {
    // Reset cursor for new frame
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
  m_ddpCountPipelineLayout.emplace(
    device,
    vk::PipelineLayoutCreateInfo{.setLayoutCount = 1, .pSetLayouts = &**m_ddpCountSetLayout});
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
    m_ppllScanLocalPipelineLayout.emplace(
      device,
      vk::PipelineLayoutCreateInfo{.setLayoutCount = 1, .pSetLayouts = &**m_ppllScanLocalSetLayout});

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
    m_ppllScanAddPipelineLayout.emplace(
      device,
      vk::PipelineLayoutCreateInfo{.setLayoutCount = 1, .pSetLayouts = &**m_ppllScanAddSetLayout});

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
  return FLAGS_atlas_vk_ddp_indirect_count && m_supportsDrawIndirectCount && m_supportsFragStoresAndAtomics;
}

void Z3DRendererVulkanBackend::primePPLLForCountPass(const glm::uvec4& viewport)
{
  primePPLLForStorePass(viewport, 0);
}

void Z3DRendererVulkanBackend::primePPLLForStorePass(const glm::uvec4& viewport, uint64_t requestedFragments)
{
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in primePPLLForStorePass";
  CHECK(m_supportsFragStoresAndAtomics) << "PPLL requires fragment storage buffer atomics (fragmentStoresAndAtomics)";

  const size_t desiredRing = std::max<size_t>(1, static_cast<size_t>(m_maxFramesInFlight));
  if (m_ppllFrameRing.size() != desiredRing) {
    m_ppllFrameRing.resize(desiredRing);
  }
  const size_t ringSize = m_ppllFrameRing.empty() ? 1 : m_ppllFrameRing.size();
  const uint64_t realFrameToken = Z3DRenderGlobalState::instance().currentPerfFrameToken();
  m_activePPLLIndex = static_cast<size_t>(realFrameToken % ringSize);

  ensurePPLLResources(viewport, requestedFragments);
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
                        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
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
  if (!m_ddpCountPipeline) {
    return;
  }
  // Allocate override set with the two buffers
  auto* ds = allocateOverrideDescriptorSet(**m_ddpCountSetLayout);
  CHECK(ds != nullptr);
  if (ds) {
    // binding 0 = flag, binding 1 = count
    if (m_activeFrame && m_activeFrame->ddpChangedFlag) {
      ds->updateStorageBuffer(0, *m_activeFrame->ddpChangedFlag);
    }
    if (m_activeFrame && m_activeFrame->ddpIndirectCount) {
      ds->updateStorageBuffer(1, *m_activeFrame->ddpIndirectCount);
    }
  }
  // Dispatch (1,1,1)
  cmd.bindPipeline(vk::PipelineBindPoint::eCompute, **m_ddpCountPipeline);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **m_ddpCountPipelineLayout, 0, {ds->descriptorSet()}, {});
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
  CHECK(m_ppllScanLocalSetLayout.has_value());

  auto* ds = allocateOverrideDescriptorSet(**m_ppllScanLocalSetLayout);
  CHECK(ds != nullptr);
  CHECK(ppll.params && ppll.counts && ppll.offsets && ppll.blockSums);
  ds->updateStorageBuffer(0, *ppll.params);
  ds->updateStorageBuffer(1, *ppll.counts);
  ds->updateStorageBuffer(2, *ppll.offsets);
  ds->updateStorageBuffer(3, *ppll.blockSums);

  cmd.bindPipeline(vk::PipelineBindPoint::eCompute, **m_ppllScanLocalPipeline);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                         **m_ppllScanLocalPipelineLayout,
                         0,
                         {ds->descriptorSet()},
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
  CHECK(m_ppllScanAddSetLayout.has_value());

  auto* ds = allocateOverrideDescriptorSet(**m_ppllScanAddSetLayout);
  CHECK(ds != nullptr);
  CHECK(ppll.params && ppll.offsets && ppll.blockPrefixes);
  ds->updateStorageBuffer(0, *ppll.params);
  ds->updateStorageBuffer(1, *ppll.offsets);
  ds->updateStorageBuffer(2, *ppll.blockPrefixes);

  cmd.bindPipeline(vk::PipelineBindPoint::eCompute, **m_ppllScanAddPipeline);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **m_ppllScanAddPipelineLayout, 0, {ds->descriptorSet()}, {});
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
  return align ? align : 256;
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

Z3DRendererVulkanBackend::StaticSlice Z3DRendererVulkanBackend::allocateStaticVB(size_t bytes, size_t alignment)
{
  ensureStaticArenas();
  StaticSlice slice{};
  if (bytes == 0) {
    return slice;
  }

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

  // First try existing segments (reusing freed ranges).
  for (const auto& seg : m_staticArena.vb) {
    if (tryAlloc(seg.get())) {
      return slice;
    }
  }

  // Need a new segment. Size it to fit this allocation and grow by powers of two
  // to avoid pathological churn while preserving correctness.
  constexpr size_t kDefaultSegment = static_cast<size_t>(32) * 1024 * 1024; // 32 MiB
  size_t requiredBytes = std::max(bytes, align);
  size_t cap = kDefaultSegment;
  while (cap < requiredBytes) {
    CHECK(cap <= std::numeric_limits<size_t>::max() / 2) << "Static VB segment capacity overflow";
    cap *= 2;
  }

  auto newSeg = createStaticArenaSegment(StaticArena::Kind::Vertex, cap);
  if (!newSeg) {
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
  ensureStaticArenas();
  StaticSlice slice{};
  if (bytes == 0) {
    return slice;
  }

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

  for (const auto& seg : m_staticArena.ib) {
    if (tryAlloc(seg.get())) {
      return slice;
    }
  }

  constexpr size_t kDefaultSegment = static_cast<size_t>(8) * 1024 * 1024; // 8 MiB
  size_t requiredBytes = std::max(bytes, align);
  size_t cap = kDefaultSegment;
  while (cap < requiredBytes) {
    CHECK(cap <= std::numeric_limits<size_t>::max() / 2) << "Static IB segment capacity overflow";
    cap *= 2;
  }

  auto newSeg = createStaticArenaSegment(StaticArena::Kind::Index, cap);
  if (!newSeg) {
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
  if (!m_activeFrame) {
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
  std::lock_guard<std::mutex> lock(m_pendingEvictionsMutex);
  m_pendingEvictStreamKeys.insert(streamKey);
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
  auto set = m_sharedDevice->createDescriptorSet(*m_activeFrame->descriptorPool, layout, /*isOverrideTransient*/ false);
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
  auto set = m_sharedDevice->createDescriptorSet(*m_activeFrame->persistentDescriptorPool,
                                                 layout,
                                                 /*isOverrideTransient*/ false);
  if (set) {
    m_activeFrame->descriptorSetsAllocated++;
  }
  return set;
}

ZVulkanDescriptorSet* Z3DRendererVulkanBackend::allocateOverrideDescriptorSet(vk::DescriptorSetLayout layout)
{
  if (!m_activeFrame) {
    return nullptr;
  }
  CHECK(m_sharedDevice != nullptr) << "Shared Vulkan device missing in allocateOverrideDescriptorSet";
  ensureArenaOnFrame(*m_activeFrame);
  if (!m_activeFrame->descriptorPool) {
    return nullptr;
  }
  auto set = m_sharedDevice->createDescriptorSet(*m_activeFrame->descriptorPool, layout, /*isOverrideTransient*/ true);
  if (!set) {
    return nullptr;
  }
  ZVulkanDescriptorSet* raw = set.get();
  m_activeFrame->transientOverrideSets.push_back(std::move(set));
  m_activeFrame->overrideSetsAllocated++;
  return raw;
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

  // If we're mid-recording and have pinned managed textures for this (not yet submitted)
  // command buffer, unpin them now to avoid carrying pins into teardown without a fence.
  if (m_activeFrame && (!m_activeFrameHandle || !m_activeFrameHandle->valid())) {
    // Defensive: if m_activeFrame exists without a valid handle, it cannot be submitted.
    if (!m_activeFrame->residencyPinnedTextures.empty()) {
      for (auto* tex : m_activeFrame->residencyPinnedTextures) {
        m_sharedDevice->residencyManager().unpinIfManaged(tex);
      }
      m_activeFrame->residencyPinnedTextures.clear();
    }
  } else if (m_activeFrame && m_activeFrameHandle && m_activeFrameHandle->valid() && m_frameRecording) {
    // We have an active recording. We do not attempt to submit during teardown; just drop
    // any pins accumulated so far since they have not been consumed by the GPU.
    if (!m_activeFrame->residencyPinnedTextures.empty()) {
      for (auto* tex : m_activeFrame->residencyPinnedTextures) {
        m_sharedDevice->residencyManager().unpinIfManaged(tex);
      }
      m_activeFrame->residencyPinnedTextures.clear();
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
  auto wrapper = [label = std::move(label), task = std::move(task)]() mutable -> folly::coro::Task<void> {
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
  };

  m_detachedTaskScope.add(folly::coro::co_withExecutor(std::move(ex), wrapper()));
}

void Z3DRendererVulkanBackend::pinTextureForActiveSubmission(ZVulkanTexture* texture)
{
  if (!texture || !m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
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
    m_sharedDescriptorLayouts = {};
    m_sharedDevice = dev;
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
  }
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
    // Destroy transient override sets BEFORE resetting the pool to avoid
    // vkFreeDescriptorSets after implicit free by pool reset.
    frame.transientOverrideSets.clear();
    frame.descriptorPool->reset();
    frame.arenaResetScheduled = false;
    frame.arenaResetsPerformed++;
  }

  // Barrier: execute all registered hooks for this frame-slot completion safe
  // point, then continue.
  //
  // We capture and rethrow after waking awaiters so cancellation/errors do not
  // strand coroutines waiting on the frame completion signal.
  FrameResources* previousSafePointFrame = m_frameInCompletionSafePoint.exchange(&frame, std::memory_order_acq_rel);

  std::exception_ptr deferredException;
  try {
    folly::coro::blockingWait(
      frame.afterFrameCompletionHooks.reach(FrameHookSpot::AfterFrameCompletionSafePoint, *this));
  }
  catch (...) {
    deferredException = std::current_exception();
  }

  m_frameInCompletionSafePoint.store(previousSafePointFrame, std::memory_order_release);

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
      slot.buffer = device().createBufferInPool(minBytes,
                                                vk::BufferUsageFlagBits::eTransferDst,
                                                vk::MemoryPropertyFlagBits::eHostVisible |
                                                  vk::MemoryPropertyFlagBits::eHostCoherent |
                                                  vk::MemoryPropertyFlagBits::eHostCached,
                                                device().uploadStagingPool());
      slot.capacity = minBytes;
      slot.mapped = slot.buffer->map(0, minBytes);
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

  const glm::uvec2 size{src.width(), src.height()};
  const vk::Format fmt = src.format();
  auto bytesPerPixel = [](vk::Format f) -> size_t {
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
        // Conservative default for common uncompressed formats.
        return 4u;
    }
  };

  const size_t bytes = static_cast<size_t>(size.x) * size.y * bytesPerPixel(fmt);
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
  if (dst) {
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

  if (frame.nextQuery > 0 && !frame.gpuScopes.empty()) {
    // Optional calibration: map GPU ticks to CPU nanoseconds for trace alignment
    if (FLAGS_atlas_perf_trace_calibrated) {
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
  } else {
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
    " | draws={} dsets={} ovsets={} pipes+={} bound={} segs={} clr={} ld={} dwr={} rew={} upload_hi={}B ubo_hi={}B static={}B rb={}B rbinflight={} sec2=a{} f{} m{} h{} b{} e{} mask=0x{:x}",
    frame.drawsSubmitted,
    frame.descriptorSetsAllocated,
    frame.overrideSetsAllocated,
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
  if (frame.nextQuery > 0 && !frame.gpuScopes.empty()) {
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
    stats.overrideSetsAllocated = frame.overrideSetsAllocated;
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
    stats.readbackBytesCopied = frame.readbackBytesCopied;
    stats.readbackSlotsInFlight = frame.readbackSlotsInFlight;
    stats.allSamples = frame.allSamples;
    stats.allMaxMs = frame.allMaxMs.value_or(0.0);
    stats.drawSecondaryCacheAttempts = frame.drawSecondaryCacheAttempts;
    stats.drawSecondaryCacheKeyFound = frame.drawSecondaryCacheKeyFound;
    stats.drawSecondaryCacheSignatureMismatches = frame.drawSecondaryCacheSignatureMismatches;
    stats.drawSecondaryCacheSignatureMismatchMaskOr = frame.drawSecondaryCacheSignatureMismatchMaskOr;
    stats.drawSecondaryCacheHits = frame.drawSecondaryCacheHits;
    stats.drawSecondaryCacheBuilds = frame.drawSecondaryCacheBuilds;
    stats.drawSecondaryCacheExecutes = frame.drawSecondaryCacheExecutes;

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
}

std::optional<size_t> Z3DRendererVulkanBackend::beginGpuScope(std::string_view label)
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid() || label.empty() || !m_frameRecording) {
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
