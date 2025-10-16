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
#include "zvulkandescriptorset.h"
#include "zvulkantexture.h"
#include "zvulkanbuffer.h"
#include "zvulkanlututils.h"
#include "zvulkanrenderconversions.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanpagedimageblockuploader.h"
#include "z3drenderglobalstate.h"
#include "zbenchtimer.h"
#include "zmesh.h"

#include <gflags/gflags.h>
#include <QString>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <vector>
#include <unordered_set>

namespace nim {

DECLARE_bool(atlas_debug_save_slice_layers);
DECLARE_bool(atlas_debug_save_slice_merge_out);
DECLARE_string(atlas_debug_save_dir);

namespace {

constexpr float kQuadDepth = 0.0f;
constexpr uint32_t kMaxPagingLevels = 16u;
constexpr uint32_t kInvalidBlockID = 0u;
constexpr uint32_t kUnmappedBlockID = 0xFFFFFFFFu;

vk::Rect2D makeRect(const glm::uvec2& size)
{
  return vk::Rect2D{
    vk::Offset2D{0,                             0                            },
    vk::Extent2D{static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)}
  };
}

// Depth-only layouts/aspects (stencil not used in this pipeline)
static inline std::pair<vk::ImageLayout, vk::ImageAspectFlags>
depthReadDescriptorLayoutAndAspect(const ZVulkanTexture& /*texture*/)
{
  return {vk::ImageLayout::eDepthReadOnlyOptimal, vk::ImageAspectFlagBits::eDepth};
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

std::vector<uint8_t> buildPageDataBuffer(const Z3DImg& image, size_t channel, float zeToScreenPixelVoxelSize)
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
  resetDescriptors();
}

void ZVulkanImgSlicePipelineContext::resetDescriptors()
{
  for (auto& channel : m_channelResources) {
    channel.fastTextureDescriptor = nullptr;
    channel.pagedTextureDescriptor = nullptr;
    channel.pageDescriptor = nullptr;
  }
  m_emptyDescriptor.reset();
  m_mergeDescriptor = nullptr;
  // Descriptors are per-frame arena allocated; nothing to reset here.
}

void ZVulkanImgSlicePipelineContext::record(Z3DRendererBase& renderer,
                                            const RenderBatch& batch,
                                            const ImgSlicePayload& payload,
                                            const vk::Viewport& viewport,
                                            const vk::Rect2D& scissor,
                                            vk::raii::CommandBuffer& cmd)
{
  if (!payload.image || payload.slices.empty()) {
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

  ZVulkanPipelineCommandRecorder recorder(cmd);

  auto makeColorAttachmentInfo = [&](const AttachmentDesc& attachment,
                                     const char* usage) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture = vulkan::textureFromHandle(attachment.handle, m_backend.device(), usage);

    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    info.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
    info.clearValue.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                     attachment.clearValue.color.g,
                                                                     attachment.clearValue.color.b,
                                                                     attachment.clearValue.color.a});
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    info.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    info.srcAccess = {};
    info.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    info.aspect = vk::ImageAspectFlagBits::eColor;
    info.trackingTexture = &texture;
    return info;
  };

  auto makeDepthAttachmentInfo = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture = vulkan::textureFromHandle(attachment.handle, m_backend.device(), "image-slice depth attachment");

    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    info.finalLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    info.clearValue.depthStencil =
      vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    info.dstStage = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    info.srcAccess = {};
    info.dstAccess =
      vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    const vk::ImageAspectFlags aspectMask =
      texture.info().aspectMask == vk::ImageAspectFlags{} ? vk::ImageAspectFlagBits::eDepth : texture.info().aspectMask;
    info.aspect = aspectMask;
    info.trackingTexture = &texture;
    return info;
  };

  std::vector<ZVulkanAttachmentInfo> finalColorAttachments;
  finalColorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = makeColorAttachmentInfo(attachment, "image-slice color attachment")) {
      // Always clear before drawing new slices to avoid residual imagery.
      info->loadOp = vk::AttachmentLoadOp::eClear;
      info->storeOp = vk::AttachmentStoreOp::eStore;
      finalColorAttachments.push_back(*info);
    }
  }
  std::optional<ZVulkanAttachmentInfo> finalDepthAttachment;
  if (batch.pass.depthAttachment) {
    finalDepthAttachment = makeDepthAttachmentInfo(*batch.pass.depthAttachment);
    if (finalDepthAttachment) {
      finalDepthAttachment->loadOp = vk::AttachmentLoadOp::eClear;
      finalDepthAttachment->storeOp = vk::AttachmentStoreOp::eStore;
      finalDepthAttachment->clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0u);
    }
  }

  VLOG(2) << fmt::format(
    "Slice::record begin fastOnly={} paging={} channels={} out={}x{} colors={} depth={} color0Fmt={} depthFmt={}",
    payload.fastPathOnly,
    (!payload.fastPathOnly && payload.image->isVolumeDownsampled()),
    payload.image->numChannels(),
    batch.pass.viewport.extent.x,
    batch.pass.viewport.extent.y,
    finalColorAttachments.size(),
    finalDepthAttachment.has_value(),
    finalColorAttachments.empty() ? -1 : static_cast<int>(vulkan::extractAttachmentFormats(batch).colorFormats.front()),
    vulkan::extractAttachmentFormats(batch).depthFormat
      ? static_cast<int>(*vulkan::extractAttachmentFormats(batch).depthFormat)
      : -1);

  const vk::Rect2D finalRenderArea{
    vk::Offset2D{static_cast<int32_t>(batch.pass.viewport.origin.x),
                 static_cast<int32_t>(batch.pass.viewport.origin.y) },
    vk::Extent2D{static_cast<uint32_t>(batch.pass.viewport.extent.x),
                 static_cast<uint32_t>(batch.pass.viewport.extent.y)}
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
  glm::vec2 pixelEyeSpaceSize =
    monoEyeState.frustumNearPlaneSize / glm::vec2(std::max(1u, outputSize.x), std::max(1u, outputSize.y));
  float zeToScreenPixelVoxelSize =
    -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / nearClip * sceneState.devicePixelRatio;

  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();

  m_channelResources.resize(channelCount);

  std::vector<ChannelInputs> channels(channelCount);

  CHECK(payload.colormaps != nullptr) << "Slice payload missing colormaps vector";
  for (size_t idx = 0; idx < channelCount; ++idx) {
    const ZImg& img = *payload.image->channelImageShared(idx);
    const uint64_t generation = payload.image->volumeGeneration(idx);
    auto& resources = m_channelResources[idx];
    ChannelInputs channel{};
    channel.volume = &ensureVolumeTexture(idx, generation, img, resources);
    CHECK(idx < payload.colormaps->size()) << "Slice payload colormaps size < channelCount";
    const ZColorMap* colorMap = (*payload.colormaps)[idx];
    CHECK(colorMap != nullptr) << "Slice payload has null ZColorMap at channel " << idx;
    channel.colormap = &ensureColormapTexture(idx, colorMap, resources);

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
      CHECK(channel.pageDirectory && channel.pageTable && channel.imageCache)
        << "Paging textures missing for Vulkan slice pipeline channel " << idx;
    }
    return true;
  };

  if (!readyForPaging()) {
    // Not ready to draw; do not begin/end rendering.
    return;
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

    ZVulkanAttachmentInfo colorInfo{};
    colorInfo.image = blockColor->image();
    colorInfo.view = blockColor->imageView();
    colorInfo.format = blockColor->format();
    colorInfo.initialLayout = blockColor->layout();
    colorInfo.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorInfo.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f})};
    colorInfo.loadOp = vk::AttachmentLoadOp::eClear;
    colorInfo.storeOp = vk::AttachmentStoreOp::eStore;
    colorInfo.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    colorInfo.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    colorInfo.srcAccess = {};
    colorInfo.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    colorInfo.aspect = vk::ImageAspectFlagBits::eColor;
    colorInfo.trackingTexture = blockColor;

    const std::vector<vk::DescriptorSet> descriptorSets = collectSliceDescriptorSets(resources, true);

    struct SlicePushConstant
    {
      glm::mat4 projectionView;
      glm::mat4 view;
    } pcB;
    {
      const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
      pcB.projectionView = eyeState.projectionMatrix * eyeState.viewMatrix;
      pcB.view = eyeState.viewMatrix;
    }

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = makeRect(outputSize);
    spec.viewports = {makeViewport(outputSize)};
    spec.scissors = {makeRect(outputSize)};
    spec.colorAttachments = {colorInfo};
    spec.pipelineHandle = blockPipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = blockPipeline.pipeline->pipelineLayoutHandle();
    spec.descriptorSets = descriptorSets;
    spec.descriptorSetFirst = 0;
    spec.expectedDescriptorSetCount = 3;
    spec.vertexBuffers = {m_vertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.vertexCount = static_cast<uint32_t>(m_vertexCount);
    spec.instanceCount = 1;
    spec.pushConstantsData = &pcB;
    spec.pushConstantsSize = sizeof(pcB);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    spec.requirePushConstants = true;

    recorder.recordGraphicsPass(spec);

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
    VLOG(2) << fmt::format("Slice layer draw: ch={} layer={} paging={} colorFmt={} depthFmt={} verts={}",
                           channel,
                           layerIndex,
                           paging,
                           static_cast<int>(colorTarget.format()),
                           depthTarget ? static_cast<int>(depthTarget->format()) : -1,
                           m_vertexCount);

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

    vk::ImageView colorView = colorTarget.layerImageView(layerIndex);
    if (colorView == vk::ImageView{}) {
      colorView = colorTarget.imageView();
    }
    CHECK(colorView != vk::ImageView{}) << "Slice channel color attachment missing image view";

    ZVulkanAttachmentInfo colorAttachment{};
    colorAttachment.image = colorTarget.image();
    colorAttachment.view = colorView;
    colorAttachment.format = colorTarget.format();
    colorAttachment.initialLayout = colorTarget.layout();
    colorAttachment.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f})};
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    colorAttachment.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    colorAttachment.srcAccess = {};
    colorAttachment.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    colorAttachment.aspect = vk::ImageAspectFlagBits::eColor;
    colorAttachment.trackingTexture = &colorTarget;

    std::optional<ZVulkanAttachmentInfo> depthAttachment{};
    if (depthTarget) {
      vk::ImageView depthView = depthTarget->layerImageView(layerIndex, vk::ImageAspectFlagBits::eDepth);
      if (depthView == vk::ImageView{}) {
        depthView = depthTarget->imageView();
      }
      if (depthView != vk::ImageView{}) {
        ZVulkanAttachmentInfo depthInfo{};
        depthInfo.image = depthTarget->image();
        depthInfo.view = depthView;
        depthInfo.format = depthTarget->format();
        depthInfo.initialLayout = depthTarget->layout();
        depthInfo.finalLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depthInfo.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
        depthInfo.loadOp = vk::AttachmentLoadOp::eClear;
        depthInfo.storeOp = vk::AttachmentStoreOp::eStore;
        depthInfo.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
        depthInfo.dstStage =
          vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
        depthInfo.srcAccess = {};
        depthInfo.dstAccess =
          vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        depthInfo.aspect = vk::ImageAspectFlagBits::eDepth;
        depthInfo.trackingTexture = depthTarget;
        depthAttachment = depthInfo;
      }
    }

    std::vector<vk::DescriptorSet> descriptorSets = collectSliceDescriptorSets(resources, paging);

    struct SlicePushConstant
    {
      glm::mat4 projectionView;
      glm::mat4 view;
    } pc;
    {
      const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
      pc.projectionView = eyeState.projectionMatrix * eyeState.viewMatrix;
      pc.view = eyeState.viewMatrix;
    }

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = makeRect(outputSize);
    spec.viewports = {makeViewport(outputSize)};
    spec.scissors = {makeRect(outputSize)};
    spec.colorAttachments = {colorAttachment};
    spec.depthStencilAttachment = depthAttachment;
    spec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    spec.descriptorSetFirst = 0;
    spec.descriptorSets = std::move(descriptorSets);
    spec.expectedDescriptorSetCount = paging ? std::optional<uint32_t>(3u) : std::optional<uint32_t>(1u);
    spec.vertexBuffers = {m_vertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.vertexCount = static_cast<uint32_t>(m_vertexCount);
    spec.instanceCount = 1;
    spec.pushConstantsData = &pc;
    spec.pushConstantsSize = sizeof(pc);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    spec.requirePushConstants = true;

    recorder.recordGraphicsPass(spec);
  };

  if (channelCount == 1) {
    SlicePipelineKey sliceKey;
    sliceKey.validInput = true;
    sliceKey.levelCount = usePaging ? m_channelResources[0].levelCount : 1u;
    sliceKey.colorFormats = formats.colorFormats;
    sliceKey.depthFormat = formats.depthFormat;
    auto& pipeline = ensureSlicePipeline(sliceKey, formats);

    if (formats.depthFormat) {
      CHECK(finalDepthAttachment.has_value())
        << "Slice single-channel path requires a depth attachment when depth format is present";
    }

    std::vector<vk::DescriptorSet> descriptorSets = collectSliceDescriptorSets(m_channelResources[0], usePaging);

    struct SlicePushConstant
    {
      glm::mat4 projectionView;
      glm::mat4 view;
    } pc;
    {
      const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
      pc.projectionView = eyeState.projectionMatrix * eyeState.viewMatrix;
      pc.view = eyeState.viewMatrix;
    }

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = finalRenderArea;
    spec.viewports = {viewport};
    spec.scissors = {scissor};
    spec.colorAttachments = finalColorAttachments;
    spec.depthStencilAttachment = finalDepthAttachment;
    spec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    spec.descriptorSetFirst = 0;
    spec.descriptorSets = std::move(descriptorSets);
    spec.expectedDescriptorSetCount = usePaging ? std::optional<uint32_t>(3u) : std::optional<uint32_t>(1u);
    spec.vertexBuffers = {m_vertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.vertexCount = static_cast<uint32_t>(m_vertexCount);
    spec.instanceCount = 1;
    spec.pushConstantsData = &pc;
    spec.pushConstantsSize = sizeof(pc);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    spec.requirePushConstants = true;

    recorder.recordGraphicsPass(spec);
    return;
  }

  CHECK(payload.layerLease) << "Vulkan slice pipeline requires a layer render target for multi-channel rendering.";

  auto& layerLease = *payload.layerLease;
  ZVulkanTexture* layerColor = layerLease.colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease.depthAttachmentTexture();
  CHECK(layerColor) << "Layer array render target is missing color attachments for Vulkan slice pipeline.";
  CHECK(layerDepth) << "Layer array render target is missing depth attachments for Vulkan slice pipeline.";
  const uint32_t layerCount = std::max<uint32_t>(1u, layerLease.descriptor.layers);

  // Clear all array layers up front to avoid residual imagery from previous frames.
  vk::ImageSubresourceRange colorRange{vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, layerCount};
  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eTransferDstOptimal);
  vk::ClearColorValue layerClearColor{
    std::array<float, 4>{0.f, 0.f, 0.f, 0.f}
  };
  cmd.clearColorImage(layerColor->image(), vk::ImageLayout::eTransferDstOptimal, layerClearColor, colorRange);
  layerColor->transitionLayout(cmd, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal);

  vk::ImageSubresourceRange depthRange{vk::ImageAspectFlagBits::eDepth, 0u, 1u, 0u, layerCount};
  layerDepth->transitionLayout(cmd,
                               layerDepth->layout(),
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ImageAspectFlagBits::eDepth);
  cmd.clearDepthStencilImage(layerDepth->image(),
                             vk::ImageLayout::eTransferDstOptimal,
                             vk::ClearDepthStencilValue{1.0f, 0u},
                             depthRange);
  layerDepth->transitionLayout(cmd,
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ImageLayout::eDepthAttachmentOptimal,
                               vk::ImageAspectFlagBits::eDepth);

  for (uint32_t idx = 0; idx < channelCount; ++idx) {
    recordChannelDraw(idx, m_channelResources[idx], *layerColor, layerDepth, idx, usePaging);
  }

  transitionToSampled(cmd, *layerColor, vk::ImageLayout::eShaderReadOnlyOptimal);
  // Depth arrays must transition to depth-read layout with depth aspect for sampling.
  if (layerDepth->layout() != vk::ImageLayout::eDepthReadOnlyOptimal) {
    layerDepth->transitionLayout(cmd,
                                 layerDepth->layout(),
                                 vk::ImageLayout::eDepthReadOnlyOptimal,
                                 vk::ImageAspectFlagBits::eDepth);
    layerDepth->setDescriptorLayout(vk::ImageLayout::eDepthReadOnlyOptimal);
  } else {
    layerDepth->setDescriptorLayout(vk::ImageLayout::eDepthReadOnlyOptimal);
  }

  if (FLAGS_atlas_debug_save_slice_layers) {
    auto leaseRef = payload.layerLease;
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend && leaseRef && leaseRef->hasVulkanImage()) {
      const uint32_t saveLayerCount = static_cast<uint32_t>(channelCount);
      backend->scheduleAfterCurrentFrameCompletion([leaseRef, saveLayerCount]() {
        ZVulkanTexture* tex = leaseRef->colorAttachment(0);
        if (tex) {
          QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!dir.isEmpty() && !dir.endsWith('/')) {
            dir += '/';
          }
          const uint32_t layers = std::min<uint32_t>(saveLayerCount, tex->arrayLayers());
          for (uint32_t layer = 0; layer < layers; ++layer) {
            const QString filename =
              dir + QString("slice_layer_%1_%2x%3.tif").arg(layer).arg(tex->width()).arg(tex->height());
            ZVulkanTexture::ImageSaveOptions opts;
            opts.arrayLayer = layer;
            if (!tex->saveToImage(filename, opts)) {
              LOG(ERROR) << "Slice layer debug save failed for color layer " << layer;
            }
          }
        }

        ZVulkanTexture* dtex = leaseRef->depthAttachmentTexture();
        if (dtex) {
          QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!dir.isEmpty() && !dir.endsWith('/')) {
            dir += '/';
          }
          const uint32_t layers = std::min<uint32_t>(saveLayerCount, dtex->arrayLayers());
          for (uint32_t layer = 0; layer < layers; ++layer) {
            const QString filename =
              dir + QString("slice_layer_depth_%1_%2x%3.tif").arg(layer).arg(dtex->width()).arg(dtex->height());
            ZVulkanTexture::ImageSaveOptions opts;
            opts.arrayLayer = layer;
            opts.aspectMask = vk::ImageAspectFlagBits::eDepth;
            if (!dtex->saveToImage(filename, opts)) {
              LOG(ERROR) << "Slice layer debug save failed for depth layer " << layer;
            }
          }
        }
      });
    }
  }

  bindMergeDescriptor(*layerColor, layerDepth);

  MergePipelineKey mergeKey;
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = payload.maxProjectionMerge;
  mergeKey.resultOpaque = false;
  mergeKey.colorFormats = formats.colorFormats;
  mergeKey.depthFormat = formats.depthFormat;
  auto& mergePipeline = ensureMergePipeline(mergeKey, formats);

  std::vector<vk::DescriptorSet> mergeSets;
  if (m_mergeDescriptor) {
    mergeSets.push_back(m_mergeDescriptor->descriptorSet());
  }
  CHECK(!mergeSets.empty()) << "Slice merge requires descriptor set with layer textures";

  VLOG(2) << fmt::format("Slice merge: quadVerts={} colorFmt0={} depthFmt={} depthsampLayout={}",
                         m_quadVertexCount,
                         formats.colorFormats.empty() ? -1 : static_cast<int>(formats.colorFormats.front()),
                         formats.depthFormat ? static_cast<int>(*formats.depthFormat) : -1,
                         layerDepth ? 1 : 0);

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
    finalScissor = vk::Rect2D{
      vk::Offset2D{0,                                      0                                     },
      vk::Extent2D{static_cast<uint32_t>(viewportState.z), static_cast<uint32_t>(viewportState.w)}
    };
  }

  ZVulkanGraphicsPassSpec mergeSpec{};
  mergeSpec.renderArea = finalRenderArea;
  mergeSpec.viewports = {finalViewport};
  mergeSpec.scissors = {finalScissor};
  mergeSpec.colorAttachments = finalColorAttachments;
  mergeSpec.depthStencilAttachment = finalDepthAttachment;
  mergeSpec.pipelineHandle = mergePipeline.pipeline->pipelineHandle();
  mergeSpec.pipelineLayoutHandle = mergePipeline.pipeline->pipelineLayoutHandle();
  mergeSpec.descriptorSetFirst = 0;
  mergeSpec.descriptorSets = std::move(mergeSets);
  mergeSpec.expectedDescriptorSetCount = 1;
  mergeSpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
  mergeSpec.vertexOffsets = {0};
  mergeSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  mergeSpec.instanceCount = 1;

  recorder.recordGraphicsPass(mergeSpec);

  if (FLAGS_atlas_debug_save_slice_merge_out) {
    std::vector<AttachmentHandle> colorHandles;
    colorHandles.reserve(batch.pass.colorAttachments.size());
    for (const auto& att : batch.pass.colorAttachments) {
      colorHandles.push_back(att.handle);
    }
    std::optional<AttachmentHandle> depthHandle = std::nullopt;
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      depthHandle = batch.pass.depthAttachment->handle;
    }
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend && !colorHandles.empty()) {
      backend->scheduleAfterCurrentFrameCompletion([this, handles = std::move(colorHandles), depthHandle]() {
        const AttachmentHandle& handle = handles.front();
        CHECK(handle.valid() && handle.backend == AttachmentBackend::Vulkan)
          << "Slice merge debug save: invalid color attachment handle";

        auto& tex = vulkan::textureFromHandle(handle, m_backend.device(), "slice merge debug");
        QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
        if (!dir.isEmpty() && !dir.endsWith('/')) {
          dir += '/';
        }
        const QString filename = dir + QString("slice_merge_%1x%2.tif").arg(tex.width()).arg(tex.height());
        ZVulkanTexture::ImageSaveOptions colorOpts;
        if (!tex.saveToImage(filename, colorOpts)) {
          LOG(ERROR) << "Slice merge debug save failed for color attachment";
        }

        if (depthHandle && depthHandle->valid() && depthHandle->backend == AttachmentBackend::Vulkan) {
          auto& dtex = vulkan::textureFromHandle(*depthHandle, m_backend.device(), "slice merge depth debug");
          QString ddir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!ddir.isEmpty() && !ddir.endsWith('/')) {
            ddir += '/';
          }
          const QString dname = ddir + QString("slice_merge_depth_%1x%2.tif").arg(dtex.width()).arg(dtex.height());
          ZVulkanTexture::ImageSaveOptions depthOpts;
          depthOpts.aspectMask = vk::ImageAspectFlagBits::eDepth;
          if (!dtex.saveToImage(dname, depthOpts)) {
            LOG(ERROR) << "Slice merge depth debug save failed";
          }
        }
      });
    }
  }
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_mergeSetLayout.emplace(vkDevice, createInfo);
  }
}

