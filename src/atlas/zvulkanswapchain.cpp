#include "zvulkanswapchain.h"
#include "zvulkandevice.h"
#include "zvulkantexture.h"
#include "zvulkanbuffer.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"

#include <fmt/format.h>

namespace nim {

ZVulkanSwapChain::ZVulkanSwapChain(ZVulkanDevice& device, uint32_t width, uint32_t height)
  : m_device(device)
  , m_width(width)
  , m_height(height)
{
  createAttachments();
  createSampler();
  LOG(INFO) << "ZVulkanSwapChain created with size " << width << "x" << height;
}

ZVulkanSwapChain::~ZVulkanSwapChain()
{
  LOG(INFO) << "Destroying ZVulkanSwapChain";
}

void ZVulkanSwapChain::createAttachments()
{
  m_colorAttachment =
    m_device.createTexture(m_width,
                           m_height,
                           vk::Format::eR8G8B8A8Unorm,
                           vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
                           vk::MemoryPropertyFlagBits::eDeviceLocal);
  m_depthAttachment = m_device.createTexture(m_width,
                                             m_height,
                                             vk::Format::eD32Sfloat,
                                             vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                             vk::MemoryPropertyFlagBits::eDeviceLocal);
}

void ZVulkanSwapChain::createSampler()
{
  vk::SamplerCreateInfo samplerInfo{.magFilter = vk::Filter::eLinear,
                                    .minFilter = vk::Filter::eLinear,
                                    .mipmapMode = vk::SamplerMipmapMode::eLinear,
                                    .addressModeU = vk::SamplerAddressMode::eRepeat,
                                    .addressModeV = vk::SamplerAddressMode::eRepeat,
                                    .addressModeW = vk::SamplerAddressMode::eRepeat,
                                    .mipLodBias = 0.0f,
                                    .anisotropyEnable = VK_TRUE,
                                    .maxAnisotropy = 16,
                                    .compareEnable = VK_FALSE,
                                    .compareOp = vk::CompareOp::eAlways,
                                    .minLod = 0.0f,
                                    .maxLod = 0.0f,
                                    .borderColor = vk::BorderColor::eIntOpaqueBlack};
  m_sampler.emplace(m_device.context().device(), samplerInfo);
}

void ZVulkanSwapChain::resize(uint32_t width, uint32_t height)
{
  if (width == m_width && height == m_height) {
    return;
  }
  m_width = width;
  m_height = height;
  createAttachments();
  LOG(INFO) << "Resized ZVulkanSwapChain to " << width << "x" << height;
}

ZVulkanTexture& ZVulkanSwapChain::colorAttachment()
{
  return *m_colorAttachment;
}
ZVulkanTexture& ZVulkanSwapChain::depthAttachment()
{
  return *m_depthAttachment;
}

void ZVulkanSwapChain::configureAcquireWait(bool enable, vk::PipelineStageFlags stageMask)
{
  m_waitOnAcquire = enable;
  m_acquireWaitStage = stageMask;
}

vk::Semaphore ZVulkanSwapChain::acquireSemaphore() const
{
  if (!m_activeFrame || !m_activeFrame->valid()) {
    return VK_NULL_HANDLE;
  }
  return static_cast<vk::Semaphore>(*m_activeFrame->acquireSemaphore());
}

vk::Semaphore ZVulkanSwapChain::releaseSemaphore() const
{
  if (!m_activeFrame || !m_activeFrame->valid()) {
    return VK_NULL_HANDLE;
  }
  return static_cast<vk::Semaphore>(*m_activeFrame->releaseSemaphore());
}

vk::raii::CommandBuffer& ZVulkanSwapChain::beginFrame(vk::ClearColorValue clearColor,
                                                      vk::ClearDepthStencilValue clearDepthStencil)
{
  CHECK(!m_activeFrame) << "Swapchain beginFrame called while previous frame active";

  m_activeFrame = m_device.frameExecutor().beginFrame();
  CHECK(m_activeFrame && m_activeFrame->valid()) << "Failed to acquire frame executor slot for swapchain";

  vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  auto& cmdBuffer = m_activeFrame->commandBuffer();
  cmdBuffer.begin(beginInfo);

  // Ensure attachments are in the correct layouts for rendering
  {
    if (m_colorAttachment->layout() != vk::ImageLayout::eColorAttachmentOptimal) {
      m_colorAttachment->transitionLayout(cmdBuffer, m_colorAttachment->layout(),
                                          vk::ImageLayout::eColorAttachmentOptimal);
    }
    if (m_depthAttachment->layout() != vk::ImageLayout::eDepthStencilAttachmentOptimal) {
      m_depthAttachment->transitionLayout(cmdBuffer, m_depthAttachment->layout(),
                                          vk::ImageLayout::eDepthStencilAttachmentOptimal);
    }
  }

  VkClearValue colorClearValue{};
  colorClearValue.color.float32[0] = clearColor.float32[0];
  colorClearValue.color.float32[1] = clearColor.float32[1];
  colorClearValue.color.float32[2] = clearColor.float32[2];
  colorClearValue.color.float32[3] = clearColor.float32[3];
  VkClearValue depthClearValue{};
  depthClearValue.depthStencil.depth = clearDepthStencil.depth;
  depthClearValue.depthStencil.stencil = clearDepthStencil.stencil;

  std::array<vk::RenderingAttachmentInfo, 1> colorAttachments;
  colorAttachments[0].imageView = m_colorAttachment->imageView();
  colorAttachments[0].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachments[0].loadOp = vk::AttachmentLoadOp::eClear;
  colorAttachments[0].storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachments[0].clearValue = *reinterpret_cast<vk::ClearValue*>(&colorClearValue);

  vk::RenderingAttachmentInfo depthAttachment;
  depthAttachment.imageView = m_depthAttachment->imageView();
  depthAttachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
  depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  depthAttachment.clearValue = *reinterpret_cast<vk::ClearValue*>(&depthClearValue);

  vk::RenderingInfo renderingInfo;
  renderingInfo.renderArea = vk::Rect2D({0, 0}, {m_width, m_height});
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
  renderingInfo.pColorAttachments = colorAttachments.data();
  renderingInfo.pDepthAttachment = &depthAttachment;

  cmdBuffer.beginRendering(renderingInfo);
  return cmdBuffer;
}

void ZVulkanSwapChain::endFrame(vk::raii::CommandBuffer& commandBuffer)
{
  commandBuffer.endRendering();
  commandBuffer.end();

  vk::SubmitInfo submitInfo{};
  vk::PipelineStageFlags waitStages[] = {m_acquireWaitStage};
  vk::Semaphore waitSemaphore = acquireSemaphore();
  if (m_waitOnAcquire && waitSemaphore) {
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &waitSemaphore;
    submitInfo.pWaitDstStageMask = waitStages;
  }
  submitInfo.commandBufferCount = 1;
  vk::CommandBuffer cmdBuffers[] = {*commandBuffer};
  submitInfo.pCommandBuffers = cmdBuffers;

  vk::Semaphore signalSemaphore = releaseSemaphore();
  if (signalSemaphore) {
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSemaphore;
  }

  auto& executor = m_device.frameExecutor();
  auto& queue = m_device.context().graphicsQueue();
  try {
    queue.submit(submitInfo, *m_activeFrame->fence());
    executor.markSubmitted(*m_activeFrame);
  } catch (const std::exception& e) {
    LOG(WARNING) << "Swapchain submit failed: " << e.what();
  }

  m_activeFrame.reset();
}

void ZVulkanSwapChain::copyToMemory(void* data, size_t size)
{
  auto stagingBuffer =
    m_device.createBuffer(size,
                          vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

  // One-shot copy via device helper (waits idle)
  m_device.frameExecutor().executeImmediate(
    [&](vk::raii::CommandBuffer& cmd) {
      if (m_colorAttachment->layout() != vk::ImageLayout::eTransferSrcOptimal) {
        m_colorAttachment->transitionLayout(cmd,
                                            m_colorAttachment->layout(),
                                            vk::ImageLayout::eTransferSrcOptimal);
      }

      vk::BufferImageCopy region{};
      region.bufferOffset = 0;
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = 0;
      region.imageSubresource.layerCount = 1;
      region.imageOffset = vk::Offset3D{0, 0, 0};
      region.imageExtent = vk::Extent3D{m_width, m_height, 1};

      cmd.copyImageToBuffer(m_colorAttachment->image(),
                            vk::ImageLayout::eTransferSrcOptimal,
                            stagingBuffer->buffer(),
                            region);

      m_colorAttachment->transitionLayout(cmd,
                                          vk::ImageLayout::eTransferSrcOptimal,
                                          vk::ImageLayout::eColorAttachmentOptimal);
    },
    "swapchain_copy_to_memory");

  void* mapped = stagingBuffer->map(0, size);
  std::memcpy(data, mapped, size);
  stagingBuffer->unmap();
}

} // namespace nim
