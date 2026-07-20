#pragma once

#include "zvulkan.h"
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
    ~ActiveFrame();

    [[nodiscard]] bool valid() const;

    explicit operator bool() const
    {
      return valid();
    }

    [[nodiscard]] vk::raii::CommandBuffer& commandBuffer() const;
    [[nodiscard]] vk::raii::Fence& fence() const;
    [[nodiscard]] vk::raii::Semaphore& acquireSemaphore() const;
    [[nodiscard]] vk::raii::Semaphore& releaseSemaphore() const;
    [[nodiscard]] void* key() const;
    [[nodiscard]] uint32_t slotIndex() const;
    [[nodiscard]] uint64_t acquisitionSerial() const;
    [[nodiscard]] bool waitedForReuse() const;

  private:
    friend class ZVulkanFrameExecutor;
    ActiveFrame(Frame* frame, ZVulkanFrameExecutor* executor);
    void release() noexcept;

    Frame* m_frame = nullptr;
    ZVulkanFrameExecutor* m_executor = nullptr;
    uint64_t m_acquisitionSerial = 0u;
  };

  explicit ZVulkanFrameExecutor(ZVulkanDevice& device, uint32_t maxFramesInFlight);
  ~ZVulkanFrameExecutor();

  ZVulkanFrameExecutor(const ZVulkanFrameExecutor&) = delete;
  ZVulkanFrameExecutor& operator=(const ZVulkanFrameExecutor&) = delete;

  [[nodiscard]] uint32_t maxFramesInFlight() const
  {
    return m_maxFramesInFlight;
  }

  ActiveFrame beginFrame();
  [[nodiscard]] bool owns(const ActiveFrame& frame) const;
  // Descriptor writes are safe only for the current slot acquisition and
  // before its command buffer is first exposed for recording. This excludes
  // submitted frames, completed-but-not-reacquired frames, and stale handles
  // whose slot has since been reused.
  [[nodiscard]] bool isPreRecordSafePoint(const ActiveFrame& frame) const;
  // True when no current acquisition has exposed a command buffer that could
  // still be submitted. Call after waitForAllInFlight() before mutating shared
  // descriptor state across every frame slot.
  [[nodiscard]] bool allFrameSlotsDescriptorMutationSafe() const;
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
  [[nodiscard]] uint32_t inFlightCount();

  void executeImmediate(const std::function<void(vk::raii::CommandBuffer&)>& record, std::string_view debugLabel = {});

  // Poll in-flight fences without blocking and run completion callbacks for any
  // frame that has already finished. Intended to reduce latency for CPU-side
  // releases (residency unpins, retained UBO lifetimes, etc.).
  // If completedKeys is non-null, appends the ActiveFrame::key() values for any
  // frames whose fences were observed complete during this poll (after running
  // their completion callbacks).
  void pollCompletions(std::vector<void*>* completedKeys = nullptr);

private:
  struct Frame
  {
    enum class Phase : uint8_t
    {
      FenceSafe,
      Acquired,
      Recording,
      Submitted,
    };

    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};
    vk::raii::Semaphore acquireSemaphore{nullptr};
    vk::raii::Semaphore releaseSemaphore{nullptr};
    uint32_t slotIndex = 0u;
    uint64_t acquisitionSerial = 0u;
    bool inFlight = false;
    bool waitedForReuse = false;
    Phase phase = Phase::FenceSafe;
    std::vector<std::function<void()>> completionCallbacks;
  };

  Frame& acquireFrame();
  void releaseFrameLease(ActiveFrame& frame) noexcept;
  void runCompletionCallbacks(Frame& frame);
  void ensureFrames();
  void createFrames();
  void checkOwnerThread(std::string_view operation) const;

  ZVulkanDevice& m_device;
  const uint32_t m_maxFramesInFlight;
  std::vector<Frame> m_frames;
  size_t m_cursor = 0;
  uint64_t m_nextAcquisitionSerial = 1u;
  uint32_t m_activeLeaseCount = 0u;
};

} // namespace nim
