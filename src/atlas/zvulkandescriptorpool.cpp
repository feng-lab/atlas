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
  std::array<vk::DescriptorPoolSize, 3> poolSizes{
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBuffer,        .descriptorCount = 2048},
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 4096},
    // Add storage buffers for compute workloads (e.g., Block-ID compaction)
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBuffer,        .descriptorCount = 1024},
  };
  vk::DescriptorPoolCreateInfo poolInfo{.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                        .maxSets = 4096,
                                        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                        .pPoolSizes = poolSizes.data()};
  m_descriptorPool.emplace(m_device.context().device(), poolInfo);
}

vk::DescriptorSet ZVulkanDescriptorPool::allocateDescriptorSet(vk::DescriptorSetLayout layout)
{
  // Use RAII allocation so we stay in the Hpp/RAII style, but explicitly
  // release ownership so the pool (not the RAII wrapper) owns the lifetime.
  vk::DescriptorSetAllocateInfo ai{.descriptorPool = **m_descriptorPool,
                                   .descriptorSetCount = 1u,
                                   .pSetLayouts = &layout};
  auto sets = m_device.context().device().allocateDescriptorSets(ai);
  if (sets.empty()) {
    throw ZException("allocateDescriptorSets returned no sets");
  }
  // Detach the descriptor set from the RAII container to avoid vkFree on
  // scope exit; we manage lifetime via pool reset after the frame fence.
  vk::DescriptorSet raw = sets.front().release();
  return raw;
}

void ZVulkanDescriptorPool::reset()
{
  if (m_descriptorPool) {
    m_descriptorPool->reset();
  }
}

} // namespace nim
