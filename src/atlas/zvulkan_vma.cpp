#define VMA_IMPLEMENTATION
// We use dynamic dispatch; don't require link-time Vulkan symbols
#define VMA_STATIC_VULKAN_FUNCTIONS 0
// Allow VMA to dynamically fetch remaining function pointers using the
// provided vkGetInstanceProcAddr/vkGetDeviceProcAddr
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>
