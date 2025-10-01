#include "z3drenderervulkanbackend.h"

#include "z3drendererbase.h"
#include "z3drendercommands.h"
#include "z3dcompositorpass.h"
#include "z3dshaderprogram.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanswapchain.h"
#include "zvulkantexture.h"
#include "z3dscratchresourcepool.h"
#include "zsysteminfo.h"
#include "zvulkanlinepipelinecontext.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace nim {

Z3DRendererVulkanBackend::Z3DRendererVulkanBackend()
  : m_lineContext(std::make_unique<ZVulkanLinePipelineContext>(*this))
{}

Z3DRendererVulkanBackend::~Z3DRendererVulkanBackend() = default;

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
  ensureDevice();

  const auto& viewport = renderer.frameState().viewport;
  const uint32_t width = viewport.z;
  const uint32_t height = viewport.w;

  if (width == 0U || height == 0U) {
    m_activeCommandBuffer.reset();
    return;
  }

  ensureSwapChain(width, height);
  if (!m_swapChain) {
    LOG(ERROR) << "Vulkan backend failed to create swap chain";
    m_activeCommandBuffer.reset();
    return;
  }

  m_activeCommandBuffer = m_swapChain->beginFrame();

  renderer.setActiveSurfaceForNextPass(describeSurfaceFromSwapChain());
}

void Z3DRendererVulkanBackend::endRender(Z3DRendererBase& renderer)
{
  (void)renderer;
  if (m_swapChain && m_activeCommandBuffer) {
    m_swapChain->endFrame(*m_activeCommandBuffer);
  }
  m_activeCommandBuffer.reset();
}

namespace {

vk::Viewport toVkViewport(const ViewportDesc& viewport)
{
  return vk::Viewport(viewport.origin.x,
                      viewport.origin.y,
                      std::max(0.0f, viewport.extent.x),
                      std::max(0.0f, viewport.extent.y),
                      viewport.minDepth,
                      viewport.maxDepth);
}

vk::Rect2D toVkScissor(const BackendPassDesc& pass)
{
  if (pass.enableScissor) {
    return vk::Rect2D({static_cast<int32_t>(pass.scissorRect.x), static_cast<int32_t>(pass.scissorRect.y)},
                      {static_cast<uint32_t>(std::max(0.0f, pass.scissorRect.z)),
                       static_cast<uint32_t>(std::max(0.0f, pass.scissorRect.w))});
  }
  return vk::Rect2D({static_cast<int32_t>(pass.viewport.origin.x), static_cast<int32_t>(pass.viewport.origin.y)},
                    {static_cast<uint32_t>(std::max(0.0f, pass.viewport.extent.x)),
                     static_cast<uint32_t>(std::max(0.0f, pass.viewport.extent.y))});
}

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
  if (std::holds_alternative<EllipsoidPayload>(geometry)) {
    return "ellipsoid";
  }
  if (std::holds_alternative<ConePayload>(geometry)) {
    return "cone";
  }
  return "unknown";
}

} // namespace

RendererFrameState::ActiveSurface Z3DRendererVulkanBackend::describeSurfaceFromSwapChain()
{
  RendererFrameState::ActiveSurface surface;

  if (!m_swapChain) {
    return surface;
  }

  AttachmentDesc colorAttachment;
  colorAttachment.handle.backend = AttachmentBackend::Vulkan;
  colorAttachment.handle.index = 0;
  colorAttachment.handle.id = reinterpret_cast<uint64_t>(&m_swapChain->colorAttachment());
  colorAttachment.loadOp = LoadOp::Clear;
  colorAttachment.storeOp = StoreOp::Store;
  colorAttachment.clearValue.color = glm::vec4(0.f, 0.f, 0.f, 1.f);
  surface.colorAttachments.push_back(colorAttachment);

  AttachmentDesc depthAttachment;
  depthAttachment.handle.backend = AttachmentBackend::Vulkan;
  depthAttachment.handle.index = 0;
  depthAttachment.handle.id = reinterpret_cast<uint64_t>(&m_swapChain->depthAttachment());
  depthAttachment.loadOp = LoadOp::Clear;
  depthAttachment.storeOp = StoreOp::Store;
  depthAttachment.clearValue.depth = 1.0f;
  depthAttachment.clearValue.stencil = 0;
  surface.depthAttachment = depthAttachment;

  return surface;
}

