#include "zvulkantexture.h"
#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cstring>

namespace nim {
namespace {

vk::ImageAspectFlags defaultAspectMask(vk::Format format)
{
  switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
      return vk::ImageAspectFlagBits::eDepth;
    case vk::Format::eS8Uint:
      return vk::ImageAspectFlagBits::eStencil;
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
      return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    default:
      return vk::ImageAspectFlagBits::eColor;
  }
}

vk::ImageCreateFlags createFlagsForView(vk::ImageViewType viewType)
{
  vk::ImageCreateFlags flags{};
  if (viewType == vk::ImageViewType::eCube || viewType == vk::ImageViewType::eCubeArray) {
    flags |= vk::ImageCreateFlagBits::eCubeCompatible;
  }
  return flags;
}

struct LayoutState
{
  vk::AccessFlags access;
  vk::PipelineStageFlags stage;
};

LayoutState layoutStateFor(vk::ImageLayout layout)
{
  using vk::AccessFlagBits;
  using vk::PipelineStageFlagBits;

  switch (layout) {
    case vk::ImageLayout::eUndefined:
      return {vk::AccessFlags{}, PipelineStageFlagBits::eTopOfPipe};
    case vk::ImageLayout::eGeneral:
      return {AccessFlagBits::eShaderRead | AccessFlagBits::eShaderWrite, PipelineStageFlagBits::eAllCommands};
    case vk::ImageLayout::eColorAttachmentOptimal:
      return {AccessFlagBits::eColorAttachmentWrite, PipelineStageFlagBits::eColorAttachmentOutput};
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
      return {AccessFlagBits::eDepthStencilAttachmentRead | AccessFlagBits::eDepthStencilAttachmentWrite,
              PipelineStageFlagBits::eEarlyFragmentTests | PipelineStageFlagBits::eLateFragmentTests};
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      return {AccessFlagBits::eShaderRead,
              PipelineStageFlagBits::eAllGraphics | PipelineStageFlagBits::eComputeShader};
    case vk::ImageLayout::eTransferDstOptimal:
      return {AccessFlagBits::eTransferWrite, PipelineStageFlagBits::eTransfer};
    case vk::ImageLayout::eTransferSrcOptimal:
      return {AccessFlagBits::eTransferRead, PipelineStageFlagBits::eTransfer};
    case vk::ImageLayout::ePreinitialized:
      return {AccessFlagBits::eHostWrite, PipelineStageFlagBits::eHost};
    default:
      return {vk::AccessFlags{}, PipelineStageFlagBits::eAllCommands};
  }
}

} // namespace

// ----- CreateInfo helpers ---------------------------------------------------------------------

ZVulkanTexture::CreateInfo
ZVulkanTexture::CreateInfo::make1D(uint32_t width,
                                   vk::Format format,
                                   vk::ImageUsageFlags usage,
                                   vk::MemoryPropertyFlags memoryProperties,
                                   uint32_t mipLevels,
                                   bool createSampler,
                                   vk::ImageLayout descriptorLayout)
{
  CreateInfo info;
  info.imageType = vk::ImageType::e1D;
  info.viewType = vk::ImageViewType::e1D;
  info.extent = vk::Extent3D{width, 1u, 1u};
  info.format = format;
  info.mipLevels = std::max(1u, mipLevels);
  info.arrayLayers = 1u;
  info.usage = usage;
  info.memoryProperties = memoryProperties;
  info.aspectMask = defaultAspectMask(format);
  info.createDefaultSampler = createSampler;
  info.descriptorLayout = descriptorLayout;
  return info;
}

ZVulkanTexture::CreateInfo
ZVulkanTexture::CreateInfo::make2D(uint32_t width,
                                   uint32_t height,
                                   vk::Format format,
                                   vk::ImageUsageFlags usage,
                                   vk::MemoryPropertyFlags memoryProperties,
                                   uint32_t mipLevels,
                                   bool createSampler,
                                   vk::ImageLayout descriptorLayout)
{
  CreateInfo info;
  info.imageType = vk::ImageType::e2D;
  info.viewType = vk::ImageViewType::e2D;
  info.extent = vk::Extent3D{width, height, 1u};
  info.format = format;
  info.mipLevels = std::max(1u, mipLevels);
  info.arrayLayers = 1u;
  info.usage = usage;
  info.memoryProperties = memoryProperties;
  info.aspectMask = defaultAspectMask(format);
  info.createDefaultSampler = createSampler;
  info.descriptorLayout = descriptorLayout;
  return info;
}

