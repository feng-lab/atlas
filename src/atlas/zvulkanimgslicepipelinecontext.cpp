#include "zvulkanimgslicepipelinecontext.h"

#include "z3dimg.h"
#include "z3dimgslicerenderer.h"
#include "zcolormap.h"
#include "zlog.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkancontext.h"
#include "zvulkanshader.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkantexture.h"
#include "zvulkanbuffer.h"
#include "zvulkanrenderconversions.h"
#include "z3drenderervulkanbackend.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace nim {

namespace {
constexpr float kQuadDepth = 0.0f;

vk::Rect2D makeRect(const glm::uvec2& size)
{
  return vk::Rect2D{vk::Offset2D{0, 0}, vk::Extent2D{static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)}};
}

vk::Viewport makeViewport(const glm::uvec2& size)
{
  return vk::Viewport{0.0f,
                      0.0f,
                      static_cast<float>(std::max<uint32_t>(1u, size.x)),
                      static_cast<float>(std::max<uint32_t>(1u, size.y)),
                      0.0f,
                      1.0f};
}
} // namespace

ZVulkanImgSlicePipelineContext::ZVulkanImgSlicePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanImgSlicePipelineContext::~ZVulkanImgSlicePipelineContext() = default;

void ZVulkanImgSlicePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_quadVertexCount = 0;
}

