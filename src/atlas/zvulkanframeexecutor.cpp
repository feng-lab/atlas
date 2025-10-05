#include "zvulkanframeexecutor.h"

#include "zlog.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"

#include <limits>

namespace nim {

namespace {
constexpr uint64_t kFenceTimeoutNs = std::numeric_limits<uint64_t>::max();
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

  auto& frame = acquireFrame();

  vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  frame.commandBuffer.begin(beginInfo);

  auto* dispatcher = m_device.context().device().getDispatcher();
  if (dispatcher && dispatcher->vkCmdBeginDebugUtilsLabelEXT && !debugLabel.empty()) {
    vk::DebugUtilsLabelEXT labelInfo{};
    labelInfo.pLabelName = debugLabel.data();
    frame.commandBuffer.beginDebugUtilsLabelEXT(labelInfo);
  }

  record(frame.commandBuffer);

  if (dispatcher && dispatcher->vkCmdEndDebugUtilsLabelEXT && !debugLabel.empty()) {
    frame.commandBuffer.endDebugUtilsLabelEXT();
  }

  frame.commandBuffer.end();

  vk::CommandBuffer rawBuffer = *frame.commandBuffer;
  vk::SubmitInfo submitInfo{};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &rawBuffer;

  auto& queue = m_device.context().graphicsQueue();
  queue.submit(submitInfo, *frame.fence);
  frame.inFlight = true;

  auto& vkDevice = m_device.context().device();
  const auto waitResult = vkDevice.waitForFences({*frame.fence}, VK_TRUE, kFenceTimeoutNs);
  if (waitResult != vk::Result::eSuccess) {
    LOG(WARNING) << "Frame executor immediate wait result " << vk::to_string(waitResult);
  }

  frame.inFlight = false;
  vkDevice.resetFences({*frame.fence});
  frame.commandBuffer.reset();
}

} // namespace nim

