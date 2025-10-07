#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkanshader.h"
#include "zvulkanpipeline.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanframeexecutor.h"
#include "zexception.h"
#include "zlog.h"

#include <utility>

namespace nim {

ZVulkanDevice::ZVulkanDevice(ZVulkanContext& context)
  : m_context(context)
{
  LOG(INFO) << "ZVulkanDevice created";
  // Do not require VK_EXT_vertex_input_dynamic_state (MoltenVK lacks it).
  // Keep the flag false by default; contexts will fall back to fixed VI.
}

ZVulkanDevice::~ZVulkanDevice()
{
  LOG(INFO) << "Destroying ZVulkanDevice";
}

std::unique_ptr<ZVulkanBuffer>
ZVulkanDevice::createBuffer(size_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties)
{
  return std::make_unique<ZVulkanBuffer>(*this, size, usage, properties);
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(const ZVulkanTexture::CreateInfo& createInfo)
{
  return std::make_unique<ZVulkanTexture>(*this, createInfo);
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(uint32_t width, uint32_t height, vk::Format format)
{
  return std::make_unique<ZVulkanTexture>(*this, width, height, format);
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(uint32_t width,
                                                             uint32_t height,
                                                             vk::Format format,
                                                             vk::ImageUsageFlags usage,
                                                             vk::MemoryPropertyFlags memoryProperties)
{
  return std::make_unique<ZVulkanTexture>(*this, width, height, format, usage, memoryProperties);
}

std::unique_ptr<ZVulkanShader> ZVulkanDevice::createShader(const std::string& vertexCode,
                                                           const std::string& fragmentCode)
{
  return std::make_unique<ZVulkanShader>(*this, vertexCode, fragmentCode);
}

std::unique_ptr<ZVulkanPipeline>
ZVulkanDevice::createPipeline(ZVulkanShader& shader,
                              const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                              const vk::PrimitiveTopology& topology)
{
  return std::make_unique<ZVulkanPipeline>(*this, shader, vertexInputInfo, topology);
}

ZVulkanFrameExecutor& ZVulkanDevice::frameExecutor()
{
  if (!m_frameExecutor) {
    m_frameExecutor = std::make_unique<ZVulkanFrameExecutor>(*this);
  }
  return *m_frameExecutor;
}

const ZVulkanFrameExecutor& ZVulkanDevice::frameExecutor() const
{
  return const_cast<ZVulkanDevice*>(this)->frameExecutor();
}

std::unique_ptr<ZVulkanDescriptorPool> ZVulkanDevice::createDescriptorPool()
{
  return std::make_unique<ZVulkanDescriptorPool>(*this);
}

std::unique_ptr<ZVulkanDescriptorSet> ZVulkanDevice::createDescriptorSet(ZVulkanDescriptorPool& pool,
                                                                         vk::DescriptorSetLayout layout,
                                                                         bool isOverrideTransient)
{
  auto descriptorSet = pool.allocateDescriptorSet(layout);
  return std::make_unique<ZVulkanDescriptorSet>(*this, descriptorSet, isOverrideTransient);
}


} // namespace nim
