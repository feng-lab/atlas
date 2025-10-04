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
#include "zvulkanlututils.h"
#include "zvulkanrenderconversions.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanpagedimageblockuploader.h"
#include "z3drenderglobalstate.h"
#include "zbenchtimer.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <limits>
#include <vector>
#include <unordered_set>
#include <fmt/format.h>

namespace nim {

namespace {

constexpr float kQuadDepth = 0.0f;
constexpr uint32_t kMaxPagingLevels = 16u;
constexpr uint32_t kInvalidBlockID = 0u;
constexpr uint32_t kUnmappedBlockID = 0xFFFFFFFFu;

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

inline void appendVec(std::vector<uint8_t>& buffer, const glm::uvec4& value)
{
  const size_t offset = buffer.size();
  buffer.resize(offset + sizeof(glm::uvec4));
  std::memcpy(buffer.data() + offset, &value, sizeof(glm::uvec4));
}

inline void appendVec(std::vector<uint8_t>& buffer, const glm::vec4& value)
{
  const size_t offset = buffer.size();
  buffer.resize(offset + sizeof(glm::vec4));
  std::memcpy(buffer.data() + offset, &value, sizeof(glm::vec4));
}

inline void appendUvec3(std::vector<uint8_t>& buffer, const glm::uvec3& value)
{
  appendVec(buffer, glm::uvec4(value, 0u));
}

inline void appendVec3(std::vector<uint8_t>& buffer, const glm::vec3& value)
{
  appendVec(buffer, glm::vec4(value, 0.0f));
}

inline void appendScalar(std::vector<uint8_t>& buffer, float value)
{
  appendVec(buffer, glm::vec4(value, 0.0f, 0.0f, 0.0f));
}

std::vector<uint8_t> buildPageDataBuffer(const Z3DImg& image,
                                         size_t channel,
                                         float zeToScreenPixelVoxelSize)
{
  const uint32_t levelCount = static_cast<uint32_t>(image.numLevels());
  CHECK(levelCount <= kMaxPagingLevels) << "Unsupported paging level count: " << levelCount;

  std::vector<uint8_t> data;
  data.reserve((levelCount * 4 + 4) * sizeof(glm::vec4));

  const auto& pageDirectoryBases = image.pageDirectoryBases();
  const auto& imageDimensions = image.imageDimensionsLevels();
  const auto& voxelWorldSizes = image.voxelWorldSizesLevels();
  const auto& posToBlockIDs = image.posToBlockIDsLevels();
  CHECK_EQ(pageDirectoryBases.size(), levelCount);
  CHECK_EQ(imageDimensions.size(), levelCount);
  CHECK_EQ(voxelWorldSizes.size(), levelCount);
  CHECK_EQ(posToBlockIDs.size(), levelCount);

  for (uint32_t level = 0; level < levelCount; ++level) {
    appendUvec3(data, pageDirectoryBases[level]);
  }

  appendUvec3(data, image.pageTableBlockSize());

  for (uint32_t level = 0; level < levelCount; ++level) {
    appendUvec3(data, imageDimensions[level]);
  }

  for (uint32_t level = 0; level < levelCount; ++level) {
    appendScalar(data, voxelWorldSizes[level]);
  }

  appendUvec3(data, image.imageBlockSize());

  const glm::vec3 addressToNorm = image.imageAddressToNormalizedTextureCoord(channel);
  appendVec3(data, addressToNorm);
  appendScalar(data, zeToScreenPixelVoxelSize);

  for (uint32_t level = 0; level < levelCount; ++level) {
    appendUvec3(data, posToBlockIDs[level]);
  }

  return data;
}

std::vector<uint32_t> collectMissingBlockIDs(const std::vector<uint32_t>& blockIDs)
{
  std::unordered_set<uint32_t> unique;
  unique.reserve(blockIDs.size());
  for (size_t idx = 0; idx + 3 < blockIDs.size(); idx += 4) {
    const uint32_t value = blockIDs[idx];
    if (value != kInvalidBlockID && value != kUnmappedBlockID) {
      unique.insert(value);
    }
  }
  std::vector<uint32_t> result;
  result.reserve(unique.size());
  result.insert(result.end(), unique.begin(), unique.end());
  return result;
}

struct ChannelInputs
{
  ZVulkanTexture* volume = nullptr;
  ZVulkanTexture* colormap = nullptr;
  ZVulkanTexture* pageDirectory = nullptr;
  ZVulkanTexture* pageTable = nullptr;
  ZVulkanTexture* imageCache = nullptr;
};

} // namespace

ZVulkanImgSlicePipelineContext::ZVulkanImgSlicePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
  , m_imageBlockUploader(std::make_unique<ZVulkanPagedImageBlockUploader>(backend.device()))
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

