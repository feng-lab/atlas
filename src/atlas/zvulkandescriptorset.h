#pragma once

#include "zvulkan.h"

#include <array>
#include <cstdint>

namespace nim {

class ZVulkanDevice;
class ZVulkanBuffer;
class ZVulkanTexture;

class ZVulkanDescriptorSet
{
public:
  ZVulkanDescriptorSet(ZVulkanDevice& device, vk::DescriptorSet descriptorSet);

  void updateUniformBuffer(uint32_t binding, ZVulkanBuffer& buffer);
  void updateUniformBufferDynamic(uint32_t binding, ZVulkanBuffer& buffer, vk::DeviceSize range);
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
  bool writeStorageBufferOnce(uint32_t binding, ZVulkanBuffer& buffer);

  vk::DescriptorSet descriptorSet() const
  {
    return m_descriptorSet;
  }

  // Monotonically increasing generation counter that changes whenever this
  // descriptor set's contents are updated via vkUpdateDescriptorSets.
  [[nodiscard]] uint64_t generation() const
  {
    return m_generation;
  }

private:
  struct BindingState
  {
    enum class Kind : uint8_t
    {
      None,
      Buffer,
      Image
    };

    Kind kind = Kind::None;
    vk::DescriptorType type = vk::DescriptorType::eUniformBuffer;
    vk::DescriptorBufferInfo bufferInfo{};
    vk::DescriptorImageInfo imageInfo{};
  };

  ZVulkanDevice& m_device;
  // Raw handle; lifetime is tied to the owning vk::DescriptorPool. We never
  // call vkFreeDescriptorSets on this, relying on descriptor-pool reset (for
  // per-frame arenas) or pool destruction (for persistent arenas).
  vk::DescriptorSet m_descriptorSet{VK_NULL_HANDLE};
  // Track which bindings have been initialized (bit per binding index, up to 64)
  uint64_t m_initializedMask = 0ull;
  // Per-binding last-write state used to avoid redundant vkUpdateDescriptorSets
  // calls that would invalidate cached command buffers.
  std::array<BindingState, 64> m_bindingStates{};
  uint64_t m_generation = 0;
};

} // namespace nim
