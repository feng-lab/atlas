#include "zvulkanframeexecutor.h"

#include "zlog.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace nim {

namespace {
constexpr uint64_t kFenceTimeoutNs = std::numeric_limits<uint64_t>::max();
}

ZVulkanFrameExecutor::ActiveFrame::ActiveFrame(Frame* frame, ZVulkanFrameExecutor* executor)
  : m_frame(frame)
  , m_executor(executor)
  , m_acquisitionSerial(frame != nullptr ? frame->acquisitionSerial : 0u)
{
  CHECK(m_frame != nullptr);
  CHECK(m_executor != nullptr);
  m_executor->checkOwnerThread("acquire active frame lease");
  CHECK_LT(m_executor->m_activeLeaseCount, std::numeric_limits<uint32_t>::max())
    << "Vulkan active frame lease count exhausted";
  ++m_executor->m_activeLeaseCount;
}

ZVulkanFrameExecutor::ActiveFrame::ActiveFrame(ActiveFrame&& other) noexcept
  : m_frame(other.m_frame)
  , m_executor(other.m_executor)
  , m_acquisitionSerial(other.m_acquisitionSerial)
{
  if (m_executor != nullptr) {
    m_executor->checkOwnerThread("move active frame lease");
  }
  other.m_frame = nullptr;
  other.m_executor = nullptr;
  other.m_acquisitionSerial = 0u;
}

ZVulkanFrameExecutor::ActiveFrame::~ActiveFrame()
{
  release();
}

ZVulkanFrameExecutor::ActiveFrame& ZVulkanFrameExecutor::ActiveFrame::operator=(ActiveFrame&& other) noexcept
{
  if (this == &other) {
    return *this;
  }
  if (other.m_executor != nullptr) {
    other.m_executor->checkOwnerThread("move-assign active frame lease");
  }
  release();
  m_frame = other.m_frame;
  m_executor = other.m_executor;
  m_acquisitionSerial = other.m_acquisitionSerial;
  other.m_frame = nullptr;
  other.m_executor = nullptr;
  other.m_acquisitionSerial = 0u;
  return *this;
}

void ZVulkanFrameExecutor::ActiveFrame::release() noexcept
{
  if (m_executor == nullptr) {
    CHECK(m_frame == nullptr);
    CHECK_EQ(m_acquisitionSerial, 0u);
    return;
  }
  m_executor->releaseFrameLease(*this);
  m_frame = nullptr;
  m_executor = nullptr;
  m_acquisitionSerial = 0u;
}

bool ZVulkanFrameExecutor::ActiveFrame::valid() const
{
  return m_executor != nullptr && m_executor->owns(*this);
}

vk::raii::CommandBuffer& ZVulkanFrameExecutor::ActiveFrame::commandBuffer() const
{
  CHECK(valid()) << "ActiveFrame command buffer requested with no current frame acquisition";
  // Descriptor sets must be fully primed before callers can begin recording.
  // Closing this window on first access also covers failed submissions: once a
  // command buffer may have captured descriptor state, the acquisition cannot
  // be mistaken for a descriptor-write safe point later.
  CHECK(m_frame->phase == Frame::Phase::Acquired || m_frame->phase == Frame::Phase::Recording)
    << "ActiveFrame command buffer requested outside an acquired/recording frame";
  m_frame->phase = Frame::Phase::Recording;
  return m_frame->commandBuffer;
}

vk::raii::Fence& ZVulkanFrameExecutor::ActiveFrame::fence() const
{
  CHECK(valid()) << "ActiveFrame fence requested with no current frame acquisition";
  return m_frame->fence;
}

vk::raii::Semaphore& ZVulkanFrameExecutor::ActiveFrame::acquireSemaphore() const
{
  CHECK(valid()) << "ActiveFrame acquire semaphore requested with no current frame acquisition";
  return m_frame->acquireSemaphore;
}

vk::raii::Semaphore& ZVulkanFrameExecutor::ActiveFrame::releaseSemaphore() const
{
  CHECK(valid()) << "ActiveFrame release semaphore requested with no current frame acquisition";
  return m_frame->releaseSemaphore;
}

