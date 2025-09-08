#include "zvulkandescriptorpool.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zlog.h"

namespace nim {

ZVulkanDescriptorPool::ZVulkanDescriptorPool(ZVulkanDevice& device)
  : m_device(device)
{
  vk::DescriptorPoolSize poolSize{.type = vk::DescriptorType::eUniformBuffer, .descriptorCount = 100};
  vk::DescriptorPoolCreateInfo poolInfo{.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                        .maxSets = 100,
                                        .poolSizeCount = 1,
                                        .pPoolSizes = &poolSize};
  m_descriptorPool.emplace(m_device.context().device(), poolInfo);
  LOG(INFO) << "ZVulkanDescriptorPool created";
}

ZVulkanDescriptorPool::~ZVulkanDescriptorPool()
{
  LOG(INFO) << "Destroying ZVulkanDescriptorPool";
}

vk::raii::DescriptorSet ZVulkanDescriptorPool::allocateDescriptorSet(vk::DescriptorSetLayout layout)
{
  vk::DescriptorSetAllocateInfo allocInfo{.descriptorPool = *m_descriptorPool,
                                          .descriptorSetCount = 1,
                                          .pSetLayouts = &layout};
  vk::raii::DescriptorSets descriptorSets(m_device.context().device(), allocInfo);
  LOG(INFO) << "Allocated descriptor set";
  return std::move(descriptorSets.front());
}

} // namespace nim
