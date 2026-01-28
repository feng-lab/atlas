#pragma once

#include "zvulkan.h"
#include <optional>
#include <utility>

namespace nim {

class ZVulkanDevice;

class ZVulkanBuffer
{
public:
  ZVulkanBuffer(ZVulkanDevice& device, size_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties);
  ZVulkanBuffer(ZVulkanDevice& device,
                size_t size,
                vk::BufferUsageFlags usage,
                vk::MemoryPropertyFlags properties,
                VmaPool poolOverride);
  ~ZVulkanBuffer();

  // RAII view over a mapped buffer range; automatically unmaps in its destructor and
  // follows move-only semantics so mappings cannot be double-unmapped accidentally.
  class ScopedMap
  {
  public:
    ScopedMap() = default;
    ScopedMap(ZVulkanBuffer& buffer, vk::DeviceSize offset, vk::DeviceSize size);
    ScopedMap(const ScopedMap&) = delete;
    ScopedMap& operator=(const ScopedMap&) = delete;
    ScopedMap(ScopedMap&& other) noexcept;
    ScopedMap& operator=(ScopedMap&& other) noexcept;
    ~ScopedMap();

    void* data() const
    {
      return m_data;
    }

    template<typename T>
    T* as() const
    {
      return static_cast<T*>(m_data);
    }

    explicit operator bool() const
    {
      return m_data != nullptr;
    }

  private:
    ZVulkanBuffer* m_buffer = nullptr;
    void* m_data = nullptr;
  };

  vk::Buffer buffer() const
  {
    return m_buffer;
  }
  vk::DeviceMemory memory() const
  {
    return VK_NULL_HANDLE; // Not exposed with VMA-backed buffers
  }
  size_t size() const
  {
    return m_size;
  }

  vk::BufferUsageFlags usage() const
  {
    return m_usage;
  }

  ZVulkanDevice& ownerDevice() const
  {
    return m_device;
  }

  void copyData(const void* data, size_t size);
  void* map(vk::DeviceSize offset, vk::DeviceSize size);
  void unmap();

  [[nodiscard]] ScopedMap mapRange(vk::DeviceSize offset, vk::DeviceSize size)
  {
    return ScopedMap(*this, offset, size);
  }

private:
  void createBuffer();

  ZVulkanDevice& m_device;
  vk::Buffer m_buffer{};
  VmaAllocation m_allocation = VK_NULL_HANDLE;
  VmaPool m_poolOverride = VK_NULL_HANDLE;
  size_t m_size;
  vk::BufferUsageFlags m_usage;
  vk::MemoryPropertyFlags m_memoryProperties;
};

} // namespace nim
