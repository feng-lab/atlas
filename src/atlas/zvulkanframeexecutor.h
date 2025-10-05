#pragma once

#include "zvulkan.h"
#include <algorithm>
#include <functional>
#include <string_view>
#include <vector>

namespace nim {

class ZVulkanDevice;

/**
 * Lightweight executor that manages a ring of command buffers, fences, and semaphores
 * for per-frame submissions (resource uploads, layout transitions, etc.). The executor
 * keeps a small pool of frames in flight and waits on their fences instead of forcing
 * device-wide idle synchronisation.
 */
class ZVulkanFrameExecutor
{
public:
  explicit ZVulkanFrameExecutor(ZVulkanDevice& device, uint32_t maxFramesInFlight = 2);
  ~ZVulkanFrameExecutor();

  ZVulkanFrameExecutor(const ZVulkanFrameExecutor&) = delete;
  ZVulkanFrameExecutor& operator=(const ZVulkanFrameExecutor&) = delete;

  void setMaxFramesInFlight(uint32_t frames);
  [[nodiscard]] uint32_t maxFramesInFlight() const
  {
    return m_maxFramesInFlight;
  }

  void executeImmediate(const std::function<void(vk::raii::CommandBuffer&)>& record,
                        std::string_view debugLabel = {});

  void trim();

private:
  struct Frame
  {
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};
    vk::raii::Semaphore acquireSemaphore{nullptr};
    vk::raii::Semaphore releaseSemaphore{nullptr};
    bool inFlight = false;
  };

  Frame& acquireFrame();
  void ensureFrames();
  void rebuildFrames();

  ZVulkanDevice& m_device;
  uint32_t m_maxFramesInFlight = 2;
  std::vector<Frame> m_frames;
  size_t m_cursor = 0;
  bool m_framesDirty = true;
};

} // namespace nim
