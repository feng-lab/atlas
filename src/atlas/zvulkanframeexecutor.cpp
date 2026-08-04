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

void rememberFirstException(std::exception_ptr& firstException) noexcept
{
  if (!firstException) {
    firstException = std::current_exception();
  }
}
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
  m_device.ensureSubmissionUsable();
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

void ZVulkanFrameExecutor::markSubmitted(ActiveFrame& frame) noexcept
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
  CHECK(owns(frame)) << "waitForCompletion called with no current frame acquisition";
  if (!frame.m_frame->inFlight) {
    CHECK(frame.m_frame->phase != Frame::Phase::Submitted)
      << "Submitted Vulkan frame lost its in-flight ownership marker";
    return;
  }

  CHECK(observeFenceCompletion(*frame.m_frame, true));
  frame.m_frame->inFlight = false;
  frame.m_frame->phase = Frame::Phase::FenceSafe;
  if (auto callbackFailure = runCompletionCallbacks(*frame.m_frame)) {
    std::rethrow_exception(callbackFailure);
  }
}

void ZVulkanFrameExecutor::waitForAllInFlight()
{
  checkOwnerThread("wait for all in-flight frames");
  if (m_frames.empty()) {
    return;
  }

  std::exception_ptr firstException;
  for (auto& frame : m_frames) {
    if (!frame.inFlight) {
      continue;
    }
    CHECK(observeFenceCompletion(frame, true));
    frame.inFlight = false;
    frame.phase = Frame::Phase::FenceSafe;
  }

  // FenceSafe slots contain only callbacks from completed submissions.
  // Releasing a definitely-unsubmitted ActiveFrame discards its callbacks
  // immediately; a still-acquired/recording frame is a teardown invariant
  // violation and remains outside this healthy drain.
  for (auto& frame : m_frames) {
    if (frame.phase == Frame::Phase::FenceSafe) {
      if (auto callbackFailure = runCompletionCallbacks(frame); callbackFailure && !firstException) {
        firstException = std::move(callbackFailure);
      }
    }
  }
  if (firstException) {
    std::rethrow_exception(firstException);
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

  std::exception_ptr firstException;
  for (auto& frame : m_frames) {
    if (!frame.inFlight) {
      continue;
    }
    if (!observeFenceCompletion(frame, false)) {
      continue;
    }
    frame.inFlight = false;
    frame.phase = Frame::Phase::FenceSafe;
    if (auto callbackFailure = runCompletionCallbacks(frame); callbackFailure && !firstException) {
      firstException = std::move(callbackFailure);
    }
    if (completedKeys) {
      completedKeys->push_back(static_cast<void*>(&frame));
    }
  }
  if (firstException) {
    std::rethrow_exception(firstException);
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
  frame.waitedForReuse = false;

  if (frame.inFlight) {
    if (!observeFenceCompletion(frame, false)) {
      frame.waitedForReuse = true;
      CHECK(observeFenceCompletion(frame, true));
      // Debug note: with frames_in_flight=1, acquiring this slot means the
      // previous submission finished (fence signaled) before we start
      // recording the next frame. This does NOT imply the next submit is done
      // — only that prior work completed and the slot is safe to reuse.
      VLOG(1) << "VK executor: waited for previous frame fence before reuse"
              << " (frames_in_flight=" << m_maxFramesInFlight << ", slot=" << slot << ")";
    }
    frame.inFlight = false;
    frame.phase = Frame::Phase::FenceSafe;
    if (auto callbackFailure = runCompletionCallbacks(frame)) {
      std::rethrow_exception(callbackFailure);
    }
  }
  CHECK(frame.completionCallbacks.empty()) << "Reusable Vulkan frame retained completion callbacks";

  CHECK(frame.phase == Frame::Phase::FenceSafe)
    << "Frame executor attempted to reuse a slot with a live unsubmitted acquisition";

  auto& vkDevice = m_device.context().device();
  vkDevice.resetFences({*frame.fence});
  frame.commandBuffer.reset();
  CHECK_LT(m_nextAcquisitionSerial, std::numeric_limits<uint64_t>::max())
    << "Vulkan frame acquisition serial exhausted";
  frame.acquisitionSerial = m_nextAcquisitionSerial++;
  frame.phase = Frame::Phase::Acquired;
  m_cursor = (slot + 1) % m_frames.size();
  return frame;
}

void ZVulkanFrameExecutor::releaseFrameLease(ActiveFrame& frame) noexcept
{
  checkOwnerThread("release active frame lease");
  CHECK_GT(m_activeLeaseCount, 0u) << "Vulkan active frame lease count underflow";
  CHECK(owns(frame)) << "Releasing a stale or foreign Vulkan active frame lease";
  if (frame.m_frame->phase == Frame::Phase::Acquired || frame.m_frame->phase == Frame::Phase::Recording) {
    // No queue submission owns this command buffer. Returning the lease makes
    // its callbacks unavailable before making the slot reusable.
    discardCompletionCallbacks(*frame.m_frame);
    frame.m_frame->phase = Frame::Phase::FenceSafe;
  }
  --m_activeLeaseCount;
}

bool ZVulkanFrameExecutor::observeFenceCompletion(Frame& frame, bool wait)
{
  CHECK(frame.inFlight) << "Fence observation requires an in-flight Vulkan frame";
  CHECK(frame.phase == Frame::Phase::Submitted) << "Fence observation requires a submitted Vulkan frame";

  vk::Result result = vk::Result::eErrorUnknown;
  try {
    result = m_device.context().device().waitForFences({*frame.fence}, true, wait ? kFenceTimeoutNs : 0u);
  }
  catch (const std::exception& e) {
    LOG(FATAL) << "Vulkan fence observation failed; physical submission failures are fatal: " << e.what();
  }
  catch (...) {
    LOG(FATAL)
      << "Vulkan fence observation failed with a non-standard exception; physical submission failures are fatal";
  }

  if (result == vk::Result::eSuccess) {
    return true;
  }
  if (!wait && result == vk::Result::eTimeout) {
    return false;
  }

  LOG(FATAL) << "Vulkan fence observation returned " << enumOrUnderlying(result, 16)
             << "; physical submission failures are fatal";
}

std::exception_ptr ZVulkanFrameExecutor::runCompletionCallbacks(Frame& frame) noexcept
{
  std::exception_ptr firstException;
  while (!frame.completionCallbacks.empty()) {
    auto callbacks = std::move(frame.completionCallbacks);
    frame.completionCallbacks.clear();
    for (auto& fn : callbacks) {
      if (!fn) {
        continue;
      }
      try {
        fn();
      }
      catch (...) {
        rememberFirstException(firstException);
      }
    }
  }
  return firstException;
}

void ZVulkanFrameExecutor::discardCompletionCallbacks(Frame& frame) noexcept
{
  while (!frame.completionCallbacks.empty()) {
    auto callbacks = std::move(frame.completionCallbacks);
    frame.completionCallbacks.clear();
    callbacks.clear();
  }
}

void ZVulkanFrameExecutor::executeImmediate(const std::function<void(vk::raii::CommandBuffer&)>& record,
                                            std::string_view debugLabel)
{
  checkOwnerThread("execute immediate Vulkan commands");
  if (!record) {
    return;
  }
  m_device.ensureSubmissionUsable();
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
  CHECK_EQ(buffers.size(), 1u);
  vk::raii::CommandBuffer& cmd = buffers[0];
  vk::raii::Fence fence(device, vk::FenceCreateInfo{});

  vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  try {
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
  }
  catch (const std::exception&) {
    m_device.recordSubmissionFailure(0u, 0u);
    throw;
  }
  catch (...) {
    m_device.recordSubmissionFailure(0u, 0u);
    throw ZException("Vulkan immediate command recording failed with a non-standard exception");
  }

  try {
    cmd.end();
  }
  catch (const std::exception&) {
    m_device.recordSubmissionFailure(0u, 0u);
    throw;
  }
  catch (...) {
    m_device.recordSubmissionFailure(0u, 0u);
    throw ZException("Vulkan immediate command-buffer finalization failed with a non-standard exception");
  }

  vk::CommandBuffer rawBuffer = *cmd;
  vk::SubmitInfo submitInfo{};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &rawBuffer;

  auto& queue = context.graphicsQueue();
  try {
    queue.submit(submitInfo, *fence);
  }
  catch (const std::exception& e) {
    LOG(FATAL) << "Immediate Vulkan queue submission failed; submission ownership is uncertain: " << e.what();
  }
  catch (...) {
    LOG(FATAL)
      << "Immediate Vulkan queue submission failed with a non-standard exception; submission ownership is uncertain";
  }

  // Immediate submissions are synchronous and return only after fence completion.
  const auto waitBeginTime = logTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  vk::Result waitResult2 = vk::Result::eErrorUnknown;
  try {
    waitResult2 = device.waitForFences({*fence}, true, kFenceTimeoutNs);
  }
  catch (const std::exception& e) {
    LOG(FATAL) << "Immediate Vulkan fence wait failed; physical submission failures are fatal: " << e.what();
  }
  catch (...) {
    LOG(FATAL)
      << "Immediate Vulkan fence wait failed with a non-standard exception; physical submission failures are fatal";
  }
  CHECK(waitResult2 == vk::Result::eSuccess)
    << "Immediate Vulkan fence wait returned " << enumOrUnderlying(waitResult2, 16);
  if (logTiming) {
    const auto endTime = std::chrono::steady_clock::now();
    VLOG(1) << fmt::format("VK immediate submission: label='{}' submissions=1 waits=1 total_ms={:.3f} wait_ms={:.3f}",
                           debugLabel.empty() ? std::string_view("<unlabeled>") : debugLabel,
                           std::chrono::duration<double, std::milli>(endTime - beginTime).count(),
                           std::chrono::duration<double, std::milli>(endTime - waitBeginTime).count());
  }
}

} // namespace nim