void* ZVulkanFrameExecutor::ActiveFrame::key() const
{
  CHECK(valid()) << "ActiveFrame key requested with no current frame acquisition";
  return static_cast<void*>(m_frame);
}

uint32_t ZVulkanFrameExecutor::ActiveFrame::slotIndex() const
{
  CHECK(valid()) << "ActiveFrame slot index requested with no current frame acquisition";
  return m_frame->slotIndex;
}

uint64_t ZVulkanFrameExecutor::ActiveFrame::acquisitionSerial() const
{
  CHECK(valid()) << "ActiveFrame acquisition serial requested with no current frame acquisition";
  return m_acquisitionSerial;
}

bool ZVulkanFrameExecutor::ActiveFrame::waitedForReuse() const
{
  CHECK(valid()) << "ActiveFrame reuse-wait state requested with no current frame acquisition";
  return m_frame->waitedForReuse;
}

ZVulkanFrameExecutor::ZVulkanFrameExecutor(ZVulkanDevice& device, uint32_t maxFramesInFlight)
  : m_device(device)
  , m_maxFramesInFlight(maxFramesInFlight)
{
  checkOwnerThread("construct frame executor");
  CHECK_GT(m_maxFramesInFlight, 0u) << "Vulkan frame executor requires at least one frame slot";
}

ZVulkanFrameExecutor::~ZVulkanFrameExecutor()
{
  checkOwnerThread("destroy frame executor");
  CHECK_EQ(m_activeLeaseCount, 0u) << "Destroying Vulkan frame executor with live ActiveFrame leases";
  for (const Frame& frame : m_frames) {
    CHECK(!frame.inFlight) << "Destroying Vulkan frame executor with an in-flight submission";
    CHECK(frame.phase == Frame::Phase::FenceSafe)
      << "Destroying Vulkan frame executor with an acquired or recording frame";
    CHECK(frame.completionCallbacks.empty()) << "Destroying Vulkan frame executor with undrained completion callbacks";
  }
}

void ZVulkanFrameExecutor::checkOwnerThread(std::string_view operation) const
{
  m_device.checkOwnerThread(operation);
}

ZVulkanFrameExecutor::ActiveFrame ZVulkanFrameExecutor::beginFrame()
{
  checkOwnerThread("begin frame acquisition");
  Frame& frame = acquireFrame();
  return ActiveFrame(&frame, this);
}

bool ZVulkanFrameExecutor::owns(const ActiveFrame& frame) const
{
  checkOwnerThread("validate active frame ownership");
  if (frame.m_executor != this || frame.m_frame == nullptr || frame.m_acquisitionSerial == 0u) {
    return false;
  }
  const auto it = std::find_if(m_frames.begin(), m_frames.end(), [&frame](const Frame& candidate) {
    return &candidate == frame.m_frame;
  });
  return it != m_frames.end() && it->acquisitionSerial == frame.m_acquisitionSerial;
}

bool ZVulkanFrameExecutor::isPreRecordSafePoint(const ActiveFrame& frame) const
{
  checkOwnerThread("query active frame pre-record safe point");
  return owns(frame) && frame.m_frame->phase == Frame::Phase::Acquired && !frame.m_frame->inFlight;
}

bool ZVulkanFrameExecutor::allFrameSlotsDescriptorMutationSafe() const
{
  checkOwnerThread("query frame-slot descriptor mutation safety");
  return std::all_of(m_frames.begin(), m_frames.end(), [](const Frame& frame) {
    return !frame.inFlight && frame.phase != Frame::Phase::Recording && frame.phase != Frame::Phase::Submitted;
  });
}

void ZVulkanFrameExecutor::markSubmitted(ActiveFrame& frame)
{
  checkOwnerThread("mark frame submitted");
  CHECK(owns(frame)) << "markSubmitted called with no current frame acquisition";
  CHECK(frame.m_frame->phase == Frame::Phase::Recording) << "markSubmitted called outside command recording";
  CHECK(!frame.m_frame->inFlight) << "markSubmitted called twice for one frame acquisition";
  frame.m_frame->inFlight = true;
  frame.m_frame->phase = Frame::Phase::Submitted;
}

