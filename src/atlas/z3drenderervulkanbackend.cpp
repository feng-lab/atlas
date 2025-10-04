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

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace nim {

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

void Z3DRendererVulkanBackend::preBackendSwitch()
{
  // Drop any active frame and ensure GPU is idle before teardown/reset
  m_activeCommandBuffer.reset();
  if (m_sharedDevice) {
    try {
      m_sharedDevice->context().device().waitIdle();
    } catch (const std::exception& e) {
      LOG(WARNING) << "Vulkan waitIdle (shared) failed: " << e.what();
    }
  }

   // Release scratch resources proactively so device teardown is clean
   if (Z3DRenderGlobalState::instance().hasScratchPool()) {
     auto& pool = Z3DRenderGlobalState::instance().scratchPool();
     pool.reset();
   }
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
    m_activeCommandBuffer.reset();
    return;
  }

  // Headless mode: record into a one-shot command buffer. Render targets are provided
  // by the compositor via scratch-pool leases; no swapchain is used.
  m_activeCommandBuffer = device().beginSingleTimeCommands();
}

void Z3DRendererVulkanBackend::endRender(Z3DRendererBase& renderer)
{
  (void)renderer;
  if (m_activeCommandBuffer) {
    device().endSingleTimeCommands(*m_activeCommandBuffer);
    m_activeCommandBuffer.reset();
  }
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
  if (!m_activeCommandBuffer || state.batches.empty()) {
    return;
  }

  auto& cmd = *m_activeCommandBuffer;

  size_t batchIndex = 0;
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
      struct
      {
        vk::ImageLayout layout;
        vk::ImageAspectFlags aspect;
      } result;

      switch (format) {
        case vk::Format::eD16Unorm:
        case vk::Format::eX8D24UnormPack32:
        case vk::Format::eD32Sfloat:
          result.layout = vk::ImageLayout::eDepthReadOnlyOptimal;
          result.aspect = vk::ImageAspectFlagBits::eDepth;
          break;
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
          result.layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
          result.aspect = vk::ImageAspectFlagBits::eDepth;
          break;
        case vk::Format::eS8Uint:
          result.layout = vk::ImageLayout::eStencilReadOnlyOptimal;
          result.aspect = vk::ImageAspectFlagBits::eStencil;
          break;
        default:
          result.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
          result.aspect = vk::ImageAspectFlags{};
          break;
      }
      return result;
    };

    auto ensureSampledReadable = [&](const AttachmentHandle& handle) {
      if (handle.backend != AttachmentBackend::Vulkan || !handle.valid()) return;
      auto* texture = reinterpret_cast<ZVulkanTexture*>(handle.id);
      if (!texture) return;
      const auto samplingState = classifyReadLayout(texture->format());
      // Transition before dynamic rendering begins
      vk::ImageAspectFlags transitionAspect = samplingState.aspect;
      if (transitionAspect == vk::ImageAspectFlagBits::eDepth &&
          (texture->format() == vk::Format::eD16UnormS8Uint || texture->format() == vk::Format::eD24UnormS8Uint ||
           texture->format() == vk::Format::eD32SfloatS8Uint)) {
        transitionAspect = texture->info().aspectMask;
      }
      texture->transitionLayout(cmd, texture->layout(), samplingState.layout, transitionAspect);
      texture->setDescriptorLayout(samplingState.layout);
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
      // When peeling, mesh shaders sample depth/front blender from hook params
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
    std::vector<vk::RenderingAttachmentInfo> colorAttachments;
    colorAttachments.reserve(batch.pass.colorAttachments.size());

    auto makeColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
      if (attachment.handle.backend != AttachmentBackend::Vulkan || attachment.handle.id == 0) {
        return std::nullopt;
      }
      auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
      if (!texture) {
        return std::nullopt;
      }
      // Enforce same logical device for backend and attachments
      CHECK(&texture->ownerDevice() == &device()) << "Vulkan device mismatch for color attachment";

      const auto desiredLayout = vk::ImageLayout::eColorAttachmentOptimal;
      texture->transitionLayout(cmd, texture->layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture->imageView();
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
      if (attachment.handle.backend != AttachmentBackend::Vulkan || attachment.handle.id == 0) {
        return std::nullopt;
      }
      auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
      if (!texture) {
        return std::nullopt;
      }
      // Enforce same logical device for backend and attachments
      CHECK(&texture->ownerDevice() == &device()) << "Vulkan device mismatch for depth attachment";

      const auto desiredLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
      texture->transitionLayout(cmd, texture->layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture->imageView();
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
      VLOG(1) << "VK: skipping batch with no attachments";
      LOG_FIRST_N(WARNING, 5) << "Vulkan backend skipping batch with no Vulkan-compatible attachments.";
      continue;
    }

    const auto vkViewport = vulkan::toVkViewport(batch.pass.viewport);
    const auto vkScissor = vulkan::toVkScissor(batch.pass);

    vk::RenderingInfo renderingInfo;
    renderingInfo.renderArea = vkScissor;
    renderingInfo.layerCount = 1;
    renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    renderingInfo.pDepthAttachment = depthAttachmentInfo ? &*depthAttachmentInfo : nullptr;

    cmd.beginRendering(renderingInfo);

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
          m_textureWeightedAverageContext =
            std::make_unique<ZVulkanTextureWeightedAveragePipelineContext>(*this);
        }
        m_textureWeightedAverageContext->record(renderer, batch, *weightedAverage, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }

    if (!handled) {
      if (const auto* weightedBlended = std::get_if<TextureWeightedBlendedPayload>(&batch.geometry)) {
        if (!m_textureWeightedBlendedContext) {
          m_textureWeightedBlendedContext =
            std::make_unique<ZVulkanTextureWeightedBlendedPipelineContext>(*this);
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
    source.setCollectOnly(true);
    source.frameState().updateViewportData(renderer.frameState().viewport);
    const auto surfaceCopy = renderer.frameState().activeSurface;
    source.setActiveSurfaceForNextPass(surfaceCopy);
    renderFn();
    auto& batches = source.cpuState().batches;
    for (auto& batch : batches) {
      renderer.appendBatch(std::move(batch));
    }
    source.resetCPUState();
    source.setCollectOnly(prevCollectOnly);
    source.frameState().updateViewportData(previousViewport);
  };

  renderer.executeVulkanBatches([&]() {
    // Opaque first
    for (auto* filter : pass.opaqueFilters) {
      recordFilterBatches(filter, [&]() { filter->renderOpaque(pass.eye); });
    }
    // Then transparent
    for (const auto& tb : pass.transparentFilters) {
      recordFilterBatches(tb.filter, [&]() { tb.filter->renderTransparent(pass.eye); });
    }
  });

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


vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer()
{
  CHECK(m_activeCommandBuffer.has_value());
  return *m_activeCommandBuffer;
}

const vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer() const
{
  CHECK(m_activeCommandBuffer.has_value());
  return *m_activeCommandBuffer;
}

void Z3DRendererVulkanBackend::ensureDevice()
{
  if (m_sharedDevice) {
    return;
  }
  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  auto* dev = pool.vulkanDevice();
  CHECK(dev != nullptr) << "Shared Vulkan device not injected into scratch pool";
  m_sharedDevice = dev;
}


std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend()
{
  return std::make_unique<Z3DRendererVulkanBackend>();
}

} // namespace nim
