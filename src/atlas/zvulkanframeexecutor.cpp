#include "zvulkanframeexecutor.h"

#include "zlog.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"

#include <limits>

namespace nim {

namespace {
constexpr uint64_t kFenceTimeoutNs = std::numeric_limits<uint64_t>::max();
}

ZVulkanFrameExecutor::ActiveFrame::ActiveFrame(Frame* frame, ZVulkanFrameExecutor* executor)
  : m_frame(frame)
  , m_executor(executor)
{}

ZVulkanFrameExecutor::ActiveFrame::ActiveFrame(ActiveFrame&& other) noexcept
  : m_frame(other.m_frame)
  , m_executor(other.m_executor)
{
  other.m_frame = nullptr;
  other.m_executor = nullptr;
}

ZVulkanFrameExecutor::ActiveFrame& ZVulkanFrameExecutor::ActiveFrame::operator=(ActiveFrame&& other) noexcept
{
  if (this == &other) {
    return *this;
  }
  m_frame = other.m_frame;
  m_executor = other.m_executor;
  other.m_frame = nullptr;
  other.m_executor = nullptr;
  return *this;
}

vk::raii::CommandBuffer& ZVulkanFrameExecutor::ActiveFrame::commandBuffer() const
{
  CHECK(m_frame != nullptr) << "ActiveFrame command buffer requested with no frame";
  return m_frame->commandBuffer;
}

vk::raii::Fence& ZVulkanFrameExecutor::ActiveFrame::fence() const
{
  CHECK(m_frame != nullptr) << "ActiveFrame fence requested with no frame";
  return m_frame->fence;
}

vk::raii::Semaphore& ZVulkanFrameExecutor::ActiveFrame::acquireSemaphore() const
{
  CHECK(m_frame != nullptr) << "ActiveFrame acquire semaphore requested with no frame";
  return m_frame->acquireSemaphore;
}

vk::raii::Semaphore& ZVulkanFrameExecutor::ActiveFrame::releaseSemaphore() const
{
  CHECK(m_frame != nullptr) << "ActiveFrame release semaphore requested with no frame";
  return m_frame->releaseSemaphore;
}

void* ZVulkanFrameExecutor::ActiveFrame::key() const
{
  return static_cast<void*>(m_frame);
}

ZVulkanFrameExecutor::ZVulkanFrameExecutor(ZVulkanDevice& device, uint32_t maxFramesInFlight)
  : m_device(device)
  , m_maxFramesInFlight(std::max(1u, maxFramesInFlight))
{}

ZVulkanFrameExecutor::~ZVulkanFrameExecutor() = default;

void ZVulkanFrameExecutor::setMaxFramesInFlight(uint32_t frames)
{
  const uint32_t clamped = std::max(1u, frames);
  if (clamped == m_maxFramesInFlight) {
    return;
  }
  m_maxFramesInFlight = clamped;
  m_framesDirty = true;
}

void ZVulkanFrameExecutor::trim()
{
  m_frames.clear();
  m_cursor = 0;
  m_framesDirty = true;
}

ZVulkanFrameExecutor::ActiveFrame ZVulkanFrameExecutor::beginFrame()
{
  Frame& frame = acquireFrame();
  return ActiveFrame(&frame, this);
}

void ZVulkanFrameExecutor::markSubmitted(ActiveFrame& frame)
{
  if (!frame.valid()) {
    return;
  }
  frame.m_frame->inFlight = true;
}

void ZVulkanFrameExecutor::scheduleAfterCompletion(ActiveFrame& frame, std::function<void()> fn)
{
  if (!fn) {
    return;
  }
  CHECK(frame.valid()) << "scheduleAfterCompletion called with an invalid ActiveFrame";
  CHECK(frame.m_executor == this) << "scheduleAfterCompletion called with a frame from a different executor";
  frame.m_frame->completionCallbacks.push_back(std::move(fn));
}

void ZVulkanFrameExecutor::waitForCompletion(ActiveFrame& frame)
{
  if (!frame.valid() || !frame.m_frame->inFlight) {
    return;
  }

  auto& vkDevice = m_device.context().device();
  const auto waitResult = vkDevice.waitForFences({*frame.fence()}, true, kFenceTimeoutNs);
  if (waitResult != vk::Result::eSuccess) {
    LOG(WARNING) << "Frame executor waitForFences returned " << vk::to_string(waitResult);
  }
  frame.m_frame->inFlight = false;
  runCompletionCallbacks(*frame.m_frame);
}

void ZVulkanFrameExecutor::waitForAllInFlight()
{
  ensureFrames();
  if (m_frames.empty()) {
    return;
  }

  auto& vkDevice = m_device.context().device();
  for (auto& frame : m_frames) {
    if (!frame.inFlight) {
      continue;
    }
    const auto waitResult = vkDevice.waitForFences({*frame.fence}, true, kFenceTimeoutNs);
    if (waitResult != vk::Result::eSuccess) {
      LOG(WARNING) << "Frame executor waitForFences returned " << vk::to_string(waitResult);
    }
    frame.inFlight = false;
    runCompletionCallbacks(frame);
  }

  // Teardown and backend-switch paths may schedule completion callbacks on a
  // frame that never gets submitted. At this point, we have waited for all
  // known in-flight submissions, so it is safe to flush any remaining callbacks
  // without tying them to a particular fence.
  for (auto& frame : m_frames) {
    runCompletionCallbacks(frame);
  }
}

bool ZVulkanFrameExecutor::hasInFlightFrames()
{
  if (m_frames.empty() && (m_framesDirty || m_maxFramesInFlight > 0)) {
    ensureFrames();
  }
  for (const auto& frame : m_frames) {
    if (frame.inFlight) {
      return true;
    }
  }
  return false;
}

void ZVulkanFrameExecutor::pollCompletions()
{
  ensureFrames();
  if (m_frames.empty()) {
    return;
  }

  auto& vkDevice = m_device.context().device();
  for (auto& frame : m_frames) {
    if (!frame.inFlight) {
      continue;
    }
    const auto status = vkDevice.waitForFences({*frame.fence}, true, 0);
    if (status == vk::Result::eTimeout) {
      continue;
    }
    if (status != vk::Result::eSuccess) {
      LOG(WARNING) << "Frame executor poll waitForFences returned " << vk::to_string(status);
    }
    frame.inFlight = false;
    runCompletionCallbacks(frame);
  }
}

void ZVulkanFrameExecutor::ensureFrames()
{
  if (!m_framesDirty && !m_frames.empty()) {
    return;
  }
  rebuildFrames();
}

void ZVulkanFrameExecutor::rebuildFrames()
{
  auto& context = m_device.context();
  auto& vkDevice = context.device();
  const vk::CommandPool commandPool = context.commandPool();

  m_frames.clear();
  m_frames.reserve(m_maxFramesInFlight);

  for (uint32_t i = 0; i < m_maxFramesInFlight; ++i) {
    vk::CommandBufferAllocateInfo allocInfo{.commandPool = commandPool,
                                            .level = vk::CommandBufferLevel::ePrimary,
                                            .commandBufferCount = 1};
    vk::raii::CommandBuffers buffers(vkDevice, allocInfo);

    vk::FenceCreateInfo fenceInfo{.flags = vk::FenceCreateFlagBits::eSignaled};
    vk::SemaphoreCreateInfo semaphoreInfo{};

    Frame frame;
    frame.commandBuffer = std::move(buffers[0]);
    frame.fence = vk::raii::Fence(vkDevice, fenceInfo);
    frame.acquireSemaphore = vk::raii::Semaphore(vkDevice, semaphoreInfo);
    frame.releaseSemaphore = vk::raii::Semaphore(vkDevice, semaphoreInfo);
    frame.inFlight = false;
    m_frames.emplace_back(std::move(frame));
  }

  m_cursor = 0;
  m_framesDirty = false;
}

ZVulkanFrameExecutor::Frame& ZVulkanFrameExecutor::acquireFrame()
{
  ensureFrames();
  CHECK(!m_frames.empty()) << "Frame executor not initialised";

  const size_t slot = m_cursor;
  auto& frame = m_frames[slot];
  m_cursor = (slot + 1) % m_frames.size();

  auto& vkDevice = m_device.context().device();
  if (frame.inFlight) {
    const auto waitResult = vkDevice.waitForFences({*frame.fence}, true, kFenceTimeoutNs);
    if (waitResult != vk::Result::eSuccess) {
      LOG(WARNING) << "Frame executor waitForFences returned " << vk::to_string(waitResult);
    } else {
      // Debug note: with frames_in_flight=1, acquiring this slot means the
      // previous submission finished (fence signaled) before we start
      // recording the next frame. This does NOT imply the next submit is done
      // — only that prior work completed and the slot is safe to reuse.
      VLOG(1) << "VK executor: waited for previous frame fence before reuse"
              << " (frames_in_flight=" << m_maxFramesInFlight << ", slot=" << slot << ")";
    }
    frame.inFlight = false;
    runCompletionCallbacks(frame);
  } else if (!frame.completionCallbacks.empty()) {
    LOG(WARNING) << "VK executor: acquiring a frame slot with pending completion callbacks but no in-flight fence;"
                 << " waiting for all in-flight frames and flushing callbacks before reuse.";
    waitForAllInFlight();
    runCompletionCallbacks(frame);
  }

  vkDevice.resetFences({*frame.fence});
  frame.commandBuffer.reset();
  return frame;
}

void ZVulkanFrameExecutor::runCompletionCallbacks(Frame& frame)
{
  if (frame.completionCallbacks.empty()) {
    return;
  }
  auto callbacks = std::move(frame.completionCallbacks);
  frame.completionCallbacks.clear();
  for (auto& fn : callbacks) {
    if (fn) {
      fn();
    }
  }
}

void ZVulkanFrameExecutor::executeImmediate(const std::function<void(vk::raii::CommandBuffer&)>& record,
                                            std::string_view debugLabel)
{
  if (!record) {
    return;
  }

  // Use a transient command buffer and fence to avoid interfering with any
  // actively-recording frame command buffer. This prevents mid-frame resets
  // when immediate work (uploads/transitions) is issued during a render.
  auto& context = m_device.context();
  auto& device = context.device();

  vk::CommandBufferAllocateInfo allocInfo{.commandPool = context.commandPool(),
                                          .level = vk::CommandBufferLevel::ePrimary,
                                          .commandBufferCount = 1};
  vk::raii::CommandBuffers buffers(device, allocInfo);
  vk::raii::CommandBuffer& cmd = buffers[0];

  vk::FenceCreateInfo fenceInfo{};
  vk::raii::Fence fence(device, fenceInfo);

  vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  cmd.begin(beginInfo);

  auto* dispatcher = device.getDispatcher();
  if (dispatcher && dispatcher->vkCmdBeginDebugUtilsLabelEXT && !debugLabel.empty()) {
    vk::DebugUtilsLabelEXT labelInfo{};
    labelInfo.pLabelName = debugLabel.data();
    cmd.beginDebugUtilsLabelEXT(labelInfo);
  }

  record(cmd);

  if (dispatcher && dispatcher->vkCmdEndDebugUtilsLabelEXT && !debugLabel.empty()) {
    cmd.endDebugUtilsLabelEXT();
  }

  cmd.end();

  vk::CommandBuffer rawBuffer = *cmd;
  vk::SubmitInfo submitInfo{};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &rawBuffer;

  auto& queue = context.graphicsQueue();
  queue.submit(submitInfo, *fence);

  // Wait for completion to keep semantics identical to the previous
  // executeImmediate behaviour.
  const auto waitResult2 = device.waitForFences({*fence}, true, kFenceTimeoutNs);
  if (waitResult2 != vk::Result::eSuccess) {
    LOG(WARNING) << "Immediate executor waitForFences returned " << vk::to_string(waitResult2);
  }
}

} // namespace nim
