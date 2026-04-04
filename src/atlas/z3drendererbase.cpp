#include "z3drendererbase.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dprimitiverenderer.h"
#include "z3drendererbackend.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanuniforms.h"
#include "z3dscratchresourcepool.h"
#include "z3dcamera.h"
#include "z3drenderglobalstate.h"
#include "zexception.h"
#include "zlog.h"
#include <folly/ScopeGuard.h>
#include <algorithm>
#include <optional>
#include <utility>

namespace nim {

Z3DRendererBase::Z3DRendererBase(RendererParameterState& parameterState,
                                 RendererFrameState& frameState,
                                 RendererViewState& viewState,
                                 RendererSceneState& sceneState,
                                 RenderBackend initialBackend)
  : m_parameters(parameterState)
  , m_frameState(frameState)
  , m_viewState(viewState)
  , m_sceneState(sceneState)
  , m_clipEnabled(true)
  , m_shaderHookType(ShaderHookType::Normal)
  , m_activeBackend(initialBackend)
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  , m_renderMethod(RenderMethod::GLSL)
#endif
{
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  m_legacyGLState = std::make_unique<LegacyGLState>();
#endif
  setBackend(initialBackend);
}

RendererViewState Z3DRendererBase::pushViewStateFromCamera(const Z3DCamera& camera)
{
  const RendererViewState previous = m_viewState;
  m_viewState = buildViewStateFromCamera(camera);
  return previous;
}

void Z3DRendererBase::restoreViewState(const RendererViewState& state)
{
  m_viewState = state;
}

void Z3DRendererBase::resetCPUState()
{
  m_cpuState.batches.clear();
  m_cpuState.uniformBytesEstimate = 0;
}

void Z3DRendererBase::appendBatch(RenderBatch batch)
{
  if (batch.originatingRenderer == nullptr) {
    batch.originatingRenderer = this;
  }
  if (!batch.clipPlanes.captured) {
    const auto* clipSource = batch.originatingRenderer;
    CHECK(clipSource != nullptr) << "Render batch missing originating renderer";
    const auto& planes = clipSource->clipPlanes();
    CHECK(planes.size() <= kRenderBatchMaxClipPlanes)
      << "Render batch clip plane overflow: planes=" << planes.size() << " max=" << kRenderBatchMaxClipPlanes;
    batch.clipPlanes.captured = true;
    batch.clipPlanes.enabled = clipSource->clipEnabled();
    batch.clipPlanes.planeCount = static_cast<uint32_t>(planes.size());
    for (size_t i = 0; i < planes.size(); ++i) {
      batch.clipPlanes.planes[i] = planes[i];
    }
  }
  if (!batch.shaderHook.captured) {
    batch.shaderHook.captured = true;
    batch.shaderHook.type = m_shaderHookType;
    batch.shaderHook.para = m_shaderHookPara;
  }
  const glm::uvec4 viewportRect = m_frameState.viewport;

  VLOG(2) << "appendBatch initial colors=" << batch.pass.colorAttachments.size()
          << " depth=" << batch.pass.depthAttachment.has_value()
          << " activeSurfaceColors=" << m_frameState.activeSurface.colorAttachments.size()
          << " activeSurfaceHasDepth=" << m_frameState.activeSurface.depthAttachment.has_value();

  if (batch.pass.viewport.extent == glm::vec2(0.0f) && viewportRect.z > 0u && viewportRect.w > 0u) {
    batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewportRect.x), static_cast<float>(viewportRect.y));
    batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewportRect.z), static_cast<float>(viewportRect.w));
    batch.pass.viewport.minDepth = 0.0f;
    batch.pass.viewport.maxDepth = 1.0f;
  }
  if (m_activeBackend == RenderBackend::Vulkan && batch.pass.kind == BackendPassDesc::Kind::Raster) {
    CHECK(batch.pass.viewport.extent.x > 0.0f && batch.pass.viewport.extent.y > 0.0f)
      << "Vulkan appendBatch missing viewport extent: label='" << m_currentPassLabel << "'";
    if (batch.pass.enableScissor) {
      CHECK(batch.pass.scissorRect.z > 0.0f && batch.pass.scissorRect.w > 0.0f)
        << "Vulkan appendBatch with scissor enabled but empty scissorRect: label='" << m_currentPassLabel << "'";
    }
    CHECK(batch.pass.viewport.maxDepth >= batch.pass.viewport.minDepth)
      << "Vulkan appendBatch invalid depth range: minDepth=" << batch.pass.viewport.minDepth
      << " maxDepth=" << batch.pass.viewport.maxDepth << " label='" << m_currentPassLabel << "'";
  }

  const bool hasColorAttachments = !batch.pass.colorAttachments.empty();
  const bool hasDepthAttachment = batch.pass.depthAttachment.has_value();
  if (!hasColorAttachments && !hasDepthAttachment) {
    // Prefer current active surface if present, but only for raster passes.
    // Compute passes intentionally run without attachments; they rely on
    // externalImageUses/externalBufferUses metadata for synchronization.
    if (batch.pass.kind == BackendPassDesc::Kind::Raster && !m_frameState.activeSurface.empty()) {
      batch.pass.colorAttachments = m_frameState.activeSurface.colorAttachments;
      batch.pass.depthAttachment = m_frameState.activeSurface.depthAttachment;
    }
  }

  // Populate explicit cross-pass image usage metadata for Vulkan so the backend
  // can transition sampled inputs without payload/label heuristics.
  if (m_activeBackend == RenderBackend::Vulkan) {
    auto addExternalUseIfMissing =
      [&](const AttachmentHandle& handle, ExternalImageUseKind kind, ExternalImageAspectHint aspectHint) {
        if (!handle.valid()) {
          return;
        }
        CHECK(handle.backend == RenderBackend::Vulkan) << "Non-Vulkan external image handle used on Vulkan backend";
        for (const auto& existing : batch.pass.externalImageUses) {
          if (existing.handle.id == handle.id && existing.kind == kind && existing.aspectHint == aspectHint) {
            return;
          }
        }
        batch.pass.externalImageUses.push_back({handle, kind, aspectHint});
      };

    if (batch.shaderHook.type == ShaderHookType::DualDepthPeelingPeel) {
      addExternalUseIfMissing(batch.shaderHook.para.dualDepthPeelingDepthBlenderHandle,
                              ExternalImageUseKind::SampledRead,
                              ExternalImageAspectHint::Color);
      addExternalUseIfMissing(batch.shaderHook.para.dualDepthPeelingFrontBlenderHandle,
                              ExternalImageUseKind::SampledRead,
                              ExternalImageAspectHint::Color);
    }
  }

  // Final invariant: for Vulkan backend, a batch must target a valid surface.
  if (m_activeBackend == RenderBackend::Vulkan) {
    const bool noColors = batch.pass.colorAttachments.empty();
    const bool noDepth = !batch.pass.depthAttachment.has_value();
    if (batch.pass.kind == BackendPassDesc::Kind::Raster) {
      CHECK(!(noColors && noDepth)) << "Vulkan appendBatch without attachments: shaderHook="
                                    << enumToStringOr(batch.shaderHook.type, "<unknown>") << " label='"
                                    << m_currentPassLabel
                                    << "' activeSurfaceColors=" << m_frameState.activeSurface.colorAttachments.size()
                                    << " activeSurfaceHasDepth="
                                    << m_frameState.activeSurface.depthAttachment.has_value();
    } else {
      CHECK(batch.pass.kind == BackendPassDesc::Kind::Compute);
      CHECK(noColors && noDepth) << "Compute batches must not specify attachments";
    }
  }

  // Strict backend separation: do not accept GL attachments when targeting Vulkan.
  if (m_activeBackend == RenderBackend::Vulkan) {
    for (const auto& att : batch.pass.colorAttachments) {
      CHECK(att.handle.backend == RenderBackend::Vulkan && att.handle.id != 0u)
        << "GL or invalid color attachment encountered in Vulkan path";
    }
    if (batch.pass.depthAttachment) {
      const auto& datt = *batch.pass.depthAttachment;
      CHECK(datt.handle.backend == RenderBackend::Vulkan && datt.handle.id != 0u)
        << "GL or invalid depth attachment encountered in Vulkan path";
    }
  }

  VLOG(2) << "appendBatch final colors=" << batch.pass.colorAttachments.size()
          << " depth=" << batch.pass.depthAttachment.has_value() << " (active colors "
          << m_frameState.activeSurface.colorAttachments.size() << " depth "
          << m_frameState.activeSurface.depthAttachment.has_value() << ")";

  if (m_activeBackend == RenderBackend::Vulkan) {
    auto* vkBackend = static_cast<Z3DRendererVulkanBackend*>(m_backend.get());
    CHECK(vkBackend != nullptr) << "Vulkan backend missing while capturing CPU batches";

    const uint64_t deviceRevision = vkBackend->deviceRevision();
    if (m_vkDeviceRevisionForUniformEstimates != deviceRevision) {
      m_vkDeviceRevisionForUniformEstimates = deviceRevision;
      m_vkUniformAlignmentForEstimates = 0;
    }

    if (m_vkUniformAlignmentForEstimates == 0) {
      m_vkUniformAlignmentForEstimates = vkBackend->uniformAlignmentForEstimates();
      CHECK_NE(m_vkUniformAlignmentForEstimates, 0u) << "Vulkan uniform alignment must be non-zero";
    }

    const size_t align = m_vkUniformAlignmentForEstimates;
    m_cpuState.uniformBytesEstimate += std::visit(
      [&](const auto& payload) -> size_t {
        using PayloadT = std::remove_cvref_t<decltype(payload)>;
        if constexpr (vulkan::kHasUniformArenaBudgetTraits<PayloadT>) {
          return vulkan::UniformArenaBudgetTraits<PayloadT>::estimateAdditionalBytes(payload, align);
        }
        return 0u;
      },
      batch.geometry);
  }

  m_cpuState.batches.push_back(std::move(batch));
}

