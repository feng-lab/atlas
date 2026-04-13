#include "zvulkandescriptorpool.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"
#include <algorithm>

namespace nim {

ZVulkanDescriptorPool::ZVulkanDescriptorPool(ZVulkanDevice& device)
  : m_device(device)
{
  const auto& bindlessCaps = m_device.context().effectiveBindlessSampledImageCapacities();
  const uint32_t bindlessSum = bindlessCaps.totalSampledImages();
  CHECK(bindlessSum > 0u) << "Bindless capacities not computed before descriptor pool creation";

  // Pool sizing policy:
  // - Atlas uses sampled-image descriptors only for the bindless table (set 0).
  //   Allocate exactly enough for the selected bindless capacities so unexpected
  //   non-bindless sampled-image usage fails fast.
  // - Bindless sampler state is provided via immutable samplers. We still
  //   include the corresponding descriptor type in the pool to satisfy Vulkan's
  //   allocation accounting for the set layout.
  const uint32_t sampledImages = bindlessSum;
  const uint32_t samplers = 3u; // linear clamp + nearest clamp + 3D border-zero clamp

  std::array<vk::DescriptorPoolSize, 6> poolSizes{
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBuffer,        .descriptorCount = 1024         },
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eUniformBufferDynamic, .descriptorCount = 2048         },
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampledImage,         .descriptorCount = sampledImages},
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampler,              .descriptorCount = samplers     },
    // Add storage buffers for compute workloads (e.g., Block-ID compaction)
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBuffer,        .descriptorCount = 1024         },
    // Add storage images for compute sampling (Block-ID compaction storage image loads)
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageImage,         .descriptorCount = 2048         },
  };
  vk::DescriptorPoolCreateFlags flags{};
  if (m_device.context().supportsDescriptorIndexingSampledImageUpdateAfterBind()) {
    flags |= vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
  }
  vk::DescriptorPoolCreateInfo poolInfo{.flags = flags,
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
