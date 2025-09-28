#pragma once

#include "zvulkan.h"

namespace nim {

class ZVulkanDevice;
class ZVulkanBuffer;
class ZVulkanTexture;

class ZVulkanDescriptorSet
{
public:
  ZVulkanDescriptorSet(ZVulkanDevice& device, vk::raii::DescriptorSet&& descriptorSet);
  ~ZVulkanDescriptorSet();

  void updateUniformBuffer(uint32_t binding, ZVulkanBuffer& buffer);
  void updateTexture(uint32_t binding, ZVulkanTexture& texture);
  void updateTexture(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler);

  vk::DescriptorSet descriptorSet() const
  {
    return *m_descriptorSet;
  }

private:
  ZVulkanDevice& m_device;
  vk::raii::DescriptorSet m_descriptorSet{nullptr};
};

} // namespace nim