const RendererCPUState& Z3DRendererBase::cpuState() const
{
  return m_cpuState;
}

RendererCPUState& Z3DRendererBase::cpuState()
{
  return m_cpuState;
}

void Z3DRendererBase::submitBatches()
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  m_backend->processBatches(*this, m_cpuState);
  m_cpuState.batches.clear();
  m_cpuState.uniformBytesEstimate = 0;
}

Z3DRendererBase::~Z3DRendererBase()
{
  releasePersistentLeases();
}

void Z3DRendererBase::releasePersistentLeases()
{
  for (auto* lease : m_persistentLeases) {
    if (lease != nullptr) {
      lease->release();
    }
  }
}

void Z3DRendererBase::registerPersistentLease(Z3DScratchResourcePool::RenderTargetLease& lease)
{
  auto* ptr = &lease;
  if (std::find(m_persistentLeases.begin(), m_persistentLeases.end(), ptr) == m_persistentLeases.end()) {
    m_persistentLeases.push_back(ptr);
  }
}

Z3DScratchResourcePool::RenderTargetLease&
Z3DRendererBase::acquirePersistentTempRenderTarget2D(Z3DScratchResourcePool::RenderTargetLease& lease,
                                                     const glm::uvec2& size,
                                                     ScratchFormat colorFormat,
                                                     ScratchFormat depthFormat)
{
  registerPersistentLease(lease);
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  lease = pool.acquireTempRenderTarget2D(size, colorFormat, depthFormat);
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease&
Z3DRendererBase::acquirePersistentDualDepthPeelRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease,
                                                            const glm::uvec2& size)
{
  registerPersistentLease(lease);
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  std::optional<RenderBackend> backend;
  if (m_activeBackend == RenderBackend::Vulkan) {
    backend = RenderBackend::Vulkan;
  }
  lease = pool.acquireDualDepthPeelRenderTarget(size, backend);
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease&
Z3DRendererBase::acquirePersistentWeightedAverageRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease,
                                                              const glm::uvec2& size)
{
  registerPersistentLease(lease);
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  std::optional<RenderBackend> backend;
  if (m_activeBackend == RenderBackend::Vulkan) {
    backend = RenderBackend::Vulkan;
  }
  lease = pool.acquireWeightedAverageRenderTarget(size, backend);
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease&
Z3DRendererBase::acquirePersistentWeightedBlendedRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease,
                                                              const glm::uvec2& size)
{
  registerPersistentLease(lease);
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  std::optional<RenderBackend> backend;
  if (m_activeBackend == RenderBackend::Vulkan) {
    backend = RenderBackend::Vulkan;
  }
  lease = pool.acquireWeightedBlendedRenderTarget(size, backend);
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease&
Z3DRendererBase::acquirePersistentRaycastAccumulatorRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease,
                                                                 const glm::uvec2& size)
{
  registerPersistentLease(lease);
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  lease = pool.acquireRaycastAccumulatorRenderTarget(size);
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease&
Z3DRendererBase::acquirePersistentLayerArrayRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease,
                                                         const glm::uvec2& size,
                                                         uint32_t layers,
                                                         ScratchFormat colorFormat,
                                                         ScratchFormat depthFormat)
{
  registerPersistentLease(lease);
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  std::optional<RenderBackend> backend;
  if (m_activeBackend == RenderBackend::Vulkan) {
    backend = RenderBackend::Vulkan;
  }
  lease = pool.acquireLayerArrayRenderTarget(size, layers, colorFormat, depthFormat, backend);
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease&
Z3DRendererBase::acquirePersistentEntryExitRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease,
                                                        const glm::uvec2& size,
                                                        uint32_t layers,
                                                        ScratchFormat colorFormat)
{
  registerPersistentLease(lease);
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  lease = pool.acquireEntryExitRenderTarget(size, layers, colorFormat);
  return lease;
}

