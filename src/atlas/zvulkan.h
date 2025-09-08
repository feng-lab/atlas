#pragma once

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

#include "zglmutils.h"
#include "zlog.h"
#include <memory>

namespace nim {

// Initializes Vulkan and logs device info
void initVulkan();

} // namespace nim
