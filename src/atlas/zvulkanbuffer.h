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
    return *m_buffer;
  }
  vk::DeviceMemory memory() const
  {
    return *m_bufferMemory;
  }
  size_t size() const
  {
    return m_size;
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
  std::optional<vk::raii::Buffer> m_buffer;
  std::optional<vk::raii::DeviceMemory> m_bufferMemory;
  size_t m_size;
  vk::BufferUsageFlags m_usage;
  vk::MemoryPropertyFlags m_memoryProperties;
};

} // namespace nim
