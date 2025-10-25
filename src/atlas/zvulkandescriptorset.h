#pragma once

#include "zvulkan.h"

namespace nim {

class ZVulkanDevice;
class ZVulkanBuffer;
class ZVulkanTexture;

class ZVulkanDescriptorSet
{
public:
  ZVulkanDescriptorSet(ZVulkanDevice& device, vk::DescriptorSet descriptorSet, bool isOverrideTransient);

  void updateUniformBuffer(uint32_t binding, ZVulkanBuffer& buffer);
  void updateUniformBufferDynamic(uint32_t binding, ZVulkanBuffer& buffer, vk::DeviceSize range);
  void updateTexture(uint32_t binding, ZVulkanTexture& texture);
  void updateTexture(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler);
  void updateTexture(uint32_t binding,
                     ZVulkanTexture& texture,
                     vk::Sampler sampler,
                     vk::ImageLayout layoutOverride,
                     vk::ImageAspectFlags aspectOverride);
  void updateStorageBuffer(uint32_t binding, ZVulkanBuffer& buffer);
  void updateStorageImage(uint32_t binding,
                          ZVulkanTexture& texture,
                          vk::ImageLayout layoutOverride = vk::ImageLayout::eGeneral,
                          vk::ImageAspectFlags aspectOverride = {});

  // Write-once convenience helpers; return true if wrote, false if skipped
  bool writeUniformBufferOnce(uint32_t binding, ZVulkanBuffer& buffer);
  // Removed two-parameter variant (range is required to satisfy validation).
  // bool writeUniformBufferDynamicOnce(uint32_t binding, ZVulkanBuffer& buffer);
  bool writeUniformBufferDynamicOnce(uint32_t binding, ZVulkanBuffer& buffer, vk::DeviceSize range);
  bool writeTextureOnce(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler);
  bool writeStorageBufferOnce(uint32_t binding, ZVulkanBuffer& buffer);

  vk::DescriptorSet descriptorSet() const
  {
    return m_descriptorSet;
  }

private:
  ZVulkanDevice& m_device;
  // Raw handle; lifetime is tied to the owning vk::DescriptorPool. We never
  // call vkFreeDescriptorSets on this, relying on pool reset per frame.
  vk::DescriptorSet m_descriptorSet{VK_NULL_HANDLE};
  bool m_isOverrideTransient = false;
  // Track which bindings have been initialized (bit per binding index, up to 64)
  uint64_t m_initializedMask = 0ull;
};

} // namespace nim