void ZVulkanFrameExecutor::scheduleAfterCompletion(ActiveFrame& frame, std::function<void()> fn)
{
  checkOwnerThread("schedule frame completion callback");
  if (!fn) {
    return;
  }
  CHECK(frame.valid()) << "scheduleAfterCompletion called with an invalid ActiveFrame";
  CHECK(owns(frame)) << "scheduleAfterCompletion called with no current frame acquisition";
  frame.m_frame->completionCallbacks.push_back(std::move(fn));
}

void ZVulkanFrameExecutor::waitForCompletion(ActiveFrame& frame)
{
  checkOwnerThread("wait for frame completion");
  if (!frame.valid() || !frame.m_frame->inFlight) {
    return;
  }

  auto& vkDevice = m_device.context().device();
  const auto waitResult = vkDevice.waitForFences({*frame.fence()}, true, kFenceTimeoutNs);
  CHECK(waitResult == vk::Result::eSuccess)
    << "Frame executor waitForFences returned " << enumOrUnderlying(waitResult, 16);
  frame.m_frame->inFlight = false;
  frame.m_frame->phase = Frame::Phase::FenceSafe;
  runCompletionCallbacks(*frame.m_frame);
}

void ZVulkanFrameExecutor::waitForAllInFlight()
{
  checkOwnerThread("wait for all in-flight frames");
  if (m_frames.empty()) {
    return;
  }

  auto& vkDevice = m_device.context().device();
  for (auto& frame : m_frames) {
    if (!frame.inFlight) {
      continue;
    }
    const auto waitResult = vkDevice.waitForFences({*frame.fence}, true, kFenceTimeoutNs);
    CHECK(waitResult == vk::Result::eSuccess)
      << "Frame executor waitForFences returned " << enumOrUnderlying(waitResult, 16);
    frame.inFlight = false;
    frame.phase = Frame::Phase::FenceSafe;
    runCompletionCallbacks(frame);
  }

  // Teardown and backend-switch paths may abandon an acquisition that already
  // scheduled callbacks. ActiveFrame release changes such a slot to FenceSafe,
  // so those callbacks can be flushed. Acquired/Recording callbacks must remain
  // pending: that unsubmitted command buffer is outside the fence wait and may
  // still be submitted after this call returns.
  for (auto& frame : m_frames) {
    if (frame.phase == Frame::Phase::FenceSafe) {
      runCompletionCallbacks(frame);
    }
  }
}

bool ZVulkanFrameExecutor::hasInFlightFrames()
{
  checkOwnerThread("query in-flight frames");
  for (const auto& frame : m_frames) {
    if (frame.inFlight) {
      return true;
    }
  }
  return false;
}

uint32_t ZVulkanFrameExecutor::inFlightCount()
{
  checkOwnerThread("count in-flight frames");
  uint32_t count = 0;
  for (const auto& frame : m_frames) {
    if (frame.inFlight) {
      count++;
    }
  }
  return count;
}

void ZVulkanFrameExecutor::pollCompletions(std::vector<void*>* completedKeys)
{
  checkOwnerThread("poll frame completions");
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
    CHECK(status == vk::Result::eSuccess)
      << "Frame executor poll waitForFences returned " << enumOrUnderlying(status, 16);
    frame.inFlight = false;
    frame.phase = Frame::Phase::FenceSafe;
    runCompletionCallbacks(frame);
    if (completedKeys) {
      completedKeys->push_back(static_cast<void*>(&frame));
    }
  }
}

void ZVulkanFrameExecutor::ensureFrames()
{
  if (!m_frames.empty()) {
    return;
  }
  createFrames();
}

void ZVulkanFrameExecutor::createFrames()
{
  CHECK(m_frames.empty()) << "Immutable Vulkan frame slots may only be created once";
  auto& context = m_device.context();
  auto& vkDevice = context.device();
  const vk::CommandPool commandPool = context.commandPool();

  // Publish the immutable ring only after every slot is constructed. A Vulkan
  // allocation failure must not leave ensureFrames() observing a partial ring.
  std::vector<Frame> frames;
  frames.reserve(m_maxFramesInFlight);

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
    frame.slotIndex = i;
    frame.acquisitionSerial = 0u;
    frame.inFlight = false;
    frame.phase = Frame::Phase::FenceSafe;
    frames.emplace_back(std::move(frame));
  }

  m_frames = std::move(frames);
  m_cursor = 0;
}

