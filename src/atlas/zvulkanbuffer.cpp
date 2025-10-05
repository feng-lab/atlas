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
  LOG(INFO) << "ZVulkanBuffer created: " << size << " bytes";
}

void ZVulkanBuffer::createBuffer()
{
  vk::BufferCreateInfo bufferInfo{.size = m_size, .usage = m_usage, .sharingMode = vk::SharingMode::eExclusive};
  try {
    m_buffer.emplace(m_device.context().device(), bufferInfo);
    const vk::MemoryRequirements memRequirements = m_buffer->getMemoryRequirements();
    const vk::PhysicalDeviceMemoryProperties memProperties = m_device.context().physicalDevice().getMemoryProperties();

    uint32_t memoryTypeIndex = 0;
    bool foundSuitableMemory = false;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((memRequirements.memoryTypeBits & (1 << i)) &&
          (memProperties.memoryTypes[i].propertyFlags & m_memoryProperties) == m_memoryProperties) {
        memoryTypeIndex = i;
        foundSuitableMemory = true;
        break;
      }
    }
    if (!foundSuitableMemory) {
      throw ZException("Failed to find suitable memory type for buffer");
    }

    vk::MemoryAllocateInfo allocInfo{.allocationSize = memRequirements.size, .memoryTypeIndex = memoryTypeIndex};
    m_bufferMemory.emplace(m_device.context().device(), allocInfo);
    m_buffer->bindMemory(*m_bufferMemory, 0);
  }
  catch (const vk::SystemError& e) {
    throw ZException(fmt::format("Failed to create buffer: {}", e.what()));
  }
}

void ZVulkanBuffer::copyData(const void* data, size_t size)
{
  if (size > m_size) {
    throw ZException("Data size exceeds buffer size");
  }
  if (!(m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible)) {
    auto stagingBuffer =
      m_device.createBuffer(size,
                            vk::BufferUsageFlagBits::eTransferSrc,
                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    stagingBuffer->copyData(data, size);
    m_device.frameExecutor().executeImmediate(
      [&](vk::raii::CommandBuffer& cmdBuffer) {
        const vk::BufferCopy copyRegion{.srcOffset = 0, .dstOffset = 0, .size = size};
        cmdBuffer.copyBuffer(stagingBuffer->buffer(), *m_buffer, copyRegion);
      },
      "buffer_copy");
  } else {
    void* mappedMemory = m_bufferMemory->mapMemory(0, size);
    std::memcpy(mappedMemory, data, size);
    m_bufferMemory->unmapMemory();
  }
}

void* ZVulkanBuffer::map(vk::DeviceSize offset, vk::DeviceSize size)
{
  if (!(m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible)) {
    throw ZException("Attempting to map memory that is not host visible");
  }
  try {
    return m_bufferMemory->mapMemory(offset, size, {});
  }
  catch (const vk::SystemError& e) {
    throw ZException(fmt::format("Failed to map buffer memory: {}", e.what()));
  }
}

void ZVulkanBuffer::unmap()
{
  if (m_bufferMemory) {
    m_bufferMemory->unmapMemory();
  }
}

} // namespace nim
