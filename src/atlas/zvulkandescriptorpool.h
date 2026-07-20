#pragma once

#include "zvulkan.h"
#include <cstdint>
#include <optional>

namespace nim {

class ZVulkanDevice;

enum class ZVulkanDescriptorPoolKind : uint8_t
{
  Transient,
  Persistent,
};

class ZVulkanDescriptorPool
{
public:
  ZVulkanDescriptorPool(ZVulkanDevice& device, ZVulkanDescriptorPoolKind kind);
  ~ZVulkanDescriptorPool();

  vk::DescriptorPool pool() const
  {
    return *m_descriptorPool;
  }
  vk::DescriptorSet allocateDescriptorSet(vk::DescriptorSetLayout layout);
  void reset();

private:
  ZVulkanDevice& m_device;
  ZVulkanDescriptorPoolKind m_kind;
  std::optional<vk::raii::DescriptorPool> m_descriptorPool;
};

} // namespace nim
