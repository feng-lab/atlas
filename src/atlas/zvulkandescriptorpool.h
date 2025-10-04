#pragma once

#include "zvulkan.h"
#include <optional>

namespace nim {

class ZVulkanDevice;

class ZVulkanDescriptorPool
{
public:
  explicit ZVulkanDescriptorPool(ZVulkanDevice& device);

  vk::DescriptorPool pool() const
  {
    return *m_descriptorPool;
  }
  vk::raii::DescriptorSet allocateDescriptorSet(vk::DescriptorSetLayout layout);

private:
  ZVulkanDevice& m_device;
  std::optional<vk::raii::DescriptorPool> m_descriptorPool;
};

} // namespace nim
