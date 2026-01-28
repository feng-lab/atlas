#pragma once

#include "zvulkandevice.h"
#include "zvulkantexture.h"

namespace nim::vulkan {

// Vulkan portability: use 2D Nx1 textures for LUTs (MoltenVK lacks native 1D).
inline void ensure1DLUTTexture(ZVulkanDevice& device,
                               std::unique_ptr<ZVulkanTexture>& texture,
                               uint32_t width,
                               vk::Format format,
                               vk::ImageLayout finalLayout)
{
  if (!texture || texture->extent().width != width || texture->extent().height != 1u ||
      texture->info().imageType != vk::ImageType::e2D) {
    auto info = ZVulkanTexture::CreateInfo::make2D(width,
                                                   1u,
                                                   format,
                                                   vk::ImageUsageFlagBits::eSampled |
                                                     vk::ImageUsageFlagBits::eTransferDst,
                                                   vk::MemoryPropertyFlagBits::eDeviceLocal,
                                                   1u,
                                                   true,
                                                   finalLayout);
    texture = device.createTexture(info);
  }
}

inline void uploadLUT(ZVulkanTexture& texture, const uint8_t* data, size_t byteSize, vk::ImageLayout finalLayout)
{
  if (!data || byteSize == 0) {
    return;
  }
  texture.uploadData(data, byteSize, finalLayout);
}

} // namespace nim::vulkan
