#include "zvulkantexture.h"
#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkancontext.h"
#include "zvulkanframeexecutor.h"
#include "zimg.h"
#include "zimgformat.h"
#include "zglmutils.h"

#include <glm/gtc/packing.hpp>
#include "zexception.h"
#include "zlog.h"

#include <QString>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <limits>

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
      return {AccessFlagBits::eShaderRead, PipelineStageFlagBits::eAllGraphics | PipelineStageFlagBits::eComputeShader};
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

ZVulkanTexture::CreateInfo ZVulkanTexture::CreateInfo::make1D(uint32_t width,
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

ZVulkanTexture::CreateInfo ZVulkanTexture::CreateInfo::make2D(uint32_t width,
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

ZVulkanTexture::CreateInfo ZVulkanTexture::CreateInfo::make2DArray(uint32_t width,
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

ZVulkanTexture::CreateInfo ZVulkanTexture::CreateInfo::make3D(uint32_t width,
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

ZVulkanTexture::CreateInfo ZVulkanTexture::CreateInfo::makeCube(uint32_t edgeLength,
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
  , m_descriptorAspectMask(m_aspectMask)
  , m_currentLayout(createInfo.initialLayout)
{
  createImage();
  allocateMemory();
  createImageView();
  createSampler();
  if (m_arrayLayers > 1u) {
    m_layerImageViews.resize(m_arrayLayers);
    m_layerDepthViews.resize(m_arrayLayers);
    m_layerStencilViews.resize(m_arrayLayers);
  }
  VLOG(2) << "ZVulkanTexture created: " << m_extent.width << "x" << m_extent.height << "x" << m_extent.depth
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
  // Destroy views/samplers first to avoid referencing the image during teardown
  m_layerImageViews.clear();
  m_layerDepthViews.clear();
  m_layerStencilViews.clear();
  m_genericAspectViews.clear();
  if (m_imageView) {
    m_imageView.reset();
  }
  if (m_sampler) {
    m_sampler.reset();
  }
  if (m_image && m_imageAllocation) {
    vmaDestroyImage(m_device.allocator(), m_image, m_imageAllocation);
    m_image = vk::Image{};
    m_imageAllocation = nullptr;
  }
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
  // Vulkan copies require a single aspect in BufferImageCopy. If this image
  // has combined depth+stencil, prefer copying DEPTH by default.
  auto effectiveAspect = m_aspectMask;
  const auto depthStencilMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
  if ((effectiveAspect & depthStencilMask) == depthStencilMask) {
    effectiveAspect = vk::ImageAspectFlagBits::eDepth;
  }
  m_device.frameExecutor().executeImmediate(
    [&](vk::raii::CommandBuffer& cmdBuffer) {
      transitionLayout(cmdBuffer, originalLayout, vk::ImageLayout::eTransferSrcOptimal, effectiveAspect);

      vk::BufferImageCopy region{};
      region.bufferOffset = 0;
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource.aspectMask = effectiveAspect;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = 0;
      region.imageSubresource.layerCount = m_createInfo.imageType == vk::ImageType::e3D ? 1u : m_arrayLayers;
      region.imageOffset = vk::Offset3D{0, 0, 0};
      region.imageExtent = vk::Extent3D{m_extent.width,
                                        m_extent.height,
                                        m_createInfo.imageType == vk::ImageType::e3D ? m_extent.depth : 1u};

      cmdBuffer.copyImageToBuffer(m_image, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer->buffer(), region);
      // Do not transition back to an undefined layout; if unknown, return to descriptor layout.
      const vk::ImageLayout restoreLayout =
        (originalLayout == vk::ImageLayout::eUndefined) ? m_descriptorLayout : originalLayout;
      transitionLayout(cmdBuffer, vk::ImageLayout::eTransferSrcOptimal, restoreLayout, effectiveAspect);
    },
    "texture_download");

  void* mapped = stagingBuffer->map(0, size);
  std::memcpy(data, mapped, size);
  stagingBuffer->unmap();
}

void ZVulkanTexture::downloadSubImage(void* data,
                                      size_t size,
                                      vk::Offset3D offset,
                                      vk::Extent3D extent,
                                      vk::ImageAspectFlags aspectMask)
{
  if (!data || size == 0) {
    throw ZException("Invalid download buffer (subimage)");
  }

  auto stagingBuffer =
    m_device.createBuffer(size,
                          vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

  auto originalLayout = m_currentLayout;
  auto aspect = (aspectMask == vk::ImageAspectFlags{}) ? m_aspectMask : aspectMask;
  const auto depthStencilMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
  if ((aspect & depthStencilMask) == depthStencilMask) {
    aspect = vk::ImageAspectFlagBits::eDepth;
  }
  m_device.frameExecutor().executeImmediate(
    [&](vk::raii::CommandBuffer& cmdBuffer) {
      transitionLayout(cmdBuffer, originalLayout, vk::ImageLayout::eTransferSrcOptimal, aspect);

      vk::BufferImageCopy region{};
      region.bufferOffset = 0;
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource.aspectMask = aspect;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = 0;
      region.imageSubresource.layerCount = 1;
      region.imageOffset = offset;
      region.imageExtent = extent;

      cmdBuffer.copyImageToBuffer(m_image, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer->buffer(), region);
      const vk::ImageLayout restoreLayout =
        (originalLayout == vk::ImageLayout::eUndefined) ? m_descriptorLayout : originalLayout;
      transitionLayout(cmdBuffer, vk::ImageLayout::eTransferSrcOptimal, restoreLayout, aspect);
    },
    "texture_download_subimage");

  void* mapped = stagingBuffer->map(0, size);
  std::memcpy(data, mapped, size);
  stagingBuffer->unmap();
}

void ZVulkanTexture::downloadArrayLayer(void* data, size_t size, uint32_t arrayLayer, vk::ImageAspectFlags aspectMask)
{
  if (!data || size == 0) {
    throw ZException("Invalid download buffer (array layer)");
  }

  auto stagingBuffer =
    m_device.createBuffer(size,
                          vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

  auto originalLayout = m_currentLayout;
  auto aspect = (aspectMask == vk::ImageAspectFlags{}) ? m_aspectMask : aspectMask;
  const auto depthStencilMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
  if ((aspect & depthStencilMask) == depthStencilMask) {
    aspect = vk::ImageAspectFlagBits::eDepth;
  }
  m_device.frameExecutor().executeImmediate(
    [&](vk::raii::CommandBuffer& cmdBuffer) {
      transitionLayout(cmdBuffer, originalLayout, vk::ImageLayout::eTransferSrcOptimal, aspect);

      vk::BufferImageCopy region{};
      region.bufferOffset = 0;
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource.aspectMask = aspect;
      region.imageSubresource.mipLevel = 0;
      const uint32_t availableLayers = std::max<uint32_t>(1u, m_arrayLayers);
      region.imageSubresource.baseArrayLayer =
        (m_createInfo.imageType == vk::ImageType::e3D) ? 0u : std::min(arrayLayer, availableLayers - 1u);
      region.imageSubresource.layerCount = 1;
      region.imageOffset = vk::Offset3D{0, 0, 0};
      region.imageExtent = vk::Extent3D{m_extent.width, m_extent.height, 1u};

      cmdBuffer.copyImageToBuffer(m_image, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer->buffer(), region);
      const vk::ImageLayout restoreLayout =
        (originalLayout == vk::ImageLayout::eUndefined) ? m_descriptorLayout : originalLayout;
      transitionLayout(cmdBuffer, vk::ImageLayout::eTransferSrcOptimal, restoreLayout, aspect);
    },
    "texture_download_layer");

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
  barrier.image = m_image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = m_mipLevels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = m_arrayLayers;

  VLOG(2) << "VK layout transition image=" << static_cast<void*>(m_image) << " old=" << enumOrUnderlying(oldLayout)
          << " new=" << enumOrUnderlying(newLayout) << " aspect=0x" << std::hex << static_cast<uint32_t>(aspect)
          << std::dec << " layers=" << m_arrayLayers << " mips=" << m_mipLevels;

  cmdBuffer.pipelineBarrier(srcState.stage, dstState.stage, {}, nullptr, nullptr, barrier);
  // Update tracked layout regardless of aspect selection; callers often
  // transition a single aspect on D/S images, and we still need an accurate
  // logical current layout for validation and descriptor restores.
  m_currentLayout = newLayout;
}

vk::DescriptorImageInfo ZVulkanTexture::descriptorInfo() const
{
  return descriptorInfo(m_descriptorLayout, m_descriptorAspectMask);
}

vk::DescriptorImageInfo ZVulkanTexture::descriptorInfo(vk::ImageLayout layoutOverride,
                                                       vk::ImageAspectFlags aspectOverride) const
{
  if (!m_imageView) {
    throw ZException("Descriptor info requested before image view creation");
  }

  const vk::ImageAspectFlags resolvedAspect =
    (aspectOverride == vk::ImageAspectFlags{}) ? m_descriptorAspectMask : aspectOverride;

  vk::DescriptorImageInfo info{};
  info.imageView = imageViewForAspect(resolvedAspect);
  info.imageLayout = layoutOverride;
  if (info.imageLayout == vk::ImageLayout::eUndefined) {
    info.imageLayout = m_descriptorLayout;
  }
  info.sampler = m_sampler ? **m_sampler : vk::Sampler{};
  return info;
}

void ZVulkanTexture::setDescriptorLayout(vk::ImageLayout layout)
{
  m_descriptorLayout = layout;

  switch (layout) {
    case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
      // When sampling a depth/stencil image in read-only, Vulkan requires the
      // image view aspect mask to be either DEPTH or STENCIL (not both).
      // Default to DEPTH for descriptors; callers that need stencil should
      // override via setDescriptorAspect() or descriptorInfo(..., STENCIL).
      setDescriptorAspect(vk::ImageAspectFlagBits::eDepth);
      break;
    case vk::ImageLayout::eDepthReadOnlyOptimal:
      setDescriptorAspect(vk::ImageAspectFlagBits::eDepth);
      break;
    case vk::ImageLayout::eStencilReadOnlyOptimal:
      setDescriptorAspect(vk::ImageAspectFlagBits::eStencil);
      break;
    default:
      setDescriptorAspect(vk::ImageAspectFlags{});
      break;
  }
}

void ZVulkanTexture::setDescriptorAspect(vk::ImageAspectFlags aspect)
{
  if (aspect == vk::ImageAspectFlags{} || (aspect & m_aspectMask) != aspect) {
    m_descriptorAspectMask = m_aspectMask;
  } else {
    m_descriptorAspectMask = aspect;
  }
}

vk::Sampler ZVulkanTexture::sampler() const
{
  return m_sampler ? **m_sampler : vk::Sampler{};
}

vk::ImageView ZVulkanTexture::layerImageView(uint32_t layer, vk::ImageAspectFlags aspect) const
{
  if (m_arrayLayers <= 1u) {
    return imageViewForAspect(aspect);
  }
  if (!m_image || !m_imageView || layer >= m_arrayLayers) {
    return vk::ImageView{};
  }

  const vk::ImageAspectFlags resolvedAspect = (aspect == vk::ImageAspectFlags{}) ? m_descriptorAspectMask : aspect;

  auto ensureCacheSized = [&](auto& cache) {
    if (cache.empty()) {
      cache.resize(m_arrayLayers);
    }
  };

  auto& cache = [&]() -> std::vector<std::optional<vk::raii::ImageView>>& {
    if (resolvedAspect == vk::ImageAspectFlagBits::eDepth) {
      ensureCacheSized(m_layerDepthViews);
      return m_layerDepthViews;
    }
    if (resolvedAspect == vk::ImageAspectFlagBits::eStencil) {
      ensureCacheSized(m_layerStencilViews);
      return m_layerStencilViews;
    }
    ensureCacheSized(m_layerImageViews);
    return m_layerImageViews;
  }();

  if (!cache[layer].has_value()) {
    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = m_image;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = m_format;
    viewInfo.components = vk::ComponentMapping{};
    vk::ImageAspectFlags subresourceAspect =
      (resolvedAspect == vk::ImageAspectFlags{}) ? m_descriptorAspectMask : resolvedAspect;
    if (subresourceAspect == vk::ImageAspectFlags{}) {
      subresourceAspect = m_aspectMask;
    }
    viewInfo.subresourceRange.aspectMask = subresourceAspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = layer;
    viewInfo.subresourceRange.layerCount = 1;
    cache[layer].emplace(m_device.context().device(), viewInfo);
  }
  return **cache[layer];
}

vk::ImageView ZVulkanTexture::imageViewForAspect(vk::ImageAspectFlags aspect) const
{
  if (!m_imageView) {
    return vk::ImageView{};
  }

  vk::ImageAspectFlags resolvedAspect = (aspect == vk::ImageAspectFlags{}) ? m_descriptorAspectMask : aspect;
  if (resolvedAspect == vk::ImageAspectFlags{}) {
    resolvedAspect = m_aspectMask;
  }

  if (resolvedAspect == m_aspectMask) {
    return **m_imageView;
  }

  if ((resolvedAspect & m_aspectMask) != resolvedAspect) {
    // Requested aspect not compatible; fall back to default view.
    return **m_imageView;
  }

  auto buildView = [&](vk::ImageAspectFlags aspectMask) {
    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = m_image;
    viewInfo.viewType = m_createInfo.viewType;
    viewInfo.format = m_format;
    viewInfo.components = vk::ComponentMapping{};
    viewInfo.subresourceRange.aspectMask = aspectMask;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = m_arrayLayers;
    return vk::raii::ImageView(m_device.context().device(), viewInfo);
  };

  if (resolvedAspect == vk::ImageAspectFlagBits::eDepth) {
    if (!m_depthAspectView.has_value()) {
      m_depthAspectView.emplace(buildView(resolvedAspect));
    }
    return **m_depthAspectView;
  }

  if (resolvedAspect == vk::ImageAspectFlagBits::eStencil) {
    if (!m_stencilAspectView.has_value()) {
      m_stencilAspectView.emplace(buildView(resolvedAspect));
    }
    return **m_stencilAspectView;
  }

  // Fallback: rebuild view for requested aspect on the fly.
  const uint32_t aspectKey = static_cast<uint32_t>(resolvedAspect);
  auto iter = m_genericAspectViews.find(aspectKey);
  if (iter == m_genericAspectViews.end()) {
    iter = m_genericAspectViews.emplace(aspectKey, buildView(resolvedAspect)).first;
  }
  return *iter->second;
}

bool ZVulkanTexture::saveToImage(const QString& filename)
{
  return saveToImage(filename, ImageSaveOptions{});
}

bool ZVulkanTexture::saveToImage(const QString& filename, const ImageSaveOptions& options)
{
  if (filename.isEmpty()) {
    LOG(ERROR) << "ZVulkanTexture::saveToImage called with empty filename";
    return false;
  }

  const uint32_t w = width();
  const uint32_t h = height();
  if (w == 0 || h == 0) {
    LOG(ERROR) << "ZVulkanTexture::saveToImage received zero-sized texture";
    return false;
  }

  const uint32_t arrayLayerCount = std::max<uint32_t>(1u, m_arrayLayers);
  const uint32_t requestedLayer = options.arrayLayer.value_or(0u);
  if (requestedLayer >= arrayLayerCount) {
    LOG(ERROR) << "ZVulkanTexture::saveToImage layer out of range: " << requestedLayer << " (layers=" << arrayLayerCount
               << ")";
    return false;
  }

  const size_t pixels = static_cast<size_t>(w) * h;
  auto aspect = options.aspectMask == vk::ImageAspectFlags{} ? m_aspectMask : options.aspectMask;
  if (aspect == vk::ImageAspectFlags{}) {
    aspect = defaultAspectMask(m_format);
  }

  auto saveColorImage = [&](auto& buffer, size_t channels) -> bool {
    ZImg src;
    src.wrapData(buffer.data(), w, h, 1, channels);
    try {
      if (channels > 1) {
        ZImg converted(src.info());
        ZImgFormat::CXYZtoXYZC(src, converted);
        if (options.flipY) {
          converted.flip(Dimension::Y);
        }
        converted.save(filename);
        return true;
      }
      if (options.flipY) {
        src.flip(Dimension::Y);
      }
      src.save(filename);
    }
    catch (const ZException& ze) {
      LOG(ERROR) << "ZVulkanTexture::saveToImage save failed: " << ze.what();
      return false;
    }
    return true;
  };

  auto saveScalarImage = [&](auto& buffer) -> bool {
    using ValueType = typename std::remove_reference_t<decltype(buffer)>::value_type;
    (void)sizeof(ValueType);
    ZImg img;
    img.wrapData(buffer.data(), w, h, 1);
    try {
      if (options.flipY) {
        img.flip(Dimension::Y);
      }
      img.save(filename);
    }
    catch (const ZException& ze) {
      LOG(ERROR) << "ZVulkanTexture::saveToImage save failed: " << ze.what();
      return false;
    }
    return true;
  };

  auto saveFromUint8 = [&](size_t channels, uint32_t layerIndex) -> bool {
    std::vector<uint8_t> data(pixels * channels);
    downloadArrayLayer(data.data(), data.size(), layerIndex, aspect);
    return saveColorImage(data, channels);
  };

  auto saveFromUint16 = [&](size_t channels, uint32_t layerIndex) -> bool {
    std::vector<uint16_t> data(pixels * channels);
    downloadArrayLayer(data.data(), data.size() * sizeof(uint16_t), layerIndex, aspect);
    return saveColorImage(data, channels);
  };

  auto saveFromFloat = [&](size_t channels, uint32_t layerIndex) -> bool {
    std::vector<float> data(pixels * channels);
    downloadArrayLayer(data.data(), data.size() * sizeof(float), layerIndex, aspect);
    return saveColorImage(data, channels);
  };

  auto saveFromHalf = [&](size_t channels, uint32_t layerIndex) -> bool {
    std::vector<uint16_t> halfData(pixels * channels);
    downloadArrayLayer(halfData.data(), halfData.size() * sizeof(uint16_t), layerIndex, aspect);
    std::vector<float> floatData(halfData.size());
    for (size_t i = 0; i < halfData.size(); ++i) {
      floatData[i] = glm::unpackHalf1x16(halfData[i]);
    }
    return saveColorImage(floatData, channels);
  };

  auto saveFromDouble = [&](size_t channels, uint32_t layerIndex) -> bool {
    std::vector<double> data(pixels * channels);
    downloadArrayLayer(data.data(), data.size() * sizeof(double), layerIndex, aspect);
    return saveColorImage(data, channels);
  };

  const auto layer = requestedLayer;
  try {
    switch (m_format) {
      case vk::Format::eR8G8B8A8Unorm:
      case vk::Format::eR8G8B8A8Srgb:
      case vk::Format::eR8G8B8A8Snorm:
      case vk::Format::eB8G8R8A8Unorm:
      case vk::Format::eB8G8R8A8Srgb: {
        if (!saveFromUint8(4u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR8G8B8Unorm:
      case vk::Format::eR8G8B8Srgb:
      case vk::Format::eR8G8B8Snorm:
      case vk::Format::eB8G8R8Unorm:
      case vk::Format::eB8G8R8Srgb: {
        if (!saveFromUint8(3u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR8G8Unorm:
      case vk::Format::eR8G8Snorm:
      case vk::Format::eR8G8Srgb: {
        if (!saveFromUint8(2u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR16G16B16A16Unorm:
      case vk::Format::eR16G16B16A16Snorm: {
        if (!saveFromUint16(4u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR16G16B16Unorm:
      case vk::Format::eR16G16B16Snorm: {
        if (!saveFromUint16(3u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR16G16Unorm:
      case vk::Format::eR16G16Snorm: {
        if (!saveFromUint16(2u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR32G32B32A32Sfloat: {
        if (!saveFromFloat(4u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR32G32B32Sfloat: {
        if (!saveFromFloat(3u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR32G32Sfloat: {
        if (!saveFromFloat(2u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR16G16B16A16Sfloat: {
        if (!saveFromHalf(4u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR16G16B16Sfloat: {
        if (!saveFromHalf(3u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR16G16Sfloat: {
        if (!saveFromHalf(2u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR64G64B64A64Sfloat: {
        if (!saveFromDouble(4u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR64G64B64Sfloat: {
        if (!saveFromDouble(3u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eR64G64Sfloat: {
        if (!saveFromDouble(2u, layer)) {
          return false;
        }
        break;
      }
      case vk::Format::eD32Sfloat:
      case vk::Format::eD16Unorm: {
        std::vector<float> depth(pixels);
        if (m_format == vk::Format::eD32Sfloat) {
          downloadArrayLayer(depth.data(), depth.size() * sizeof(float), layer, aspect);
        } else {
          std::vector<uint16_t> raw(pixels);
          downloadArrayLayer(raw.data(), raw.size() * sizeof(uint16_t), layer, aspect);
          constexpr float denom = 65535.0f;
          for (size_t i = 0; i < pixels; ++i) {
            depth[i] = static_cast<float>(raw[i]) / denom;
          }
        }
        if (!saveScalarImage(depth)) {
          return false;
        }
        break;
      }
      case vk::Format::eD32SfloatS8Uint: {
        std::vector<float> depth(pixels);
        downloadArrayLayer(depth.data(), depth.size() * sizeof(float), layer, aspect);
        if (!saveScalarImage(depth)) {
          return false;
        }
        break;
      }
      case vk::Format::eD24UnormS8Uint:
      case vk::Format::eX8D24UnormPack32: {
        std::vector<uint32_t> raw(pixels);
        downloadArrayLayer(raw.data(), raw.size() * sizeof(uint32_t), layer, aspect);
        std::vector<float> depth(pixels);
        constexpr float denom = 16777215.0f;
        for (size_t i = 0; i < pixels; ++i) {
          const uint32_t value = raw[i] & 0x00FFFFFFu;
          depth[i] = static_cast<float>(value) / denom;
        }
        if (!saveScalarImage(depth)) {
          return false;
        }
        break;
      }
      default:
        LOG(ERROR) << "ZVulkanTexture::saveToImage unsupported format: " << enumOrUnderlying(m_format, 16);
        return false;
    }
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "ZVulkanTexture::saveToImage download failed: " << e.what();
    return false;
  }

  if (arrayLayerCount > 1 || options.arrayLayer.has_value()) {
    LOG(INFO) << "Saved Vulkan texture layer " << layer << " (" << w << "x" << h
              << ") to image: " << filename.toStdString();
  } else {
    LOG(INFO) << "Saved Vulkan texture (" << w << "x" << h << ") to image: " << filename.toStdString();
  }
  return true;
}

// ----- Private helpers ------------------------------------------------------------------------

void ZVulkanTexture::createImage()
{
  // No-op: image creation is handled by VMA in allocateMemory().
}

void ZVulkanTexture::allocateMemory()
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

  VmaAllocationCreateInfo allocCI{};
  // Prefer usage-based auto selection
  allocCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (m_memoryProperties & vk::MemoryPropertyFlagBits::eDeviceLocal) {
    allocCI.requiredFlags |= static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eDeviceLocal);
  }
  if (m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible) {
    allocCI.requiredFlags |= static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eHostVisible);
  }
  if (m_memoryProperties & vk::MemoryPropertyFlagBits::eHostCoherent) {
    allocCI.requiredFlags |= static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  // Prefer device-local pool for images
  // Choose pool
  VmaPool pool = nullptr;
  if ((m_memoryProperties & vk::MemoryPropertyFlagBits::eDeviceLocal) && m_device.deviceLocalPool() != nullptr) {
    pool = m_device.deviceLocalPool();
  }

  auto tryCreate = [&](VmaPool p, bool dedicated) -> VkResult {
    VmaAllocationCreateInfo ci = allocCI;
    ci.pool = p;
    if (dedicated) {
      ci.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
      ci.pool = nullptr;
    }
    VkImage img{};
    VkResult r = vmaCreateImage(m_device.allocator(),
                                reinterpret_cast<const VkImageCreateInfo*>(&imageInfo),
                                &ci,
                                &img,
                                &m_imageAllocation,
                                nullptr);
    if (static_cast<vk::Result>(r) == vk::Result::eSuccess) {
      m_image = img;
    }
    return r;
  };

  // Heuristic: prefer dedicated for large images
  const VkDeviceSize approxSize = static_cast<VkDeviceSize>(m_extent.width) * m_extent.height *
                                  std::max<VkDeviceSize>(1, m_extent.depth) * 4; // 4B/px guess
  bool preferDedicated = (approxSize >= (128ull * 1024ull * 1024ull) / 2); // >=64MiB

  vk::Result res = vk::Result::eErrorInitializationFailed;
  if (preferDedicated) {
    res = static_cast<vk::Result>(tryCreate(nullptr, true));
  } else if (pool != nullptr) {
    res = static_cast<vk::Result>(tryCreate(pool, false));
  }
  if (res != vk::Result::eSuccess) {
    if (!preferDedicated) {
      res = static_cast<vk::Result>(tryCreate(nullptr, true));
    }
    if (res != vk::Result::eSuccess) {
      res = static_cast<vk::Result>(tryCreate(nullptr, false));
    }
  }
  if (res != vk::Result::eSuccess) {
    throw ZException("Failed to create VMA image (with fallbacks)");
  }
}

void ZVulkanTexture::createImageView()
{
  vk::ImageViewCreateInfo viewInfo{};
  viewInfo.image = m_image;
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
  samplerInfo.anisotropyEnable = false;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.compareEnable = false;
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
  const uint32_t depth = m_createInfo.imageType == vk::ImageType::e3D ? std::max(1u, m_extent.depth >> mipLevel) : 1u;
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
    if (region.baseArrayLayer >= m_arrayLayers || region.baseArrayLayer + region.layerCount > m_arrayLayers) {
      throw ZException("Upload region exceeds array layer bounds");
    }
  }

  auto stagingBuffer =
    m_device.createBuffer(size,
                          vk::BufferUsageFlagBits::eTransferSrc,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  stagingBuffer->copyData(data, size);

  const vk::ImageLayout finalLayout = region.finalLayout;
  m_device.frameExecutor().executeImmediate(
    [&](vk::raii::CommandBuffer& cmdBuffer) {
      transitionLayout(cmdBuffer, m_currentLayout, vk::ImageLayout::eTransferDstOptimal, m_aspectMask);

      vk::BufferImageCopy copyRegion{};
      copyRegion.bufferOffset = 0;
      copyRegion.bufferRowLength = region.bufferRowLength;
      copyRegion.bufferImageHeight = region.bufferImageHeight;
      copyRegion.imageSubresource.aspectMask = m_aspectMask;
      copyRegion.imageSubresource.mipLevel = region.mipLevel;
      copyRegion.imageSubresource.baseArrayLayer =
        m_createInfo.imageType == vk::ImageType::e3D ? 0u : region.baseArrayLayer;
      copyRegion.imageSubresource.layerCount = m_createInfo.imageType == vk::ImageType::e3D ? 1u : region.layerCount;
      copyRegion.imageOffset = region.offset;
      copyRegion.imageExtent = region.extent;

      cmdBuffer.copyBufferToImage(stagingBuffer->buffer(), m_image, vk::ImageLayout::eTransferDstOptimal, copyRegion);

      transitionLayout(cmdBuffer, vk::ImageLayout::eTransferDstOptimal, finalLayout, m_aspectMask);
    },
    "texture_upload");

  if (finalLayout != vk::ImageLayout::eUndefined && finalLayout != vk::ImageLayout::eTransferDstOptimal) {
    m_descriptorLayout = finalLayout;
  }
}

} // namespace nim
