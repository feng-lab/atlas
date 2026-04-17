#pragma once

#include "zvulkan.h"
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include <cstddef>
#include <cstdint>

class QString;

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
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
    vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor;
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    vk::ImageLayout initialLayout = vk::ImageLayout::eUndefined;
    vk::ImageLayout descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    bool createDefaultSampler = true;
    std::optional<vk::SamplerCreateInfo> samplerInfo = std::nullopt;

    static CreateInfo make1D(uint32_t width,
                             vk::Format format,
                             vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled |
                                                         vk::ImageUsageFlagBits::eTransferDst,
                             vk::MemoryPropertyFlags memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal,
                             uint32_t mipLevels = 1u,
                             bool createSampler = true,
                             vk::ImageLayout descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal);

    static CreateInfo make2D(uint32_t width,
                             uint32_t height,
                             vk::Format format,
                             vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled |
                                                         vk::ImageUsageFlagBits::eTransferDst,
                             vk::MemoryPropertyFlags memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal,
                             uint32_t mipLevels = 1u,
                             bool createSampler = true,
                             vk::ImageLayout descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal);

    static CreateInfo make2DArray(uint32_t width,
                                  uint32_t height,
                                  uint32_t arrayLayers,
                                  vk::Format format,
                                  vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled |
                                                              vk::ImageUsageFlagBits::eTransferDst,
                                  vk::MemoryPropertyFlags memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal,
                                  uint32_t mipLevels = 1u,
                                  bool createSampler = true,
                                  vk::ImageLayout descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal);

    static CreateInfo make3D(uint32_t width,
                             uint32_t height,
                             uint32_t depth,
                             vk::Format format,
                             vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled |
                                                         vk::ImageUsageFlagBits::eTransferDst,
                             vk::MemoryPropertyFlags memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal,
                             uint32_t mipLevels = 1u,
                             bool createSampler = true,
                             vk::ImageLayout descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal);

    static CreateInfo makeCube(uint32_t edgeLength,
                               vk::Format format,
                               uint32_t mipLevels = 1u,
                               uint32_t cubeCount = 1u,
                               vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled |
                                                           vk::ImageUsageFlagBits::eTransferDst,
                               vk::MemoryPropertyFlags memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal,
                               bool createSampler = true,
                               vk::ImageLayout descriptorLayout = vk::ImageLayout::eShaderReadOnlyOptimal);
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
    // Must be specified by callers; Vulkan does not infer post-upload layouts.
    vk::ImageLayout finalLayout = vk::ImageLayout::eUndefined;
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

  void uploadData(const void* data, size_t size, vk::ImageLayout finalLayout);
  void uploadSubImage(const void* data, size_t size, const UploadRegion& region);
  void downloadData(void* data, size_t size);
  void downloadSubImage(void* data,
                        size_t size,
                        vk::Offset3D offset,
                        vk::Extent3D extent,
                        vk::ImageAspectFlags aspectMask = {});
  // Download a single array layer (for 2D array images). The buffer size must be
  // width*height*pixelStride bytes for color images (depth=1). For 3D images this
  // behaves like downloadSubImage with baseArrayLayer=0.
  void downloadArrayLayer(void* data, size_t size, uint32_t arrayLayer, vk::ImageAspectFlags aspectMask = {});
  void transitionLayout(vk::raii::CommandBuffer& cmdBuffer,
                        vk::ImageLayout oldLayout,
                        vk::ImageLayout newLayout,
                        vk::ImageAspectFlags aspectMask = {});
  void overrideCurrentLayout(vk::ImageLayout layout);

  // ---------------------------------------------------------------------------
  // Residency / allocation helpers
  // ---------------------------------------------------------------------------
  // Some subsystems (e.g., large paging caches) may temporarily evict device-local
  // images under memory pressure and later recreate them. These helpers allow
  // doing so while keeping the ZVulkanTexture object identity stable.
  [[nodiscard]] bool resident() const
  {
    return m_image != vk::Image{};
  }

  // Monotonically increasing counter that changes every time the underlying
  // VkImage is (re)created. Consumers that cache descriptor sets can use this
  // to detect when an image view handle is no longer valid.
  [[nodiscard]] uint64_t imageGeneration() const
  {
    return m_imageGeneration;
  }

  // Best-effort: returns the VMA allocation size when resident, otherwise 0.
  [[nodiscard]] uint64_t allocationSizeBytes() const;

  // Release the underlying VkImage + view, freeing device memory via VMA.
  // After this call, the texture is non-resident and cannot be used for
  // descriptors or layout transitions until recreateDeviceResources().
  void releaseDeviceResources();

  // Recreate the underlying VkImage + view using the original CreateInfo.
  // This increments imageGeneration(). Callers are responsible for reinitializing
  // image contents (upload/clear) as needed.
  void recreateDeviceResources();

  // Change the create-info used by later recreateDeviceResources() calls while
  // preserving this ZVulkanTexture object's identity. The texture must already
  // be non-resident so callers can batch metadata changes across related images
  // before allocating any new backing VkImage memory.
  void resetNonResidentCreateInfo(const CreateInfo& createInfo);

  vk::DescriptorImageInfo descriptorInfo() const;
  vk::DescriptorImageInfo descriptorInfo(vk::ImageLayout layoutOverride, vk::ImageAspectFlags aspectOverride) const;
  void setDescriptorLayout(vk::ImageLayout layout);
  void setDescriptorAspect(vk::ImageAspectFlags aspect);
  vk::ImageLayout descriptorLayout() const
  {
    return m_descriptorLayout;
  }
  vk::ImageAspectFlags descriptorAspect() const
  {
    return m_descriptorAspectMask;
  }

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
  vk::ImageAspectFlags aspectMask() const
  {
    return m_aspectMask;
  }
  vk::Image image() const
  {
    return m_image;
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
  vk::ImageView layerImageView(uint32_t layer, vk::ImageAspectFlags aspect = {}) const;
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
  ZVulkanDevice& ownerDevice() const
  {
    return m_device;
  }

  struct ImageSaveOptions
  {
    std::optional<uint32_t> arrayLayer;
    vk::ImageAspectFlags aspectMask = {};
    bool flipY = true;
  };

  bool saveToImage(const QString& filename, const ImageSaveOptions& options);
  bool saveToImage(const QString& filename);

  // Save an image file from an already-captured GPU->CPU readback buffer.
  //
  // The input buffer is expected to be the tightly-packed output of
  // vkCmdCopyImageToBuffer for the given `format`/`width`/`height`.
  // This performs the same format conversions as saveToImage() (including
  // depth normalization for depth formats), but does not touch Vulkan.
  static bool saveReadbackToImage(const QString& filename,
                                  vk::Format format,
                                  uint32_t width,
                                  uint32_t height,
                                  const void* data,
                                  size_t bytes,
                                  bool flipY = true);

private:
  void createImage();
  void allocateMemory();
  void createImageView();
  void createSampler();
  vk::Extent3D mipExtent(uint32_t mipLevel) const;
  void uploadInternal(const void* data, size_t size, const UploadRegion& region);
  vk::ImageView imageViewForAspect(vk::ImageAspectFlags aspect) const;

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
  vk::ImageAspectFlags m_descriptorAspectMask;
  vk::Image m_image{};
  VmaAllocation m_imageAllocation = nullptr;
  std::optional<vk::raii::ImageView> m_imageView;
  std::optional<vk::raii::Sampler> m_sampler;
  mutable std::optional<vk::raii::ImageView> m_depthAspectView;
  mutable std::optional<vk::raii::ImageView> m_stencilAspectView;
  mutable std::vector<std::optional<vk::raii::ImageView>> m_layerImageViews;
  mutable std::vector<std::optional<vk::raii::ImageView>> m_layerDepthViews;
  mutable std::vector<std::optional<vk::raii::ImageView>> m_layerStencilViews;
  mutable std::unordered_map<uint32_t, vk::raii::ImageView> m_genericAspectViews;
  vk::ImageLayout m_currentLayout = vk::ImageLayout::eUndefined;
  uint64_t m_imageGeneration = 0;
};

} // namespace nim
