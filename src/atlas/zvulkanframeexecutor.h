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
  struct Frame;

public:
  class ActiveFrame
  {
  public:
    ActiveFrame() = default;
    ActiveFrame(const ActiveFrame&) = delete;
    ActiveFrame& operator=(const ActiveFrame&) = delete;
    ActiveFrame(ActiveFrame&& other) noexcept;
    ActiveFrame& operator=(ActiveFrame&& other) noexcept;
    ~ActiveFrame() = default;

    [[nodiscard]] bool valid() const
    {
      return m_frame != nullptr;
    }

    explicit operator bool() const
    {
      return valid();
    }

    [[nodiscard]] vk::raii::CommandBuffer& commandBuffer() const;
    [[nodiscard]] vk::raii::Fence& fence() const;
    [[nodiscard]] vk::raii::Semaphore& acquireSemaphore() const;
    [[nodiscard]] vk::raii::Semaphore& releaseSemaphore() const;
    [[nodiscard]] void* key() const;

  private:
    friend class ZVulkanFrameExecutor;
    ActiveFrame(Frame* frame, ZVulkanFrameExecutor* executor);

    Frame* m_frame = nullptr;
    ZVulkanFrameExecutor* m_executor = nullptr;
  };

  explicit ZVulkanFrameExecutor(ZVulkanDevice& device, uint32_t maxFramesInFlight = 2);
  ~ZVulkanFrameExecutor();

  ZVulkanFrameExecutor(const ZVulkanFrameExecutor&) = delete;
  ZVulkanFrameExecutor& operator=(const ZVulkanFrameExecutor&) = delete;

  void setMaxFramesInFlight(uint32_t frames);
  [[nodiscard]] uint32_t maxFramesInFlight() const
  {
    return m_maxFramesInFlight;
  }

  ActiveFrame beginFrame();
  void markSubmitted(ActiveFrame& frame);
  // Schedule a callback to run once the frame's submission fence signals.
  // Callbacks are executed on the caller thread when the executor observes
  // fence completion (waitForCompletion, acquireFrame reuse, or waitForAllInFlight).
  void scheduleAfterCompletion(ActiveFrame& frame, std::function<void()> fn);
  void waitForCompletion(ActiveFrame& frame);
  // Wait for all in-flight frames managed by this executor. This is intended
  // for teardown paths that need to guarantee GPU-idle with respect to all
  // per-frame submissions before releasing resources.
  void waitForAllInFlight();
  // Query whether any frames are currently marked in flight. This reflects
  // whether the executor believes there are outstanding GPU submissions.
  [[nodiscard]] bool hasInFlightFrames();

  void executeImmediate(const std::function<void(vk::raii::CommandBuffer&)>& record, std::string_view debugLabel = {});

  // Poll in-flight fences without blocking and run completion callbacks for any
  // frame that has already finished. Intended to reduce latency for CPU-side
  // releases (residency unpins, retained UBO lifetimes, etc.).
  // If completedKeys is non-null, appends the ActiveFrame::key() values for any
  // frames whose fences were observed complete during this poll (after running
  // their completion callbacks).
  void pollCompletions(std::vector<void*>* completedKeys = nullptr);

  void trim();

private:
  struct Frame
  {
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};
    vk::raii::Semaphore acquireSemaphore{nullptr};
    vk::raii::Semaphore releaseSemaphore{nullptr};
    bool inFlight = false;
    std::vector<std::function<void()>> completionCallbacks;
  };

  Frame& acquireFrame();
  void runCompletionCallbacks(Frame& frame);
  void ensureFrames();
  void rebuildFrames();

  ZVulkanDevice& m_device;
  uint32_t m_maxFramesInFlight = 2;
  std::vector<Frame> m_frames;
  size_t m_cursor = 0;
  bool m_framesDirty = true;
};

} // namespace nim
