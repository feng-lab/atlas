#pragma once

#include "zvulkan.h"
#include <memory>
#include <optional>
#include <vector>
#include <cstddef>

// Stage 1 prototype helper focused on self-contained texture management.

namespace nim {

class ZVulkanDevice;

class ZVulkanTexture
{
public:
  struct CreateInfo
  {
    vk::ImageType imageType = vk::ImageType::e2D;
    vk::ImageViewType viewType = vk::ImageViewType::e2D;
    vk::Extent3D extent{1u, 1u, 1u};
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    uint32_t mipLevels = 1u;
    uint32_t arrayLayers = 1u;
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
    vk::ImageUsageFlags usage =
      vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
    vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor;
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    vk::ImageLayout initialLayout = vk::ImageLayout::eUndefined;
    vk::ImageLayout descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    bool createDefaultSampler = true;
    std::optional<vk::SamplerCreateInfo> samplerInfo = std::nullopt;

    static CreateInfo make1D(uint32_t width,
                             vk::Format format,
                             vk::ImageUsageFlags usage =
                               vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eTransferDst,
                             vk::MemoryPropertyFlags memoryProperties =
                               vk::MemoryPropertyFlagBits::eDeviceLocal,
                             uint32_t mipLevels = 1u,
                             bool createSampler = true,
                             vk::ImageLayout descriptorLayout =
                               vk::ImageLayout::eShaderReadOnlyOptimal);

    static CreateInfo make2D(uint32_t width,
                             uint32_t height,
                             vk::Format format,
                             vk::ImageUsageFlags usage =
                               vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eTransferDst,
                             vk::MemoryPropertyFlags memoryProperties =
                               vk::MemoryPropertyFlagBits::eDeviceLocal,
                             uint32_t mipLevels = 1u,
                             bool createSampler = true,
                             vk::ImageLayout descriptorLayout =
                               vk::ImageLayout::eShaderReadOnlyOptimal);

    static CreateInfo make2DArray(uint32_t width,
                                  uint32_t height,
                                  uint32_t arrayLayers,
                                  vk::Format format,
                                  vk::ImageUsageFlags usage =
                                    vk::ImageUsageFlagBits::eSampled |
                                    vk::ImageUsageFlagBits::eTransferDst,
                                  vk::MemoryPropertyFlags memoryProperties =
                                    vk::MemoryPropertyFlagBits::eDeviceLocal,
                                  uint32_t mipLevels = 1u,
                                  bool createSampler = true,
                                  vk::ImageLayout descriptorLayout =
                                    vk::ImageLayout::eShaderReadOnlyOptimal);

    static CreateInfo make3D(uint32_t width,
                             uint32_t height,
                             uint32_t depth,
                             vk::Format format,
                             vk::ImageUsageFlags usage =
                               vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eTransferDst,
                             vk::MemoryPropertyFlags memoryProperties =
                               vk::MemoryPropertyFlagBits::eDeviceLocal,
                             uint32_t mipLevels = 1u,
                             bool createSampler = true,
                             vk::ImageLayout descriptorLayout =
                               vk::ImageLayout::eShaderReadOnlyOptimal);

    static CreateInfo makeCube(uint32_t edgeLength,
                               vk::Format format,
                               uint32_t mipLevels = 1u,
                               uint32_t cubeCount = 1u,
                               vk::ImageUsageFlags usage =
                                 vk::ImageUsageFlagBits::eSampled |
                                 vk::ImageUsageFlagBits::eTransferDst,
                               vk::MemoryPropertyFlags memoryProperties =
                                 vk::MemoryPropertyFlagBits::eDeviceLocal,
                               bool createSampler = true,
                               vk::ImageLayout descriptorLayout =
                                 vk::ImageLayout::eShaderReadOnlyOptimal);
  };

  struct UploadRegion
  {
    vk::Offset3D offset{0, 0, 0};
    vk::Extent3D extent{0, 0, 0};
    uint32_t mipLevel = 0u;
    uint32_t baseArrayLayer = 0u;
    uint32_t layerCount = 0u; // 0 => fill remaining array layers
    uint32_t bufferRowLength = 0u;
    uint32_t bufferImageHeight = 0u;
    vk::ImageLayout finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  };

  ZVulkanTexture(ZVulkanDevice& device, const CreateInfo& createInfo);
  ZVulkanTexture(ZVulkanDevice& device, uint32_t width, uint32_t height, vk::Format format);
  ZVulkanTexture(ZVulkanDevice& device,
                 uint32_t width,
                 uint32_t height,
                 vk::Format format,
                 vk::ImageUsageFlags usage,
                 vk::MemoryPropertyFlags memoryProperties);
  ~ZVulkanTexture();

  void uploadData(const void* data, size_t size);
  void uploadData(const void* data, size_t size, vk::ImageLayout finalLayout);
  void uploadSubImage(const void* data, size_t size, const UploadRegion& region);
  void downloadData(void* data, size_t size);
  void downloadSubImage(void* data,
                        size_t size,
                        vk::Offset3D offset,
                        vk::Extent3D extent,
                        vk::ImageAspectFlags aspectMask = {});
  void transitionLayout(vk::raii::CommandBuffer& cmdBuffer,
                        vk::ImageLayout oldLayout,
                        vk::ImageLayout newLayout,
                        vk::ImageAspectFlags aspectMask = {});

  vk::DescriptorImageInfo descriptorInfo() const;
  void setDescriptorLayout(vk::ImageLayout layout);

  uint32_t width() const
  {
    return m_extent.width;
  }
  uint32_t height() const
  {
    return m_extent.height;
  }
  uint32_t depth() const
  {
    return m_extent.depth;
  }
  vk::Extent3D extent() const
  {
    return m_extent;
  }
  vk::Format format() const
  {
    return m_format;
  }
  vk::Image image() const
  {
    return *m_image;
  }
  vk::ImageView imageView() const
  {
    return *m_imageView;
  }
  vk::ImageLayout layout() const
  {
    return m_currentLayout;
  }
  vk::Sampler sampler() const;
  vk::ImageView layerImageView(uint32_t layer) const;
  uint32_t mipLevels() const
  {
    return m_mipLevels;
  }
  uint32_t arrayLayers() const
  {
    return m_arrayLayers;
  }
  const CreateInfo& info() const
  {
    return m_createInfo;
  }

  // Access owning device for validation/assertions
  ZVulkanDevice& ownerDevice() const { return m_device; }

private:
  void createImage();
  void allocateMemory();
  void createImageView();
  void createSampler();
  vk::Extent3D mipExtent(uint32_t mipLevel) const;
  void uploadInternal(const void* data, size_t size, const UploadRegion& region);

  ZVulkanDevice& m_device;
  CreateInfo m_createInfo;
  vk::Extent3D m_extent;
  vk::Format m_format;
  uint32_t m_mipLevels;
  uint32_t m_arrayLayers;
  vk::ImageUsageFlags m_usage;
  vk::MemoryPropertyFlags m_memoryProperties;
  vk::ImageAspectFlags m_aspectMask;
  vk::ImageLayout m_descriptorLayout;
  std::optional<vk::raii::Image> m_image;
  std::optional<vk::raii::DeviceMemory> m_imageMemory;
  std::optional<vk::raii::ImageView> m_imageView;
  std::optional<vk::raii::Sampler> m_sampler;
  mutable std::vector<std::optional<vk::raii::ImageView>> m_layerImageViews;
  vk::ImageLayout m_currentLayout = vk::ImageLayout::eUndefined;
};

} // namespace nim
