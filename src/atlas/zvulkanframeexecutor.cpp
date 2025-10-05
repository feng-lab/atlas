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

ZVulkanFrameExecutor::ActiveFrame&
ZVulkanFrameExecutor::ActiveFrame::operator=(ActiveFrame&& other) noexcept
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

void ZVulkanFrameExecutor::waitForCompletion(ActiveFrame& frame)
{
  if (!frame.valid() || !frame.m_frame->inFlight) {
    return;
  }

  auto& vkDevice = m_device.context().device();
  const auto waitResult = vkDevice.waitForFences({*frame.fence()}, VK_TRUE, kFenceTimeoutNs);
  if (waitResult != vk::Result::eSuccess) {
    LOG(WARNING) << "Frame executor waitForFences returned " << vk::to_string(waitResult);
  }
  frame.m_frame->inFlight = false;
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

  auto& frame = m_frames[m_cursor];
  m_cursor = (m_cursor + 1) % m_frames.size();

  auto& vkDevice = m_device.context().device();
  if (frame.inFlight) {
    const auto waitResult = vkDevice.waitForFences({*frame.fence}, VK_TRUE, kFenceTimeoutNs);
    if (waitResult != vk::Result::eSuccess) {
      LOG(WARNING) << "Frame executor waitForFences returned " << vk::to_string(waitResult);
    }
    frame.inFlight = false;
  }

  vkDevice.resetFences({*frame.fence});
  frame.commandBuffer.reset();
  return frame;
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
  const uint64_t kFenceTimeoutNs = std::numeric_limits<uint64_t>::max();
  device.waitForFences({*fence}, VK_TRUE, kFenceTimeoutNs);
}

} // namespace nim