ZVulkanFrameExecutor::Frame& ZVulkanFrameExecutor::acquireFrame()
{
  ensureFrames();
  CHECK(!m_frames.empty()) << "Frame executor not initialised";

  const size_t slot = m_cursor;
  auto& frame = m_frames[slot];
  m_cursor = (slot + 1) % m_frames.size();
  frame.waitedForReuse = false;

  auto& vkDevice = m_device.context().device();
  if (frame.inFlight) {
    const auto fenceStatus = frame.fence.getStatus();
    CHECK(fenceStatus == vk::Result::eSuccess || fenceStatus == vk::Result::eNotReady)
      << "Frame executor getFenceStatus returned " << enumOrUnderlying(fenceStatus, 16);
    if (fenceStatus == vk::Result::eNotReady) {
      frame.waitedForReuse = true;
      const auto waitResult = vkDevice.waitForFences({*frame.fence}, true, kFenceTimeoutNs);
      CHECK(waitResult == vk::Result::eSuccess)
        << "Frame executor waitForFences returned " << enumOrUnderlying(waitResult, 16);
      // Debug note: with frames_in_flight=1, acquiring this slot means the
      // previous submission finished (fence signaled) before we start
      // recording the next frame. This does NOT imply the next submit is done
      // — only that prior work completed and the slot is safe to reuse.
      VLOG(1) << "VK executor: waited for previous frame fence before reuse"
              << " (frames_in_flight=" << m_maxFramesInFlight << ", slot=" << slot << ")";
    }
    frame.inFlight = false;
    frame.phase = Frame::Phase::FenceSafe;
    runCompletionCallbacks(frame);
  } else if (!frame.completionCallbacks.empty()) {
    LOG(WARNING) << "VK executor: acquiring a frame slot with pending completion callbacks but no in-flight fence;"
                 << " waiting for all in-flight frames and flushing callbacks before reuse.";
    waitForAllInFlight();
    runCompletionCallbacks(frame);
  }

  CHECK(frame.phase == Frame::Phase::FenceSafe)
    << "Frame executor attempted to reuse a slot with a live unsubmitted acquisition";

  vkDevice.resetFences({*frame.fence});
  frame.commandBuffer.reset();
  CHECK_LT(m_nextAcquisitionSerial, std::numeric_limits<uint64_t>::max())
    << "Vulkan frame acquisition serial exhausted";
  frame.acquisitionSerial = m_nextAcquisitionSerial++;
  frame.phase = Frame::Phase::Acquired;
  return frame;
}

void ZVulkanFrameExecutor::releaseFrameLease(ActiveFrame& frame) noexcept
{
  checkOwnerThread("release active frame lease");
  CHECK_GT(m_activeLeaseCount, 0u) << "Vulkan active frame lease count underflow";
  if (owns(frame) &&
      (frame.m_frame->phase == Frame::Phase::Acquired || frame.m_frame->phase == Frame::Phase::Recording)) {
    // No queue submission owns this command buffer. Returning the lease makes
    // the slot safe for reuse and for device-wide descriptor maintenance.
    frame.m_frame->phase = Frame::Phase::FenceSafe;
  }
  --m_activeLeaseCount;
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
  checkOwnerThread("execute immediate Vulkan commands");
  if (!record) {
    return;
  }
  const bool logTiming = VLOG_IS_ON(1);
  const auto beginTime = logTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

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
  const auto waitBeginTime = logTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const auto waitResult2 = device.waitForFences({*fence}, true, kFenceTimeoutNs);
  CHECK(waitResult2 == vk::Result::eSuccess)
    << "Immediate executor waitForFences returned " << enumOrUnderlying(waitResult2, 16);
  if (logTiming) {
    const auto endTime = std::chrono::steady_clock::now();
    VLOG(1) << fmt::format("VK immediate submission: label='{}' submissions=1 waits=1 total_ms={:.3f} wait_ms={:.3f}",
                           debugLabel.empty() ? std::string_view("<unlabeled>") : debugLabel,
                           std::chrono::duration<double, std::milli>(endTime - beginTime).count(),
                           std::chrono::duration<double, std::milli>(endTime - waitBeginTime).count());
  }
}

} // namespace nim