ZVulkanTexture::CreateInfo
ZVulkanTexture::CreateInfo::make2DArray(uint32_t width,
                                        uint32_t height,
                                        uint32_t arrayLayers,
                                        vk::Format format,
                                        vk::ImageUsageFlags usage,
                                        vk::MemoryPropertyFlags memoryProperties,
                                        uint32_t mipLevels,
                                        bool createSampler,
                                        vk::ImageLayout descriptorLayout)
{
  CreateInfo info;
  info.imageType = vk::ImageType::e2D;
  info.arrayLayers = std::max(1u, arrayLayers);
  info.viewType = info.arrayLayers > 1u ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
  info.extent = vk::Extent3D{width, height, 1u};
  info.format = format;
  info.mipLevels = std::max(1u, mipLevels);
  info.usage = usage;
  info.memoryProperties = memoryProperties;
  info.aspectMask = defaultAspectMask(format);
  info.createDefaultSampler = createSampler;
  info.descriptorLayout = descriptorLayout;
  return info;
}

ZVulkanTexture::CreateInfo
ZVulkanTexture::CreateInfo::make3D(uint32_t width,
                                   uint32_t height,
                                   uint32_t depth,
                                   vk::Format format,
                                   vk::ImageUsageFlags usage,
                                   vk::MemoryPropertyFlags memoryProperties,
                                   uint32_t mipLevels,
                                   bool createSampler,
                                   vk::ImageLayout descriptorLayout)
{
  CreateInfo info;
  info.imageType = vk::ImageType::e3D;
  info.viewType = vk::ImageViewType::e3D;
  info.extent = vk::Extent3D{width, height, depth};
  info.format = format;
  info.mipLevels = std::max(1u, mipLevels);
  info.arrayLayers = 1u;
  info.usage = usage;
  info.memoryProperties = memoryProperties;
  info.aspectMask = defaultAspectMask(format);
  info.createDefaultSampler = createSampler;
  info.descriptorLayout = descriptorLayout;
  return info;
}

ZVulkanTexture::CreateInfo
ZVulkanTexture::CreateInfo::makeCube(uint32_t edgeLength,
                                     vk::Format format,
                                     uint32_t mipLevels,
                                     uint32_t cubeCount,
                                     vk::ImageUsageFlags usage,
                                     vk::MemoryPropertyFlags memoryProperties,
                                     bool createSampler,
                                     vk::ImageLayout descriptorLayout)
{
  if (cubeCount == 0u) {
    throw ZException("Cube texture must have at least one cube");
  }

  CreateInfo info;
  info.imageType = vk::ImageType::e2D;
  info.viewType = cubeCount > 1u ? vk::ImageViewType::eCubeArray : vk::ImageViewType::eCube;
  info.extent = vk::Extent3D{edgeLength, edgeLength, 1u};
  info.format = format;
  info.mipLevels = std::max(1u, mipLevels);
  info.arrayLayers = cubeCount * 6u;
  info.usage = usage;
  info.memoryProperties = memoryProperties;
  info.aspectMask = defaultAspectMask(format);
  info.createDefaultSampler = createSampler;
  info.descriptorLayout = descriptorLayout;
  return info;
}

// ----- Construction ---------------------------------------------------------------------------

ZVulkanTexture::ZVulkanTexture(ZVulkanDevice& device, const CreateInfo& createInfo)
  : m_device(device)
  , m_createInfo(createInfo)
  , m_extent(createInfo.extent)
  , m_format(createInfo.format)
  , m_mipLevels(std::max(1u, createInfo.mipLevels))
  , m_arrayLayers(std::max(1u, createInfo.arrayLayers))
  , m_usage(createInfo.usage)
  , m_memoryProperties(createInfo.memoryProperties)
  , m_aspectMask(createInfo.aspectMask == vk::ImageAspectFlags{} ? defaultAspectMask(createInfo.format)
                                                                  : createInfo.aspectMask)
  , m_descriptorLayout(createInfo.descriptorLayout)
  , m_currentLayout(createInfo.initialLayout)
{
  createImage();
  allocateMemory();
  createImageView();
  createSampler();
  if (m_arrayLayers > 1u) {
    m_layerImageViews.resize(m_arrayLayers);
  }
  LOG(INFO) << "ZVulkanTexture created: " << m_extent.width << "x" << m_extent.height << "x" << m_extent.depth
            << " layers=" << m_arrayLayers;
}