Z3DScratchResourcePool::RenderTargetLease&
Z3DRendererBase::acquirePersistentBlockIdRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease,
                                                      const glm::uvec2& viewport,
                                                      int requestedAttachments,
                                                      double scale)
{
  registerPersistentLease(lease);
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  lease = pool.acquireBlockIdRenderTarget(viewport, requestedAttachments, scale);
  return lease;
}

RendererFrameState::ActiveSurface
Z3DRendererBase::describeSurface(const Z3DScratchResourcePool::RenderTargetLease& lease)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  return m_backend->describeSurfaceFromLease(lease);
}

Z3DRendererBase::VulkanSurfaceBindings
Z3DRendererBase::prepareVulkanSurface(const Z3DScratchResourcePool::RenderTargetLease& lease)
{
  VulkanSurfaceBindings bindings;

  if (!lease || lease.backend != RenderBackend::Vulkan) {
    return bindings;
  }

  bindings.surface = describeSurface(lease);
  if (bindings.surface.colorAttachments.empty() && !bindings.surface.depthAttachment.has_value()) {
    return bindings;
  }

  bindings.colorHandles.reserve(bindings.surface.colorAttachments.size());
  for (const auto& attachment : bindings.surface.colorAttachments) {
    bindings.colorHandles.push_back(attachment.handle);
  }

  if (bindings.surface.depthAttachment.has_value()) {
    bindings.depthHandle = bindings.surface.depthAttachment->handle;
  }

  return bindings;
}

void Z3DRendererBase::beginVulkanFrame(std::string_view frameLabel)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  CHECK(m_activeBackend == RenderBackend::Vulkan) << "beginVulkanFrame requires a Vulkan backend";

  if (m_vulkanFrameActive) {
    // If a frame is already active, optionally update the label for diagnostics
    if (!frameLabel.empty()) {
      m_currentFrameLabel = std::string(frameLabel);
    }
    return;
  }

  // Set frame label before notifying backend so it can capture it
  m_currentFrameLabel = std::string(frameLabel);
  m_backend->beginRender(*this);
  m_vulkanFrameActive = true;
}

