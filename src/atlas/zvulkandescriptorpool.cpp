#include "zvulkandescriptorpool.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zlog.h"

namespace nim {

ZVulkanDescriptorPool::ZVulkanDescriptorPool(ZVulkanDevice& device)
  : m_device(device)
{
  std::array<vk::DescriptorPoolSize, 2> poolSizes{
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBuffer, .descriptorCount = 256},
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 256},
  };
  vk::DescriptorPoolCreateInfo poolInfo{.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                        .maxSets = 256,
                                        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                        .pPoolSizes = poolSizes.data()};
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
