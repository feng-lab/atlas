#include "z3drenderervulkanbackend.h"

#include "z3drendererbase.h"
#include "z3dshaderprogram.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanswapchain.h"

namespace nim {

Z3DRendererVulkanBackend::Z3DRendererVulkanBackend() = default;

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
}

void Z3DRendererVulkanBackend::endRender(Z3DRendererBase& renderer)
{
  (void)renderer;
  if (m_swapChain && m_activeCommandBuffer) {
    m_swapChain->endFrame(*m_activeCommandBuffer);
  }
  m_activeCommandBuffer.reset();
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

