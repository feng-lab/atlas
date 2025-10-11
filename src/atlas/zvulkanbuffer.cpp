#include "zvulkanbuffer.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanframeexecutor.h"
#include "zexception.h"
#include "zlog.h"

#include <cstring>

namespace nim {

ZVulkanBuffer::ScopedMap::ScopedMap(ZVulkanBuffer& buffer, vk::DeviceSize offset, vk::DeviceSize size)
  : m_buffer(&buffer)
  , m_data(buffer.map(offset, size))
{}

ZVulkanBuffer::ScopedMap::ScopedMap(ScopedMap&& other) noexcept
  : m_buffer(std::exchange(other.m_buffer, nullptr))
  , m_data(std::exchange(other.m_data, nullptr))
{}

ZVulkanBuffer::ScopedMap& ZVulkanBuffer::ScopedMap::operator=(ScopedMap&& other) noexcept
{
  if (this != &other) {
    if (m_buffer) {
      m_buffer->unmap();
    }
    m_buffer = std::exchange(other.m_buffer, nullptr);
    m_data = std::exchange(other.m_data, nullptr);
  }
  return *this;
}

ZVulkanBuffer::ScopedMap::~ScopedMap()
{
  if (m_buffer) {
    m_buffer->unmap();
  }
}

ZVulkanBuffer::ZVulkanBuffer(ZVulkanDevice& device,
                             size_t size,
                             vk::BufferUsageFlags usage,
                             vk::MemoryPropertyFlags properties)
  : m_device(device)
  , m_size(size)
  , m_usage(usage)
  , m_memoryProperties(properties)
{
  createBuffer();
  VLOG(2) << "ZVulkanBuffer created: " << size << " bytes";
}

ZVulkanBuffer::ZVulkanBuffer(ZVulkanDevice& device,
                             size_t size,
                             vk::BufferUsageFlags usage,
                             vk::MemoryPropertyFlags properties,
                             VmaPool poolOverride)
  : m_device(device)
  , m_poolOverride(poolOverride)
  , m_size(size)
  , m_usage(usage)
  , m_memoryProperties(properties)
{
  createBuffer();
  VLOG(2) << "ZVulkanBuffer (pool) created: " << size << " bytes";
}

ZVulkanBuffer::~ZVulkanBuffer()
{
  if (m_buffer && m_allocation) {
    vmaDestroyBuffer(m_device.allocator(), m_buffer, m_allocation);
    m_buffer = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
  }
}

void ZVulkanBuffer::createBuffer()
{
  vk::BufferCreateInfo bufferInfo{.size = m_size, .usage = m_usage, .sharingMode = vk::SharingMode::eExclusive};
  VmaAllocationCreateInfo allocInfo{};
  // Map common memory properties to VMA flags
  if (m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible) {
    allocInfo.requiredFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  }
  if (m_memoryProperties & vk::MemoryPropertyFlagBits::eHostCoherent) {
    allocInfo.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }
  if (m_memoryProperties & vk::MemoryPropertyFlagBits::eDeviceLocal) {
    allocInfo.requiredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }
  // Prefer usage-based automatic memory selection
  if (m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible) {
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  } else if (m_memoryProperties & vk::MemoryPropertyFlagBits::eDeviceLocal) {
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  } else {
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
  }
  // Route allocation to usage-specific pools when available
  VmaPool chosenPool = VK_NULL_HANDLE;
  if (m_poolOverride != VK_NULL_HANDLE) {
    chosenPool = m_poolOverride;
  } else if ((m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible) &&
             m_device.uploadStagingPool() != VK_NULL_HANDLE) {
    chosenPool = m_device.uploadStagingPool();
  } else if ((m_memoryProperties & vk::MemoryPropertyFlagBits::eDeviceLocal) &&
             m_device.deviceLocalPool() != VK_NULL_HANDLE) {
    chosenPool = m_device.deviceLocalPool();
  }

  auto tryCreate = [&](VmaPool pool, bool dedicated) -> VkResult {
    VmaAllocationCreateInfo ci = allocInfo;
    ci.pool = pool;
    if (dedicated) {
      ci.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
      ci.pool = VK_NULL_HANDLE; // dedicated allocations are outside pools
    }
    VkBuffer raw = VK_NULL_HANDLE;
    VkResult r = vmaCreateBuffer(m_device.allocator(),
                                 reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo),
                                 &ci,
                                 &raw,
                                 &m_allocation,
                                 nullptr);
    if (r == VK_SUCCESS) {
      m_buffer = raw;
    }
    return r;
  };