void ZVulkanImgSlicePipelineContext::record(Z3DRendererBase& renderer,
                                            const RenderBatch& batch,
                                            const ImgSlicePayload& payload,
                                            const vk::Viewport& viewport,
                                            const vk::Rect2D& scissor,
                                            vk::raii::CommandBuffer& cmd)
{
  if (!payload.renderer || !payload.image || payload.slices.empty()) {
    return;
  }

  if (!payload.fastPathOnly && payload.image->isVolumeDownsampled()) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan slice pipeline currently skips non-fast rendering paths.";
    return;
  }

  const size_t channelCount = payload.image->numChannels();
  if (channelCount == 0) {
    return;
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  uploadSliceGeometry(payload.slices);
  ensureQuadVertexBuffer();

  if (m_vertexCount == 0) {
    return;
  }

  // Gather attachment formats to build pipelines.
  const auto formats = vulkan::extractAttachmentFormats(batch);

  // Describe the currently active rendering attachments so we can restore them if we branch into
  // offscreen passes.
  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (attachment.handle.backend != AttachmentBackend::Vulkan || attachment.handle.id == 0) {
      return std::nullopt;
    }
    auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
    if (!texture) {
      return std::nullopt;
    }

    vk::RenderingAttachmentInfo info;
    const auto desiredLayout = vk::ImageLayout::eColorAttachmentOptimal;
    texture->transitionLayout(cmd, texture->layout(), desiredLayout);
    info.imageView = texture->imageView();
    info.imageLayout = desiredLayout;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    vk::ClearValue clear{};
    clear.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                           attachment.clearValue.color.g,
                                                           attachment.clearValue.color.b,
                                                           attachment.clearValue.color.a});
    info.clearValue = clear;
    return info;
  };

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (attachment.handle.backend != AttachmentBackend::Vulkan || attachment.handle.id == 0) {
      return std::nullopt;
    }
    auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
    if (!texture) {
      return std::nullopt;
    }

    vk::RenderingAttachmentInfo info;
    const auto desiredLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    texture->transitionLayout(cmd, texture->layout(), desiredLayout);
    info.imageView = texture->imageView();
    info.imageLayout = desiredLayout;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    vk::ClearValue clear{};
    clear.depthStencil = vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
    info.clearValue = clear;
    return info;
  };

  std::vector<vk::RenderingAttachmentInfo> originalColorAttachments;
  originalColorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      originalColorAttachments.push_back(*info);
    }
  }
  std::optional<vk::RenderingAttachmentInfo> originalDepthAttachment;
  if (batch.pass.depthAttachment) {
    originalDepthAttachment = buildDepthAttachment(*batch.pass.depthAttachment);
  }

  m_channelResources.resize(channelCount);
  std::vector<ZVulkanTexture*> volumeTextures;
  std::vector<ZVulkanTexture*> colormapTextures;
  volumeTextures.reserve(channelCount);
  colormapTextures.reserve(channelCount);

  for (size_t idx = 0; idx < channelCount; ++idx) {
    const ZImg& img = payload.image->channelVolumeImage(idx);
    const uint64_t generation = payload.image->volumeGeneration(idx);
    auto& resources = m_channelResources[idx];

    ZVulkanTexture& volumeTex =
      ensureVolumeTexture(idx, generation, img, resources);

    const ZColorMapParameter* cmapParam = (payload.colormaps && idx < payload.colormaps->size())
                                            ? (*payload.colormaps)[idx].get()
                                            : nullptr;
    ZVulkanTexture& colormapTex = ensureColormapTexture(idx, cmapParam, resources);

    volumeTextures.push_back(&volumeTex);
    colormapTextures.push_back(&colormapTex);
  }

  glm::uvec2 outputSize = payload.outputSize;
  if (outputSize.x == 0u || outputSize.y == 0u) {
    const auto& viewportState = renderer.frameState().viewport;
    outputSize = glm::uvec2(std::max<uint32_t>(1u, viewportState.z), std::max<uint32_t>(1u, viewportState.w));
  }

  auto recordChannelDraw = [&](ZVulkanTexture& colorTarget,
                               ZVulkanTexture* depthTarget,
                               uint32_t layerIndex,
                               ZVulkanTexture& volumeTex,
                               ZVulkanTexture& colormapTex) {
    colorTarget.transitionLayout(cmd, colorTarget.layout(), vk::ImageLayout::eColorAttachmentOptimal);
    if (depthTarget) {
      depthTarget->transitionLayout(cmd, depthTarget->layout(), vk::ImageLayout::eDepthStencilAttachmentOptimal);
    }

    vk::RenderingAttachmentInfo colorAttachment{};
    auto colorView = colorTarget.layerImageView(layerIndex);
    if (colorView == vk::ImageView{}) {
      colorView = colorTarget.imageView();
    }
    if (colorView == vk::ImageView{}) {
      return;
    }
    colorAttachment.imageView = colorView;
    colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f})};

    std::optional<vk::RenderingAttachmentInfo> depthAttachment{};
    if (depthTarget) {
      auto depthView = depthTarget->layerImageView(layerIndex);
      if (depthView == vk::ImageView{}) {
        depthView = depthTarget->imageView();
      }
      if (depthView != vk::ImageView{}) {
        depthAttachment = vk::RenderingAttachmentInfo{};
        depthAttachment->imageView = depthView;
        depthAttachment->imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        depthAttachment->loadOp = vk::AttachmentLoadOp::eClear;
        depthAttachment->storeOp = vk::AttachmentStoreOp::eStore;
        depthAttachment->clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
      }
    }

    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = makeRect(outputSize);
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    vk::RenderingAttachmentInfo depthAttachmentInfo{};
    if (depthAttachment) {
      depthAttachmentInfo = *depthAttachment;
      renderingInfo.pDepthAttachment = &depthAttachmentInfo;
    }

    cmd.beginRendering(renderingInfo);

    bindSliceDescriptor(volumeTex, colormapTex);

    SlicePipelineKey sliceKey;
    sliceKey.validInput = true;
    sliceKey.colorFormats = formats.colorFormats;
    sliceKey.depthFormat = formats.depthFormat;
    auto& pipeline = ensureSlicePipeline(sliceKey, formats);

    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offset});
    if (m_sliceDescriptor) {
      std::array<vk::DescriptorSet, 1> sets{m_sliceDescriptor->descriptorSet()};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
    }

    const auto channelViewport = makeViewport(outputSize);
    const auto channelScissor = makeRect(outputSize);
    cmd.setViewport(0, channelViewport);
    cmd.setScissor(0, channelScissor);

    cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
    cmd.endRendering();
  };

  if (channelCount == 1) {
    // Rendering is already active for the final surface; emit draw directly.
    bindSliceDescriptor(*volumeTextures[0], *colormapTextures[0]);

    SlicePipelineKey sliceKey;
    sliceKey.validInput = true;
    sliceKey.colorFormats = formats.colorFormats;
    sliceKey.depthFormat = formats.depthFormat;
    auto& pipeline = ensureSlicePipeline(sliceKey, formats);

    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offset});
    if (m_sliceDescriptor) {
      std::array<vk::DescriptorSet, 1> sets{m_sliceDescriptor->descriptorSet()};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
    }

    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
    cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
    return;
  }

  if (!payload.layerLease) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan slice pipeline requires a layer render target for multi-channel rendering.";
    return;
  }

  auto& layerLease = *payload.layerLease;
  ZVulkanTexture* layerColor = layerLease.colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease.depthAttachmentTexture();
  if (!layerColor) {
    LOG_FIRST_N(WARNING, 5) << "Layer array render target is missing color attachments for Vulkan slice pipeline.";
    return;
  }

  cmd.endRendering();

  for (uint32_t idx = 0; idx < channelCount; ++idx) {
    recordChannelDraw(*layerColor,
                      layerDepth,
                      idx,
                      *volumeTextures[idx],
                      *colormapTextures[idx]);
  }

  transitionToSampled(cmd, *layerColor, vk::ImageLayout::eShaderReadOnlyOptimal);
  if (layerDepth) {
    transitionToSampled(cmd, *layerDepth, vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  vk::RenderingInfo finalInfo{};
  finalInfo.renderArea = vk::Rect2D{vk::Offset2D{static_cast<int32_t>(batch.pass.viewport.origin.x),
                                                 static_cast<int32_t>(batch.pass.viewport.origin.y)},
                                    vk::Extent2D{static_cast<uint32_t>(batch.pass.viewport.extent.x),
                                                 static_cast<uint32_t>(batch.pass.viewport.extent.y)}};
  finalInfo.layerCount = 1;
  finalInfo.colorAttachmentCount = static_cast<uint32_t>(originalColorAttachments.size());
  finalInfo.pColorAttachments = originalColorAttachments.empty() ? nullptr : originalColorAttachments.data();
  vk::RenderingAttachmentInfo depthAttachmentInfo;
  if (originalDepthAttachment) {
    depthAttachmentInfo = *originalDepthAttachment;
    finalInfo.pDepthAttachment = &depthAttachmentInfo;
  } else {
    finalInfo.pDepthAttachment = nullptr;
  }

  cmd.beginRendering(finalInfo);

  bindMergeDescriptor(*layerColor, layerDepth);

  MergePipelineKey mergeKey;
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = payload.maxProjectionMerge;
  mergeKey.resultOpaque = false;
  mergeKey.colorFormats = formats.colorFormats;
  mergeKey.depthFormat = formats.depthFormat;
  auto& mergePipeline = ensureMergePipeline(mergeKey, formats);

  vk::DeviceSize quadOffset = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {quadOffset});
  if (m_mergeDescriptor) {
    std::array<vk::DescriptorSet, 1> sets{m_mergeDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipelineLayout(), 0, sets, {});
  }

  vk::Viewport finalViewport = viewport;
  vk::Rect2D finalScissor = scissor;
  if (finalViewport.width <= 0.f || finalViewport.height <= 0.f) {
    const auto& viewportState = renderer.frameState().viewport;
    finalViewport = vk::Viewport{0.0f,
                                 0.0f,
                                 static_cast<float>(std::max<uint32_t>(1u, viewportState.z)),
                                 static_cast<float>(std::max<uint32_t>(1u, viewportState.w)),
                                 0.0f,
                                 1.0f};
    finalScissor = vk::Rect2D{vk::Offset2D{0, 0},
                              vk::Extent2D{static_cast<uint32_t>(viewportState.z),
                                           static_cast<uint32_t>(viewportState.w)}};
  }

  cmd.setViewport(0, finalViewport);
  cmd.setScissor(0, finalScissor);
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
}

