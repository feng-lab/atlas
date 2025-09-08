#pragma once

#include "zvulkan.h"
#include <memory>
#include <optional>

namespace nim {

class ZVulkanDevice;

class ZVulkanTexture
{
public:
  ZVulkanTexture(ZVulkanDevice& device, uint32_t width, uint32_t height, vk::Format format);
  ZVulkanTexture(ZVulkanDevice& device,
                 uint32_t width,
                 uint32_t height,
                 vk::Format format,
                 vk::ImageUsageFlags usage,
                 vk::MemoryPropertyFlags memoryProperties);
  ~ZVulkanTexture();

  void uploadData(const void* data, size_t size);
  void downloadData(void* data, size_t size);
  void transitionLayout(vk::raii::CommandBuffer& cmdBuffer, vk::ImageLayout oldLayout, vk::ImageLayout newLayout);

  uint32_t width() const
  {
    return m_width;
  }
  uint32_t height() const
  {
    return m_height;
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

private:
  void createImage();
  void createImageView();

  ZVulkanDevice& m_device;
  uint32_t m_width;
  uint32_t m_height;
  vk::Format m_format;
  std::optional<vk::raii::Image> m_image;
  std::optional<vk::raii::DeviceMemory> m_imageMemory;
  std::optional<vk::raii::ImageView> m_imageView;
  vk::ImageUsageFlags m_usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
  vk::MemoryPropertyFlags m_memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
  vk::ImageLayout m_currentLayout = vk::ImageLayout::eUndefined;
};

} // namespace nim