void Z3DRendererBase::endVulkanFrame()
{
  if (!m_vulkanFrameActive) {
    return;
  }

  CHECK(m_backend != nullptr) << "Renderer backend not set";
  auto clearGuard = folly::makeGuard([&]() {
    m_vulkanFrameActive = false;
    m_currentFrameLabel.clear();
  });
  m_backend->endRender(*this);
}

void Z3DRendererBase::flushVulkanWorkForTeardown(std::string_view reason)
{
  if (m_activeBackend != RenderBackend::Vulkan || m_backend == nullptr) {
    return;
  }

  // Do not flush with a frame left open; end it first so submissions (and their
  // corresponding post-fence callbacks) have a well-defined lifetime.
  if (m_vulkanFrameActive) {
    endVulkanFrame();
  }

  auto* vkBackend = dynamic_cast<Z3DRendererVulkanBackend*>(m_backend.get());
  if (vkBackend == nullptr) {
    return;
  }
  vkBackend->flushForTeardown(reason);
}

void Z3DRendererBase::recordVulkanBatchesInActiveFrame(const std::function<void()>& recordBatches,
                                                       std::string_view label)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  CHECK(m_activeBackend == RenderBackend::Vulkan) << "recordVulkanBatchesInActiveFrame called with non-Vulkan backend";
  CHECK(m_vulkanFrameActive)
    << "recordVulkanBatchesInActiveFrame requires an active Vulkan frame (call beginVulkanFrame first)";

  // Start a new recording session within the already-open frame
  m_recordingSessionOpen = true;
  m_currentPassLabel = std::string(label);
  auto sessionGuard = folly::makeGuard([&]() {
    // Always reset session state and discard any partially captured batches.
    m_recordingSessionOpen = false;
    m_currentPassLabel.clear();
    resetCPUState();
  });

  // Clear any previous CPU batches (surface/lifetime are managed by caller).
  resetCPUState();

  VLOG(1) << "recordVulkanBatchesInActiveFrame('" << m_currentPassLabel
          << "') activeSurface colors=" << m_frameState.activeSurface.colorAttachments.size()
          << " depth=" << m_frameState.activeSurface.depthAttachment.has_value();

  std::optional<size_t> gpuScope;
  auto* vkBackend = dynamic_cast<Z3DRendererVulkanBackend*>(m_backend.get());
  bool beganPassScope = false;
  auto passScopeGuard = folly::makeGuard([&]() {
    if (!beganPassScope || vkBackend == nullptr) {
      return;
    }
    if (gpuScope.has_value()) {
      vkBackend->endGpuScope(*gpuScope);
    }
    m_backend->endPassScope();
  });
  if (vkBackend != nullptr) {
    m_backend->beginPassScope(label);
    beganPassScope = true;
    if (!label.empty()) {
      gpuScope = vkBackend->beginGpuScope(label);
    }
  }
  if (recordBatches) {
    recordBatches();
  }
  submitBatches();
}

RendererCPUState Z3DRendererBase::captureVulkanBatches(const std::function<void()>& recordBatches,
                                                       std::string_view label)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  CHECK(m_activeBackend == RenderBackend::Vulkan) << "captureVulkanBatches requires Vulkan backend";
  CHECK(!m_recordingSessionOpen) << "captureVulkanBatches does not support nested recording sessions";

  // Start a recording session for diagnostics (appendBatch invariants) and to
  // propagate a stable pass label into any CHECK messages during capture.
  m_recordingSessionOpen = true;
  m_currentPassLabel = std::string(label);
  auto sessionGuard = folly::makeGuard([&]() {
    m_recordingSessionOpen = false;
    m_currentPassLabel.clear();
    resetCPUState();
  });

  resetCPUState();

  if (VLOG_IS_ON(1)) {
    VLOG(1) << "captureVulkanBatches('" << m_currentPassLabel
            << "') activeSurface colors=" << m_frameState.activeSurface.colorAttachments.size()
            << " depth=" << m_frameState.activeSurface.depthAttachment.has_value();
  }

  if (recordBatches) {
    recordBatches();
  }

  RendererCPUState captured = std::move(m_cpuState);
  resetCPUState();
  return captured;
}

void Z3DRendererBase::executeVulkanBatches(const std::function<void()>& recordBatches, std::string_view label)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  CHECK(m_activeBackend == RenderBackend::Vulkan) << "executeVulkanBatches called with non-Vulkan backend";

  const bool startedFrame = !m_vulkanFrameActive;
  if (startedFrame) {
    beginVulkanFrame();
  }

  // Delegate to the in-active-frame variant for session + submission handling
  recordVulkanBatchesInActiveFrame(recordBatches, label);

  if (startedFrame && !m_keepVulkanFrameOpen) {
    endVulkanFrame();
  }
}