void ZVulkanImgSlicePipelineContext::ensureDescriptorLayouts()
{
  if (!m_sliceSetLayout) {
    auto& device = m_backend.device();
    auto& vkDevice = device.context().device();

    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}};

    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_sliceSetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_mergeSetLayout) {
    auto& device = m_backend.device();
    auto& vkDevice = device.context().device();

    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}};

    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_mergeSetLayout.emplace(vkDevice, createInfo);
  }
}

void ZVulkanImgSlicePipelineContext::ensureDescriptorPool()
{
  if (!m_descriptorPool) {
    m_descriptorPool = m_backend.device().createDescriptorPool();
  }
}

vk::PipelineVertexInputStateCreateInfo ZVulkanImgSlicePipelineContext::makeSliceVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(SliceVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 2> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(SliceVertex, texCoord))}};

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanImgSlicePipelineContext::makeQuadVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static vk::VertexInputAttributeDescription attr{.location = 0,
                                                  .binding = 0,
                                                  .format = vk::Format::eR32G32B32Sfloat,
                                                  .offset = 0};
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = 1;
  info.pVertexAttributeDescriptions = &attr;
  return info;
}

void ZVulkanImgSlicePipelineContext::ensureSliceVertexCapacity(size_t vertexCount)
{
  if (vertexCount <= m_vertexCapacity) {
    return;
  }

  auto& device = m_backend.device();
  m_vertexCapacity = std::max(vertexCount, m_vertexCapacity * 2 + 1);
  m_vertexBuffer = device.createBuffer(m_vertexCapacity * sizeof(SliceVertex),
                                       vk::BufferUsageFlagBits::eVertexBuffer,
                                       vk::MemoryPropertyFlagBits::eHostVisible |
                                         vk::MemoryPropertyFlagBits::eHostCoherent);
}

