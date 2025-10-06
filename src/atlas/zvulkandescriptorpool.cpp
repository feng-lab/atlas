#include "zvulkandescriptorpool.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zlog.h"
#include <stdexcept>

namespace nim {

ZVulkanDescriptorPool::ZVulkanDescriptorPool(ZVulkanDevice& device)
  : m_device(device)
{
  // Generous arena to accommodate per-draw override sets in OIT flows.
  std::array<vk::DescriptorPoolSize, 2> poolSizes{
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBuffer, .descriptorCount = 2048},
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 4096},
  };
  vk::DescriptorPoolCreateInfo poolInfo{.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                        .maxSets = 4096,
                                         .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                         .pPoolSizes = poolSizes.data()};
  m_descriptorPool.emplace(m_device.context().device(), poolInfo);
}

vk::DescriptorSet ZVulkanDescriptorPool::allocateDescriptorSet(vk::DescriptorSetLayout layout)
{
  VkDescriptorSet rawSet = VK_NULL_HANDLE;
  VkDescriptorSetLayout rawLayout = layout;
  VkDescriptorSetAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = static_cast<VkDescriptorPool>(**m_descriptorPool);
  ai.descriptorSetCount = 1u;
  ai.pSetLayouts = &rawLayout;

  auto& dev = m_device.context().device();
  VkResult res = dev.getDispatcher()->vkAllocateDescriptorSets(static_cast<VkDevice>(*dev), &ai, &rawSet);
  if (res != VK_SUCCESS) {
    throw std::runtime_error("vkAllocateDescriptorSets failed");
  }
  return rawSet;
}

void ZVulkanDescriptorPool::reset()
{
  if (m_descriptorPool) {
    m_descriptorPool->reset();
  }
}

} // namespace nim
