#pragma once

#include "zvulkan.h"
#include "zvulkantexture.h"
#include <memory>
#include <string>

namespace nim {

class ZVulkanContext;
class ZVulkanBuffer;
class ZVulkanTexture;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanFrameExecutor;

class ZVulkanDevice
{
public:
  explicit ZVulkanDevice(ZVulkanContext& context);
  ~ZVulkanDevice();

  ZVulkanContext& context()
  {
    return m_context;
  }

  std::unique_ptr<ZVulkanBuffer>
  createBuffer(size_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties);

  std::unique_ptr<ZVulkanTexture> createTexture(const ZVulkanTexture::CreateInfo& createInfo);
  std::unique_ptr<ZVulkanTexture> createTexture(uint32_t width, uint32_t height, vk::Format format);
  std::unique_ptr<ZVulkanTexture> createTexture(uint32_t width,
                                                uint32_t height,
                                                vk::Format format,
                                                vk::ImageUsageFlags usage,
                                                vk::MemoryPropertyFlags memoryProperties);

  std::unique_ptr<ZVulkanShader> createShader(const std::string& vertexCode, const std::string& fragmentCode);

  std::unique_ptr<ZVulkanPipeline>
  createPipeline(ZVulkanShader& shader,
                 const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                 const vk::PrimitiveTopology& topology = vk::PrimitiveTopology::eTriangleList);

  std::unique_ptr<ZVulkanDescriptorPool> createDescriptorPool();
  std::unique_ptr<ZVulkanDescriptorSet> createDescriptorSet(ZVulkanDescriptorPool& pool,
                                                            vk::DescriptorSetLayout layout,
                                                            bool isOverrideTransient = false);

  ZVulkanFrameExecutor& frameExecutor();
  const ZVulkanFrameExecutor& frameExecutor() const;

  // Optional features
  bool supportsVertexInputDynamicState() const { return m_supportsVertexInputDynamicState; }

private:
  ZVulkanContext& m_context;
  std::unique_ptr<ZVulkanFrameExecutor> m_frameExecutor;
  bool m_supportsVertexInputDynamicState = false; // guarded for MoltenVK
};

} // namespace nim