void Z3DRendererVulkanBackend::processBatches(Z3DRendererBase& renderer, const RendererCPUState& state)
{
  if (!m_activeCommandBuffer || state.batches.empty()) {
    return;
  }

  auto& cmd = *m_activeCommandBuffer;

  for (const auto& batch : state.batches) {
    std::vector<vk::RenderingAttachmentInfo> colorAttachments;
    colorAttachments.reserve(batch.pass.colorAttachments.size());

    auto convertLoadOp = [](LoadOp op) {
      switch (op) {
        case LoadOp::Clear:
          return vk::AttachmentLoadOp::eClear;
        case LoadOp::Load:
          return vk::AttachmentLoadOp::eLoad;
        case LoadOp::DontCare:
        default:
          return vk::AttachmentLoadOp::eDontCare;
      }
    };

    auto convertStoreOp = [](StoreOp op) {
      switch (op) {
        case StoreOp::Store:
          return vk::AttachmentStoreOp::eStore;
        case StoreOp::DontCare:
        default:
          return vk::AttachmentStoreOp::eDontCare;
      }
    };

    auto makeColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
      if (attachment.handle.backend != AttachmentBackend::Vulkan || attachment.handle.id == 0) {
        return std::nullopt;
      }
      auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
      if (!texture) {
        return std::nullopt;
      }

      const auto desiredLayout = vk::ImageLayout::eColorAttachmentOptimal;
      texture->transitionLayout(cmd, texture->layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture->imageView();
      info.imageLayout = desiredLayout;
      info.loadOp = convertLoadOp(attachment.loadOp);
      info.storeOp = convertStoreOp(attachment.storeOp);
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

      const auto desiredLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
      texture->transitionLayout(cmd, texture->layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture->imageView();
      info.imageLayout = desiredLayout;
      info.loadOp = convertLoadOp(attachment.loadOp);
      info.storeOp = convertStoreOp(attachment.storeOp);
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
      LOG_FIRST_N(WARNING, 5) << "Vulkan backend skipping batch with no Vulkan-compatible attachments.";
      continue;
    }

    const auto vkViewport = toVkViewport(batch.pass.viewport);
    const auto vkScissor = toVkScissor(batch.pass);

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
  (void)renderer;
  (void)pass;
  LOG_FIRST_N(WARNING, 5) << "Vulkan compositor pass execution is not implemented yet.";
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
  CHECK(m_device != nullptr);
  return *m_device;
}

const ZVulkanDevice& Z3DRendererVulkanBackend::device() const
{
  CHECK(m_device != nullptr);
  return *m_device;
}

ZVulkanSwapChain& Z3DRendererVulkanBackend::swapChain()
{
  CHECK(m_swapChain != nullptr);
  return *m_swapChain;
}

const ZVulkanSwapChain& Z3DRendererVulkanBackend::swapChain() const
{
  CHECK(m_swapChain != nullptr);
  return *m_swapChain;
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
  if (!m_context) {
    m_context = std::make_unique<ZVulkanContext>();
  }
  if (!m_device && m_context) {
    m_device = m_context->createDevice();
  }
}

void Z3DRendererVulkanBackend::ensureSwapChain(uint32_t width, uint32_t height)
{
  if (width == 0U || height == 0U) {
    return;
  }

  ensureDevice();
  if (!m_device) {
    return;
  }

  if (!m_swapChain) {
    m_swapChain = m_device->createSwapChain(width, height);
    m_swapChainExtent = glm::uvec2(width, height);
    return;
  }

  if (m_swapChainExtent.x != width || m_swapChainExtent.y != height) {
    m_swapChain->resize(width, height);
    m_swapChainExtent = glm::uvec2(width, height);
  }
}

std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend()
{
  return std::make_unique<Z3DRendererVulkanBackend>();
}

} // namespace nim
