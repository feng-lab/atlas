#pragma once

#include "z3dprimitiverenderer.h"
#include "zglmutils.h"
#include "zvulkan.h"

namespace nim {

class Z3DRendererVulkanBackend;
class ZVulkanDevice;

class ZVulkanRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit ZVulkanRenderer(Z3DRendererBase& rendererBase);
  ~ZVulkanRenderer() override;

protected:
  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye eye) override;

  virtual void recordRender(Z3DEye eye, vk::raii::CommandBuffer& cmdBuffer) = 0;

  virtual void recordPicking(Z3DEye eye, vk::raii::CommandBuffer& cmdBuffer)
  {
    (void)eye;
    (void)cmdBuffer;
  }

  Z3DRendererVulkanBackend& vulkanBackend();

  const Z3DRendererVulkanBackend& vulkanBackend() const;

  ZVulkanDevice& device();

  const ZVulkanDevice& device() const;

  vk::raii::CommandBuffer& commandBuffer();

  glm::uvec2 framebufferExtent() const;
};

} // namespace nim

