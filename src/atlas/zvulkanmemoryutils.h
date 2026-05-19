#pragma once

#include "zvulkan.h"
#include <cstdint>

namespace nim {

inline uint64_t vulkanDeviceLocalMemoryBytes(const vk::PhysicalDeviceMemoryProperties& memoryProperties)
{
  uint64_t bytes = 0;
  for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i) {
    if (memoryProperties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
      bytes += memoryProperties.memoryHeaps[i].size;
    }
  }
  return bytes;
}

} // namespace nim
