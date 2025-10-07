#pragma once

#include "zvulkandevice.h"
#include "zvulkantexture.h"

namespace nim::vulkan {

inline void ensure1DLUTTexture(ZVulkanDevice& device,
                               std::unique_ptr<ZVulkanTexture>& texture,
                               uint32_t width,
                               vk::Format format = vk::Format::eR8G8B8A8Unorm,
                               vk::ImageLayout finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal)
{
  if (!texture || texture->extent().width != width) {
    auto info =
      ZVulkanTexture::CreateInfo::make1D(width,
                                         format,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal,
                                         1u,
                                         true,
                                         finalLayout);
    texture = device.createTexture(info);
  }
}

inline void uploadLUT(ZVulkanTexture& texture,
                      const uint8_t* data,
                      size_t byteSize,
                      vk::ImageLayout finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal)
{
  if (!data || byteSize == 0) {
    return;
  }
  texture.uploadData(data, byteSize, finalLayout);
}

} // namespace nim::vulkan
