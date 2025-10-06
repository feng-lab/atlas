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
#include "zvulkanfontpipelinecontext.h"
#include "zvulkanrenderconversions.h"
#include "z3drenderglobalstate.h"
#include "z3dscratchresourcepool.h"
#include "zvulkanbuffer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace nim {

namespace {
constexpr uint32_t kMaxTimestampQueries = 64u;
}

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
    } catch (const std::exception& e) {
      LOG(WARNING) << "Vulkan command buffer end during backend switch failed: " << e.what();
    }
    m_frameRecording = false;
  }
  m_activeFrameHandle.reset();
  m_activeFrame = nullptr;

  if (m_sharedDevice) {
    try {
      m_sharedDevice->context().device().waitIdle();
    }
    catch (const std::exception& e) {
      LOG(WARNING) << "Vulkan waitIdle (shared) failed: " << e.what();
    }

    for (auto& frame : m_frames) {
      collectFrameTimings(frame);
    }
  }

  // Release scratch resources proactively so device teardown is clean
  if (Z3DRenderGlobalState::instance().hasScratchPool()) {
    auto& pool = Z3DRenderGlobalState::instance().scratchPool();
    pool.reset();
  }

  resetFrameResources();
}

void Z3DRendererVulkanBackend::setGlobalShaderParameters(Z3DRendererBase& renderer,
                                                         Z3DShaderProgram& shader,
                                                         Z3DEye eye)
{
  (void)renderer;
  (void)shader;
  (void)eye;
  LOG_FIRST_N(WARNING, 1) << "Vulkan backend does not provide GLSL shader parameter bindings";
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
  if (m_backgroundContext) {
    m_backgroundContext->resetFrame();
  }
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
  if (m_fontContext) {
    m_fontContext->resetFrame();
  }
  ensureDevice();

  const auto& viewport = renderer.frameState().viewport;
  const uint32_t width = viewport.z;
  const uint32_t height = viewport.w;
  const auto& surf = renderer.frameState().activeSurface;
  VLOG(1) << fmt::format("VK beginRender: viewport={}x{}, colors={}, depth={}",
                         width,
                         height,
                         surf.colorAttachments.size(),
                         surf.depthAttachment.has_value());

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
  frameResources.cpuStart = std::chrono::steady_clock::now();
  frameResources.cpuEnd = {};
  // Reset Stage 3 instrumentation
  frameResources.renderingSegmentsBegan = 0;
  frameResources.attachmentClears = 0;
  frameResources.attachmentLoads = 0;
  frameResources.pipelinesCreated = 0;
  frameResources.pipelinesBound.clear();

  vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  auto& cmdBuffer = m_activeFrameHandle->commandBuffer();
  cmdBuffer.begin(beginInfo);
  cmdBuffer.resetQueryPool(*frameResources.queryPool, 0, kMaxTimestampQueries);

  m_activeFrame = &frameResources;
  m_frameRecording = true;

  // Install scratch-pool deferred release scheduler for this active frame
  {
    auto& pool = Z3DRenderGlobalState::instance().scratchPool();
    pool.setVulkanReleaseScheduler([this](std::function<void()> fn) { this->scheduleAfterCurrentFrameCompletion(std::move(fn)); });
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

  if (m_frameRecording) {
    try {
      frameHandle.commandBuffer().end();
    } catch (const std::exception& e) {
      LOG(WARNING) << "Vulkan command buffer end failed: " << e.what();
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

  try {
    queue.submit(submitInfo, *frameHandle.fence());
    device().frameExecutor().markSubmitted(frameHandle);
  } catch (const std::exception& e) {
    LOG(WARNING) << "Vulkan queue submit failed: " << e.what();
  }

  // Stage 2: schedule exactly one descriptor arena reset for this frame.
  scheduleArenaReset(frame);

  // VLOG(1) frame recycling stats (descriptors and arena reset scheduling)
  vlogFrameRecyclingStats(frame);

  // Stage 3: VLOG instrumentation for dynamic rendering segments and pipeline stats
  VLOG(1) << fmt::format("VK segments: began={} clears={} loads={} pipelines_created={} pipelines_bound_unique={}",
                         frame.renderingSegmentsBegan,
                         frame.attachmentClears,
                         frame.attachmentLoads,
                         frame.pipelinesCreated,
                         frame.pipelinesBound.size());

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

  // Build a simple attachment key for coalescing
  struct AttachKey {
    std::vector<uint64_t> colors;
    uint64_t depth = 0;
    bool operator==(const AttachKey& o) const { return depth == o.depth && colors == o.colors; }
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

  // Begin a dynamic rendering segment for the batch's attachments
  auto beginSegmentForBatch = [&](const RenderBatch& batch) {
    std::vector<vk::RenderingAttachmentInfo> colorAttachments;
    colorAttachments.reserve(batch.pass.colorAttachments.size());

    auto makeColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
      if (!attachment.handle.valid()) {
        return std::nullopt;
      }
      auto& texture = vulkan::textureFromHandle(attachment.handle, device(), "renderer color attachment");
      const auto desiredLayout = vk::ImageLayout::eColorAttachmentOptimal;
      texture.transitionLayout(cmd, texture.layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture.imageView();
      info.imageLayout = desiredLayout;
      info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
      info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
      vk::ClearValue clear{};
      clear.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                             attachment.clearValue.color.g,
                                                             attachment.clearValue.color.b,
                                                             attachment.clearValue.color.a});
      info.clearValue = clear;
      return info;
    };

    auto makeDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
      if (!attachment.handle.valid()) {
        return std::nullopt;
      }
      auto& texture = vulkan::textureFromHandle(attachment.handle, device(), "renderer depth attachment");
      const auto desiredLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
      texture.transitionLayout(cmd, texture.layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture.imageView();
      info.imageLayout = desiredLayout;
      info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
      info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
      vk::ClearValue clear{};
      clear.depthStencil = vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
      info.clearValue = clear;
      return info;
    };

    for (const auto& attachment : batch.pass.colorAttachments) {
      if (auto vkAttachment = makeColorAttachment(attachment)) {
        colorAttachments.push_back(*vkAttachment);
      }
    }

    std::optional<vk::RenderingAttachmentInfo> depthAttachmentInfo;
    if (batch.pass.depthAttachment) {
      depthAttachmentInfo = makeDepthAttachment(*batch.pass.depthAttachment);
    }

    if (colorAttachments.empty() && !depthAttachmentInfo) {
      VLOG(1) << "VK: skipping segment with no attachments";
      return false;
    }

    const auto vkScissor = vulkan::toVkScissor(batch.pass);
    vk::RenderingInfo renderingInfo;
    renderingInfo.renderArea = vkScissor;
    renderingInfo.layerCount = 1;
    renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    renderingInfo.pDepthAttachment = depthAttachmentInfo ? &*depthAttachmentInfo : nullptr;

    cmd.beginRendering(renderingInfo);

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

    // Ensure any sampled inputs referenced by this batch are transitioned to shader-read
    auto classifyReadLayout = [](vk::Format format) {
      struct { vk::ImageLayout layout; vk::ImageAspectFlags aspect; } result;
      switch (format) {
        case vk::Format::eD16Unorm:
        case vk::Format::eX8D24UnormPack32:
        case vk::Format::eD32Sfloat:
          result.layout = vk::ImageLayout::eDepthReadOnlyOptimal; result.aspect = vk::ImageAspectFlagBits::eDepth; break;
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
          result.layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal; result.aspect = vk::ImageAspectFlagBits::eDepth; break;
        case vk::Format::eS8Uint:
          result.layout = vk::ImageLayout::eStencilReadOnlyOptimal; result.aspect = vk::ImageAspectFlagBits::eStencil; break;
        default:
          result.layout = vk::ImageLayout::eShaderReadOnlyOptimal; result.aspect = {};
      }
      return result;
    };
    auto ensureSampledReadable = [&](const AttachmentHandle& handle) {
      if (!handle.valid()) return;
      auto& texture = vulkan::textureFromHandle(handle, device(), "renderer sampled attachment");
      const auto samplingState = classifyReadLayout(texture.format());
      vk::ImageAspectFlags transitionAspect = samplingState.aspect;
      if (transitionAspect == vk::ImageAspectFlagBits::eDepth &&
          (texture.format() == vk::Format::eD16UnormS8Uint || texture.format() == vk::Format::eD24UnormS8Uint ||
           texture.format() == vk::Format::eD32SfloatS8Uint)) {
        transitionAspect = texture.info().aspectMask;
      }
      texture.transitionLayout(cmd, texture.layout(), samplingState.layout, transitionAspect);
      texture.setDescriptorLayout(samplingState.layout);
    };
    if (const auto* weightedAverage = std::get_if<TextureWeightedAveragePayload>(&batch.geometry)) {
      ensureSampledReadable(weightedAverage->accumulationAttachment);
      ensureSampledReadable(weightedAverage->momentsAttachment);
    } else if (const auto* dualPeel = std::get_if<TextureDualPeelPayload>(&batch.geometry)) {
      if (dualPeel->stage == TextureDualPeelPayload::Stage::Blend) {
        ensureSampledReadable(dualPeel->tempAttachment);
      } else {
        ensureSampledReadable(dualPeel->depthAttachment);
        ensureSampledReadable(dualPeel->frontAttachment);
        ensureSampledReadable(dualPeel->backAttachment);
      }
    } else if (const auto* blend = std::get_if<TextureBlendPayload>(&batch.geometry)) {
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
    } else if (const auto* mesh = std::get_if<MeshPayload>(&batch.geometry)) {
      (void)mesh;
      if (renderer.shaderHookType() == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
        const auto& hookPara = renderer.shaderHookPara();
        if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
          ensureSampledReadable(hookPara.dualDepthPeelingDepthBlenderHandle);
        }
        if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
          ensureSampledReadable(hookPara.dualDepthPeelingFrontBlenderHandle);
        }
      }
    }

    const auto vkViewport = vulkan::toVkViewport(batch.pass.viewport);
    const auto vkScissor = vulkan::toVkScissor(batch.pass);

    const bool selfManaged = isSelfManaged(batch.geometry);
    const auto key = buildKey(batch);

    if (!segmentOpen) {
      if (beginSegmentForBatch(batch)) {
        segmentOpen = true;
        currentKey = key;
      } else {
        continue;
      }
    } else if (!currentKey || *currentKey != key) {
      cmd.endRendering();
      segmentOpen = false;
      currentKey.reset();
      if (beginSegmentForBatch(batch)) {
        segmentOpen = true;
        currentKey = key;
      } else {
        continue;
      }
    }

    bool handled = false;
    if (const auto* line = std::get_if<LinePayload>(&batch.geometry)) {
      if (line->renderer) {
        if (!m_lineContext) {
          m_lineContext = std::make_unique<ZVulkanLinePipelineContext>(*this);
        }
        m_lineContext->record(renderer, batch, *line, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }
    if (!handled) {
      if (const auto* mesh = std::get_if<MeshPayload>(&batch.geometry)) {
        if (mesh->renderer) {
          if (!m_meshContext) {
            m_meshContext = std::make_unique<ZVulkanMeshPipelineContext>(*this);
          }
          m_meshContext->record(renderer, batch, *mesh, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* sphere = std::get_if<SpherePayload>(&batch.geometry)) {
        if (sphere->renderer) {
          if (!m_sphereContext) {
            m_sphereContext = std::make_unique<ZVulkanSpherePipelineContext>(*this);
          }
          m_sphereContext->record(renderer, batch, *sphere, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* background = std::get_if<BackgroundPayload>(&batch.geometry)) {
        if (background->renderer) {
          if (!m_backgroundContext) {
            m_backgroundContext = std::make_unique<ZVulkanBackgroundPipelineContext>(*this);
          }
          m_backgroundContext->record(renderer, batch, *background, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* slice = std::get_if<ImgSlicePayload>(&batch.geometry)) {
        if (slice->renderer) {
          if (!m_imgSliceContext) {
            m_imgSliceContext = std::make_unique<ZVulkanImgSlicePipelineContext>(*this);
          }
          m_imgSliceContext->record(renderer, batch, *slice, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* font = std::get_if<FontPayload>(&batch.geometry)) {
        if (font->renderer) {
          if (!m_fontContext) {
            m_fontContext = std::make_unique<ZVulkanFontPipelineContext>(*this);
          }
          m_fontContext->record(renderer, batch, *font, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* textureCopy = std::get_if<TextureCopyPayload>(&batch.geometry)) {
        if (textureCopy->renderer) {
          if (!m_textureCopyContext) {
            m_textureCopyContext = std::make_unique<ZVulkanTextureCopyPipelineContext>(*this);
          }
          m_textureCopyContext->record(renderer, batch, *textureCopy, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* textureBlend = std::get_if<TextureBlendPayload>(&batch.geometry)) {
        if (textureBlend->renderer) {
          if (!m_textureBlendContext) {
            m_textureBlendContext = std::make_unique<ZVulkanTextureBlendPipelineContext>(*this);
          }
          m_textureBlendContext->record(renderer, batch, *textureBlend, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* textureGlow = std::get_if<TextureGlowPayload>(&batch.geometry)) {
        if (textureGlow->renderer) {
          if (!m_textureGlowContext) {
            m_textureGlowContext = std::make_unique<ZVulkanTextureGlowPipelineContext>(*this);
          }
          m_textureGlowContext->record(renderer, batch, *textureGlow, vkViewport, vkScissor, cmd);
          handled = true;
        }
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
        if (ellipsoid->renderer) {
          if (!m_ellipsoidContext) {
            m_ellipsoidContext = std::make_unique<ZVulkanEllipsoidPipelineContext>(*this);
          }
          m_ellipsoidContext->record(renderer, batch, *ellipsoid, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* cone = std::get_if<ConePayload>(&batch.geometry)) {
        if (cone->renderer) {
          if (!m_coneContext) {
            m_coneContext = std::make_unique<ZVulkanConePipelineContext>(*this);
          }
          m_coneContext->record(renderer, batch, *cone, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      if (const auto* raycaster = std::get_if<ImgRaycasterPayload>(&batch.geometry)) {
        if (raycaster->renderer && raycaster->image) {
          if (!m_imgRaycasterContext) {
            m_imgRaycasterContext = std::make_unique<ZVulkanImgRaycasterPipelineContext>(*this);
          }
          m_imgRaycasterContext->record(renderer, batch, *raycaster, vkViewport, vkScissor, cmd);
          handled = true;
        }
      }
    }
    if (!handled) {
      cmd.setViewport(0, vkViewport);
      cmd.setScissor(0, vkScissor);
      LOG_FIRST_N(WARNING, 5) << "Vulkan backend has not yet implemented draw emission for geometry type '"
                              << describeGeometry(batch.geometry) << "'.";
    }

    // If the context managed its own begin/end logic, treat the segment as closed.
    if (selfManaged) {
      segmentOpen = false;
      currentKey.reset();
    }
  }

  if (segmentOpen) {
    cmd.endRendering();
  }
}

void Z3DRendererVulkanBackend::processCompositorPass(Z3DRendererBase& renderer, const Z3DCompositorPass& pass)
{
  // Collect-only recording of batches, then execute as a single begin/end.
  renderer.setCollectOnly(true);
  LOG(INFO) << "processCompositorPass surface colors=" << pass.surface.colorAttachments.size()
            << " depth=" << pass.surface.depthAttachment.has_value();
  renderer.setActiveSurfaceForNextPass(pass.surface);
  // Honor clear policy on this pass
  const LoadOp colorLoad = pass.clearColor ? LoadOp::Clear : LoadOp::Load;
  const LoadOp depthLoad = pass.clearDepth ? LoadOp::Clear : LoadOp::Load;
  renderer.setPendingColorAttachmentsLoadStore(colorLoad, StoreOp::Store, pass.clearValue);
  renderer.setPendingDepthAttachmentLoadStore(depthLoad, StoreOp::Store, pass.clearValue);

  auto recordFilterBatches = [&](Z3DBoundedFilter* filter, auto&& renderFn) {
    if (!filter) {
      return;
    }
    auto& source = filter->rendererBase();
    const bool prevCollectOnly = source.collectOnly();
    const glm::uvec4 previousViewport = source.frameState().viewport;
    const auto previousSurface = source.frameState().activeSurface;
    const auto surfaceCopy = renderer.frameState().activeSurface;
    source.setCollectOnly(true);
    source.frameState().updateViewportData(renderer.frameState().viewport);
    source.frameState().setActiveSurface(surfaceCopy);
    source.clearPendingActiveSurface();
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
    source.setCollectOnly(prevCollectOnly);
    source.frameState().updateViewportData(previousViewport);
    source.frameState().setActiveSurface(previousSurface);
  };

  std::string_view scopeLabel = pass.debugLabel ? std::string_view(pass.debugLabel) : std::string_view();

  renderer.executeVulkanBatches([&]() {
    // Opaque first
    for (auto* filter : pass.opaqueFilters) {
      recordFilterBatches(filter, [&]() {
        filter->renderOpaque(pass.eye);
      });
    }
    // Then transparent
    for (const auto& tb : pass.transparentFilters) {
      recordFilterBatches(tb.filter, [&]() {
        tb.filter->renderTransparent(pass.eye);
      });
    }
  }, scopeLabel);

  renderer.setCollectOnly(false);
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
    m_defaultPlaceholder2D = m_sharedDevice->createTexture(1,
                                                           1,
                                                           vk::Format::eR8G8B8A8Unorm,
                                                           vk::ImageUsageFlagBits::eSampled |
                                                             vk::ImageUsageFlagBits::eTransferDst,
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
  auto set = m_sharedDevice->createDescriptorSet(*m_activeFrame->descriptorPool, layout);
  if (set) {
    m_activeFrame->descriptorSetsAllocated++;
  }
  return set;
}

void Z3DRendererVulkanBackend::scheduleAfterCurrentFrameCompletion(std::function<void()> fn)
{
  if (!fn) {
    return;
  }
  if (!m_activeFrame) {
    // No active frame; execute immediately to avoid leaks.
    fn();
    VLOG(1) << "VK scheduleAfterCurrentFrameCompletion with no active frame; executed immediately";
    return;
  }
  m_activeFrame->deferredReleases.push_back(std::move(fn));
  m_activeFrame->leaseRecycleQueued++;
}

vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer()
{
  CHECK(m_activeFrameHandle && m_activeFrameHandle->valid())
    << "Command buffer requested outside active frame";
  return m_activeFrameHandle->commandBuffer();
}

const vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer() const
{
  CHECK(m_activeFrameHandle && m_activeFrameHandle->valid())
    << "Command buffer requested outside active frame";
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

Z3DRendererVulkanBackend::FrameResources&
Z3DRendererVulkanBackend::ensureFrameResourcesForKey(void* key)
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
  if (frame.arenaResetScheduled) {
    LOG_FIRST_N(ERROR, 1) << "Descriptor arena reset scheduled more than once for the same frame";
    return;
  }
  frame.arenaResetScheduled = true;
}

void Z3DRendererVulkanBackend::vlogFrameRecyclingStats(const FrameResources& frame) const
{
  VLOG(1) << fmt::format("VK frame: descriptor_sets={} arena_resets={} lease_recycle_queued={} executed={}",
                         frame.descriptorSetsAllocated,
                         frame.arenaResetsPerformed,
                         frame.leaseRecycleQueued,
                         frame.leaseRecycleExecuted);
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
  m_fullscreenQuadVbo = dev.createBuffer(bytes,
                                         vk::BufferUsageFlagBits::eVertexBuffer,
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                           vk::MemoryPropertyFlagBits::eHostCoherent);
  m_fullscreenQuadVbo->copyData(vertices.data(), bytes);
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
  std::string message = fmt::format("VK batches CPU {:.3f} ms", cpuMs);

  if (frame.nextQuery > 0 && !frame.gpuScopes.empty()) {
    std::vector<uint64_t> queryData(frame.nextQuery, 0u);
    auto& device = m_sharedDevice->context().device();
    const auto* dispatcher = device.getDispatcher();
    if (dispatcher && dispatcher->vkGetQueryPoolResults) {
      const VkResult rawResult = dispatcher->vkGetQueryPoolResults(static_cast<VkDevice>(*device),
                                                                   static_cast<VkQueryPool>(*frame.queryPool),
                                                                   0,
                                                                   frame.nextQuery,
                                                                   queryData.size() * sizeof(uint64_t),
                                                                   queryData.data(),
                                                                   sizeof(uint64_t),
                                                                   static_cast<VkQueryResultFlags>(
                                                                     vk::QueryResultFlagBits::e64));
      const auto result = static_cast<vk::Result>(rawResult);

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
      VLOG(1) << "Vulkan dispatcher missing vkGetQueryPoolResults";
    }

  }

  for (const auto& cpuScope : frame.cpuScopes) {
    message += fmt::format(" | {} {:.3f} ms", cpuScope.label, cpuScope.milliseconds);
  }

  VLOG(1) << message;

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
    LOG_FIRST_N(WARNING, 1) << "Vulkan timestamp query budget exceeded";
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