void Z3DRendererBase::setActiveSurfaceWithLoadStore(const RendererFrameState::ActiveSurface& surface,
                                                    LoadOp colorLoad,
                                                    StoreOp colorStore,
                                                    LoadOp depthLoad,
                                                    StoreOp depthStore,
                                                    const ClearValue& clearValue)
{
  m_frameState.setActiveSurface(surface);
  VLOG(2) << "activeSurface set: colors=" << m_frameState.activeSurface.colorAttachments.size()
          << " depth=" << m_frameState.activeSurface.depthAttachment.has_value()
          << " colorLoad=" << enumToString(colorLoad) << " colorStore=" << enumToString(colorStore)
          << " depthLoad=" << enumToString(depthLoad) << " depthStore=" << enumToString(depthStore);
  for (auto& attachment : m_frameState.activeSurface.colorAttachments) {
    attachment.loadOp = colorLoad;
    attachment.storeOp = colorStore;
    attachment.clearValue = clearValue;
  }
  if (m_frameState.activeSurface.depthAttachment) {
    m_frameState.activeSurface.depthAttachment->loadOp = depthLoad;
    m_frameState.activeSurface.depthAttachment->storeOp = depthStore;
    m_frameState.activeSurface.depthAttachment->clearValue = clearValue;
  }
}

void Z3DRendererBase::setActiveSurfaceWithLoadStore(const RendererFrameState::ActiveSurface& surface,
                                                    PreserveLoadStoreTag)
{
  // Apply the surface as-is without overriding per-attachment load/store.
  m_frameState.setActiveSurface(surface);
  VLOG(2) << "activeSurface preserved: colors=" << m_frameState.activeSurface.colorAttachments.size()
          << " depth=" << m_frameState.activeSurface.depthAttachment.has_value();
}

bool Z3DRendererBase::supportsCommandLists() const
{
  return m_backend != nullptr && m_backend->supportsCommandLists();
}

RendererViewState Z3DRendererBase::buildViewStateFromCamera(const Z3DCamera& camera)
{
  RendererViewState state;
  state.nearClip = camera.nearDist();
  state.farClip = camera.farDist();

  for (int eyeValue = LeftEye; eyeValue <= RightEye; ++eyeValue) {
    auto eye = static_cast<Z3DEye>(eyeValue);
    auto& eyeState = state.eyes[static_cast<size_t>(eye)];
    eyeState.viewMatrix = camera.viewMatrix(eye);
    eyeState.projectionMatrix = camera.projectionMatrix(eye);
    eyeState.projectionViewMatrix = camera.projectionViewMatrix(eye);
    eyeState.inverseViewMatrix = camera.inverseViewMatrix(eye);
    eyeState.inverseProjectionMatrix = camera.inverseProjectionMatrix(eye);
    eyeState.normalMatrix = camera.normalMatrix(eye);
    eyeState.eyePosition = camera.eye();
    eyeState.isPerspective = camera.isPerspectiveProjection();
    eyeState.frustumNearPlaneSize = camera.frustumNearPlaneSize();
    eyeState.fieldOfView = camera.fieldOfView();
  }

  return state;
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)

struct Z3DRendererBase::LegacyGLState
{
  GLuint displayList = 0;
  GLuint pickingDisplayList = 0;
  std::set<Z3DPrimitiveRenderer*> lastRenderingState;
  std::set<Z3DPrimitiveRenderer*> lastPickingRenderingState;
};

Z3DRendererBase::LegacyGLState& Z3DRendererBase::legacyGL()
{
  DCHECK(m_legacyGLState != nullptr);
  return *m_legacyGLState;
}

const Z3DRendererBase::LegacyGLState& Z3DRendererBase::legacyGL() const
{
  DCHECK(m_legacyGLState != nullptr);
  return *m_legacyGLState;
}

#endif

void Z3DRendererBase::setBackend(RenderBackend backendType)
{
  const bool initializing = (m_backend == nullptr);
  if (initializing) {
  } else if (m_activeBackend == backendType) {
    VLOG(1) << "RendererBase backend already active; skipping switch";
    return;
  } else {
    VLOG(1) << fmt::format("RendererBase backend switch: {} -> {}",
                           enumToString(m_activeBackend),
                           enumToString(backendType));
  }

  std::unique_ptr<Z3DRendererBackend> newBackend;
  VLOG(1) << fmt::format("Creating {} renderer backend", enumToString(backendType));
  try {
    switch (backendType) {
      case RenderBackend::OpenGL:
        newBackend = createGLRendererBackend();
        break;
      case RenderBackend::Vulkan:
        newBackend = createVulkanRendererBackend();
        break;
    }
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("ZException creating {} renderer backend: {}", enumToString(backendType), e.what());
    LOG(ERROR) << errorMsg;
    throw;
  }
  catch (const std::exception& e) {
    auto errorMsg = fmt::format("Exception creating {} renderer backend: {}", enumToString(backendType), e.what());
    LOG(ERROR) << errorMsg;
    throw ZException(errorMsg);
  }
  if (!newBackend) {
    auto errorMsg = fmt::format("Failed to create {} renderer backend", enumToString(backendType));
    LOG(ERROR) << errorMsg;
    throw ZException(errorMsg);
  }

  if (!initializing) {
    VLOG(1) << fmt::format("Invoking preBackendSwitch on {}", enumToString(m_activeBackend));
    m_backend->preBackendSwitch();
    VLOG(1) << "Releasing renderer backend resources prior to switch";
    releaseBackendResources();
    VLOG(1) << "Releasing persistent leases prior to switch";
    releasePersistentLeases();
  }

  m_backend = std::move(newBackend);
  m_activeBackend = backendType;
  m_vkUniformAlignmentForEstimates = 0;
  m_vkDeviceRevisionForUniformEstimates = 0;
  buildBackendResources(backendType);
  if (initializing) {
    VLOG(2) << fmt::format("RendererBase backend init complete: active={}", enumToString(m_activeBackend));
  } else {
    VLOG(1) << fmt::format("RendererBase backend switch complete: active={}", enumToString(m_activeBackend));
  }
}