ZVulkanTexture::ZVulkanTexture(ZVulkanDevice& device, uint32_t width, uint32_t height, vk::Format format)
  : ZVulkanTexture(device, CreateInfo::make2D(width, height, format))
{}

ZVulkanTexture::ZVulkanTexture(ZVulkanDevice& device,
                               uint32_t width,
                               uint32_t height,
                               vk::Format format,
                               vk::ImageUsageFlags usage,
                               vk::MemoryPropertyFlags memoryProperties)
  : ZVulkanTexture(device, CreateInfo::make2D(width, height, format, usage, memoryProperties))
{}

ZVulkanTexture::~ZVulkanTexture()
{
  LOG(INFO) << "Destroying ZVulkanTexture";
}

// ----- Public API -----------------------------------------------------------------------------

void ZVulkanTexture::uploadData(const void* data, size_t size)
{
  uploadData(data, size, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void ZVulkanTexture::uploadData(const void* data, size_t size, vk::ImageLayout finalLayout)
{
  UploadRegion region{};
  region.finalLayout = finalLayout;
  uploadInternal(data, size, region);
}

void ZVulkanTexture::uploadSubImage(const void* data, size_t size, const UploadRegion& region)
{
  uploadInternal(data, size, region);
}

void ZVulkanTexture::downloadData(void* data, size_t size)
{
  if (!data || size == 0) {
    throw ZException("Invalid download buffer");
  }

  auto stagingBuffer =
    m_device.createBuffer(size,
                          vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

  auto originalLayout = m_currentLayout;
  auto cmdBuffer = m_device.beginSingleTimeCommands();
  transitionLayout(cmdBuffer, originalLayout, vk::ImageLayout::eTransferSrcOptimal, m_aspectMask);

  vk::BufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = m_aspectMask;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = m_createInfo.imageType == vk::ImageType::e3D ? 1u : m_arrayLayers;
  region.imageOffset = vk::Offset3D{0, 0, 0};
  region.imageExtent = vk::Extent3D{m_extent.width, m_extent.height,
                                    m_createInfo.imageType == vk::ImageType::e3D ? m_extent.depth : 1u};

  cmdBuffer.copyImageToBuffer(*m_image, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer->buffer(), region);
  transitionLayout(cmdBuffer, vk::ImageLayout::eTransferSrcOptimal, originalLayout, m_aspectMask);
  m_device.endSingleTimeCommands(cmdBuffer);

  void* mapped = stagingBuffer->map(0, size);
  std::memcpy(data, mapped, size);
  stagingBuffer->unmap();
}

void ZVulkanTexture::transitionLayout(vk::raii::CommandBuffer& cmdBuffer,
                                      vk::ImageLayout oldLayout,
                                      vk::ImageLayout newLayout,
                                      vk::ImageAspectFlags aspectMask)
{
  const auto aspect = aspectMask == vk::ImageAspectFlags{} ? m_aspectMask : aspectMask;

  if (oldLayout == newLayout) {
    return;
  }

  const auto srcState = layoutStateFor(oldLayout);
  const auto dstState = layoutStateFor(newLayout);

  vk::ImageMemoryBarrier barrier{};
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcAccessMask = srcState.access;
  barrier.dstAccessMask = dstState.access;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = *m_image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = m_mipLevels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = m_arrayLayers;

  cmdBuffer.pipelineBarrier(srcState.stage, dstState.stage, {}, nullptr, nullptr, barrier);
  m_currentLayout = newLayout;
}

vk::DescriptorImageInfo ZVulkanTexture::descriptorInfo() const
{
  if (!m_imageView) {
    throw ZException("Descriptor info requested before image view creation");
  }

  vk::DescriptorImageInfo info{};
  info.imageView = *m_imageView;
  info.imageLayout = m_descriptorLayout;
  info.sampler = m_sampler ? **m_sampler : vk::Sampler{};
  return info;
}

void ZVulkanTexture::setDescriptorLayout(vk::ImageLayout layout)
{
  m_descriptorLayout = layout;
}

vk::Sampler ZVulkanTexture::sampler() const
{
  return m_sampler ? **m_sampler : vk::Sampler{};
}

vk::ImageView ZVulkanTexture::layerImageView(uint32_t layer) const
{
  if (m_arrayLayers <= 1u) {
    return m_imageView ? **m_imageView : vk::ImageView{};
  }
  if (!m_image || !m_imageView || layer >= m_arrayLayers) {
    return vk::ImageView{};
  }
  if (m_layerImageViews.empty()) {
    m_layerImageViews.resize(m_arrayLayers);
  }
  if (!m_layerImageViews[layer].has_value()) {
    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = **m_image;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = m_format;
    viewInfo.components = vk::ComponentMapping{};
    viewInfo.subresourceRange.aspectMask = m_aspectMask;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = layer;
    viewInfo.subresourceRange.layerCount = 1;
    m_layerImageViews[layer].emplace(m_device.context().device(), viewInfo);
  }
  return **m_layerImageViews[layer];
}

// ----- Private helpers ------------------------------------------------------------------------

void ZVulkanTexture::createImage()
{
  vk::ImageCreateInfo imageInfo{};
  imageInfo.flags = createFlagsForView(m_createInfo.viewType);
  imageInfo.imageType = m_createInfo.imageType;
  imageInfo.format = m_format;
  imageInfo.extent = m_extent;
  imageInfo.mipLevels = m_mipLevels;
  imageInfo.arrayLayers = m_arrayLayers;
  imageInfo.samples = m_createInfo.samples;
  imageInfo.tiling = m_createInfo.tiling;
  imageInfo.usage = m_usage;
  imageInfo.sharingMode = vk::SharingMode::eExclusive;
  m_image.emplace(m_device.context().device(), imageInfo);
}

void ZVulkanTexture::allocateMemory()
{
  const auto memRequirements = m_image->getMemoryRequirements();
  const auto memProperties = m_device.context().physicalDevice().getMemoryProperties();

  uint32_t memoryTypeIndex = 0;
  bool found = false;
  for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
    const bool typeSupported = (memRequirements.memoryTypeBits & (1u << i)) != 0u;
    const bool flagsMatch =
      (memProperties.memoryTypes[i].propertyFlags & m_memoryProperties) == m_memoryProperties;
    if (typeSupported && flagsMatch) {
      memoryTypeIndex = i;
      found = true;
      break;
    }
  }

  if (!found) {
    throw ZException("Failed to find suitable memory type for Vulkan image");
  }

  vk::MemoryAllocateInfo allocInfo{.allocationSize = memRequirements.size, .memoryTypeIndex = memoryTypeIndex};
  m_imageMemory.emplace(m_device.context().device(), allocInfo);
  m_image->bindMemory(*m_imageMemory, 0);
}

void ZVulkanTexture::createImageView()
{
  vk::ImageViewCreateInfo viewInfo{};
  viewInfo.image = *m_image;
  viewInfo.viewType = m_createInfo.viewType;
  viewInfo.format = m_format;
  viewInfo.subresourceRange.aspectMask = m_aspectMask;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = m_mipLevels;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = m_arrayLayers;

  m_imageView.emplace(m_device.context().device(), viewInfo);
}

void ZVulkanTexture::createSampler()
{
  if (m_createInfo.samplerInfo.has_value()) {
    m_sampler.emplace(m_device.context().device(), m_createInfo.samplerInfo.value());
    return;
  }

  if (!m_createInfo.createDefaultSampler || !(m_usage & vk::ImageUsageFlagBits::eSampled)) {
    return;
  }

  vk::SamplerCreateInfo samplerInfo{};
  samplerInfo.magFilter = vk::Filter::eLinear;
  samplerInfo.minFilter = vk::Filter::eLinear;
  samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
  samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
  samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
  samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = static_cast<float>(std::max(1u, m_mipLevels) - 1u);
  samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueWhite;

  m_sampler.emplace(m_device.context().device(), samplerInfo);
}

vk::Extent3D ZVulkanTexture::mipExtent(uint32_t mipLevel) const
{
  mipLevel = std::min(mipLevel, m_mipLevels - 1u);
  const uint32_t width = std::max(1u, m_extent.width >> mipLevel);
  const uint32_t height = std::max(1u, m_extent.height >> mipLevel);
  const uint32_t depth =
    m_createInfo.imageType == vk::ImageType::e3D ? std::max(1u, m_extent.depth >> mipLevel) : 1u;
  return vk::Extent3D{width, height, depth};
}

void ZVulkanTexture::uploadInternal(const void* data, size_t size, const UploadRegion& request)
{
  if (!data || size == 0) {
    throw ZException("Cannot upload empty texture payload");
  }
  if (!m_image) {
    throw ZException("Texture resources not initialized before upload");
  }

  UploadRegion region = request;
  region.mipLevel = std::min(region.mipLevel, m_mipLevels - 1u);

  const auto subresourceExtent = mipExtent(region.mipLevel);

  if (m_createInfo.imageType == vk::ImageType::e3D) {
    region.extent.width = region.extent.width == 0 ? subresourceExtent.width : region.extent.width;
    region.extent.height = region.extent.height == 0 ? subresourceExtent.height : region.extent.height;
    region.extent.depth = region.extent.depth == 0 ? subresourceExtent.depth : region.extent.depth;
    region.layerCount = 1u;
    region.baseArrayLayer = 0u;
  } else {
    region.extent.width = region.extent.width == 0 ? subresourceExtent.width : region.extent.width;
    region.extent.height = region.extent.height == 0 ? subresourceExtent.height : region.extent.height;
    region.extent.depth = 1u;
    region.offset.z = 0;
    const uint32_t remainingLayers = m_arrayLayers - std::min(region.baseArrayLayer, m_arrayLayers - 1u);
    region.layerCount = region.layerCount == 0 ? remainingLayers : region.layerCount;
  }

  if (region.extent.width == 0 || region.extent.height == 0 || region.extent.depth == 0) {
    throw ZException("Upload region has zero extent");
  }

  if (region.offset.x + region.extent.width > subresourceExtent.width ||
      region.offset.y + region.extent.height > subresourceExtent.height) {
    throw ZException("Upload region exceeds image dimensions");
  }

  if (m_createInfo.imageType == vk::ImageType::e3D) {
    if (region.offset.z + region.extent.depth > subresourceExtent.depth) {
      throw ZException("3D upload region exceeds image depth");
    }
  } else {
    if (region.baseArrayLayer >= m_arrayLayers ||
        region.baseArrayLayer + region.layerCount > m_arrayLayers) {
      throw ZException("Upload region exceeds array layer bounds");
    }
  }

  auto stagingBuffer =
    m_device.createBuffer(size,
                          vk::BufferUsageFlagBits::eTransferSrc,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  stagingBuffer->copyData(data, size);

  auto cmdBuffer = m_device.beginSingleTimeCommands();
  transitionLayout(cmdBuffer, m_currentLayout, vk::ImageLayout::eTransferDstOptimal, m_aspectMask);

  vk::BufferImageCopy copyRegion{};
  copyRegion.bufferOffset = 0;
  copyRegion.bufferRowLength = region.bufferRowLength;
  copyRegion.bufferImageHeight = region.bufferImageHeight;
  copyRegion.imageSubresource.aspectMask = m_aspectMask;
  copyRegion.imageSubresource.mipLevel = region.mipLevel;
  copyRegion.imageSubresource.baseArrayLayer = m_createInfo.imageType == vk::ImageType::e3D ? 0u : region.baseArrayLayer;
  copyRegion.imageSubresource.layerCount = m_createInfo.imageType == vk::ImageType::e3D ? 1u : region.layerCount;
  copyRegion.imageOffset = region.offset;
  copyRegion.imageExtent = region.extent;

  cmdBuffer.copyBufferToImage(stagingBuffer->buffer(), *m_image, vk::ImageLayout::eTransferDstOptimal, copyRegion);

  const vk::ImageLayout finalLayout = region.finalLayout;
  transitionLayout(cmdBuffer, vk::ImageLayout::eTransferDstOptimal, finalLayout, m_aspectMask);
  m_device.endSingleTimeCommands(cmdBuffer);

  if (finalLayout != vk::ImageLayout::eUndefined && finalLayout != vk::ImageLayout::eTransferDstOptimal) {
    m_descriptorLayout = finalLayout;
  }
}

} // namespace nim