  if (m_imageBlockUploader) {
    m_imageBlockUploader->bindToImage(*payload.image);
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

  const bool usePaging = !payload.fastPathOnly && payload.image->isVolumeDownsampled();

  // Gather attachment formats to build pipelines.
  const auto formats = vulkan::extractAttachmentFormats(batch);

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

  vk::RenderingAttachmentInfo depthAttachmentInfo;
  vk::RenderingInfo finalInfo{};
  finalInfo.renderArea = vk::Rect2D{vk::Offset2D{static_cast<int32_t>(batch.pass.viewport.origin.x),
                                                static_cast<int32_t>(batch.pass.viewport.origin.y)},
                                    vk::Extent2D{static_cast<uint32_t>(batch.pass.viewport.extent.x),
                                                 static_cast<uint32_t>(batch.pass.viewport.extent.y)}};
  finalInfo.layerCount = 1;
  finalInfo.colorAttachmentCount = static_cast<uint32_t>(originalColorAttachments.size());
  finalInfo.pColorAttachments = originalColorAttachments.empty() ? nullptr : originalColorAttachments.data();
  if (originalDepthAttachment) {
    depthAttachmentInfo = *originalDepthAttachment;
    finalInfo.pDepthAttachment = &depthAttachmentInfo;
  } else {
    finalInfo.pDepthAttachment = nullptr;
  }

  bool renderingActive = true;
  auto ensureFinalRendering = [&]() {
    if (!renderingActive) {
      cmd.beginRendering(finalInfo);
      renderingActive = true;
    }
  };

  glm::uvec2 outputSize = payload.outputSize;
  if (outputSize.x == 0u || outputSize.y == 0u) {
    const auto& viewportState = renderer.frameState().viewport;
    outputSize = glm::uvec2(std::max<uint32_t>(1u, viewportState.z), std::max<uint32_t>(1u, viewportState.w));
  }

  const auto& viewState = renderer.viewState();
  const auto& sceneState = renderer.sceneState();
  auto monoEyeIndex = static_cast<size_t>(Z3DEye::MonoEye);
  const auto& monoEyeState = viewState.eyes[monoEyeIndex];
  float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
  glm::vec2 pixelEyeSpaceSize = monoEyeState.frustumNearPlaneSize /
                                glm::vec2(std::max(1u, outputSize.x), std::max(1u, outputSize.y));
  float zeToScreenPixelVoxelSize = -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / nearClip * sceneState.devicePixelRatio;

  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();

  m_channelResources.resize(channelCount);

  std::vector<ChannelInputs> channels(channelCount);

  for (size_t idx = 0; idx < channelCount; ++idx) {
    const ZImg& img = *payload.image->channelImageShared(idx);
    const uint64_t generation = payload.image->volumeGeneration(idx);
    auto& resources = m_channelResources[idx];
    ChannelInputs channel{};
    channel.volume = &ensureVolumeTexture(idx, generation, img, resources);
    const ZColorMapParameter* cmapParam = (payload.colormaps && idx < payload.colormaps->size())
                                            ? (*payload.colormaps)[idx].get()
                                            : nullptr;
    channel.colormap = &ensureColormapTexture(idx, cmapParam, resources);

    if (m_imageBlockUploader) {
      channel.pageDirectory = m_imageBlockUploader->pageDirectoryTexture(*payload.image, idx);
      channel.pageTable = m_imageBlockUploader->pageTableTexture(*payload.image, idx);
      channel.imageCache = m_imageBlockUploader->imageCacheTexture(*payload.image, idx);
    }

    updateFastDescriptors(resources, *channel.volume, *channel.colormap);
    channels[idx] = channel;
  }

  auto readyForPaging = [&]() {
    if (!usePaging) {
      return true;
    }
    for (size_t idx = 0; idx < channelCount; ++idx) {
      const auto& channel = channels[idx];
      if (!channel.pageDirectory || !channel.pageTable || !channel.imageCache) {
        LOG_FIRST_N(WARNING, 5) << "Paging textures missing for Vulkan slice pipeline channel " << idx;
        return false;
      }
    }
    return true;
  };

  if (!readyForPaging()) {
    ensureFinalRendering();
    return;
  }

  if (usePaging || channelCount > 1) {
    cmd.endRendering();
    renderingActive = false;
  }

  auto runBlockIdPass = [&](size_t channel, ChannelResources& resources) -> std::vector<uint32_t> {
    auto& channelInputs = channels[channel];
    ZVulkanTexture* pageDirectory = channelInputs.pageDirectory;
    ZVulkanTexture* pageTable = channelInputs.pageTable;
    ZVulkanTexture* imageCache = channelInputs.imageCache;
    ZVulkanTexture* volumeTex = channelInputs.volume;
    ZVulkanTexture* colormapTex = channelInputs.colormap;
    if (!pageDirectory || !pageTable || !imageCache || !volumeTex || !colormapTex) {
      return {};
    }

    if (!updatePagedDescriptors(resources,
                                *pageDirectory,
                                *pageTable,
                                *imageCache,
                                *volumeTex,
                                *colormapTex,
                                *payload.image,
                                channel,
                                zeToScreenPixelVoxelSize)) {
      return {};
    }

    auto blockLease = scratchPool.acquireBlockIdRenderTarget(outputSize, 1, 1.0, RenderBackend::Vulkan);
    ZVulkanTexture* blockColor = blockLease.colorAttachment(0);
    if (!blockColor) {
      return {};
    }

    BlockIdPipelineKey blockKey{resources.levelCount, blockColor->format()};
    auto& blockPipeline = ensureBlockIdPipeline(blockKey, blockColor->format());

    blockColor->transitionLayout(cmd, blockColor->layout(), vk::ImageLayout::eColorAttachmentOptimal);

    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.imageView = blockColor->imageView();
    colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f})};

    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = makeRect(outputSize);
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    cmd.beginRendering(renderingInfo);

    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, blockPipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offset});
    bindPagedDescriptors(resources, blockPipeline.pipeline->pipelineLayout(), cmd, true);
    cmd.setViewport(0, makeViewport(outputSize));
    cmd.setScissor(0, makeRect(outputSize));
    cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
    cmd.endRendering();

    const vk::Extent3D extent = blockColor->extent();
    const size_t pixelCount = static_cast<size_t>(extent.width) * extent.height;
    std::vector<uint32_t> blockData(pixelCount * 4u, 0u);
    blockColor->downloadData(blockData.data(), blockData.size() * sizeof(uint32_t));
    return collectMissingBlockIDs(blockData);
  };

  if (usePaging) {
    for (size_t idx = 0; idx < channelCount; ++idx) {
      auto& resources = m_channelResources[idx];
      auto missingBlocks = runBlockIdPass(idx, resources);
      if (!missingBlocks.empty()) {
        ZBenchTimer timer(fmt::format("vulkan_slice_channel_{}", idx));
        payload.image->updateAndUploadPageDirectoryCaches(missingBlocks, idx, cancellationToken, timer);
      }
    }
  }

  for (size_t idx = 0; idx < channelCount; ++idx) {
    auto& resources = m_channelResources[idx];
    auto& channelInputs = channels[idx];
    updateFastDescriptors(resources, *channelInputs.volume, *channelInputs.colormap);
    if (usePaging && channelInputs.pageDirectory && channelInputs.pageTable && channelInputs.imageCache) {
      if (!updatePagedDescriptors(resources,
                                  *channelInputs.pageDirectory,
                                  *channelInputs.pageTable,
                                  *channelInputs.imageCache,
                                  *channelInputs.volume,
                                  *channelInputs.colormap,
                                  *payload.image,
                                  idx,
                                  zeToScreenPixelVoxelSize)) {
        continue;
      }
    }
  }

  auto recordChannelDraw = [&]([[maybe_unused]] size_t channel,
                               ChannelResources& resources,
                               ZVulkanTexture& colorTarget,
                               ZVulkanTexture* depthTarget,
                               uint32_t layerIndex,
                               bool paging) {
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

    vulkan::AttachmentFormats channelFormats;
    channelFormats.colorFormats.push_back(colorTarget.format());
    if (depthTarget) {
      channelFormats.depthFormat = depthTarget->format();
    }

    SlicePipelineKey sliceKey;
    sliceKey.validInput = true;
    sliceKey.levelCount = paging ? resources.levelCount : 1u;
    sliceKey.colorFormats = channelFormats.colorFormats;
    sliceKey.depthFormat = channelFormats.depthFormat;
    auto& pipeline = ensureSlicePipeline(sliceKey, channelFormats);

    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offset});
    bindPagedDescriptors(resources, pipeline.pipeline->pipelineLayout(), cmd, paging);
    cmd.setViewport(0, makeViewport(outputSize));
    cmd.setScissor(0, makeRect(outputSize));
    cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
    cmd.endRendering();
  };

  if (channelCount == 1) {
    ensureFinalRendering();
    SlicePipelineKey sliceKey;
    sliceKey.validInput = true;
    sliceKey.levelCount = usePaging ? m_channelResources[0].levelCount : 1u;
    sliceKey.colorFormats = formats.colorFormats;
    sliceKey.depthFormat = formats.depthFormat;
    auto& pipeline = ensureSlicePipeline(sliceKey, formats);

    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offset});
    bindPagedDescriptors(m_channelResources[0], pipeline.pipeline->pipelineLayout(), cmd, usePaging);
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
    cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
    return;
  }

  if (!payload.layerLease) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan slice pipeline requires a layer render target for multi-channel rendering.";
    ensureFinalRendering();
    return;
  }

  auto& layerLease = *payload.layerLease;
  ZVulkanTexture* layerColor = layerLease.colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease.depthAttachmentTexture();
  if (!layerColor) {
    LOG_FIRST_N(WARNING, 5) << "Layer array render target is missing color attachments for Vulkan slice pipeline.";
    ensureFinalRendering();
    return;
  }

  for (uint32_t idx = 0; idx < channelCount; ++idx) {
    recordChannelDraw(idx,
                      m_channelResources[idx],
                      *layerColor,
                      layerDepth,
                      idx,
                      usePaging);
  }

  transitionToSampled(cmd, *layerColor, vk::ImageLayout::eShaderReadOnlyOptimal);
  if (layerDepth) {
    transitionToSampled(cmd, *layerDepth, vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  ensureFinalRendering();

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
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_fastSliceSetLayout) {
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
    m_fastSliceSetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_pagedSliceSetLayout) {
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 2,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 3,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 4,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_pagedSliceSetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_slicePageSetLayout) {
    vk::DescriptorSetLayoutBinding binding{.binding = 2,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_slicePageSetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_emptySetLayout) {
    vk::DescriptorSetLayoutCreateInfo createInfo{};
    m_emptySetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_mergeSetLayout) {
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

void ZVulkanImgSlicePipelineContext::ensureEmptyDescriptor()
{
  if (m_emptyDescriptor || !m_emptySetLayout) {
    return;
  }
  ensureDescriptorPool();
  auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_emptySetLayout);
  m_emptyDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
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
  const ZColorMap* colorMap = parameter ? &parameter->get() : nullptr;
  std::vector<uint8_t> texels;
  if (colorMap) {
    colorMap->buildLUTBGRA8(texels, kColormapWidth);
  } else {
    texels.assign(static_cast<size_t>(kColormapWidth) * 4u, 0xFF);
  }
  vulkan::ensure1DLUTTexture(device, resources.colormapTexture, kColormapWidth);
  vulkan::uploadLUT(*resources.colormapTexture, texels.data(), texels.size());
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

void ZVulkanImgSlicePipelineContext::updateFastDescriptors(ChannelResources& resources,
                                                           ZVulkanTexture& volume,
                                                           ZVulkanTexture& colormap)
{
  if (!resources.fastTextureDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_fastSliceSetLayout);
    resources.fastTextureDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }
  resources.fastTextureDescriptor->updateTexture(0, volume);
  resources.fastTextureDescriptor->updateTexture(1, colormap);
  if (!resources.pagedTextureDescriptor) {
    resources.levelCount = 1u;
  }
}

bool ZVulkanImgSlicePipelineContext::updatePagedDescriptors(ChannelResources& resources,
                                                            ZVulkanTexture& pageDirectory,
                                                            ZVulkanTexture& pageTable,
                                                            ZVulkanTexture& imageCache,
                                                            ZVulkanTexture& volume,
                                                            ZVulkanTexture& colormap,
                                                            const Z3DImg& image,
                                                            size_t channel,
                                                            float zeToScreenPixelVoxelSize)
{
  if (!resources.pagedTextureDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_pagedSliceSetLayout);
    resources.pagedTextureDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }
  resources.pagedTextureDescriptor->updateTexture(0, pageDirectory);
  resources.pagedTextureDescriptor->updateTexture(1, pageTable);
  resources.pagedTextureDescriptor->updateTexture(2, imageCache);
  resources.pagedTextureDescriptor->updateTexture(3, volume);
  resources.pagedTextureDescriptor->updateTexture(4, colormap);

  auto pageData = buildPageDataBuffer(image, channel, zeToScreenPixelVoxelSize);
  if (!resources.pageDataBuffer || resources.pageDataCapacity < pageData.size()) {
    resources.pageDataBuffer = m_backend.device().createBuffer(pageData.size(),
                                                              vk::BufferUsageFlagBits::eUniformBuffer,
                                                              vk::MemoryPropertyFlagBits::eHostVisible |
                                                                vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!resources.pageDataBuffer) {
    LOG_FIRST_N(ERROR, 5) << "Failed to allocate paging uniform buffer for Vulkan slice pipeline.";
    return false;
  }
  resources.pageDataBuffer->copyData(pageData.data(), pageData.size());
  resources.pageDataCapacity = pageData.size();

  if (!resources.pageDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_slicePageSetLayout);
    resources.pageDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }
  resources.pageDescriptor->updateUniformBuffer(2, *resources.pageDataBuffer);

  resources.levelCount = static_cast<uint32_t>(std::min<size_t>(image.numLevels(), kMaxPagingLevels));
  return true;
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

  const bool paged = key.levelCount > 1u;
  const std::string fragmentShader = paged ? "image3d_slice_with_colormap.frag.spv"
                                           : "volume_slice_with_colormap_single_channel.frag.spv";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "transform_with_3dtexture_and_eye_coordinate.vert.spv",
                                                    shaderBase + fragmentShader,
                                                    std::nullopt);

  auto vertexState = makeSliceVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexState, vk::PrimitiveTopology::eTriangleList);
  if (paged) {
    instance.pipeline->setDescriptorSetLayouts({**m_pagedSliceSetLayout, **m_emptySetLayout, **m_slicePageSetLayout});
  } else {
    instance.pipeline->setDescriptorSetLayouts({**m_fastSliceSetLayout});
  }
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

  if (paged) {
    std::array<vk::SpecializationMapEntry, 1> entries{
      vk::SpecializationMapEntry{.constantID = 70, .offset = 0, .size = sizeof(uint32_t)}};
    uint32_t levelCount = std::max(1u, key.levelCount);
    std::vector<uint8_t> data(sizeof(uint32_t));
    std::memcpy(data.data(), &levelCount, sizeof(uint32_t));
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                                std::vector(entries.begin(), entries.end()),
                                                data);
  } else {
    std::array<vk::SpecializationMapEntry, 1> entries{
      vk::SpecializationMapEntry{.constantID = 50, .offset = 0, .size = sizeof(uint32_t)}};
    uint32_t value = key.validInput ? 1u : 0u;
    std::vector<uint8_t> data(sizeof(uint32_t));
    std::memcpy(data.data(), &value, sizeof(uint32_t));
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                                std::vector(entries.begin(), entries.end()),
                                                data);
  }

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