  // Heuristic: if size is large relative to common pool block sizes, prefer dedicated upfront
  const VkDeviceSize sz = static_cast<VkDeviceSize>(m_size);
  bool preferDedicated = false;
  if (chosenPool == m_device.deviceLocalPool()) {
    preferDedicated = (sz >= (128ull * 1024ull * 1024ull) / 2); // >= 64 MiB
  } else if (chosenPool == m_device.uploadStagingPool()) {
    preferDedicated = (sz >= (64ull * 1024ull * 1024ull) / 2); // >= 32 MiB
  } else if (chosenPool == m_device.uploadTransientPool()) {
    preferDedicated = (sz >= (32ull * 1024ull * 1024ull) / 2); // >= 16 MiB
  }

  VkResult res = VK_ERROR_INITIALIZATION_FAILED;
  if (preferDedicated) {
    res = tryCreate(VK_NULL_HANDLE, true);
  } else if (chosenPool != VK_NULL_HANDLE) {
    res = tryCreate(chosenPool, false);
  }
  if (res != VK_SUCCESS) {
    // Fallback: try dedicated, then no-pool default
    if (!preferDedicated) {
      res = tryCreate(VK_NULL_HANDLE, true);
    }
    if (res != VK_SUCCESS) {
      res = tryCreate(VK_NULL_HANDLE, false);
    }
  }
  if (res != VK_SUCCESS) {
    throw ZException("Failed to create VMA buffer (with fallbacks)");
  }
}

void ZVulkanBuffer::copyData(const void* data, size_t size)
{
  if (size > m_size) {
    throw ZException("Data size exceeds buffer size");
  }
  // Try to map directly; if not host visible, fall back to staging
  void* mappedMemory = nullptr;
  if (vmaMapMemory(m_device.allocator(), m_allocation, &mappedMemory) != VK_SUCCESS || mappedMemory == nullptr) {
    auto stagingBuffer =
      m_device.createBuffer(size,
                            vk::BufferUsageFlagBits::eTransferSrc,
                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    stagingBuffer->copyData(data, size);
    m_device.frameExecutor().executeImmediate(
      [&](vk::raii::CommandBuffer& cmdBuffer) {
        const vk::BufferCopy copyRegion{.srcOffset = 0, .dstOffset = 0, .size = size};
        cmdBuffer.copyBuffer(stagingBuffer->buffer(), m_buffer, copyRegion);
      },
      "buffer_copy");
  } else {
    std::memcpy(mappedMemory, data, size);
    vmaUnmapMemory(m_device.allocator(), m_allocation);
  }
}

void* ZVulkanBuffer::map(vk::DeviceSize offset, vk::DeviceSize size)
{
  (void)size; // VMA doesn't require a size for mapping the whole allocation
  void* ptr = nullptr;
  VkResult res = vmaMapMemory(m_device.allocator(), m_allocation, &ptr);
  if (res != VK_SUCCESS || !ptr) {
    throw ZException("Failed to map VMA buffer memory");
  }
  return static_cast<char*>(ptr) + static_cast<size_t>(offset);
}

void ZVulkanBuffer::unmap()
{
  if (m_allocation) {
    vmaUnmapMemory(m_device.allocator(), m_allocation);
  }
}

} // namespace nim
