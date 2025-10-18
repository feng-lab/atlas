#include "z3drenderervulkanbackend.h"

#include "z3drendererbase.h"
#include "z3drendercommands.h"
#include "z3dcompositorpass.h"
#include "z3dboundedfilter.h"
#include "z3dshaderprogram.h"
#include "zlog.h"
#include "zvulkandevice.h"
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
#include "zvulkantextureglowpipelinecontext.h"
#include "zvulkanimgslicepipelinecontext.h"
#include "zvulkanimgraycasterpipelinecontext.h"
#include "z3dimgraycasterrenderer.h"
#include "zvulkanfontpipelinecontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "z3drenderglobalstate.h"
#include "z3dscratchresourcepool.h"
#include "zvulkanbuffer.h"
#include "z3dperfcollector.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include <gflags/gflags.h>

DEFINE_bool(vk_reserve_upload_slices,
            true,
            "Reserve per-draw upload arena capacity (precise) before suballocation to avoid mid-upload growth");
DECLARE_string(atlas_perf_mode);
DECLARE_bool(atlas_perf_trace_calibrated);

// Optional: collect frame timings at endRender instead of next beginRender.
// When true, endRender will fence-wait to harvest GPU timestamps immediately
// (may reduce concurrency). When false, timings are collected at next beginRender.
DEFINE_bool(atlas_vk_collect_timings_on_end,
            false,
            "Collect and log Vulkan frame timings at endRender (fence-wait as needed)");

namespace nim {

namespace {
constexpr uint32_t kMaxTimestampQueries = 64u;
constexpr uint32_t kMaxOcclusionQueries = 64u;

// TODO: option (2) - split each peel pass into its own command buffer, wait for the GPU to finish,
// read the occlusion query immediately, and stop submitting once no fragments are left. This would
// mirror GL behaviour but requires a per-pass submit/wait model.

inline bool queryPoolValid(const vk::raii::QueryPool& pool)
{
  return static_cast<VkQueryPool>(*pool) != VK_NULL_HANDLE;
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
  , m_textureGlowContext(std::make_unique<ZVulkanTextureGlowPipelineContext>(*this))
  , m_imgSliceContext(std::make_unique<ZVulkanImgSlicePipelineContext>(*this))
{}

Z3DRendererVulkanBackend::~Z3DRendererVulkanBackend() = default;

Z3DRendererVulkanBackend* Z3DRendererVulkanBackend::current()
{
  return s_currentBackend;
}

void Z3DRendererVulkanBackend::preBackendSwitch()
{
  // Drop shared placeholders; they'll be recreated lazily on next use.
  m_defaultPlaceholder2D.reset();
  m_defaultSampler.reset();
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

  // Global coordination (device waitIdle, scratch-pool reset) is handled by
  // the rendering engine during backend switches. Avoid touching global state
  // here to prevent ordering conflicts with persistent lease release.
  if (m_sharedDevice) {
    for (auto& frame : m_frames) {
      collectFrameTimings(frame);
    }
  }

  // Flush any closed perf tokens now that we've ingested outstanding timings.
  Z3DPerfCollector::instance().maybeFlush(true);

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
    "pass_end pass='{}' cpu={:.3f} ms draws={} segs={} clr={} ld={} dsets={} ovsets={} pipes_bound_delta={} dwr={} rew={} uploads_delta={}B static_delta={}B rb_delta={}B rbinflight_delta={} transitions={} noop={}",
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
    m_passScope.layoutNoops);
  m_passScope = {};
}

void Z3DRendererVulkanBackend::notifyDrawSubmitted()
{
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

void Z3DRendererVulkanBackend::beginRender(Z3DRendererBase& renderer)
{
  s_currentBackend = this;
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

  const auto& viewport = renderer.frameState().viewport;
  const uint32_t width = viewport.z;
  const uint32_t height = viewport.w;
  const auto& surf = renderer.frameState().activeSurface;
  if (VLOG_IS_ON(2)) {
    uint64_t c0Handle = 0;
    uint32_t c0W = 0, c0H = 0;
    auto c0Fmt = enumOrUnderlying(vk::Format{}, 16);
    if (!surf.colorAttachments.empty() && surf.colorAttachments[0].handle.valid() &&
        surf.colorAttachments[0].handle.backend == AttachmentBackend::Vulkan) {
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
        surf.depthAttachment->handle.backend == AttachmentBackend::Vulkan) {
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
    return;
  }

  m_activeFrameHandle = device().frameExecutor().beginFrame();
  if (!m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    m_activeFrame = nullptr;
    m_frameRecording = false;
    return;
  }

  auto& frameResources = ensureFrameResourcesForKey(m_activeFrameHandle->key());

  // Stage 2: apply descriptor arena reset when reusing this in-flight frame.
  // Safe point: frame executor waited for the fence when acquiring the frame.
  applyPendingArenaReset(frameResources);
  ensureArenaOnFrame(frameResources);
  frameResources.descriptorSetsAllocated = 0;
  frameResources.leaseRecycleQueued = 0;
  frameResources.leaseRecycleExecuted = 0;

  collectFrameTimings(frameResources);
  frameResources.gpuScopes.clear();
  frameResources.cpuScopes.clear();
  frameResources.nextQuery = 0;
  // Capture the frame name from the renderer (if provided)
  frameResources.frameName = std::string(renderer.currentFrameLabel());
  frameResources.cpuStart = std::chrono::steady_clock::now();
  frameResources.cpuEnd = {};
  // Tag this submission with the current real-frame token and a submission index
  frameResources.realFrameToken = Z3DRenderGlobalState::instance().currentPerfFrameToken();
  frameResources.submissionId = ++m_submissionCursor[frameResources.realFrameToken];
  // Reset Stage 3 instrumentation
  frameResources.renderingSegmentsBegan = 0;
  frameResources.attachmentClears = 0;
  frameResources.attachmentLoads = 0;
  frameResources.pipelinesCreated = 0;
  frameResources.pipelinesBound.clear();
  frameResources.transientOverrideSets.clear();
  frameResources.overrideSetsAllocated = 0;
  frameResources.activeSegmentFormats.reset();
  frameResources.skippedBatchesFormatMismatch = 0;
  frameResources.descriptorWritesWhileRecording = 0;
  frameResources.boundSetRewriteAttempts = 0;
  // Reset Stage 4 (readback) bookkeeping
  frameResources.pendingColorReadbacks.clear();
  frameResources.readbackBytesCopied = 0;
  frameResources.readbackSlotsInFlight = 0;
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

  // Expose frame resources to allow pre-record descriptor priming
  m_activeFrame = &frameResources;
  frameResources.nextOcclusionQuery = 0;
  frameResources.occlusionQueryNeedsWait = false;

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
    m_textureCopyContext->ensureOITResources();
  }
  if (m_textureWeightedAverageContext) {
    m_textureWeightedAverageContext->ensureDescriptorLayout();
    m_textureWeightedAverageContext->ensureOITResources();
  }
  if (m_textureWeightedBlendedContext) {
    m_textureWeightedBlendedContext->ensureDescriptorLayout();
    m_textureWeightedBlendedContext->ensureOITResources();
  }

  vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  auto& cmdBuffer = m_activeFrameHandle->commandBuffer();
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "VK cmdBegin: flags=eOneTimeSubmit";
  }
  cmdBuffer.begin(beginInfo);
  cmdBuffer.resetQueryPool(*frameResources.queryPool, 0, kMaxTimestampQueries);
  if (queryPoolValid(frameResources.occlusionQueryPool)) {
    cmdBuffer.resetQueryPool(*frameResources.occlusionQueryPool, 0, kMaxOcclusionQueries);
  }

  m_frameRecording = true;

  // Install scratch-pool deferred release scheduler for this active frame
  {
    auto& pool = Z3DRenderGlobalState::instance().scratchPool();
    pool.setVulkanReleaseScheduler([this](std::function<void()> fn) {
      this->scheduleAfterCurrentFrameCompletion(std::move(fn));
    });
  }
}

void Z3DRendererVulkanBackend::endRender(Z3DRendererBase& renderer)
{
  (void)renderer;
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    s_currentBackend = nullptr;
    return;
  }

