#include "zvulkandescriptorpool.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"

namespace nim {

ZVulkanDescriptorPool::ZVulkanDescriptorPool(ZVulkanDevice& device, ZVulkanDescriptorPoolKind kind)
  : m_device(device)
  , m_kind(kind)
{
  m_device.checkOwnerThread("create ordinary descriptor pool");
  // Ordinary backend pools contain no sampled-image or sampler descriptors and
  // deliberately omit eUpdateAfterBind. Only the device-owned bindless pool
  // consumes the device-global update-after-bind descriptor budget.
  const std::array<vk::DescriptorPoolSize, 4> poolSizes{
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBuffer,
                           .descriptorCount = ZVulkanDeviceSupport::DescriptorPoolPolicy::kUniformBufferDescriptors},
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBufferDynamic,
                           .descriptorCount =
                             ZVulkanDeviceSupport::DescriptorPoolPolicy::kUniformBufferDynamicDescriptors          },
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = ZVulkanDeviceSupport::DescriptorPoolPolicy::kStorageBufferDescriptors},
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageImage,
                           .descriptorCount = ZVulkanDeviceSupport::DescriptorPoolPolicy::kStorageImageDescriptors },
  };
  vk::DescriptorPoolCreateInfo poolInfo{.flags = {},
                                        .maxSets = ZVulkanDeviceSupport::DescriptorPoolPolicy::kMaxSets,
                                        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                        .pPoolSizes = poolSizes.data()};
  m_descriptorPool.emplace(m_device.context().device(), poolInfo);
}

ZVulkanDescriptorPool::~ZVulkanDescriptorPool()
{
  m_device.checkOwnerThread("destroy ordinary descriptor pool");
  m_descriptorPool.reset();
}

vk::DescriptorSet ZVulkanDescriptorPool::allocateDescriptorSet(vk::DescriptorSetLayout layout)
{
  m_device.checkOwnerThread("allocate ordinary descriptor set");
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
  m_device.checkOwnerThread("reset transient descriptor pool");
  CHECK(m_kind == ZVulkanDescriptorPoolKind::Transient) << "Only transient Vulkan descriptor pools may be reset";
  if (m_descriptorPool) {
    m_descriptorPool->reset();
  }
}

} // namespace nim
