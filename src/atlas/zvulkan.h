#pragma once

#include <fmt/core.h>

#ifdef __APPLE__

#include <Availability.h>

#define VK_USE_PLATFORM_METAL_EXT 1

#define VK_ENABLE_BETA_EXTENSIONS 1 // VK_KHR_portability_subset

#ifdef __IPHONE_OS_VERSION_MAX_ALLOWED
#define VK_USE_PLATFORM_IOS_MVK 1
#endif

#ifdef __MAC_OS_X_VERSION_MAX_ALLOWED
#define VK_USE_PLATFORM_MACOS_MVK 1
#endif

#endif

#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan_raii.hpp>

namespace nim {

void initVulkan();

} // namespace nim

namespace vk {

template<size_t N>
auto format_as(const ArrayWrapper1D<char, N>& s)
{
  return std::string_view(s);
}

} // namespace vk