void Z3DRendererBase::setGlobalShaderParameters(Z3DShaderProgram& shader, Z3DEye eye)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  m_backend->setGlobalShaderParameters(*this, shader, eye);
}

void Z3DRendererBase::setGlobalShaderParameters(Z3DShaderProgram* shader, Z3DEye eye)
{
  CHECK(shader != nullptr) << "Shader pointer must not be null";
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  m_backend->setGlobalShaderParameters(*this, *shader, eye);
}

std::string Z3DRendererBase::generateHeader() const
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  return m_backend->generateHeader(*this);
}

std::string Z3DRendererBase::generateGeomHeader() const
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  return m_backend->generateGeomHeader(*this);
}

void Z3DRendererBase::registerRenderer(Z3DPrimitiveRenderer* renderer)
{
  CHECK(renderer && !m_renderers.contains(renderer));

  m_renderers.insert(renderer);
}

void Z3DRendererBase::unregisterRenderer(Z3DPrimitiveRenderer* renderer)
{
  CHECK(renderer && m_renderers.contains(renderer));

  m_renderers.erase(renderer);
}

bool Z3DRendererBase::isRendererRegistered(const Z3DPrimitiveRenderer* renderer) const
{
  if (!renderer) {
    return false;
  }
  return m_renderers.contains(const_cast<Z3DPrimitiveRenderer*>(renderer));
}

void Z3DRendererBase::releaseBackendResources()
{
  for (auto* renderer : m_renderers) {
    renderer->releaseBackendResources();
  }
}

void Z3DRendererBase::buildBackendResources(RenderBackend backend)
{
  for (auto* renderer : m_renderers) {
    renderer->buildBackendResources(backend);
  }
}

void Z3DRendererBase::setClipPlanes(std::vector<glm::vec4>* clipPlanes)
{
  size_t nOldClipPlanes = m_clipPlanes.size();
  m_clipPlanes.clear();
  m_doubleClipPlanes.clear();
  if (clipPlanes && !clipPlanes->empty()) {
    glm::mat4 itCoordTrans = glm::inverse(glm::transpose(m_parameters.coordTransform));
    for (auto& clipPlane : *clipPlanes) {
      m_clipPlanes.push_back(itCoordTrans * clipPlane);
    }
  }
  size_t nNewClipPlanes = m_clipPlanes.size();
  if (nNewClipPlanes > 0) {
    size_t clipDistanceBudget = 0;
    if (m_activeBackend == RenderBackend::Vulkan) {
      clipDistanceBudget = kVulkanMaxClipDistances;
    } else {
      const int deviceMaxClipDistances = std::max(0, Z3DGpuInfo::instance().maxClipDistances());
      clipDistanceBudget = static_cast<size_t>(deviceMaxClipDistances);
    }

    if (nNewClipPlanes > clipDistanceBudget) {
      if (!m_lastLoggedClipPlaneOverflowCount.has_value() || *m_lastLoggedClipPlaneOverflowCount != nNewClipPlanes) {
        LOG(WARNING) << "Clip planes (" << nNewClipPlanes << ") exceed device clip-distance budget ("
                     << clipDistanceBudget << "). Atlas will export the first " << clipDistanceBudget
                     << " planes via gl_ClipDistance for early clipping and apply the remaining "
                     << (nNewClipPlanes - clipDistanceBudget)
                     << " plane(s) in the fragment shader (correct but slower).";
        m_lastLoggedClipPlaneOverflowCount = nNewClipPlanes;
      }
    } else {
      m_lastLoggedClipPlaneOverflowCount.reset();
    }
  } else {
    m_lastLoggedClipPlaneOverflowCount.reset();
  }
  if (nNewClipPlanes != nOldClipPlanes) { // need to recompile shader to define or undefine HAS_CLIP_PLANE
    compile();
  }
  for (auto& m_clipPlane : m_clipPlanes) {
    m_doubleClipPlanes.emplace_back(m_clipPlane);
  }
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateDisplayList();
  invalidatePickingDisplayList();
#endif
}

