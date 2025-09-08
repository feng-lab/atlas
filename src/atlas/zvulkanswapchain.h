#pragma once

#include "zvulkan.h"
#include <memory>
#include <optional>

namespace nim {

class ZVulkanDevice;
class ZVulkanTexture;

class ZVulkanSwapChain
{
public:
  ZVulkanSwapChain(ZVulkanDevice& device, uint32_t width, uint32_t height);
  ~ZVulkanSwapChain();

  uint32_t width() const
  {
    return m_width;
  }
  uint32_t height() const
  {
    return m_height;
  }

  void resize(uint32_t width, uint32_t height);

  ZVulkanTexture& colorAttachment();
  ZVulkanTexture& depthAttachment();

  vk::raii::CommandBuffer beginFrame(vk::ClearColorValue clearColor = vk::ClearColorValue(std::array<float, 4>{
                                       {0.0f, 0.0f, 0.0f, 1.0f}
  }),
                                     vk::ClearDepthStencilValue clearDepthStencil = vk::ClearDepthStencilValue(1.0f,
                                                                                                               0));
  void endFrame(vk::raii::CommandBuffer& commandBuffer);
  void copyToMemory(void* data, size_t size);

private:
  void createAttachments();
  void createSampler();
  void createCommandBuffers();

  ZVulkanDevice& m_device;
  uint32_t m_width;
  uint32_t m_height;

  std::unique_ptr<ZVulkanTexture> m_colorAttachment;
  std::unique_ptr<ZVulkanTexture> m_depthAttachment;
  std::optional<vk::raii::Sampler> m_sampler;
  std::optional<vk::raii::CommandPool> m_commandPool;
  std::optional<vk::raii::CommandBuffers> m_commandBuffers;
  std::optional<vk::raii::Fence> m_inFlightFence;
  uint32_t m_currentBuffer = 0;
};

} // namespace nim