void ZVulkanImgSlicePipelineContext::ensureQuadVertexBuffer()
{
  if (m_quadVertexBuffer && m_quadVertexCount == 4) {
    return;
  }

  auto& device = m_backend.device();
  m_quadVertexCapacity = 4;
  m_quadVertexBuffer = device.createBuffer(m_quadVertexCapacity * sizeof(glm::vec3),
                                           vk::BufferUsageFlagBits::eVertexBuffer,
                                           vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent);

  std::array<glm::vec3, 4> quadVertices{glm::vec3{-1.f, -1.f, kQuadDepth},
                                        glm::vec3{-1.f, 1.f, kQuadDepth},
                                        glm::vec3{1.f, -1.f, kQuadDepth},
                                        glm::vec3{1.f, 1.f, kQuadDepth}};
  m_quadVertexBuffer->copyData(quadVertices.data(), quadVertices.size() * sizeof(glm::vec3));
  m_quadVertexCount = quadVertices.size();
}

void ZVulkanImgSlicePipelineContext::uploadSliceGeometry(std::span<const ZMesh> slices)
{
  size_t estimatedVertices = 0;
  for (const auto& slice : slices) {
    if (slice.hasIndices()) {
      estimatedVertices += slice.indices().size();
    } else {
      estimatedVertices += slice.vertices().size();
    }
  }

  ensureSliceVertexCapacity(estimatedVertices);
  if (!m_vertexBuffer) {
    m_vertexCount = 0;
    return;
  }

  std::vector<SliceVertex> vertices;
  vertices.reserve(estimatedVertices);

  for (const auto& slice : slices) {
    const auto& positions = slice.vertices();
    const auto& texCoords = slice.textureCoordinates3D();
    if (positions.size() != texCoords.size()) {
      continue;
    }

    if (slice.hasIndices()) {
      const auto& indices = slice.indices();
      for (auto idx : indices) {
        if (idx >= positions.size()) {
          continue;
        }
        vertices.push_back(SliceVertex{positions[idx], texCoords[idx]});
      }
    } else {
      for (size_t idx = 0; idx < positions.size(); ++idx) {
        vertices.push_back(SliceVertex{positions[idx], texCoords[idx]});
      }
    }
  }

  m_vertexCount = vertices.size();
  if (m_vertexCount == 0) {
    return;
  }

  m_vertexBuffer->copyData(vertices.data(), vertices.size() * sizeof(SliceVertex));
}