void ZVulkanImgSlicePipelineContext::ensureDescriptorPool() {}

void ZVulkanImgSlicePipelineContext::ensureEmptyDescriptor()
{
  if (m_emptyDescriptor || !m_emptySetLayout) {
    return;
  }
  m_emptyDescriptor = m_backend.allocateFrameDescriptorSet(**m_emptySetLayout);
  CHECK(m_emptyDescriptor != nullptr) << "Slice pipeline: failed to allocate empty descriptor set";
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
                                        .offset = static_cast<uint32_t>(offsetof(SliceVertex, texCoord))}
  };

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanImgSlicePipelineContext::makeQuadVertexInputState() const
{
  // Align with pass.vert (vec3 attribute at location 0)
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
  m_vertexBuffer =
    device.createBuffer(m_vertexCapacity * sizeof(SliceVertex),
                        vk::BufferUsageFlagBits::eVertexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  CHECK(m_vertexBuffer != nullptr) << "Failed to allocate slice vertex buffer, count=" << m_vertexCapacity;
}

void ZVulkanImgSlicePipelineContext::ensureQuadVertexBuffer()
{
  if (m_quadVertexBuffer && m_quadVertexCount == 4) {
    return;
  }

  auto& device = m_backend.device();
  m_quadVertexCapacity = 4;
  m_quadVertexBuffer =
    device.createBuffer(m_quadVertexCapacity * sizeof(glm::vec3),
                        vk::BufferUsageFlagBits::eVertexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  CHECK(m_quadVertexBuffer != nullptr) << "Failed to allocate slice quad vertex buffer";

  std::array<glm::vec3, 4> quadVertices{
    glm::vec3{-1.f, -1.f, kQuadDepth},
    glm::vec3{-1.f, 1.f,  kQuadDepth},
    glm::vec3{1.f,  -1.f, kQuadDepth},
    glm::vec3{1.f,  1.f,  kQuadDepth}
  };
  m_quadVertexBuffer->copyData(quadVertices.data(), quadVertices.size() * sizeof(glm::vec3));
  m_quadVertexCount = quadVertices.size();
}

void ZVulkanImgSlicePipelineContext::uploadSliceGeometry(std::span<const ZMesh> slices)
{
  size_t triangleCount = 0;
  for (const auto& slice : slices) {
    triangleCount += slice.numTriangles();
  }
  const size_t estimatedVertices = triangleCount * 3;

  ensureSliceVertexCapacity(estimatedVertices);
  if (!m_vertexBuffer) {
    m_vertexCount = 0;
    return;
  }

  std::vector<SliceVertex> vertices;
  vertices.reserve(estimatedVertices);

  for (const auto& slice : slices) {
    const size_t triCount = slice.numTriangles();
    if (triCount == 0) {
      continue;
    }
    const auto& texCoords = slice.textureCoordinates3D();
    CHECK_EQ(texCoords.size(), slice.vertices().size()) << "Slice mesh missing 3D texture coordinates";
    for (size_t tri = 0; tri < triCount; ++tri) {
      const auto triangleVerts = slice.triangleVertices(tri);
      const auto triangleIdx = slice.triangleIndices(tri);
      for (int v = 0; v < 3; ++v) {
        vertices.push_back(SliceVertex{triangleVerts[v], texCoords[triangleIdx[v]]});
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

  const bool needsRecreate = !resources.volumeTexture || resources.volumeTexture->extent().width != width ||
                             resources.volumeTexture->extent().height != height ||
                             resources.volumeTexture->extent().depth != depth;

  if (needsRecreate) {
    auto info =
      ZVulkanTexture::CreateInfo::make3D(width,
                                         height,
                                         depth,
                                         vk::Format::eR8Unorm,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal,
                                         1u,
                                         true,
                                         vk::ImageLayout::eShaderReadOnlyOptimal);
    resources.volumeTexture = device.createTexture(info);
    CHECK(resources.volumeTexture != nullptr)
      << "Slice: failed to create 3D volume texture (" << width << "x" << height << "x" << depth << ")";
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
                                                                      const ZColorMap* colorMap,
                                                                      ChannelResources& resources)
{
  (void)channel;
  constexpr uint32_t kColormapWidth = 256u;
  auto& device = m_backend.device();

  // Ensure texture exists and matches desired width
  bool createdOrResized = false;
  if (!resources.colormapTexture || resources.colormapWidth != kColormapWidth) {
    // Prefer RGBA textures in Vulkan; build an RGBA8 LUT and use
    // eR8G8B8A8Unorm to match channel order.
    vulkan::ensure1DLUTTexture(device, resources.colormapTexture, kColormapWidth);
    resources.colormapWidth = kColormapWidth;
    createdOrResized = true;
  }

  CHECK(colorMap != nullptr) << "ensureColormapTexture called with null ZColorMap";
  const uint64_t gen = colorMap->generation();

  // Upload on first create/resize or when the colormap generation changes.
  if (createdOrResized || gen != resources.colormapGeneration) {
    std::vector<uint8_t> texels;
    colorMap->buildLUTRGBA8(texels, kColormapWidth);
    if (!texels.empty()) {
      vulkan::uploadLUT(*resources.colormapTexture, texels.data(), texels.size());
      resources.colormapGeneration = gen;
    }
  }

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
    resources.fastTextureDescriptor = m_backend.allocateOverrideDescriptorSet(**m_fastSliceSetLayout);
  }
  CHECK(resources.fastTextureDescriptor != nullptr) << "Slice fast path: override descriptor allocation failed (fatal)";
  resources.fastTextureDescriptor->updateTexture(0, volume, m_backend.defaultSampler());
  resources.fastTextureDescriptor->updateTexture(1, colormap, m_backend.defaultSampler());
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
    resources.pagedTextureDescriptor = m_backend.allocateOverrideDescriptorSet(**m_pagedSliceSetLayout);
  }
  CHECK(resources.pagedTextureDescriptor != nullptr)
    << "Slice paged path: override descriptor allocation failed (fatal)";
  resources.pagedTextureDescriptor->updateTexture(0, pageDirectory, m_backend.defaultSampler());
  resources.pagedTextureDescriptor->updateTexture(1, pageTable, m_backend.defaultSampler());
  resources.pagedTextureDescriptor->updateTexture(2, imageCache, m_backend.defaultSampler());
  resources.pagedTextureDescriptor->updateTexture(3, volume, m_backend.defaultSampler());
  resources.pagedTextureDescriptor->updateTexture(4, colormap, m_backend.defaultSampler());

  auto pageData = buildPageDataBuffer(image, channel, zeToScreenPixelVoxelSize);
  if (!resources.pageDataBuffer || resources.pageDataCapacity < pageData.size()) {
    resources.pageDataBuffer = m_backend.device().createBuffer(pageData.size(),
                                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!resources.pageDataBuffer) {
    LOG(ERROR) << "Failed to allocate paging uniform buffer for Vulkan slice pipeline.";
    return false;
  }
  resources.pageDataBuffer->copyData(pageData.data(), pageData.size());
  resources.pageDataCapacity = pageData.size();

  if (!resources.pageDescriptor) {
    resources.pageDescriptor = m_backend.allocateOverrideDescriptorSet(**m_slicePageSetLayout);
  }
  CHECK(resources.pageDescriptor != nullptr) << "Slice page params: override descriptor allocation failed (fatal)";
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
  const std::string vertexShader =
    paged ? "transform_with_3dtexture_and_eye_coordinate.vert.spv" : "transform_with_3dtexture.vert.spv";
  const std::string fragmentShader =
    paged ? "image3d_slice_with_colormap.frag.spv" : "volume_slice_with_colormap_single_channel.frag.spv";

  PipelineInstance instance;
  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + vertexShader, shaderBase + fragmentShader, std::nullopt);

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
  const bool hasDepth = formats.depthFormat.has_value();
  instance.pipeline->setDepthTestEnable(hasDepth);
  instance.pipeline->setDepthWriteEnable(hasDepth);
  instance.pipeline->setDepthCompareOp(hasDepth ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);

  // Slice vertex shaders share a push-constant block for projection/view matrices.
  vk::PushConstantRange vsRange{.stageFlags = vk::ShaderStageFlagBits::eVertex,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(glm::mat4) * 2)};
  instance.pipeline->setPushConstantRanges({vsRange});

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = vk::BlendFactor::eOne;
  blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blend.colorBlendOp = vk::BlendOp::eAdd;
  blend.srcAlphaBlendFactor = vk::BlendFactor::eOne;
  blend.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blend.alphaBlendOp = vk::BlendOp::eAdd;
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blend);

  if (paged) {
    std::array<vk::SpecializationMapEntry, 1> entries{
      vk::SpecializationMapEntry{.constantID = 70, .offset = 0, .size = sizeof(uint32_t)}
    };
    uint32_t levelCount = std::max(1u, key.levelCount);
    std::vector<uint8_t> data(sizeof(uint32_t));
    std::memcpy(data.data(), &levelCount, sizeof(uint32_t));
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
  const bool hasDepth = formats.depthFormat.has_value();
  instance.pipeline->setDepthTestEnable(hasDepth);
  instance.pipeline->setDepthWriteEnable(hasDepth);
  instance.pipeline->setDepthCompareOp(hasDepth ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE;
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blend);

  std::array<vk::SpecializationMapEntry, 3> entries{
    vk::SpecializationMapEntry{.constantID = 70, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 71, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 51, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };
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

// depthArray is nullable per pointer contract
void ZVulkanImgSlicePipelineContext::bindMergeDescriptor(ZVulkanTexture& colorArray,
                                                         /*nullable*/ ZVulkanTexture* depthArray)
{
  if (!m_mergeDescriptor) {
    m_mergeDescriptor = m_backend.allocateOverrideDescriptorSet(**m_mergeSetLayout);
  }
  CHECK(m_mergeDescriptor != nullptr) << "Slice merge: override descriptor allocation failed (fatal)";
  m_mergeDescriptor->updateTexture(0, colorArray, m_backend.defaultSampler());
  if (depthArray) {
    const auto [depthLayout, depthAspect] = depthReadDescriptorLayoutAndAspect(*depthArray);
    m_mergeDescriptor->updateTexture(1, *depthArray, m_backend.defaultSampler(), depthLayout, depthAspect);
  } else {
    m_mergeDescriptor->updateTexture(1, colorArray, m_backend.defaultSampler());
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

  // Vertex shader uses push constants for transforms
  vk::PushConstantRange vsRange{.stageFlags = vk::ShaderStageFlagBits::eVertex,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(glm::mat4) * 2)};
  instance.pipeline->setPushConstantRanges({vsRange});

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE;
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blend);

  std::array<vk::SpecializationMapEntry, 1> entries{
    vk::SpecializationMapEntry{.constantID = 70, .offset = 0, .size = sizeof(uint32_t)}
  };
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

std::vector<vk::DescriptorSet> ZVulkanImgSlicePipelineContext::collectSliceDescriptorSets(ChannelResources& resources,
                                                                                          bool usePaging)
{
  if (usePaging) {
    CHECK(resources.pagedTextureDescriptor != nullptr)
      << "Slice pipeline requires paged descriptor when paging is enabled";
    CHECK(resources.pageDescriptor != nullptr) << "Slice pipeline missing paging params descriptor set";
    ensureEmptyDescriptor();
    CHECK(m_emptyDescriptor != nullptr) << "Slice pipeline missing fallback empty descriptor";
    return {resources.pagedTextureDescriptor->descriptorSet(),
            m_emptyDescriptor->descriptorSet(),
            resources.pageDescriptor->descriptorSet()};
  }

  CHECK(resources.fastTextureDescriptor != nullptr) << "Slice pipeline fast path missing descriptor set";
  return {resources.fastTextureDescriptor->descriptorSet()};
}

} // namespace nim
