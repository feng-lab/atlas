#include "zvulkanbuffer.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"

#include <fmt/format.h>
#include <cstring>

namespace nim {

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
    auto cmdBuffer = m_device.beginSingleTimeCommands();
    const vk::BufferCopy copyRegion{.srcOffset = 0, .dstOffset = 0, .size = size};
    cmdBuffer.copyBuffer(stagingBuffer->buffer(), *m_buffer, copyRegion);
    m_device.endSingleTimeCommands(cmdBuffer);
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