ZVulkanTexture& ZVulkanImgSlicePipelineContext::ensureVolumeTexture(size_t channel,
                                                                    uint64_t generation,
                                                                    const ZImg& image,
                                                                    ChannelResources& resources)
{
  (void)channel;

  const uint32_t width = static_cast<uint32_t>(image.width());
  const uint32_t height = static_cast<uint32_t>(image.height());
  const uint32_t depth = static_cast<uint32_t>(image.depth());
  const size_t byteSize = image.byteNumber();
  CHECK_EQ(image.info().bytesPerVoxel, 1u) << "Vulkan slice renderer expects 8-bit single-channel volumes.";
  const uint8_t* data = image.channelData<uint8_t>(0);

  auto& device = m_backend.device();

  const bool needsRecreate = !resources.volumeTexture ||
                             resources.volumeTexture->extent().width != width ||
                             resources.volumeTexture->extent().height != height ||
                             resources.volumeTexture->extent().depth != depth;

  if (needsRecreate) {
    auto info = ZVulkanTexture::CreateInfo::make3D(width,
                                                  height,
                                                  depth,
                                                  vk::Format::eR8Unorm,
                                                  vk::ImageUsageFlagBits::eSampled |
                                                    vk::ImageUsageFlagBits::eTransferDst,
                                                  vk::MemoryPropertyFlagBits::eDeviceLocal,
                                                  1u,
                                                  true,
                                                  vk::ImageLayout::eShaderReadOnlyOptimal);
    resources.volumeTexture = device.createTexture(info);
  } else if (resources.volumeGeneration == generation && resources.volumeTexture) {
    return *resources.volumeTexture;
  }

  if (byteSize > 0 && data != nullptr) {
    resources.volumeTexture->uploadData(data, byteSize);
  }

  resources.volumeGeneration = generation;
  return *resources.volumeTexture;
}

ZVulkanTexture& ZVulkanImgSlicePipelineContext::ensureColormapTexture(size_t channel,
                                                                      const ZColorMapParameter* parameter,
                                                                      ChannelResources& resources)

