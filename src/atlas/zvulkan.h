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
// Vulkan-Hpp's generated to_string helpers use std::format when available.
// Apple libc++ exposes that path before all formatter internals are available
// for Atlas' macOS 12 deployment target, so keep enum logging in Atlas code.
#define VULKAN_HPP_NO_TO_STRING
#include <vulkan/vulkan_raii.hpp>
#include <vma/vk_mem_alloc.h>

#include "zglmutils.h"
#include "zlog.h"
#include <memory>

namespace nim {

// Initializes Vulkan and logs device info
void initVulkan();

} // namespace nim
