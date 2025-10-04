#pragma once

#include "zvulkan.h"
#include "zvulkantexture.h"
#include <memory>
#include <optional>
#include <string>

namespace nim {

class ZVulkanContext;
class ZVulkanBuffer;
class ZVulkanTexture;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;

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
                                                            vk::DescriptorSetLayout layout);


  vk::raii::CommandBuffer beginSingleTimeCommands();
  void endSingleTimeCommands(vk::raii::CommandBuffer& cmdBuffer);

private:
  ZVulkanContext& m_context;
};

} // namespace nim
