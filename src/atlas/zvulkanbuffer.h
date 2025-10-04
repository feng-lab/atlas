#pragma once

#include "zvulkan.h"
#include <optional>

namespace nim {

class ZVulkanDevice;

class ZVulkanBuffer
{
public:
  ZVulkanBuffer(ZVulkanDevice& device, size_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties);

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