void Z3DRendererBase::render(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  // GL-only path; Vulkan must use renderVulkan inside a Vulkan batches block
  CHECK(m_activeBackend != RenderBackend::Vulkan) << "render() is GL-only. Use renderVulkan for Vulkan.";

  resetCPUState();
  for (auto* renderer : renderers) {
    CHECK(m_renderers.contains(renderer));
  }

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  if (m_renderMethod == RenderMethod::LegacyOpenGL) {
    const auto& eyeState = m_viewState.eyes[static_cast<size_t>(eye)];
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(glm::value_ptr(eyeState.projectionMatrix));
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(glm::value_ptr(eyeState.viewMatrix));

    if (!useDisplayList(renderers)) {
      renderInstant(renderers);
    } else {
      auto& legacy = legacyGL();
      if (legacy.displayList != 0 && legacy.lastRenderingState != m_renderers) {
        invalidateDisplayList();
      }
      if (legacy.displayList == 0) {
        generateDisplayList(renderers);
      }
      if (glIsList(legacy.displayList)) {
        glCallList(legacy.displayList);
      }
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  } else {
    renderUsingGLSL(eye, renderers);
  }
#else
  renderUsingGLSL(eye, renderers);
#endif
}

void Z3DRendererBase::renderPicking(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  // GL-only path; Vulkan must use renderPickingVulkan inside a Vulkan batches block
  CHECK(m_activeBackend != RenderBackend::Vulkan) << "renderPicking() is GL-only. Use renderPickingVulkan for Vulkan.";

  resetCPUState();
  for (auto* renderer : renderers) {
    CHECK(m_renderers.contains(renderer));
  }

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  if (m_renderMethod == RenderMethod::LegacyOpenGL) {
    const auto& eyeState = m_viewState.eyes[static_cast<size_t>(eye)];
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(glm::value_ptr(eyeState.projectionMatrix));
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(glm::value_ptr(eyeState.viewMatrix));

    if (!useDisplayList(renderers)) {
      renderPickingInstant(renderers);
    } else {
      auto& legacy = legacyGL();
      if (legacy.pickingDisplayList != 0 && legacy.lastPickingRenderingState != m_renderers) {
        invalidatePickingDisplayList();
      }
      if (legacy.pickingDisplayList == 0) {
        generatePickingDisplayList(renderers);
      }
      if (glIsList(legacy.pickingDisplayList)) {
        glCallList(legacy.pickingDisplayList);
      }
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  } else {
    renderPickingUsingGLSL(eye, renderers);
  }
#else
  renderPickingUsingGLSL(eye, renderers);
#endif
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DRendererBase::generateDisplayList(Z3DRendererBase::RendererSpan renderers)
{
  auto& legacy = legacyGL();
  if ((bool)glIsList(legacy.displayList)) {
    glDeleteLists(legacy.displayList, 1);
  }

  legacy.displayList = glGenLists(1);
  glNewList(legacy.displayList, GL_COMPILE);
  renderInstant(renderers);
  glEndList();
  legacy.lastRenderingState = m_renderers;
}

void Z3DRendererBase::generatePickingDisplayList(Z3DRendererBase::RendererSpan renderers)
{
  auto& legacy = legacyGL();
  if ((bool)glIsList(legacy.pickingDisplayList)) {
    glDeleteLists(legacy.pickingDisplayList, 1);
  }

  legacy.pickingDisplayList = glGenLists(1);
  glNewList(legacy.pickingDisplayList, GL_COMPILE);
  renderPickingInstant(renderers);
  glEndList();
  legacy.lastPickingRenderingState = m_renderers;
}

void Z3DRendererBase::renderInstant(Z3DRendererBase::RendererSpan renderers)
{
  glPushAttrib(GL_ALL_ATTRIB_BITS);

  if (needLighting(renderers)) {
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_LIGHTING);
    const auto& lighting = m_sceneState.lighting;
    const int lightCount = std::max(0, std::min(lighting.lightCount, static_cast<int>(lighting.positions.size())));

    for (int lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
      const GLenum lightEnum = static_cast<GLenum>(GL_LIGHT0 + lightIndex);
      glEnable(lightEnum);
      glLightfv(lightEnum, GL_AMBIENT, glm::value_ptr(lighting.ambient[static_cast<size_t>(lightIndex)]));
      glLightfv(lightEnum, GL_DIFFUSE, glm::value_ptr(lighting.diffuse[static_cast<size_t>(lightIndex)]));
      glLightfv(lightEnum, GL_SPECULAR, glm::value_ptr(lighting.specular[static_cast<size_t>(lightIndex)]));
      glLightfv(lightEnum, GL_POSITION, glm::value_ptr(lighting.positions[static_cast<size_t>(lightIndex)]));
      glLightfv(lightEnum, GL_SPOT_DIRECTION, glm::value_ptr(lighting.spotDirection[static_cast<size_t>(lightIndex)]));
      glLightf(lightEnum, GL_SPOT_EXPONENT, lighting.spotExponent[static_cast<size_t>(lightIndex)]);
      glLightf(lightEnum, GL_SPOT_CUTOFF, lighting.spotCutoff[static_cast<size_t>(lightIndex)]);
      const glm::vec3& attenuation = lighting.attenuation[static_cast<size_t>(lightIndex)];
      glLightf(lightEnum, GL_CONSTANT_ATTENUATION, attenuation.x);
      glLightf(lightEnum, GL_LINEAR_ATTENUATION, attenuation.y);
      glLightf(lightEnum, GL_QUADRATIC_ATTENUATION, attenuation.z);
    }

    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, glm::value_ptr(m_parameters.materialAmbient));
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, std::min(m_parameters.materialShininess, 128.f));
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, glm::value_ptr(m_parameters.materialSpecular));
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
    glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_NORMALIZE);

    glPopMatrix();
  }

  activateClipPlanesOpenGL();
  for (auto* renderer : renderers) {
    renderer->renderUsingOpengl();
  }
  deactivateClipPlanesOpenGL();

  glPopAttrib();
}

void Z3DRendererBase::renderPickingInstant(Z3DRendererBase::RendererSpan renderers)
{
  glPushAttrib(GL_ALL_ATTRIB_BITS);

  activateClipPlanesOpenGL();
  for (auto* renderer : renderers) {
    renderer->renderPickingUsingOpengl();
  }
  deactivateClipPlanesOpenGL();

  glPopAttrib();
}
#endif