  auto& frame = *m_activeFrame;
  auto& frameHandle = *m_activeFrameHandle;

  // Insert end-of-frame image->buffer copies for pending readbacks
  if (m_frameRecording && !frame.pendingColorReadbacks.empty()) {
    auto& cmd = frameHandle.commandBuffer();
    for (auto& pr : frame.pendingColorReadbacks) {
      if (pr.slotIndex < 0 || pr.src == nullptr) {
        continue;
      }
      try {
        const auto originalLayout = pr.src->layout();
        pr.src->transitionLayout(cmd,
                                 originalLayout,
                                 vk::ImageLayout::eTransferSrcOptimal,
                                 vk::ImageAspectFlagBits::eColor);
        vk::BufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = vk::Offset3D{0, 0, 0};
        region.imageExtent = vk::Extent3D{pr.size.x, pr.size.y, 1u};
        const auto& slot = m_readbackSlots[static_cast<size_t>(pr.slotIndex)];
        cmd.copyImageToBuffer(pr.src->image(), vk::ImageLayout::eTransferSrcOptimal, slot.buffer->buffer(), region);
        const vk::ImageLayout restore =
          (originalLayout == vk::ImageLayout::eUndefined) ? vk::ImageLayout::eGeneral : originalLayout;
        pr.src->transitionLayout(cmd, vk::ImageLayout::eTransferSrcOptimal, restore, vk::ImageAspectFlagBits::eColor);
        frame.readbackBytesCopied += pr.bytes;
        frame.readbackSlotsInFlight++;
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Vulkan readback copy failed: " << e.what();
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
      m_frameRecording = false;
      m_activeFrameHandle.reset();
      m_activeFrame = nullptr;
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
  const bool waitForReadbacks = (m_activeFrame && !m_activeFrame->pendingColorReadbacks.empty());
  const bool waitForOcclusion = frame.occlusionQueryNeedsWait;
  const bool needFenceWait = waitForReadbacks || waitForOcclusion || m_pumpFenceAfterFirstSubmit;
  if (VLOG_IS_ON(1)) {
    VLOG(1) << fmt::format(
      "VK queueSubmit: frame='{}' token={} submit#{} cmd=0x{:x} fence=0x{:x} wait_readback={} wait_occlusion={} pending_readbacks={} queries={}",
      frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
      frame.realFrameToken,
      frame.submissionId,
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkCommandBuffer>(rawCmd))),
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkFence>(*frameHandle.fence()))),
      waitForReadbacks,
      waitForOcclusion,
      frame.pendingColorReadbacks.size(),
      frame.nextOcclusionQuery);
  }

  try {
    queue.submit(submitInfo, *frameHandle.fence());
    device().frameExecutor().markSubmitted(frameHandle);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Vulkan queue submit failed: " << e.what();
  }

  // Stage 2: schedule exactly one descriptor arena reset for this frame.
  scheduleArenaReset(frame);

  if (needFenceWait) {
    device().frameExecutor().waitForCompletion(frameHandle);
    applyPendingArenaReset(frame); // runs deferred readback consumers now

    // Fence signaled; deferred readbacks/releases executed.

    if (waitForOcclusion && queryPoolValid(frame.occlusionQueryPool) && frame.nextOcclusionQuery > 0) {
      const vk::QueryResultFlags queryFlags = vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait;
      auto [result, queryData] =
        frame.occlusionQueryPool.getResults<uint64_t>(0,
                                                      frame.nextOcclusionQuery,
                                                      frame.nextOcclusionQuery * sizeof(uint64_t),
                                                      sizeof(uint64_t),
                                                      queryFlags);
      if (result == vk::Result::eSuccess) {
        m_recentOcclusionResults.assign(queryData.begin(), queryData.begin() + frame.nextOcclusionQuery);
      } else {
        VLOG(1) << "Vulkan occlusion query results unavailable: " << vk::to_string(result);
        m_recentOcclusionResults.clear();
      }
    } else {
      m_recentOcclusionResults.clear();
    }

    frame.occlusionQueryNeedsWait = false;
    frame.nextOcclusionQuery = 0;

    if (m_pumpFenceAfterFirstSubmit) {
      VLOG(1) << "VK pumped first-frame fence: delivered readback callbacks";
      m_pumpFenceAfterFirstSubmit = false;
    }
  } else {
    m_recentOcclusionResults.clear();
    frame.nextOcclusionQuery = 0;
    frame.occlusionQueryNeedsWait = false;
  }

  // VLOG(1) frame recycling stats (descriptors and arena reset scheduling)
  vlogFrameRecyclingStats(frame);

  // Optionally collect and print frame timings at endRender instead of next beginRender.
  // This provides earlier visibility when we have already waited for the fence
  // (e.g., due to readbacks or occlusion), or when explicitly requested via flag.
  const bool collectNow = needFenceWait || FLAGS_atlas_vk_collect_timings_on_end;
  if (collectNow) {
    if (!needFenceWait && FLAGS_atlas_vk_collect_timings_on_end) {
      // Explicitly wait only when the flag requests immediate timing collection.
      device().frameExecutor().waitForCompletion(frameHandle);
      applyPendingArenaReset(frame);
      // Fence signaled for timings collection.
    }
    collectFrameTimings(frame);
  }

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

  // Stage 5: VLOG(1) static/upload arena usage
  VLOG(1) << fmt::format(
    "VK arena: frame='{}' token={} submit#{} upload_high_watermark={}B static_vb_used={}B static_ib_used={}B",
    frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
    frame.realFrameToken,
    frame.submissionId,
    m_activeFrame ? m_activeFrame->uploadArena.highWatermark : 0,
    m_staticArena.vbHighWatermark,
    m_staticArena.ibHighWatermark);

  m_activeFrameHandle.reset();
  m_activeFrame = nullptr;
  s_currentBackend = nullptr;
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

  auto& cmd = m_activeFrameHandle->commandBuffer();

  // Reset self-managed clear tracking for this sequence of batches
  m_selfManagedClearKeys.clear();

