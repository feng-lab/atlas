#include "zvulkantexture.h"
#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"

#include <fmt/format.h>

namespace nim {

ZVulkanTexture::ZVulkanTexture(ZVulkanDevice& device, uint32_t width, uint32_t height, vk::Format format)
  : m_device(device)
  , m_width(width)
  , m_height(height)
  , m_format(format)
{
  createImage();
  createImageView();
  LOG(INFO) << "ZVulkanTexture created: " << width << "x" << height;
}

ZVulkanTexture::ZVulkanTexture(ZVulkanDevice& device,
                               uint32_t width,
                               uint32_t height,
                               vk::Format format,
                               vk::ImageUsageFlags usage,
                               vk::MemoryPropertyFlags memoryProperties)
  : m_device(device)
  , m_width(width)
  , m_height(height)
  , m_format(format)
  , m_usage(usage)
  , m_memoryProperties(memoryProperties)
{
  createImage();
  createImageView();
}

ZVulkanTexture::~ZVulkanTexture()
{
  LOG(INFO) << "Destroying ZVulkanTexture";
}

void ZVulkanTexture::createImage()
{
  vk::ImageCreateInfo imageInfo{
    .imageType = vk::ImageType::e2D,
    .format = m_format,
    .extent = vk::Extent3D{m_width, m_height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = vk::SampleCountFlagBits::e1,
    .tiling = vk::ImageTiling::eOptimal,
    .usage = m_usage
  };
  m_image.emplace(m_device.context().device(), imageInfo);

  const auto memRequirements = m_image->getMemoryRequirements();
  const auto memProperties = m_device.context().physicalDevice().getMemoryProperties();
  uint32_t memoryTypeIndex = 0;
  bool found = false;
  for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
    if ((memRequirements.memoryTypeBits & (1 << i)) &&
        (memProperties.memoryTypes[i].propertyFlags & m_memoryProperties) == m_memoryProperties) {
      memoryTypeIndex = i;
      found = true;
      break;
    }
  }
  if (!found) {
    throw ZException("Failed to find suitable memory type for image");
  }

  vk::MemoryAllocateInfo allocInfo{.allocationSize = memRequirements.size, .memoryTypeIndex = memoryTypeIndex};
  m_imageMemory.emplace(m_device.context().device(), allocInfo);
  m_image->bindMemory(*m_imageMemory, 0);
}

void ZVulkanTexture::createImageView()
{
  vk::ImageAspectFlags aspect = {};
  switch (m_format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
      aspect = vk::ImageAspectFlagBits::eDepth;
      break;
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
      aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
      break;
    default:
      aspect = vk::ImageAspectFlagBits::eColor;
      break;
  }

  vk::ImageViewCreateInfo viewInfo{
    .image = *m_image,
    .viewType = vk::ImageViewType::e2D,
    .format = m_format,
    .subresourceRange = {aspect, 0, 1, 0, 1}
  };
  m_imageView.emplace(m_device.context().device(), viewInfo);
}

void ZVulkanTexture::uploadData(const void* data, size_t size)
{
  auto stagingBuffer =
    m_device.createBuffer(size,
                          vk::BufferUsageFlagBits::eTransferSrc,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  stagingBuffer->copyData(data, size);

  auto cmdBuffer = m_device.beginSingleTimeCommands();
  transitionLayout(cmdBuffer, m_currentLayout, vk::ImageLayout::eTransferDstOptimal);

  vk::BufferImageCopy region{
    .bufferOffset = 0,
    .bufferRowLength = 0,
    .bufferImageHeight = 0,
    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
    .imageOffset = {0, 0, 0},
    .imageExtent = {m_width, m_height, 1}
  };
  cmdBuffer.copyBufferToImage(stagingBuffer->buffer(), *m_image, vk::ImageLayout::eTransferDstOptimal, region);
  transitionLayout(cmdBuffer, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
  m_device.endSingleTimeCommands(cmdBuffer);
}

void ZVulkanTexture::downloadData(void* data, size_t size)
{
  auto stagingBuffer =
    m_device.createBuffer(size,
                          vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

  auto cmdBuffer = m_device.beginSingleTimeCommands();
  transitionLayout(cmdBuffer, m_currentLayout, vk::ImageLayout::eTransferSrcOptimal);
  vk::BufferImageCopy region{
    .bufferOffset = 0,
    .bufferRowLength = 0,
    .bufferImageHeight = 0,
    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
    .imageOffset = {0, 0, 0},
    .imageExtent = {m_width, m_height, 1}
  };
  cmdBuffer.copyImageToBuffer(*m_image, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer->buffer(), region);
  transitionLayout(cmdBuffer, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
  m_device.endSingleTimeCommands(cmdBuffer);

  void* mapped = stagingBuffer->map(0, size);
  std::memcpy(data, mapped, size);
  stagingBuffer->unmap();
}

void ZVulkanTexture::transitionLayout(vk::raii::CommandBuffer& cmdBuffer,
                                      vk::ImageLayout oldLayout,
                                      vk::ImageLayout newLayout)
{
  vk::ImageMemoryBarrier barrier{};
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = *m_image;
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  vk::PipelineStageFlags srcStage, dstStage;
  if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
    dstStage = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal &&
             newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    srcStage = vk::PipelineStageFlagBits::eTransfer;
    dstStage = vk::PipelineStageFlagBits::eFragmentShader;
  } else if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask =
      vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
    dstStage = vk::PipelineStageFlagBits::eEarlyFragmentTests;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
  } else if (oldLayout == vk::ImageLayout::eTransferSrcOptimal &&
             newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    srcStage = vk::PipelineStageFlagBits::eTransfer;
    dstStage = vk::PipelineStageFlagBits::eFragmentShader;
  } else if (oldLayout == vk::ImageLayout::eColorAttachmentOptimal &&
             newLayout == vk::ImageLayout::eTransferSrcOptimal) {
    // Prepare color attachment for copy/read
    barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    srcStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dstStage = vk::PipelineStageFlagBits::eTransfer;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  } else if (oldLayout == vk::ImageLayout::eTransferSrcOptimal &&
             newLayout == vk::ImageLayout::eColorAttachmentOptimal) {
    // Restore for rendering
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    srcStage = vk::PipelineStageFlagBits::eTransfer;
    dstStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  } else if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eColorAttachmentOptimal) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
    dstStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  } else {
    throw ZException("Unsupported layout transition");
  }

  cmdBuffer.pipelineBarrier(srcStage, dstStage, {}, nullptr, nullptr, barrier);
  m_currentLayout = newLayout;
}

} // namespace nim