void Z3DRendererBase::renderUsingGLSL(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  CHECK(m_activeBackend != RenderBackend::Vulkan)
    << "renderUsingGLSL is GL-only. Use renderVulkan inside a Vulkan batches block.";
  activateClipPlanesGLSL();
  for (auto* renderer : renderers) {
    renderer->render(eye);
  }
  deactivateClipPlanesGLSL();
}

void Z3DRendererBase::renderPickingUsingGLSL(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  CHECK(m_activeBackend != RenderBackend::Vulkan)
    << "renderPickingUsingGLSL is GL-only. Use renderPickingVulkan inside a Vulkan batches block.";
  activateClipPlanesGLSL();
  for (auto* renderer : renderers) {
    renderer->renderPicking(eye);
  }
  deactivateClipPlanesGLSL();
}

void Z3DRendererBase::renderVulkan(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  CHECK(m_activeBackend == RenderBackend::Vulkan) << "renderVulkan requires Vulkan backend (got GL)";
  // Note: Aggregators (e.g., compositor/backend) may invoke renderVulkan on a
  // different renderer purely to collect CPU batches. Do not require an active
  // recording session here; submission happens on the owning renderer.

  VLOG(2) << "VK render label='" << m_currentPassLabel << "' renderers=" << renderers.size()
          << " activeSurface colors=" << m_frameState.activeSurface.colorAttachments.size()
          << " depth=" << m_frameState.activeSurface.depthAttachment.has_value();

  for (auto* renderer : renderers) {
    renderer->enqueueRenderBatches(eye, m_activeBackend, false);
  }
}

void Z3DRendererBase::renderPickingVulkan(Z3DEye eye, Z3DRendererBase::RendererSpan renderers)
{
  CHECK(m_backend != nullptr) << "Renderer backend not set";
  CHECK(m_activeBackend == RenderBackend::Vulkan) << "renderPickingVulkan requires Vulkan backend (got GL)";
  // See comment in renderVulkan: allow out-of-session collection for aggregators.

  VLOG(2) << "VK renderPicking label='" << m_currentPassLabel << "' renderers=" << renderers.size()
          << " activeSurface colors=" << m_frameState.activeSurface.colorAttachments.size()
          << " depth=" << m_frameState.activeSurface.depthAttachment.has_value();

  for (auto* renderer : renderers) {
    renderer->enqueueRenderBatches(eye, m_activeBackend, true /*picking*/);
  }
}

bool Z3DRendererBase::needLighting(Z3DRendererBase::RendererSpan renderers) const
{
  bool needLighting = false;
  for (auto* renderer : renderers) {
    needLighting = needLighting || renderer->needLighting();
  }
  return needLighting;
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
bool Z3DRendererBase::useDisplayList(Z3DRendererBase::RendererSpan renderers) const
{
  bool useDisplayList = false;
  for (auto* renderer : renderers) {
    useDisplayList = useDisplayList || renderer->useDisplayList();
  }
  return useDisplayList;
}
#endif

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DRendererBase::activateClipPlanesOpenGL()
{
  if (!m_clipEnabled) {
    return;
  }
  for (size_t i = 0; i < m_clipPlanes.size(); ++i) {
    glClipPlane(GL_CLIP_PLANE0 + i, glm::value_ptr(m_doubleClipPlanes[i]));
    glEnable(GL_CLIP_PLANE0 + i);
  }
}

void Z3DRendererBase::deactivateClipPlanesOpenGL()
{
  if (!m_clipEnabled) {
    return;
  }
  for (size_t i = 0; i < m_clipPlanes.size(); ++i) {
    glDisable(GL_CLIP_PLANE0 + i);
  }
}
#endif

void Z3DRendererBase::activateClipPlanesGLSL()
{
  if (!m_clipEnabled) {
    return;
  }
  const int deviceMaxClipDistances = std::max(0, Z3DGpuInfo::instance().maxClipDistances());
  const size_t clipDistanceCount = std::min(m_clipPlanes.size(), static_cast<size_t>(deviceMaxClipDistances));
  for (size_t i = 0; i < clipDistanceCount; ++i) {
    glEnable(GL_CLIP_DISTANCE0 + i);
  }
}

void Z3DRendererBase::deactivateClipPlanesGLSL()
{
  if (!m_clipEnabled) {
    return;
  }
  const int deviceMaxClipDistances = std::max(0, Z3DGpuInfo::instance().maxClipDistances());
  const size_t clipDistanceCount = std::min(m_clipPlanes.size(), static_cast<size_t>(deviceMaxClipDistances));
  for (size_t i = 0; i < clipDistanceCount; ++i) {
    glDisable(GL_CLIP_DISTANCE0 + i);
  }
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DRendererBase::invalidateDisplayList()
{
  auto& legacy = legacyGL();
  if ((bool)glIsList(legacy.displayList)) {
    glDeleteLists(legacy.displayList, 1);
  }
  legacy.displayList = 0;
}

void Z3DRendererBase::invalidatePickingDisplayList()
{
  auto& legacy = legacyGL();
  if ((bool)glIsList(legacy.pickingDisplayList)) {
    glDeleteLists(legacy.pickingDisplayList, 1);
  }
  legacy.pickingDisplayList = 0;
}
#endif

void Z3DRendererBase::compile()
{
  if (m_activeBackend != RenderBackend::OpenGL) {
    return;
  }
  for (auto renderer : m_renderers) {
    renderer->compile();
  }
}

} // namespace nim