  // Build a simple attachment key for coalescing
  struct AttachKey
  {
    std::vector<uint64_t> colors;
    uint64_t depth = 0;
    bool operator==(const AttachKey& o) const
    {
      return depth == o.depth && colors == o.colors;
    }
  };
  auto buildKey = [](const RenderBatch& b) {
    AttachKey k;
    k.colors.reserve(b.pass.colorAttachments.size());
    for (const auto& a : b.pass.colorAttachments) {
      k.colors.push_back(a.handle.id);
    }
    if (b.pass.depthAttachment) {
      k.depth = b.pass.depthAttachment->handle.id;
    }
    return k;
  };

  // Begin a dynamic rendering segment for the batch's attachments via the recorder
  auto beginSegmentForBatch = [&](const RenderBatch& batch,
                                  ZVulkanPipelineCommandRecorder& recorder,
                                  ZVulkanPipelineCommandRecorder::RenderingSegmentSpec& outSpec) {
    outSpec = {};
    // Render area from pass viewport/scissor
    outSpec.renderArea = vulkan::toVkScissor(batch.pass);

    // Build color attachments
    outSpec.colorAttachments.reserve(batch.pass.colorAttachments.size());
    for (const auto& attachment : batch.pass.colorAttachments) {
      if (!attachment.handle.valid()) {
        continue;
      }
      auto& texture = vulkan::textureFromHandle(attachment.handle, device(), "renderer color attachment");
      ZVulkanAttachmentInfo info{};
      info.image = texture.image();
      info.view = texture.imageView();
      info.format = texture.format();
      info.initialLayout = texture.layout();
      // Default to color-attachment final layout, but allow shader-hook/geometry-driven overrides
      // to optimize subsequent sampling passes.
      info.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
      info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
      info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
      info.aspect = vk::ImageAspectFlagBits::eColor;
      info.trackingTexture = &texture;
      vk::ClearValue clear{};
      clear.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                             attachment.clearValue.color.g,
                                                             attachment.clearValue.color.b,
                                                             attachment.clearValue.color.a});
      info.clearValue = clear;
      // Optimizations to reduce read-after-write transitions between segments:
      const auto hook = renderer.shaderHookType();
      if (hook == Z3DRendererBase::ShaderHookType::WeightedAverageInit ||
          hook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit ||
          hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
          hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
        info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
      }
      // DDP blend (texture stage) renders to the back-blend accumulation, which is sampled in the final pass.
      if (const auto* ddp = std::get_if<TextureDualPeelPayload>(&batch.geometry)) {
        if (ddp->stage == TextureDualPeelPayload::Stage::Blend) {
          info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }
      }
      // Image filter subpasses should leave outputs readable for downstream compositors.
      // Explicitly detect the labels emitted by Z3DImgFilter.
      {
        std::string_view lbl = renderer.currentPassLabel();
        if (lbl == std::string_view("raycaster") || lbl == std::string_view("bbox_overlay")) {
          info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }
      }
      outSpec.colorAttachments.push_back(info);
    }

    // Optional depth attachment
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      const auto& attachment = *batch.pass.depthAttachment;
      auto& texture = vulkan::textureFromHandle(attachment.handle, device(), "renderer depth attachment");
      ZVulkanAttachmentInfo info{};
      info.image = texture.image();
      info.view = texture.imageView();
      info.format = texture.format();
      info.initialLayout = texture.layout();
      info.finalLayout = vk::ImageLayout::eDepthAttachmentOptimal;
      info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
      info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
      // Treat all depth(-stencil) formats as depth aspect for rendering; recorder will bind stencil if needed
      info.aspect = vk::ImageAspectFlagBits::eDepth;
      info.trackingTexture = &texture;
      vk::ClearValue clear{};
      clear.depthStencil = vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
      info.clearValue = clear;
      // Match color override for image filter passes: make depth sampled-readable too.
      {
        std::string_view lbl = renderer.currentPassLabel();
        if (lbl == std::string_view("raycaster") || lbl == std::string_view("bbox_overlay")) {
          info.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
        }
      }
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
    if (VLOG_IS_ON(2)) {
      if (!batch.pass.colorAttachments.empty()) {
        const auto& a = batch.pass.colorAttachments.front();
        if (a.handle.valid() && a.handle.backend == AttachmentBackend::Vulkan) {
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
          batch.pass.depthAttachment->handle.backend == AttachmentBackend::Vulkan) {
        auto* dtex = reinterpret_cast<ZVulkanTexture*>(batch.pass.depthAttachment->handle.id);
        if (dtex) {
          depthHandle = reinterpret_cast<uint64_t>(dtex);
          depthFmt = enumOrUnderlying(dtex->format(), 16);
          depthW = dtex->width();
          depthH = dtex->height();
        }
      }
      std::string passLabel = std::string(renderer.currentPassLabel());
      if (passLabel.empty()) {
        passLabel = "<unnamed>";
      }
      outSpec.debugLabel = passLabel;
      VLOG(2) << fmt::format(
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
        if (a.loadOp == LoadOp::Clear) {
          m_activeFrame->attachmentClears++;
        } else {
          m_activeFrame->attachmentLoads++;
        }
      }
      if (batch.pass.depthAttachment) {
        if (batch.pass.depthAttachment->loadOp == LoadOp::Clear) {
          m_activeFrame->attachmentClears++;
        } else {
          m_activeFrame->attachmentLoads++;
        }
      }
    }
    return true;
  };

  auto isSelfManaged = [](const GeometryPayload& g) -> bool {
    return std::holds_alternative<TextureGlowPayload>(g) || std::holds_alternative<ImgSlicePayload>(g) ||
           std::holds_alternative<ImgRaycasterPayload>(g);
  };

  size_t batchIndex = 0;
  std::optional<AttachKey> currentKey;
  bool segmentOpen = false;
  std::optional<ZVulkanPipelineCommandRecorder::RenderingSegmentSpec> openSpec{};
  ZVulkanPipelineCommandRecorder recorder(cmd);
  for (const auto& batch : state.batches) {
    const auto geom = describeGeometry(batch.geometry);
    VLOG(1) << fmt::format("VK batch[{}]: geom={}, colors={}, depth={} viewport=({},{} {}x{})",
                           batchIndex++,
                           geom,
                           batch.pass.colorAttachments.size(),
                           batch.pass.depthAttachment.has_value(),
                           static_cast<int>(batch.pass.viewport.origin.x),
                           static_cast<int>(batch.pass.viewport.origin.y),
                           static_cast<int>(batch.pass.viewport.extent.x),
                           static_cast<int>(batch.pass.viewport.extent.y));

    // Note: do not forcibly close segments before texture_copy; rely on
    // producers to end in sampled layouts to avoid in-segment transitions.

    // Ensure any sampled inputs referenced by this batch are transitioned to shader-read

    auto classifyReadLayout = [&](vk::Format format) {
      struct
      {
        vk::ImageLayout layout;
        vk::ImageAspectFlags aspect;
      } result;
      switch (format) {
        case vk::Format::eD16Unorm:
        case vk::Format::eX8D24UnormPack32:
        case vk::Format::eD32Sfloat:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
          // Treat all depth(-stencil) formats as depth-only for sampling
          result.layout = vk::ImageLayout::eDepthReadOnlyOptimal;
          result.aspect = vk::ImageAspectFlagBits::eDepth;
          break;
        case vk::Format::eS8Uint:
          result.layout = vk::ImageLayout::eStencilReadOnlyOptimal;
          result.aspect = vk::ImageAspectFlagBits::eStencil;
          break;
        default:
          result.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
          result.aspect = {};
      }
      return result;
    };
    auto ensureSampledReadable = [&](const AttachmentHandle& handle) {
      if (!handle.valid()) {
        return;
      }
      auto& texture = vulkan::textureFromHandle(handle, device(), "renderer sampled attachment");
      const auto samplingState = classifyReadLayout(texture.format());
      vk::ImageAspectFlags transitionAspect = samplingState.aspect;
      const bool needsTransition = (texture.layout() != samplingState.layout);
      // Only log when a transition is actually required to reduce noise.
      if (needsTransition && VLOG_IS_ON(2)) {
        std::string passLabel = std::string(renderer.currentPassLabel());
        if (passLabel.empty()) {
          passLabel = "<unlabeled-pass>";
        }
        VLOG(2) << fmt::format("ensureSampledReadable('{}'): handle=0x{:x} {} -> {} fmt={}",
                               passLabel,
                               handle.id,
                               enumOrUnderlying(texture.layout(), 16),
                               enumOrUnderlying(samplingState.layout, 16),
                               enumOrUnderlying(texture.format(), 16));
      }
      // Skip transition if already in the desired sampled layout; still update descriptor layout for depth aspects.
      if (auto* be = Z3DRendererVulkanBackend::current()) {
        be->notifyLayoutTransition(!needsTransition);
      }
      if (needsTransition) {
        texture.transitionLayout(cmd, texture.layout(), samplingState.layout, transitionAspect);
      }
      texture.setDescriptorLayout(samplingState.layout);
    };

    // Intentionally do not close active segments here. We want transitions to surface during
    // development if producers are not ending in the correct sampled layout.
    if (const auto* weightedAverage = std::get_if<TextureWeightedAveragePayload>(&batch.geometry)) {
      const bool hasDepth = batch.pass.depthAttachment.has_value() && batch.pass.depthAttachment->handle.valid();
      VLOG(1) << fmt::format("WA resolve depthAttachment present={} handle=0x{:x}",
                             hasDepth,
                             hasDepth ? batch.pass.depthAttachment->handle.id : 0ull);
      CHECK(hasDepth) << "WA resolve batch missing depth attachment (required to write gl_FragDepth).";
      ensureSampledReadable(weightedAverage->accumulationAttachment);
      ensureSampledReadable(weightedAverage->momentsAttachment);
    } else if (const auto* dualPeel = std::get_if<TextureDualPeelPayload>(&batch.geometry)) {
      if (dualPeel->stage == TextureDualPeelPayload::Stage::Blend) {
        ensureSampledReadable(dualPeel->tempAttachment);
      } else {
        const bool hasDepth = batch.pass.depthAttachment.has_value() && batch.pass.depthAttachment->handle.valid();
        VLOG(1) << fmt::format("DDP final depthAttachment present={} handle=0x{:x}",
                               hasDepth,
                               hasDepth ? batch.pass.depthAttachment->handle.id : 0ull);
        CHECK(hasDepth) << "DDP final batch missing depth attachment (required to write gl_FragDepth).";
        ensureSampledReadable(dualPeel->depthAttachment);
        ensureSampledReadable(dualPeel->frontAttachment);
        ensureSampledReadable(dualPeel->backAttachment);
      }
    } else if (const auto* blend = std::get_if<TextureBlendPayload>(&batch.geometry)) {
      // Alpha/depth composition requires both depth inputs to be present; a missing
      // depth handle will cause incorrect z resolution (e.g., opaque dominates).
      CHECK(blend->depthAttachmentHandle0.valid())
        << "TextureBlendPayload missing depthAttachmentHandle0 (base depth).";
      CHECK(blend->depthAttachmentHandle1.valid())
        << "TextureBlendPayload missing depthAttachmentHandle1 (overlay depth).";

      ensureSampledReadable(blend->colorAttachmentHandle0);
      ensureSampledReadable(blend->depthAttachmentHandle0);
      ensureSampledReadable(blend->colorAttachmentHandle1);
      ensureSampledReadable(blend->depthAttachmentHandle1);
    } else if (const auto* weightedBlended = std::get_if<TextureWeightedBlendedPayload>(&batch.geometry)) {
      ensureSampledReadable(weightedBlended->accumulationAttachment);
      ensureSampledReadable(weightedBlended->transmittanceAttachment);
    } else if (const auto* textureCopy = std::get_if<TextureCopyPayload>(&batch.geometry)) {
      ensureSampledReadable(textureCopy->colorAttachmentHandle);
      ensureSampledReadable(textureCopy->depthAttachmentHandle);
    } else if (const auto* textureGlow = std::get_if<TextureGlowPayload>(&batch.geometry)) {
      ensureSampledReadable(textureGlow->colorAttachmentHandle);
      ensureSampledReadable(textureGlow->depthAttachmentHandle);
    }

    // Dual-depth peeling peel stage: regardless of the specific geometry type
    // (mesh, sphere, cone, ellipsoid), the fragment shader samples the prior
    // ping's depth and front blender textures. Transition them to a sampled
    // layout before any draw in this batch. Previously we only did this for
    // MeshPayload, which left sphere/cone draws sampling attachments still in
    // COLOR_ATTACHMENT_OPTIMAL, triggering validation errors at submit.
    if (renderer.shaderHookType() == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
      const auto& hookPara = renderer.shaderHookPara();
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        ensureSampledReadable(hookPara.dualDepthPeelingDepthBlenderHandle);
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        ensureSampledReadable(hookPara.dualDepthPeelingFrontBlenderHandle);
      }
    }

    const auto vkViewport = vulkan::toVkViewport(batch.pass.viewport);
    const auto vkScissor = vulkan::toVkScissor(batch.pass);

    const bool selfManaged = isSelfManaged(batch.geometry);
    const auto key = buildKey(batch);

    if (selfManaged) {
      if (segmentOpen) {
        if (openSpec) {
          recorder.endRenderingSegment(*openSpec);
          openSpec.reset();
        }
        segmentOpen = false;
        currentKey.reset();
      }
    } else {
      if (!segmentOpen) {
        ZVulkanPipelineCommandRecorder::RenderingSegmentSpec spec;
        if (beginSegmentForBatch(batch, recorder, spec)) {
          segmentOpen = true;
          currentKey = key;
          openSpec = spec;
          // Track active segment formats for validation
          if (m_activeFrame) {
            m_activeFrame->activeSegmentFormats = vulkan::extractAttachmentFormats(batch);
          }
        } else {
          continue;
        }
      } else if (!currentKey || *currentKey != key) {
        if (openSpec) {
          recorder.endRenderingSegment(*openSpec);
          openSpec.reset();
        }
        segmentOpen = false;
        currentKey.reset();
        ZVulkanPipelineCommandRecorder::RenderingSegmentSpec spec;
        if (beginSegmentForBatch(batch, recorder, spec)) {
          segmentOpen = true;
          currentKey = key;
          openSpec = spec;
          if (m_activeFrame) {
            m_activeFrame->activeSegmentFormats = vulkan::extractAttachmentFormats(batch);
          }
        } else {
          continue;
        }
      }
    }

    bool handled = false;
    if (const auto* line = std::get_if<LinePayload>(&batch.geometry)) {
      if (!m_lineContext) {
        m_lineContext = std::make_unique<ZVulkanLinePipelineContext>(*this);
      }
      m_lineContext->record(renderer, batch, *line, vkViewport, vkScissor, cmd);
      handled = true;
    }
    if (!handled) {
      if (const auto* mesh = std::get_if<MeshPayload>(&batch.geometry)) {
        if (!m_meshContext) {
          m_meshContext = std::make_unique<ZVulkanMeshPipelineContext>(*this);
        }
        m_meshContext->record(renderer, batch, *mesh, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* sphere = std::get_if<SpherePayload>(&batch.geometry)) {
        if (!m_sphereContext) {
          m_sphereContext = std::make_unique<ZVulkanSpherePipelineContext>(*this);
        }
        m_sphereContext->record(renderer, batch, *sphere, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* background = std::get_if<BackgroundPayload>(&batch.geometry)) {
        if (!m_backgroundContext) {
          m_backgroundContext = std::make_unique<ZVulkanBackgroundPipelineContext>(*this);
        }
        m_backgroundContext->record(renderer, batch, *background, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* slice = std::get_if<ImgSlicePayload>(&batch.geometry)) {
        if (!m_imgSliceContext) {
          m_imgSliceContext = std::make_unique<ZVulkanImgSlicePipelineContext>(*this);
        }
        m_imgSliceContext->record(renderer, batch, *slice, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* font = std::get_if<FontPayload>(&batch.geometry)) {
        if (!m_fontContext) {
          m_fontContext = std::make_unique<ZVulkanFontPipelineContext>(*this);
        }
        m_fontContext->record(renderer, batch, *font, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* textureCopy = std::get_if<TextureCopyPayload>(&batch.geometry)) {
        if (!m_textureCopyContext) {
          m_textureCopyContext = std::make_unique<ZVulkanTextureCopyPipelineContext>(*this);
        }
        m_textureCopyContext->record(renderer, batch, *textureCopy, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* textureBlend = std::get_if<TextureBlendPayload>(&batch.geometry)) {
        if (!m_textureBlendContext) {
          m_textureBlendContext = std::make_unique<ZVulkanTextureBlendPipelineContext>(*this);
        }
        m_textureBlendContext->record(renderer, batch, *textureBlend, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* textureGlow = std::get_if<TextureGlowPayload>(&batch.geometry)) {
        if (!m_textureGlowContext) {
          m_textureGlowContext = std::make_unique<ZVulkanTextureGlowPipelineContext>(*this);
        }
        m_textureGlowContext->record(renderer, batch, *textureGlow, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* dualPeel = std::get_if<TextureDualPeelPayload>(&batch.geometry)) {
        if (!m_textureDualPeelContext) {
          m_textureDualPeelContext = std::make_unique<ZVulkanTextureDualPeelPipelineContext>(*this);
        }
        m_textureDualPeelContext->record(renderer, batch, *dualPeel, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* weightedAverage = std::get_if<TextureWeightedAveragePayload>(&batch.geometry)) {
        if (!m_textureWeightedAverageContext) {
          m_textureWeightedAverageContext = std::make_unique<ZVulkanTextureWeightedAveragePipelineContext>(*this);
        }
        m_textureWeightedAverageContext->record(renderer, batch, *weightedAverage, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* weightedBlended = std::get_if<TextureWeightedBlendedPayload>(&batch.geometry)) {
        if (!m_textureWeightedBlendedContext) {
          m_textureWeightedBlendedContext = std::make_unique<ZVulkanTextureWeightedBlendedPipelineContext>(*this);
        }
        m_textureWeightedBlendedContext->record(renderer, batch, *weightedBlended, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* ellipsoid = std::get_if<EllipsoidPayload>(&batch.geometry)) {
        if (!m_ellipsoidContext) {
          m_ellipsoidContext = std::make_unique<ZVulkanEllipsoidPipelineContext>(*this);
        }
        m_ellipsoidContext->record(renderer, batch, *ellipsoid, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* cone = std::get_if<ConePayload>(&batch.geometry)) {
        if (!m_coneContext) {
          m_coneContext = std::make_unique<ZVulkanConePipelineContext>(*this);
        }
        m_coneContext->record(renderer, batch, *cone, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* raycaster = std::get_if<ImgRaycasterPayload>(&batch.geometry)) {
        if (raycaster->image) {
          if (!m_imgRaycasterContext) {
            m_imgRaycasterContext = std::make_unique<ZVulkanImgRaycasterPipelineContext>(*this);
          }
          m_imgRaycasterContext->record(renderer, batch, *raycaster, vkViewport, vkScissor, cmd);
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

    // If the context managed its own begin/end logic, treat the segment as closed.
    if (selfManaged) {
      segmentOpen = false;
      currentKey.reset();
      openSpec.reset();
    }
  }

  // Close any remaining open segment at the end of batches
  if (segmentOpen) {
    if (openSpec) {
      recorder.endRenderingSegment(*openSpec);
      openSpec.reset();
    }
    segmentOpen = false;
  }
  // Execute any queued upload->static copies now (outside dynamic rendering)
  flushScheduledCopies(cmd);
  if (m_activeFrame) {
    m_activeFrame->activeSegmentFormats.reset();
  }
}

// Removed: shouldSelfManagedClear was used in earlier self-managed flows.

void Z3DRendererVulkanBackend::processCompositorPass(Z3DRendererBase& renderer, const Z3DCompositorPass& pass)
{
  // Record batches, then execute as a single begin/end.
  VLOG(1) << "processCompositorPass surface colors=" << pass.surface.colorAttachments.size()
          << " depth=" << pass.surface.depthAttachment.has_value();
  // Honor clear policy on this pass
  const LoadOp colorLoad = pass.clearColor ? LoadOp::Clear : LoadOp::Load;
  const LoadOp depthLoad = pass.clearDepth ? LoadOp::Clear : LoadOp::Load;
  renderer
    .setActiveSurfaceWithLoadStore(pass.surface, colorLoad, StoreOp::Store, depthLoad, StoreOp::Store, pass.clearValue);

  auto recordFilterBatches = [&](Z3DBoundedFilter* filter, auto&& renderFn) {
    if (!filter) {
      return;
    }
    auto& source = filter->rendererBase();
    const glm::uvec4 previousViewport = source.frameState().viewport;
    const auto previousSurface = source.frameState().activeSurface;
    const auto surfaceCopy = renderer.frameState().activeSurface;
    source.frameState().updateViewportData(renderer.frameState().viewport);
    source.setActiveSurfaceWithLoadStore(surfaceCopy, Z3DRendererBase::Preserve);
    renderFn();
    auto& batches = source.cpuState().batches;
    for (auto& batch : batches) {
      if (batch.pass.colorAttachments.empty() && !surfaceCopy.colorAttachments.empty()) {
        batch.pass.colorAttachments = surfaceCopy.colorAttachments;
      }
      if (!batch.pass.depthAttachment.has_value() && surfaceCopy.depthAttachment.has_value()) {
        batch.pass.depthAttachment = surfaceCopy.depthAttachment;
      }
      if (batch.pass.viewport.extent == glm::vec2(0.0f) && renderer.frameState().viewport.z > 0u &&
          renderer.frameState().viewport.w > 0u) {
        batch.pass.viewport.origin = glm::vec2(static_cast<float>(renderer.frameState().viewport.x),
                                               static_cast<float>(renderer.frameState().viewport.y));
        batch.pass.viewport.extent = glm::vec2(static_cast<float>(renderer.frameState().viewport.z),
                                               static_cast<float>(renderer.frameState().viewport.w));
        batch.pass.viewport.minDepth = 0.0f;
        batch.pass.viewport.maxDepth = 1.0f;
      }
      renderer.appendBatch(std::move(batch));
    }
    source.resetCPUState();
    source.frameState().updateViewportData(previousViewport);
    source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
  };

  // Use pass.debugLabel if provided; otherwise leave empty.
  std::string_view scopeLabel = pass.debugLabel ? std::string_view(pass.debugLabel) : std::string_view();

  // Surface is already applied via setActiveSurfaceWithLoadStore

  {
    const bool startedHere = !renderer.isVulkanFrameActive();
    if (startedHere) {
      renderer.beginVulkanFrame(scopeLabel);
    }
    auto endGuard = folly::makeGuard([&]() {
      if (startedHere) {
        renderer.endVulkanFrame();
      }
    });
    renderer.recordVulkanBatchesInActiveFrame(
      [&]() {
        // Opaque first
        for (auto* filter : pass.opaqueFilters) {
          const bool fullPerf = (FLAGS_atlas_perf_mode == std::string("full"));
          if (fullPerf && filter) {
            auto label =
              fmt::format("comp.opaque:{}@{}", filter->className().toStdString(), static_cast<const void*>(filter));
            if (auto scope = beginGpuScope(label)) {
              recordFilterBatches(filter, [&]() {
                filter->renderOpaque(pass.eye);
              });
              endGpuScope(*scope);
            } else {
              recordFilterBatches(filter, [&]() {
                filter->renderOpaque(pass.eye);
              });
            }
          } else {
            recordFilterBatches(filter, [&]() {
              filter->renderOpaque(pass.eye);
            });
          }
        }
        // Then transparent
        for (const auto& tb : pass.transparentFilters) {
          const bool fullPerf = (FLAGS_atlas_perf_mode == std::string("full"));
          auto* filter = tb.filter;
          if (fullPerf && filter) {
            auto label = fmt::format("comp.transparent:{}@{}",
                                     filter->className().toStdString(),
                                     static_cast<const void*>(filter));
            if (auto scope = beginGpuScope(label)) {
              recordFilterBatches(filter, [&]() {
                filter->renderTransparent(pass.eye);
              });
              endGpuScope(*scope);
            } else {
              recordFilterBatches(filter, [&]() {
                filter->renderTransparent(pass.eye);
              });
            }
          } else {
            recordFilterBatches(filter, [&]() {
              filter->renderTransparent(pass.eye);
            });
          }
        }
      },
      scopeLabel);
  }
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
        desc.handle.backend = AttachmentBackend::Vulkan;
        desc.handle.id = reinterpret_cast<uint64_t>(texture);
        desc.handle.index = attachment.index;
        surface.colorAttachments.push_back(desc);
      }
    } else if (attachment.kind == ScratchAttachmentKind::Depth) {
      if (auto* texture = lease.depthAttachmentTexture()) {
        AttachmentDesc desc;
        desc.handle.backend = AttachmentBackend::Vulkan;
        desc.handle.id = reinterpret_cast<uint64_t>(texture);
        desc.handle.index = attachment.index;
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
  if (!m_sharedDevice) {
    return;
  }
  if (!m_defaultPlaceholder2D) {
    // 1x1 RGBA8 white texture for placeholder sampling
    m_defaultPlaceholder2D =
      m_sharedDevice->createTexture(1,
                                    1,
                                    vk::Format::eR8G8B8A8Unorm,
                                    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                    vk::MemoryPropertyFlagBits::eDeviceLocal);
    uint32_t pixel = 0xffffffffu;
    m_defaultPlaceholder2D->uploadData(&pixel, sizeof(pixel));
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

void Z3DRendererVulkanBackend::ensureSharedSamplers()
{
  if (!m_sharedDevice) {
    return;
  }
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
  if (!m_activeFrame || !m_sharedDevice) {
    VLOG(2) << "suballocateUpload: inactive frame or device; returning null slice for " << bytes << " bytes";
    return slice;
  }

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
  if (!m_activeFrame || !m_sharedDevice) {
    return;
  }
  auto& arena = m_activeFrame->uploadArena;
  size_t cursor = 0; // virtual block will handle placement; we just size the buffer
  for (const auto& s : slices) {
    const size_t bytes = s.first;
    const size_t align = s.second ? s.second : 1;
    if (bytes == 0) {
      continue;
    }
    const size_t aligned = alignUp(cursor, align);
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

void Z3DRendererVulkanBackend::ensureStaticArenas()
{
  if (!m_sharedDevice) {
    return;
  }
  // Create on first use with conservative capacities. Grow policy can be
  // revisited; first cut avoids reallocation to keep slices stable.
  if (!m_staticArena.vb) {
    const size_t cap = static_cast<size_t>(32) * 1024 * 1024; // 32 MiB
    m_staticArena.vb =
      m_sharedDevice->createBuffer(cap,
                                   vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_staticArena.vbCapacity = cap;
    m_staticArena.vbOffset = 0;
    m_staticArena.vbHighWatermark = 0;
  }
  if (!m_staticArena.ib) {
    const size_t cap = static_cast<size_t>(8) * 1024 * 1024; // 8 MiB
    m_staticArena.ib =
      m_sharedDevice->createBuffer(cap,
                                   vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_staticArena.ibCapacity = cap;
    m_staticArena.ibOffset = 0;
    m_staticArena.ibHighWatermark = 0;
  }
}

Z3DRendererVulkanBackend::StaticSlice Z3DRendererVulkanBackend::allocateStaticVB(size_t bytes, size_t alignment)
{
  ensureStaticArenas();
  StaticSlice slice{};
  if (!m_staticArena.vb) {
    return slice;
  }
  const size_t aligned = alignUp(m_staticArena.vbOffset, std::max<size_t>(1, alignment));
  if (aligned + bytes > m_staticArena.vbCapacity) {
    VLOG(1) << fmt::format("Static VB arena full: requested={}B have={}B", bytes, m_staticArena.vbCapacity - aligned);
    return slice;
  }
  slice.buffer = m_staticArena.vb->buffer();
  slice.offset = static_cast<vk::DeviceSize>(aligned);
  slice.size = bytes;
  m_staticArena.vbOffset = aligned + bytes;
  m_staticArena.vbHighWatermark = std::max(m_staticArena.vbHighWatermark, m_staticArena.vbOffset);
  return slice;
}

Z3DRendererVulkanBackend::StaticSlice Z3DRendererVulkanBackend::allocateStaticIB(size_t bytes, size_t alignment)
{
  ensureStaticArenas();
  StaticSlice slice{};
  if (!m_staticArena.ib) {
    return slice;
  }
  const size_t aligned = alignUp(m_staticArena.ibOffset, std::max<size_t>(1, alignment));
  if (aligned + bytes > m_staticArena.ibCapacity) {
    VLOG(1) << fmt::format("Static IB arena full: requested={}B have={}B", bytes, m_staticArena.ibCapacity - aligned);
    return slice;
  }
  slice.buffer = m_staticArena.ib->buffer();
  slice.offset = static_cast<vk::DeviceSize>(aligned);
  slice.size = bytes;
  m_staticArena.ibOffset = aligned + bytes;
  m_staticArena.ibHighWatermark = std::max(m_staticArena.ibHighWatermark, m_staticArena.ibOffset);
  return slice;
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
  if (!m_activeFrame || !m_sharedDevice) {
    VLOG(1) << "allocateFrameDescriptorSet called with no active frame or device";
    return {};
  }
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

ZVulkanDescriptorSet* Z3DRendererVulkanBackend::allocateOverrideDescriptorSet(vk::DescriptorSetLayout layout)
{
  auto set =
    m_sharedDevice && m_activeFrame && m_activeFrame->descriptorPool
      ? m_sharedDevice->createDescriptorSet(*m_activeFrame->descriptorPool, layout, /*isOverrideTransient*/ true)
      : nullptr;
  if (!set) {
    return nullptr;
  }
  ZVulkanDescriptorSet* raw = set.get();
  if (m_activeFrame) {
    m_activeFrame->transientOverrideSets.push_back(std::move(set));
    m_activeFrame->overrideSetsAllocated++;
  }
  return raw;
}

void Z3DRendererVulkanBackend::scheduleAfterCurrentFrameCompletion(std::function<void()> fn)
{
  if (!fn) {
    return;
  }
  if (!m_activeFrame) {
    // No active frame; execute immediately to avoid leaks.
    fn();
    VLOG(2) << "VK scheduleAfterCurrentFrameCompletion with no active frame; executed immediately";
    return;
  }
  m_activeFrame->deferredReleases.push_back(std::move(fn));
  m_activeFrame->leaseRecycleQueued++;
}

std::optional<uint32_t> Z3DRendererVulkanBackend::allocateOcclusionQuery()
{
  if (!m_activeFrame || !m_activeFrameHandle || !m_activeFrameHandle->valid()) {
    return std::nullopt;
  }
  auto& frame = *m_activeFrame;
  if (!queryPoolValid(frame.occlusionQueryPool)) {
    return std::nullopt;
  }
  if (frame.nextOcclusionQuery >= kMaxOcclusionQueries) {
    VLOG(1) << "Vulkan occlusion query budget exceeded";
    return std::nullopt;
  }
  frame.occlusionQueryNeedsWait = true;
  const uint32_t index = frame.nextOcclusionQuery++;
  return index;
}

void Z3DRendererVulkanBackend::beginOcclusionQuery(vk::raii::CommandBuffer& cmd, uint32_t index)
{
  if (!m_activeFrame || !queryPoolValid(m_activeFrame->occlusionQueryPool)) {
    return;
  }
  cmd.beginQuery(*m_activeFrame->occlusionQueryPool, index, {});
}

void Z3DRendererVulkanBackend::endOcclusionQuery(vk::raii::CommandBuffer& cmd, uint32_t index)
{
  if (!m_activeFrame || !queryPoolValid(m_activeFrame->occlusionQueryPool)) {
    return;
  }
  cmd.endQuery(*m_activeFrame->occlusionQueryPool, index);
}

uint64_t Z3DRendererVulkanBackend::lastOcclusionQueryResult(uint32_t index) const
{
  if (m_recentOcclusionResults.empty()) {
    return 1u;
  }
  if (index < m_recentOcclusionResults.size()) {
    return m_recentOcclusionResults[index];
  }
  return 0u;
}

vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer()
{
  CHECK(m_activeFrameHandle && m_activeFrameHandle->valid()) << "Command buffer requested outside active frame";
  return m_activeFrameHandle->commandBuffer();
}

const vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer() const
{
  CHECK(m_activeFrameHandle && m_activeFrameHandle->valid()) << "Command buffer requested outside active frame";
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
    m_sharedDevice = dev;
    resetFrameResources();
    // Refresh timestamp period from the new physical device (ns per tick)
    try {
      auto& phys = m_sharedDevice->context().physicalDevice();
      auto props = phys.getProperties();
      m_timestampPeriod = props.limits.timestampPeriod; // nanoseconds per tick
      if (m_timestampPeriod <= 0.0f) {
        m_timestampPeriod = 1.0f;
      }
      if (!m_loggedCalibrationInfo) {
        try {
          auto domains = phys.getCalibrateableTimeDomainsEXT();
          bool supported = !domains.empty();
          std::string domList;
          for (size_t i = 0; i < domains.size(); ++i) {
            domList += vk::to_string(domains[i]);
            if (i + 1 < domains.size()) {
              domList += ",";
            }
          }
          LOG(INFO) << "VK calibrated timestamps: supported=" << supported << " domains=[" << domList
                    << "] timestampPeriod=" << m_timestampPeriod << " ns/tick";
        }
        catch (...) {
          LOG(INFO) << "VK calibrated timestamps: query not available; timestampPeriod=" << m_timestampPeriod
                    << " ns/tick";
        }
        m_loggedCalibrationInfo = true;
      }
    }
    catch (...) {
      m_timestampPeriod = 1.0f;
    }
  }
  // Keep local ring sizes aligned with the device executor setting.
  if (m_sharedDevice) {
    m_maxFramesInFlight = m_sharedDevice->frameExecutor().maxFramesInFlight();
  }
}

void Z3DRendererVulkanBackend::resetFrameResources()
{
  m_activeFrameHandle.reset();
  m_activeFrame = nullptr;
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
  vk::QueryPoolCreateInfo occlusionInfo{.queryType = vk::QueryType::eOcclusion, .queryCount = kMaxOcclusionQueries};
  frame.occlusionQueryPool = vk::raii::QueryPool(vkDevice, occlusionInfo);
  return frame;
}

void Z3DRendererVulkanBackend::ensureArenaOnFrame(FrameResources& frame)
{
  if (!frame.descriptorPool) {
    frame.descriptorPool = m_sharedDevice->createDescriptorPool();
  }
}

void Z3DRendererVulkanBackend::applyPendingArenaReset(FrameResources& frame)
{
  if (!frame.descriptorPool) {
    return;
  }
  if (frame.arenaResetScheduled) {
    // Destroy transient override sets BEFORE resetting the pool to avoid
    // vkFreeDescriptorSets after implicit free by pool reset.
    frame.transientOverrideSets.clear();
    frame.descriptorPool->reset();
    frame.arenaResetScheduled = false;
    frame.arenaResetsPerformed++;
  }
  if (!frame.deferredReleases.empty()) {
    for (auto& fn : frame.deferredReleases) {
      if (fn) {
        fn();
        // Count after execution
        frame.leaseRecycleExecuted++;
      }
    }
    frame.deferredReleases.clear();
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
  VLOG(1) << fmt::format(
    "VK frame: frame='{}' token={} submit#{} descriptor_sets={} arena_resets={} lease_recycle_queued={} executed={}",
    frame.frameName.empty() ? std::string("<unlabeled-frame>") : frame.frameName,
    frame.realFrameToken,
    frame.submissionId,
    frame.descriptorSetsAllocated,
    frame.arenaResetsPerformed,
    frame.leaseRecycleQueued,
    frame.leaseRecycleExecuted);
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
  if (m_dummyVertexBuffer || !m_sharedDevice) {
    return;
  }
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

void Z3DRendererVulkanBackend::requestEndOfFrameColorReadback(
  ZVulkanTexture& src,
  Z3DEye eye,
  std::function<
    void(const void* mapped, size_t bytes, vk::Format format, glm::uvec2 size, std::function<void()> releaseSlot)>
    onReady)
{
  CHECK(m_activeFrame && m_activeFrameHandle && m_activeFrameHandle->valid())
    << "VK readback requested outside of an active frame";
  const glm::uvec2 size{src.width(), src.height()};
  const vk::Format fmt = src.format();
  auto bytesPerPixel = [](vk::Format f) -> size_t {
    switch (f) {
      case vk::Format::eR8Unorm:
        return 1u;
      case vk::Format::eR8G8B8A8Unorm:
        return 4u;
      case vk::Format::eR16Sfloat:
        return 2u;
      case vk::Format::eR16G16B16A16Unorm:
      case vk::Format::eR16G16B16A16Sfloat:
        return 8u;
      case vk::Format::eR32Sfloat:
        return 4u;
      case vk::Format::eR32G32Sfloat:
        return 8u;
      case vk::Format::eR32G32B32A32Sfloat:
      case vk::Format::eR32G32B32A32Uint:
        return 16u;
      default:
        // Conservative default for typical color formats
        return 4u;
    }
  };
  const size_t bytes = static_cast<size_t>(size.x) * size.y * bytesPerPixel(fmt);
  const int slotIndex = acquireReadbackSlot(bytes);
  CHECK(slotIndex >= 0) << "VK readback slot unavailable (bytes=" << bytes << ")";
  VLOG(1) << fmt::format("VK readback enqueue src=0x{:x} size={}x{} bytes={} slot={} eye={}",
                         reinterpret_cast<uint64_t>(&src),
                         size.x,
                         size.y,
                         bytes,
                         slotIndex,
                         static_cast<int>(eye));

  FrameResources::PendingReadback pr{};
  pr.src = &src;
  pr.eye = eye;
  pr.size = size;
  pr.format = fmt;
  pr.bytes = bytes;
  pr.slotIndex = slotIndex;
  pr.onReady = std::move(onReady);
  m_activeFrame->pendingColorReadbacks.emplace_back(std::move(pr));

  // Schedule consumer after the frame fence signals.
  auto consumer = [this, slotIndex, fmt, size, bytes, onReady = m_activeFrame->pendingColorReadbacks.back().onReady]() {
    CHECK(static_cast<size_t>(slotIndex) < m_readbackSlots.size());
    const void* mapped = m_readbackSlots[static_cast<size_t>(slotIndex)].mapped;
    CHECK(mapped != nullptr) << "VK readback mapped pointer is null";
    VLOG(1) << fmt::format("VK readback consumer begin slot={} bytes={} size={}x{}", slotIndex, bytes, size.x, size.y);
    if (onReady) {
      const auto t0 = std::chrono::steady_clock::now();
      // Hand out a release function to free the slot when the UI is done with it
      std::function<void()> releaseFn = [this, si = slotIndex]() {
        this->releaseReadbackSlot(si);
      };
      onReady(mapped, bytes, fmt, size, std::move(releaseFn));
      const auto t1 = std::chrono::steady_clock::now();
      const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      VLOG(1) << fmt::format("VK readback consumer: {} bytes, {:.3f} ms", bytes, ms);
    }
  };
  scheduleAfterCurrentFrameCompletion(std::move(consumer));
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

// Queue a static copy to be performed outside dynamic rendering in this frame
void Z3DRendererVulkanBackend::scheduleStaticCopy(vk::Buffer dst,
                                                  vk::DeviceSize dstOffset,
                                                  const UploadSlice& src,
                                                  bool isIndexBuffer)
{
  if (!m_activeFrame) {
    return;
  }
  FrameResources::ScheduledCopy sc{};
  sc.dst = dst;
  sc.dstOffset = dstOffset;
  sc.src = src;
  sc.isIndex = isIndexBuffer;
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

    // Make transfer writes visible to vertex input for subsequent frames
    vk::BufferMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eVertexInput;
    barrier.dstAccessMask = sc.isIndex ? vk::AccessFlagBits2::eIndexRead : vk::AccessFlagBits2::eVertexAttributeRead;
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
        message += fmt::format(" | {} {:.3f} ms", scope.label, ms);
      }
    } else {
      VLOG(1) << "Vulkan query results unavailable: " << vk::to_string(result);
    }
  } else {
    VLOG(1) << "No GPU timestamp scopes recorded this frame";
  }

  for (const auto& cpuScope : frame.cpuScopes) {
    message += fmt::format(" | {} {:.3f} ms", cpuScope.label, cpuScope.milliseconds);
  }

  // Append concise per-submission stats (counts and resource deltas)
  message += fmt::format(
    " | dsets={} ovsets={} pipes+={} bound={} segs={} clr={} ld={} dwr={} rew={} upload_hi={}B static={}B rb={}B rbinflight={}",
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
    frame.staticBytesStaged,
    frame.readbackBytesCopied,
    frame.readbackSlotsInFlight);

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
        gpuScopesForCollector.push_back(std::move(sc));
      }
    }
  }

  for (const auto& cs : frame.cpuScopes) {
    cpuScopesForCollector.push_back(Z3DPerfCollector::Scope{cs.label, cs.milliseconds});
  }

  if (frame.realFrameToken != 0) {
    Z3DPerfCollector::Stats stats{};
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
    stats.staticBytesStaged = frame.staticBytesStaged;
    stats.linesBytesStaged = frame.linesBytesStaged;
    stats.fontsBytesStaged = frame.fontsBytesStaged;
    stats.meshesBytesStaged = frame.meshesBytesStaged;
    stats.spheresBytesStaged = frame.spheresBytesStaged;
    stats.readbackBytesCopied = frame.readbackBytesCopied;
    stats.readbackSlotsInFlight = frame.readbackSlotsInFlight;

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
  frame.gpuScopes.push_back(GpuScopeRecord{std::string(label), startIndex, endIndex});
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