ZVulkanImgSlicePipelineContext::PipelineInstance&
ZVulkanImgSlicePipelineContext::ensureBlockIdPipeline(const BlockIdPipelineKey& key, vk::Format colorFormat)
{
  auto it = m_blockIdPipelines.find(key);
  if (it != m_blockIdPipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "transform_with_3dtexture_and_eye_coordinate.vert.spv",
                                                    shaderBase + "image3d_slice_with_transfun_blockID.frag.spv",
                                                    std::nullopt);

  auto vertexState = makeSliceVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexState, vk::PrimitiveTopology::eTriangleList);
  instance.pipeline->setDescriptorSetLayouts({**m_pagedSliceSetLayout, **m_emptySetLayout, **m_slicePageSetLayout});
  instance.pipeline->setAttachmentFormats({colorFormat}, std::nullopt);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE;
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blend);

  std::array<vk::SpecializationMapEntry, 1> entries{
    vk::SpecializationMapEntry{.constantID = 70, .offset = 0, .size = sizeof(uint32_t)}};
  uint32_t levelCount = std::max(1u, key.levelCount);
  std::vector<uint8_t> data(sizeof(uint32_t));
  std::memcpy(data.data(), &levelCount, sizeof(uint32_t));
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                              std::vector(entries.begin(), entries.end()),
                                              data);

  instance.pipeline->create();

  auto [inserted, _] = m_blockIdPipelines.emplace(key, std::move(instance));
  return inserted->second;
}

void ZVulkanImgSlicePipelineContext::bindPagedDescriptors(ChannelResources& resources,
                                                          vk::PipelineLayout layout,
                                                          vk::raii::CommandBuffer& cmd,
                                                          bool usePaging)
{
  if (usePaging) {
    if (resources.pagedTextureDescriptor) {
      std::array<vk::DescriptorSet, 1> sets{resources.pagedTextureDescriptor->descriptorSet()};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, sets, {});
    }
    ensureEmptyDescriptor();
    if (m_emptyDescriptor) {
      std::array<vk::DescriptorSet, 1> sets{m_emptyDescriptor->descriptorSet()};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, sets, {});
    }
    if (resources.pageDescriptor) {
      std::array<vk::DescriptorSet, 1> sets{resources.pageDescriptor->descriptorSet()};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 2, sets, {});
    }
  } else {
    if (resources.fastTextureDescriptor) {
      std::array<vk::DescriptorSet, 1> sets{resources.fastTextureDescriptor->descriptorSet()};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, sets, {});
    }
  }
}

} // namespace nim