{
  (void)channel;
  constexpr uint32_t kColormapWidth = 256u;
  auto& device = m_backend.device();

  if (!resources.colormapTexture || resources.colormapTexture->extent().width != kColormapWidth) {
    auto info = ZVulkanTexture::CreateInfo::make1D(kColormapWidth,
                                                   vk::Format::eR8G8B8A8Unorm,
                                                   vk::ImageUsageFlagBits::eSampled |
                                                     vk::ImageUsageFlagBits::eTransferDst,
                                                   vk::MemoryPropertyFlagBits::eDeviceLocal,
                                                   1u,
                                                   true,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
    resources.colormapTexture = device.createTexture(info);
  }

  std::vector<uint8_t> texels(static_cast<size_t>(kColormapWidth) * 4u);
  const ZColorMap* colorMap = parameter ? &parameter->get() : nullptr;
  for (uint32_t x = 0; x < kColormapWidth; ++x) {
    const double fraction = kColormapWidth > 1 ? static_cast<double>(x) / static_cast<double>(kColormapWidth - 1) : 0.0;
    glm::col4 color = colorMap ? colorMap->mappedColor(fraction) : glm::col4(255, 255, 255, 255);
    texels[x * 4 + 0] = color.r;
    texels[x * 4 + 1] = color.g;
    texels[x * 4 + 2] = color.b;
    texels[x * 4 + 3] = color.a;
  }

  resources.colormapTexture->uploadData(texels.data(), texels.size());
  return *resources.colormapTexture;
}

void ZVulkanImgSlicePipelineContext::transitionToSampled(vk::raii::CommandBuffer& cmd,
                                                         ZVulkanTexture& texture,
                                                         vk::ImageLayout desiredLayout)
{
  if (texture.layout() == desiredLayout) {
    return;
  }
  texture.transitionLayout(cmd, texture.layout(), desiredLayout);
  texture.setDescriptorLayout(desiredLayout);
}

ZVulkanImgSlicePipelineContext::PipelineInstance&
ZVulkanImgSlicePipelineContext::ensureSlicePipeline(const SlicePipelineKey& key,
                                                    const vulkan::AttachmentFormats& formats)
{
  auto it = m_slicePipelines.find(key);
  if (it != m_slicePipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "transform_with_3dtexture_and_eye_coordinate.vert.spv",
                                                    shaderBase + "volume_slice_with_colormap_single_channel.frag.spv",
                                                    std::nullopt);

  auto vertexState = makeSliceVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexState, vk::PrimitiveTopology::eTriangleList);
  instance.pipeline->setDescriptorSetLayouts({**m_sliceSetLayout});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthWriteEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE;
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blend);

  std::array<vk::SpecializationMapEntry, 1> entries{
    vk::SpecializationMapEntry{.constantID = 50, .offset = 0, .size = sizeof(uint32_t)}};
  std::vector<vk::SpecializationMapEntry> entryVec(entries.begin(), entries.end());
  uint32_t value = key.validInput ? 1u : 0u;
  std::vector<uint8_t> data(sizeof(uint32_t));
  std::memcpy(data.data(), &value, sizeof(uint32_t));
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, entryVec, data);

  instance.pipeline->create();

  auto [inserted, _] = m_slicePipelines.emplace(key, std::move(instance));
  return inserted->second;
}

ZVulkanImgSlicePipelineContext::PipelineInstance&
ZVulkanImgSlicePipelineContext::ensureMergePipeline(const MergePipelineKey& key,
                                                    const vulkan::AttachmentFormats& formats)
{
  auto it = m_mergePipelines.find(key);
  if (it != m_mergePipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "image2d_array_compositor.frag.spv",
                                                    std::nullopt);

  auto vertexState = makeQuadVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexState, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_mergeSetLayout});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthWriteEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE;
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blend);

  std::array<vk::SpecializationMapEntry, 3> entries{
    vk::SpecializationMapEntry{.constantID = 70, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 71, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 51, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}};
  std::vector<vk::SpecializationMapEntry> entryVec(entries.begin(), entries.end());
  std::array<uint32_t, 3> values{static_cast<uint32_t>(std::max(1, key.numVolumes)),
                                 key.maxProjectionMerge ? 1u : 0u,
                                 key.resultOpaque ? 1u : 0u};
  std::vector<uint8_t> data(sizeof(values));
  std::memcpy(data.data(), values.data(), sizeof(values));
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, entryVec, data);

  instance.pipeline->create();

  auto [inserted, _] = m_mergePipelines.emplace(key, std::move(instance));
  return inserted->second;
}

void ZVulkanImgSlicePipelineContext::bindSliceDescriptor(ZVulkanTexture& volume, ZVulkanTexture& colormap)
{
  if (!m_sliceDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_sliceSetLayout);
    m_sliceDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }

  m_sliceDescriptor->updateTexture(0, volume);
  m_sliceDescriptor->updateTexture(1, colormap);
}

void ZVulkanImgSlicePipelineContext::bindMergeDescriptor(ZVulkanTexture& colorArray, ZVulkanTexture* depthArray)
{
  if (!m_mergeDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_mergeSetLayout);
    m_mergeDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }

  m_mergeDescriptor->updateTexture(0, colorArray);
  if (depthArray) {
    m_mergeDescriptor->updateTexture(1, *depthArray);
  } else {
    m_mergeDescriptor->updateTexture(1, colorArray);
  }
}

} // namespace nim
