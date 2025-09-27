#include "zvulkanrenderer.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zlog.h"
#include "zvulkandevice.h"

namespace nim {

ZVulkanRenderer::ZVulkanRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
{
  auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(&m_rendererBase.backend());
  CHECK(backend != nullptr) << "ZVulkanRenderer requires a Vulkan backend";
  (void)backend;
}

ZVulkanRenderer::~ZVulkanRenderer() = default;

void ZVulkanRenderer::render(Z3DEye eye)
{
  const auto extent = framebufferExtent();
  if (extent.x == 0U || extent.y == 0U) {
    return;
  }

  auto& cmdBuffer = commandBuffer();
  recordRender(eye, cmdBuffer);
}

void ZVulkanRenderer::renderPicking(Z3DEye eye)
{
  const auto extent = framebufferExtent();
  if (extent.x == 0U || extent.y == 0U) {
    return;
  }

  auto& cmdBuffer = commandBuffer();
  recordPicking(eye, cmdBuffer);
}

Z3DRendererVulkanBackend& ZVulkanRenderer::vulkanBackend()
{
  auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(&m_rendererBase.backend());
  CHECK(backend != nullptr) << "Vulkan backend not active for renderer";
  return *backend;
}

const Z3DRendererVulkanBackend& ZVulkanRenderer::vulkanBackend() const
{
  auto* backend = dynamic_cast<const Z3DRendererVulkanBackend*>(&m_rendererBase.backend());
  CHECK(backend != nullptr) << "Vulkan backend not active for renderer";
  return *backend;
}

ZVulkanDevice& ZVulkanRenderer::device()
{
  return vulkanBackend().device();
}

const ZVulkanDevice& ZVulkanRenderer::device() const
{
  return vulkanBackend().device();
}

vk::raii::CommandBuffer& ZVulkanRenderer::commandBuffer()
{
  return vulkanBackend().commandBuffer();
}

glm::uvec2 ZVulkanRenderer::framebufferExtent() const
{
  const auto& viewport = m_rendererBase.frameState().viewport;
  return glm::uvec2(viewport.z, viewport.w);
}

} // namespace nim

