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
#include "zvulkanresourcemetadata.h"
#include "zvulkanresidencymanager.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanpagedimageblockuploader.h"
#include "z3drenderglobalstate.h"
#include "zcancellation.h"
#include "zbenchtimer.h"
#include "zmesh.h"
#include "zcoro.h"
#include "zrenderthreadexecutor_tls.h"

#include <folly/coro/Task.h>
#include <gflags/gflags.h>
#include <QString>

#include <algorithm>
#include <fstream>
#include <cmath>
#include <array>
#include <cstring>
#include <limits>
#include <vector>
#include <unordered_set>

namespace nim {

DECLARE_bool(atlas_debug_save_slice_layers);
DECLARE_bool(atlas_debug_save_slice_merge_out);
DECLARE_string(atlas_debug_save_dir);
DECLARE_string(atlas_vk_blockid_compaction_source);

namespace {

constexpr float kQuadDepth = 0.0f;
// Device-driven cap is applied at callsites.
constexpr uint32_t kInvalidBlockID = 0u;
constexpr uint32_t kUnmappedBlockID = 0xFFFFFFFFu;

vk::Rect2D makeRect(const glm::uvec2& size)
{
  return vk::Rect2D{
    vk::Offset2D{0,                             0                            },
    vk::Extent2D{static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)}
  };
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

enum class BlockIdCompactionSource
{
  Buffer,
  Storage,
  Sampled
};

inline BlockIdCompactionSource vkBlockIdCompactionSource()
{
  const std::string v = FLAGS_atlas_vk_blockid_compaction_source;
  if (v.empty() || v == "buffer" || v == "Buffer" || v == "BUFFER") {
    return BlockIdCompactionSource::Buffer;
  }
  if (v == "storage" || v == "Storage" || v == "STORAGE") {
    return BlockIdCompactionSource::Storage;
  }
  if (v == "sampled" || v == "Sampled" || v == "SAMPLED") {
    return BlockIdCompactionSource::Sampled;
  }
  CHECK(false) << "Unknown --atlas_vk_blockid_compaction_source='" << v
               << "'. Expected one of: buffer, storage, sampled.";
  return BlockIdCompactionSource::Buffer;
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

// Device-driven cap for paging levels given PageData pack: 64B header + 64B per-level
inline uint32_t deviceLevelCap(nim::ZVulkanDevice& device)
{
  const auto limits = device.context().physicalDevice().getProperties().limits;
  const uint32_t header = 64u;
  const uint32_t stride = 64u;
  if (limits.maxUniformBufferRange <= header) {
    return 0u;
  }
  const uint64_t maxBytes = static_cast<uint64_t>(limits.maxUniformBufferRange);
  return static_cast<uint32_t>((maxBytes - header) / stride);
}

std::vector<uint8_t>
buildPageDataBuffer(const Z3DImg& image, size_t channel, float zeToScreenPixelVoxelSize, uint32_t levelCount)
{
  (void)channel; // channel not needed for addrNorm when using imageCacheSize
  CHECK_GT(levelCount, 0u) << "Image has zero paging levels (incomplete setup)";

  std::vector<uint8_t> data;
  data.reserve(256);

  const auto& pageDirectoryBases = image.pageDirectoryBases();
  const auto& imageDimensions = image.imageDimensionsLevels();
  const auto& voxelWorldSizes = image.voxelWorldSizesLevels();
  const auto& posToBlockIDs = image.posToBlockIDsLevels();
  CHECK_EQ(pageDirectoryBases.size(), levelCount);
  CHECK_EQ(imageDimensions.size(), levelCount);
  CHECK_EQ(voxelWorldSizes.size(), levelCount);
  CHECK_EQ(posToBlockIDs.size(), levelCount);

  // Validate invariants before packing
  const glm::uvec3 ptb = image.pageTableBlockSize();
  CHECK(ptb.x > 0u && ptb.y > 0u && ptb.z > 0u)
    << "Invalid pageTableBlockSize: (" << ptb.x << ", " << ptb.y << ", " << ptb.z << ")";
  const glm::uvec3 ibs = image.imageBlockSize();
  CHECK(ibs.x > 0u && ibs.y > 0u && ibs.z > 0u)
    << "Invalid imageBlockSize: (" << ibs.x << ", " << ibs.y << ", " << ibs.z << ")";

  // Fixed header in std140: ptb, imageBlock, addrNorm, ze2px (each aligned to 16B)
  appendUvec3(data, ptb);
  appendUvec3(data, ibs);
  const glm::uvec3 cacheSize = image.imageCacheSize();
  CHECK(cacheSize.x > 0u && cacheSize.y > 0u && cacheSize.z > 0u)
    << "Invalid image cache size: " << cacheSize.x << ", " << cacheSize.y << ", " << cacheSize.z;
  const glm::vec3 addrNorm = 1.0f / glm::vec3(cacheSize);
  appendVec3(data, addrNorm);
  appendScalar(data, zeToScreenPixelVoxelSize);

  // Per-level (grouped): dirBase, dims, posToIDs, voxelWorld
  for (uint32_t level = 0; level < levelCount; ++level) {
    const glm::uvec3 dims = imageDimensions[level];
    CHECK(dims.x > 0u && dims.y > 0u && dims.z > 0u)
      << "Invalid image dimensions at level " << level << ": (" << dims.x << ", " << dims.y << ", " << dims.z
      << ") levelCount=" << levelCount;
    appendUvec3(data, pageDirectoryBases[level]);
    appendUvec3(data, dims);
    appendUvec3(data, posToBlockIDs[level]);
    const float vws = voxelWorldSizes[level];
    CHECK(std::isfinite(vws) && vws > 0.0f) << "Invalid voxelWorldSize at level " << level << ": " << vws;
    appendScalar(data, vws);
  }

  // Sanity: std140 pack size should be 64B header + 64B per level
  const size_t expectedBytes = 64u + static_cast<size_t>(levelCount) * 64u;
  CHECK_EQ(data.size(), expectedBytes) << "Unexpected PageData UBO size: got=" << data.size()
                                       << " expected=" << expectedBytes << " (levels=" << levelCount << ")";

  return data;
}

std::vector<uint32_t> readSpirvFile(const std::string& path)
{
  VLOG(2) << "opening SPIR-V file: " << path;
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  CHECK(file) << "Failed to open SPIR-V file: " << path;
  const size_t fileSize = static_cast<size_t>(file.tellg());
  CHECK(fileSize % 4 == 0) << "Invalid SPIR-V size (must be multiple of 4): " << path;
  std::vector<uint32_t> buffer(fileSize / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
  return buffer;
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
{}

ZVulkanImgSlicePipelineContext::~ZVulkanImgSlicePipelineContext() = default;

void ZVulkanImgSlicePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_quadVertexCount = 0;
  m_sliceDrawRanges.clear();
  resetDescriptors();
}

std::optional<ZVulkanImgSlicePipelineContext::Finalization> ZVulkanImgSlicePipelineContext::takePendingFinalization()
{
  auto ret = m_pendingFinalization;
  m_pendingFinalization.reset();
  return ret;
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
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  processEventsAndMaybeCancel(cancellationToken);
  if (!payload.image || payload.slices.empty()) {
    return;
  }

  const bool usePaging = !payload.fastPathOnly && payload.image->isVolumeDownsampled();
  if (usePaging) {
    CHECK(payload.streamKey != 0) << "Vulkan slice paging requires a non-zero streamKey for progressive orchestration.";
    if (!m_imageBlockUploader) {
      // Device is guaranteed to be available after beginRender() ensured it on the backend.
      m_imageBlockUploader = std::make_unique<ZVulkanPagedImageBlockUploader>(m_backend.device());
    }
    m_imageBlockUploader->bindToImage(*payload.image);
  }
  const size_t channelCount = payload.image->numChannels();
  if (channelCount == 0) {
    return;
  }

  ensureDescriptorLayouts();
  uploadSliceGeometry(payload.slices);
  ensureQuadVertexBuffer();

  processEventsAndMaybeCancel(cancellationToken);

  if (m_vertexCount == 0) {
    return;
  }

  // usePaging computed above

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
    // End slice color outputs in sampled layout so downstream passes (e.g., WA/WB image resolve
    // or texture-copy) can read without issuing a transition inside dynamic rendering.
    info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    info.clearValue.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                     attachment.clearValue.color.g,
                                                                     attachment.clearValue.color.b,
                                                                     attachment.clearValue.color.a});
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.srcStage = {};
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
    // Depth is also sampled in downstream composition; prefer depth-read layout.
    info.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    info.clearValue.depthStencil =
      vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.srcStage = {};
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

  // cancellationToken obtained above
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

    if (usePaging) {
      CHECK(m_imageBlockUploader) << "Slice pipeline expected uploader when paging is enabled";
      channel.pageDirectory = m_imageBlockUploader->pageDirectoryTexture(*payload.image, idx);
      channel.pageTable = m_imageBlockUploader->pageTableTexture(*payload.image, idx);
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
      CHECK(channel.pageDirectory && channel.pageTable)
        << "Paging textures missing for Vulkan slice pipeline channel " << idx;
    }
    return true;
  };

  if (!readyForPaging()) {
    // Not ready to draw; do not begin/end rendering.
    return;
  }

  // For multi-channel paging, the progressive scheduler draws a single channel per frame;
  // defer paged descriptor binding until the draw stage selects the active channel.
  const bool deferPagedDescriptorUpdates = usePaging && channelCount > 1;
  for (size_t idx = 0; idx < channelCount; ++idx) {
    processEventsAndMaybeCancel(cancellationToken);
    auto& resources = m_channelResources[idx];
    auto& channelInputs = channels[idx];
    updateFastDescriptors(resources, *channelInputs.volume, *channelInputs.colormap);
    if (usePaging && !deferPagedDescriptorUpdates && channelInputs.pageDirectory && channelInputs.pageTable) {
      CHECK(m_imageBlockUploader) << "Slice pipeline expected uploader when paging is enabled";
      channelInputs.imageCache = m_imageBlockUploader->imageCacheTexture(*payload.image, idx);
      CHECK(channelInputs.imageCache != nullptr) << "Slice paging missing image cache for channel " << idx;
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
                               bool paging,
                               vk::AttachmentLoadOp loadOp,
                               bool finalizeToSampled,
                               ZVulkanPipelineCommandRecorder& passRecorder) {
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
    colorAttachment.finalLayout =
      finalizeToSampled ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f})};
    colorAttachment.loadOp = loadOp;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.srcStage = {};
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
        depthInfo.finalLayout =
          finalizeToSampled ? vk::ImageLayout::eDepthReadOnlyOptimal : vk::ImageLayout::eDepthAttachmentOptimal;
        depthInfo.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
        depthInfo.loadOp = loadOp;
        depthInfo.storeOp = vk::AttachmentStoreOp::eStore;
        depthInfo.srcStage = {};
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

    passRecorder.recordGraphicsPass(spec);
  };

  // Record slice block-ID discovery + GPU compaction and schedule cache uploads.
  // This mirrors the raycaster pattern but for per-slice geometry: we clear the
  // block-ID RT per slice to avoid depth-occlusion between planes, then union
  // the compacted IDs on CPU after the frame fence signals.
  auto scheduleBlockIdDiscoveryAndCacheUpload =
    [&](uint32_t channelIndex, ChannelResources& resources, const ChannelInputs& channelInputs, uint32_t roundIndex) {
      CHECK(payload.image != nullptr) << "Slice block-ID discovery requires a valid image";
      CHECK(channelInputs.pageDirectory && channelInputs.pageTable)
        << "Slice block-ID discovery requires paging textures (page directory/table)";
      CHECK(channelInputs.volume && channelInputs.colormap) << "Slice block-ID discovery requires volume+colormap";

      if (!m_placeholder3D) {
        auto info =
          ZVulkanTexture::CreateInfo::make3D(1,
                                             1,
                                             1,
                                             vk::Format::eR8Unorm,
                                             vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                             vk::MemoryPropertyFlagBits::eDeviceLocal,
                                             1u,
                                             true,
                                             vk::ImageLayout::eShaderReadOnlyOptimal);
        m_placeholder3D = m_backend.device().createTexture(info);
        CHECK(m_placeholder3D != nullptr) << "Failed to create slice placeholder 3D texture";
        const uint8_t zero = 0u;
        m_placeholder3D->uploadData(&zero, sizeof(zero), vk::ImageLayout::eShaderReadOnlyOptimal);
      }
      CHECK(m_placeholder3D != nullptr);

      // Bind pageDirectory/pageTable for the block-ID shader. It does not require
      // the actual image cache contents, so a tiny placeholder texture is sufficient.
      if (!updatePagedDescriptors(resources,
                                  *channelInputs.pageDirectory,
                                  *channelInputs.pageTable,
                                  *m_placeholder3D,
                                  *channelInputs.volume,
                                  *channelInputs.colormap,
                                  *payload.image,
                                  channelIndex,
                                  zeToScreenPixelVoxelSize)) {
        return;
      }

      auto blockLease = scratchPool.acquireBlockIdRenderTarget(outputSize, 1, 1.0, RenderBackend::Vulkan);
      ZVulkanTexture* blockColor = blockLease.colorAttachment(0);
      CHECK(blockColor != nullptr) << "Slice block-ID render target missing color attachment";

      BlockIdPipelineKey blockKey{resources.levelCount, blockColor->format()};
      auto& blockPipeline = ensureBlockIdPipeline(blockKey, blockColor->format());
      ensureBlockIdCompactionPipeline();

      const vk::Extent3D extent = blockColor->extent();
      const uint32_t imgW = std::max<uint32_t>(1u, extent.width);
      const uint32_t imgH = std::max<uint32_t>(1u, extent.height);
      const uint32_t capacityIDs = std::max<uint32_t>(1u, imgW * imgH * 4u);
      const uint32_t headerWords = 1u + 8u; // [count][counts[8]]
      const size_t bytesPerSlice = static_cast<size_t>(headerWords + capacityIDs) * sizeof(uint32_t);
      const size_t headerBytes = static_cast<size_t>(headerWords) * sizeof(uint32_t);
      const size_t mapBytes = bytesPerSlice;

      void* frameKey = m_backend.activeFrameKey();
      CHECK(frameKey != nullptr) << "Slice block-ID compaction requested with no active Vulkan frame";

      auto& frameOutputs = m_blockIdOutputsByFrame[frameKey];
      if (frameOutputs.bytesPerSlice != bytesPerSlice) {
        frameOutputs.bytesPerSlice = bytesPerSlice;
        frameOutputs.sliceOutputs.clear();
      }
      if (frameOutputs.sliceOutputs.size() < m_sliceDrawRanges.size()) {
        frameOutputs.sliceOutputs.resize(m_sliceDrawRanges.size());
      }
      for (size_t sliceIndex = 0; sliceIndex < m_sliceDrawRanges.size(); ++sliceIndex) {
        if (!frameOutputs.sliceOutputs[sliceIndex]) {
          frameOutputs.sliceOutputs[sliceIndex] = m_backend.device().createBuffer(
            bytesPerSlice,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
          CHECK(frameOutputs.sliceOutputs[sliceIndex] != nullptr) << "Slice block-ID: failed to allocate output buffer";
        }
      }

      const std::vector<vk::DescriptorSet> descriptorSets = collectSliceDescriptorSets(resources, /*usePaging=*/true);

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

      const BlockIdCompactionSource compSource = vkBlockIdCompactionSource();
      const bool storageRead = (compSource == BlockIdCompactionSource::Storage);
      const bool bufferRead = (compSource == BlockIdCompactionSource::Buffer);

      for (size_t sliceIndex = 0; sliceIndex < m_sliceDrawRanges.size(); ++sliceIndex) {
        processEventsAndMaybeCancel(cancellationToken);

        const auto& range = m_sliceDrawRanges[sliceIndex];
        ZVulkanBuffer* outBuffer = frameOutputs.sliceOutputs[sliceIndex].get();
        CHECK(outBuffer != nullptr);

        // Zero the output header before dispatch (count + counts[]).
        if (void* mapped = outBuffer->map(0, headerBytes)) {
          std::memset(mapped, 0x00, headerBytes);
          outBuffer->unmap();
        }

        if (range.vertexCount == 0) {
          continue;
        }

        ZVulkanAttachmentInfo colorInfo{};
        colorInfo.image = blockColor->image();
        colorInfo.view = blockColor->imageView();
        colorInfo.format = blockColor->format();
        colorInfo.initialLayout = blockColor->layout();
        colorInfo.finalLayout = storageRead ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal;
        colorInfo.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f})};
        colorInfo.loadOp = vk::AttachmentLoadOp::eClear;
        colorInfo.storeOp = vk::AttachmentStoreOp::eStore;
        colorInfo.srcStage = {};
        // Compaction reads the block-ID output in compute.
        colorInfo.dstStage = vk::PipelineStageFlagBits2::eComputeShader;
        colorInfo.srcAccess = {};
        colorInfo.dstAccess = vk::AccessFlagBits2::eShaderRead;
        colorInfo.aspect = vk::ImageAspectFlagBits::eColor;
        colorInfo.trackingTexture = blockColor;

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
        CHECK((range.vertexOffsetBytes % sizeof(SliceVertex)) == 0u)
          << "Slice vertex offset not aligned to vertex stride";
        spec.vertexCount = range.vertexCount;
        spec.firstVertex = static_cast<uint32_t>(range.vertexOffsetBytes / sizeof(SliceVertex));
        spec.instanceCount = 1;
        spec.pushConstantsData = &pcB;
        spec.pushConstantsSize = sizeof(pcB);
        spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
        spec.requirePushConstants = true;

        recorder.recordGraphicsPass(spec);

        // Buffer-path compaction: copy the block-ID image into an SSBO, then compact.
        if (bufferRead) {
          const size_t needed = static_cast<size_t>(imgW) * imgH * sizeof(uint32_t) * 4ull;
          if (!m_blockIdPixelBuffer || m_blockIdPixelBufferCapacity < needed) {
            m_blockIdPixelBuffer = m_backend.device().createBuffer(needed,
                                                                   vk::BufferUsageFlagBits::eTransferDst |
                                                                     vk::BufferUsageFlagBits::eStorageBuffer,
                                                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
            m_blockIdPixelBufferCapacity = needed;
          }
          CHECK(m_blockIdPixelBuffer) << "Slice block-ID compaction: failed to allocate pixel buffer";

          blockColor->transitionLayout(cmd,
                                       blockColor->layout(),
                                       vk::ImageLayout::eTransferSrcOptimal,
                                       vk::ImageAspectFlagBits::eColor);
          vk::BufferImageCopy region{};
          region.bufferOffset = 0;
          region.bufferRowLength = 0;
          region.bufferImageHeight = 0;
          region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
          region.imageSubresource.mipLevel = 0;
          region.imageSubresource.baseArrayLayer = 0;
          region.imageSubresource.layerCount = 1;
          region.imageOffset = vk::Offset3D{0, 0, 0};
          region.imageExtent = vk::Extent3D{imgW, imgH, 1};
          cmd.copyImageToBuffer(blockColor->image(),
                                vk::ImageLayout::eTransferSrcOptimal,
                                m_blockIdPixelBuffer->buffer(),
                                region);

          // Restore to sampled layout for any downstream use/debug.
          blockColor->transitionLayout(cmd,
                                       vk::ImageLayout::eTransferSrcOptimal,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       vk::ImageAspectFlagBits::eColor);

          // Make transfer writes visible to compute reads.
          vk::BufferMemoryBarrier2 bufBarrier{};
          bufBarrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
          bufBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
          bufBarrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
          bufBarrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
          bufBarrier.buffer = m_blockIdPixelBuffer->buffer();
          bufBarrier.offset = 0;
          bufBarrier.size = VK_WHOLE_SIZE;
          vk::DependencyInfo dep{};
          dep.bufferMemoryBarrierCount = 1;
          dep.pBufferMemoryBarriers = &bufBarrier;
          cmd.pipelineBarrier2(dep);
        }

        ZVulkanComputePassSpec compSpec{};
        if (bufferRead) {
          compSpec.pipeline = &*m_blockIdCompactPipelineBufferAppend;
          compSpec.pipelineLayout = &*m_blockIdCompactPipelineLayoutBuffer;
        } else if (storageRead) {
          compSpec.pipeline = &*m_blockIdCompactPipelineStorage;
          compSpec.pipelineLayout = &*m_blockIdCompactPipelineLayoutStorage;
        } else {
          compSpec.pipeline = &*m_blockIdCompactPipelineSampled;
          compSpec.pipelineLayout = &*m_blockIdCompactPipelineLayoutSampled;
        }
        compSpec.descriptorSetFirst = 0;
        compSpec.expectedDescriptorSetCount = 1;
        compSpec.groupX = (imgW + 15) / 16;
        compSpec.groupY = (imgH + 15) / 16;
        compSpec.groupZ = 1;

        auto* ds = bufferRead    ? m_backend.allocateOverrideDescriptorSet(**m_blockIdCompactSetLayoutBuffer)
                   : storageRead ? m_backend.allocateOverrideDescriptorSet(**m_blockIdCompactSetLayoutStorage)
                                 : m_backend.allocateOverrideDescriptorSet(**m_blockIdCompactSetLayoutSampled);
        CHECK(ds) << "Slice block-ID compaction: failed to allocate override descriptor set";
        if (bufferRead) {
          ds->updateStorageBuffer(0, *m_blockIdPixelBuffer);
        } else if (storageRead) {
          blockColor->setDescriptorLayout(vk::ImageLayout::eGeneral);
          ds->updateStorageImage(0, *blockColor, vk::ImageLayout::eGeneral, vk::ImageAspectFlagBits::eColor);
        } else {
          blockColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
          ds->updateTexture(0,
                            *blockColor,
                            m_backend.nearestClampSampler(),
                            vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::ImageAspectFlags{});
        }
        ds->updateStorageBuffer(1, *outBuffer);
        compSpec.descriptorSets = {ds->descriptorSet()};

        struct PC
        {
          uint32_t width;
          uint32_t height;
          uint32_t stride;
          uint32_t capacity;
          uint32_t att;
        } pc{imgW, imgH, imgW, capacityIDs, 0u};
        compSpec.pushConstantsData = &pc;
        compSpec.pushConstantsSize = sizeof(pc);
        compSpec.pushConstantsStages = vk::ShaderStageFlagBits::eCompute;
        recorder.recordComputePass(compSpec);
      }

      // After the frame completion safe point: union the compacted IDs and
      // upload missing blocks for this channel.
      m_backend.scheduleAfterCurrentFrameCompletion(
        [this,
         frameKey,
         bytesPerSlice,
         mapBytes,
         capacityIDs,
         streamKey = payload.streamKey,
         eye = batch.eye,
         channelCountU32 = static_cast<uint32_t>(channelCount),
         channelIndexSize = static_cast<size_t>(channelIndex),
         roundIndex,
         imagePtr = payload.image]() mutable {
          if (!imagePtr) {
            return;
          }

          auto it = m_blockIdOutputsByFrame.find(frameKey);
          if (it == m_blockIdOutputsByFrame.end()) {
            return;
          }
          auto& frameOutputs = it->second;
          if (frameOutputs.bytesPerSlice != bytesPerSlice) {
            return;
          }

          std::unordered_set<uint32_t> unique;
          unique.reserve(1024);

          for (const auto& bufOwner : frameOutputs.sliceOutputs) {
            ZVulkanBuffer* buf = bufOwner.get();
            if (!buf) {
              continue;
            }
            const void* mapped = buf->map(0, mapBytes);
            CHECK(mapped);
            const uint32_t* u32 = static_cast<const uint32_t*>(mapped);
            const uint32_t count = u32[0];
            const uint32_t clamped = std::min<uint32_t>(count, capacityIDs);
            for (uint32_t i = 0; i < clamped; ++i) {
              const uint32_t v = u32[headerWords + i];
              if (v != kInvalidBlockID && v != kUnmappedBlockID) {
                unique.insert(v);
              }
            }
            buf->unmap();
          }

          std::vector<uint32_t> missingBlocks;
          missingBlocks.reserve(unique.size());
          missingBlocks.insert(missingBlocks.end(), unique.begin(), unique.end());

          if (!missingBlocks.empty()) {
            auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
            ZBenchTimer timer(fmt::format("vulkan_slice_channel_{}", channelIndexSize));
            imagePtr->updateAndUploadPageDirectoryCaches(missingBlocks,
                                                         channelIndexSize,
                                                         cancellationToken,
                                                         timer,
                                                         static_cast<uint32_t>(roundIndex));
          }

          if (m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
              m_deferredProgressive->eye == eye) {
            m_deferredProgressive.reset();
          }

          // Advance to the next progressive stage (round++). The backend will consume this
          // on the next record() for the stream/eye.
          if (streamKey != 0) {
            m_pendingFinalization =
              Finalization{.streamKey = streamKey, .eye = eye, .lastRound = false, .channelCount = channelCountU32};
          }
        },
        "VK slice blockid compaction parse");
    };

  if (channelCount == 1) {
    // Progressive Vulkan paging for slices (GL parity: preview -> full-res):
    // - channelIndexRaw < 0 : fast preview and initialize progressive state (progress=0.5)
    // - roundIndexRaw == 0  : block-ID discovery + cache upload (async)
    // - roundIndexRaw > 0   : paged full-res draw (final), then advance channel (done for single-channel)
    if (usePaging && payload.streamKey != 0) {
      const int32_t rawIdx = payload.channelIndexRaw;
      const int32_t rawRound = payload.roundIndexRaw;
      CHECK_GE(rawRound, 0);

      auto recordFastSingle = [&]() {
        SlicePipelineKey sliceKey;
        sliceKey.validInput = true;
        sliceKey.levelCount = 1u;
        sliceKey.colorFormats = formats.colorFormats;
        sliceKey.depthFormat = formats.depthFormat;
        auto& pipeline = ensureSlicePipeline(sliceKey, formats);

        if (formats.depthFormat) {
          CHECK(finalDepthAttachment.has_value())
            << "Slice single-channel fast path requires a depth attachment when depth format is present";
        }

        std::vector<vk::DescriptorSet> descriptorSets = collectSliceDescriptorSets(m_channelResources[0],
                                                                                   /*usePaging=*/false);

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
        spec.expectedDescriptorSetCount = 1;
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

      if (rawIdx < 0) {
        // Preview: render fast slice, then request renderer to flip channelIdx from -1 -> 0.
        recordFastSingle();
        m_pendingFinalization =
          Finalization{.streamKey = payload.streamKey, .eye = batch.eye, .lastRound = false, .channelCount = 1u};
        return;
      }

      CHECK_EQ(rawIdx, 0) << "Slice single-channel paging expects channelIndexRaw==0";

      if (rawRound == 0) {
        // Stage 0: schedule cache discovery/upload once, while continuing to show the fast preview.
        const bool advancePending = m_pendingFinalization && m_pendingFinalization->streamKey == payload.streamKey &&
                                    m_pendingFinalization->eye == batch.eye;
        const bool inFlight =
          advancePending || (m_deferredProgressive && m_deferredProgressive->streamKey == payload.streamKey &&
                             m_deferredProgressive->eye == batch.eye);
        if (!inFlight) {
          m_deferredProgressive =
            DeferredProgressive{.streamKey = payload.streamKey, .eye = batch.eye, .channelCount = 1u};
          scheduleBlockIdDiscoveryAndCacheUpload(/*channelIndex=*/0u,
                                                 m_channelResources[0],
                                                 channels[0],
                                                 /*roundIndex=*/0u);
        }

        recordFastSingle();
        return;
      }

      // Stage 1+: paged draw using the current cache state, then advance to completion.
      auto& channelInputs = channels[0];
      CHECK(channelInputs.pageDirectory && channelInputs.pageTable) << "Slice paging missing page tables for channel 0";
      const glm::uvec3 cacheSize = payload.image->imageCacheSize();
      ZVulkanTexture* imageCache =
        m_backend.device().residencyManager().pagedImageCacheTexture(payload.image, /*channel=*/0u, cacheSize);
      CHECK(imageCache != nullptr) << "Slice paging missing residency image cache for channel 0";
      if (!updatePagedDescriptors(m_channelResources[0],
                                  *channelInputs.pageDirectory,
                                  *channelInputs.pageTable,
                                  *imageCache,
                                  *channelInputs.volume,
                                  *channelInputs.colormap,
                                  *payload.image,
                                  /*channel=*/0u,
                                  zeToScreenPixelVoxelSize)) {
        return;
      }

      SlicePipelineKey sliceKey;
      sliceKey.validInput = true;
      sliceKey.levelCount = m_channelResources[0].levelCount;
      sliceKey.colorFormats = formats.colorFormats;
      sliceKey.depthFormat = formats.depthFormat;
      auto& pipeline = ensureSlicePipeline(sliceKey, formats);

      if (formats.depthFormat) {
        CHECK(finalDepthAttachment.has_value())
          << "Slice single-channel path requires a depth attachment when depth format is present";
      }

      std::vector<vk::DescriptorSet> descriptorSets =
        collectSliceDescriptorSets(m_channelResources[0], /*usePaging=*/true);

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
      spec.expectedDescriptorSetCount = 3;
      spec.vertexBuffers = {m_vertexBuffer->buffer()};
      spec.vertexOffsets = {0};
      spec.vertexCount = static_cast<uint32_t>(m_vertexCount);
      spec.instanceCount = 1;
      spec.pushConstantsData = &pc;
      spec.pushConstantsSize = sizeof(pc);
      spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
      spec.requirePushConstants = true;

      recorder.recordGraphicsPass(spec);

      m_pendingFinalization =
        Finalization{.streamKey = payload.streamKey, .eye = batch.eye, .lastRound = true, .channelCount = 1u};
      return;
    }

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

  if (usePaging) {
    // Progressive orchestration (GL parity):
    // - channelIndexRaw < 0 : fast preview, then start paging rounds
    // - roundIndexRaw == 0  : block-ID discovery + cache upload (deferred)
    // - roundIndexRaw > 0   : draw one channel using paging (final for that channel)
    if (payload.streamKey != 0) {
      const int32_t rawIdx = payload.channelIndexRaw;
      const int32_t rawRound = payload.roundIndexRaw;
      CHECK_GE(rawRound, 0);

      auto clearAllLayers = [&]() {
        vk::ImageSubresourceRange colorRange{vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, layerCount};
        layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eTransferDstOptimal);
        vk::ClearColorValue layerClearColor{
          std::array<float, 4>{0.f, 0.f, 0.f, 0.f}
        };
        cmd.clearColorImage(layerColor->image(), vk::ImageLayout::eTransferDstOptimal, layerClearColor, colorRange);
        layerColor->transitionLayout(cmd,
                                     vk::ImageLayout::eTransferDstOptimal,
                                     vk::ImageLayout::eColorAttachmentOptimal);

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
      };

      auto clearLayer = [&](uint32_t layerIndex) {
        vk::ImageSubresourceRange colorRange{vk::ImageAspectFlagBits::eColor, 0u, 1u, layerIndex, 1u};
        layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eTransferDstOptimal);
        vk::ClearColorValue layerClearColor{
          std::array<float, 4>{0.f, 0.f, 0.f, 0.f}
        };
        cmd.clearColorImage(layerColor->image(), vk::ImageLayout::eTransferDstOptimal, layerClearColor, colorRange);

        vk::ImageSubresourceRange depthRange{vk::ImageAspectFlagBits::eDepth, 0u, 1u, layerIndex, 1u};
        layerDepth->transitionLayout(cmd,
                                     layerDepth->layout(),
                                     vk::ImageLayout::eTransferDstOptimal,
                                     vk::ImageAspectFlagBits::eDepth);
        cmd.clearDepthStencilImage(layerDepth->image(),
                                   vk::ImageLayout::eTransferDstOptimal,
                                   vk::ClearDepthStencilValue{1.0f, 0u},
                                   depthRange);
      };

      if (rawIdx < 0) {
        // Preview: render all channels via the fast path into the layer array.
        clearAllLayers();
        for (uint32_t idx = 0; idx < channelCount; ++idx) {
          processEventsAndMaybeCancel(cancellationToken);
          recordChannelDraw(idx,
                            m_channelResources[idx],
                            *layerColor,
                            layerDepth,
                            idx,
                            /*paging=*/false,
                            vk::AttachmentLoadOp::eLoad,
                            /*finalizeToSampled=*/true,
                            recorder);
        }

        if (m_deferredProgressive && m_deferredProgressive->streamKey == payload.streamKey &&
            m_deferredProgressive->eye == batch.eye) {
          m_deferredProgressive.reset();
        }

        m_pendingFinalization = Finalization{.streamKey = payload.streamKey,
                                             .eye = batch.eye,
                                             .lastRound = false,
                                             .channelCount = static_cast<uint32_t>(channelCount)};
      } else {
        CHECK_LT(static_cast<uint32_t>(rawIdx), channelCount) << "Slice paging channelIndexRaw out of range";
        const uint32_t activeChannel = static_cast<uint32_t>(rawIdx);

        if (rawRound == 0) {
          // Cache discovery/upload stage: submit block-ID work once and keep the preview layers.
          const bool advancePending = m_pendingFinalization && m_pendingFinalization->streamKey == payload.streamKey &&
                                      m_pendingFinalization->eye == batch.eye;
          const bool inFlight =
            advancePending || (m_deferredProgressive && m_deferredProgressive->streamKey == payload.streamKey &&
                               m_deferredProgressive->eye == batch.eye);
          if (!inFlight) {
            m_deferredProgressive = DeferredProgressive{.streamKey = payload.streamKey,
                                                        .eye = batch.eye,
                                                        .channelCount = static_cast<uint32_t>(channelCount)};
            scheduleBlockIdDiscoveryAndCacheUpload(activeChannel,
                                                   m_channelResources[activeChannel],
                                                   channels[activeChannel],
                                                   /*roundIndex=*/0u);
          }
        } else {
          // Draw stage: update just the active channel layer with paging enabled.
          if (m_deferredProgressive && m_deferredProgressive->streamKey == payload.streamKey &&
              m_deferredProgressive->eye == batch.eye) {
            m_deferredProgressive.reset();
          }

          auto& resources = m_channelResources[activeChannel];
          auto& channelInputs = channels[activeChannel];
          CHECK(channelInputs.pageDirectory && channelInputs.pageTable)
            << "Slice paging missing page tables for channel " << activeChannel;
          const glm::uvec3 cacheSize = payload.image->imageCacheSize();
          ZVulkanTexture* imageCache =
            m_backend.device().residencyManager().pagedImageCacheTexture(payload.image, activeChannel, cacheSize);
          CHECK(imageCache != nullptr) << "Slice paging missing residency image cache for channel " << activeChannel;
          if (updatePagedDescriptors(resources,
                                     *channelInputs.pageDirectory,
                                     *channelInputs.pageTable,
                                     *imageCache,
                                     *channelInputs.volume,
                                     *channelInputs.colormap,
                                     *payload.image,
                                     activeChannel,
                                     zeToScreenPixelVoxelSize)) {
            clearLayer(activeChannel);
            recordChannelDraw(activeChannel,
                              resources,
                              *layerColor,
                              layerDepth,
                              activeChannel,
                              /*paging=*/true,
                              vk::AttachmentLoadOp::eLoad,
                              /*finalizeToSampled=*/true,
                              recorder);
          }

          m_pendingFinalization = Finalization{.streamKey = payload.streamKey,
                                               .eye = batch.eye,
                                               .lastRound = true,
                                               .channelCount = static_cast<uint32_t>(channelCount)};
        }
      }
    }
  } else {
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
      processEventsAndMaybeCancel(cancellationToken);
      recordChannelDraw(idx,
                        m_channelResources[idx],
                        *layerColor,
                        layerDepth,
                        idx,
                        usePaging,
                        vk::AttachmentLoadOp::eLoad,
                        /*finalizeToSampled=*/true,
                        recorder);
    }
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
      const auto completion = backend->awaitCurrentFrameCompletion("VK debug save slice layers");
      auto keepAlive = currentRenderThreadExecutorKeepAlive("VK debug save slice layers");
      auto task = [completion, leaseRef, saveLayerCount]() mutable -> folly::coro::Task<void> {
        co_await completion;
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
      };
      startCoroTaskChecked(folly::coro::co_withExecutor(std::move(keepAlive), task()), "VK debug save slice layers");
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
  processEventsAndMaybeCancel(cancellationToken);

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
      const auto completion = backend->awaitCurrentFrameCompletion("VK debug save slice merge output");
      auto keepAlive = currentRenderThreadExecutorKeepAlive("VK debug save slice merge output");
      auto task =
        [this, completion, handles = std::move(colorHandles), depthHandle]() mutable -> folly::coro::Task<void> {
        co_await completion;
        const AttachmentHandle& handle = handles.front();
        CHECK(handle.valid() && handle.backend == RenderBackend::Vulkan)
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

        if (depthHandle && depthHandle->valid() && depthHandle->backend == RenderBackend::Vulkan) {
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
      };
      startCoroTaskChecked(folly::coro::co_withExecutor(std::move(keepAlive), task()),
                           "VK debug save slice merge output");
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
    m_sliceDrawRanges.clear();
    return;
  }

  std::vector<SliceVertex> vertices;
  vertices.reserve(estimatedVertices);

  m_sliceDrawRanges.clear();
  m_sliceDrawRanges.resize(slices.size());

  for (size_t sliceIndex = 0; sliceIndex < slices.size(); ++sliceIndex) {
    const auto& slice = slices[sliceIndex];
    const size_t startVertex = vertices.size();
    const size_t triCount = slice.numTriangles();
    if (triCount != 0) {
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

    const size_t sliceVertexCount = vertices.size() - startVertex;
    CHECK(sliceVertexCount <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
      << "Slice vertex count exceeds Vulkan draw limits";
    m_sliceDrawRanges[sliceIndex] =
      SliceDrawRange{.vertexOffsetBytes = static_cast<vk::DeviceSize>(startVertex * sizeof(SliceVertex)),
                     .vertexCount = static_cast<uint32_t>(sliceVertexCount)};
  }

  m_vertexCount = vertices.size();
  CHECK(m_vertexCount <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    << "Slice vertex buffer exceeds Vulkan draw limits";
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
    resources.volumeTexture->uploadData(data, byteSize, vk::ImageLayout::eShaderReadOnlyOptimal);
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
    vulkan::ensure1DLUTTexture(device,
                               resources.colormapTexture,
                               kColormapWidth,
                               vk::Format::eR8G8B8A8Unorm,
                               vk::ImageLayout::eShaderReadOnlyOptimal);
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
      vulkan::uploadLUT(*resources.colormapTexture,
                        texels.data(),
                        texels.size(),
                        vk::ImageLayout::eShaderReadOnlyOptimal);
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
  // Integer 3D samplers must use nearest sampling on Vulkan/Metal
  resources.pagedTextureDescriptor->updateTexture(0, pageDirectory, m_backend.nearestClampSampler());
  resources.pagedTextureDescriptor->updateTexture(1, pageTable, m_backend.nearestClampSampler());
  resources.pagedTextureDescriptor->updateTexture(2, imageCache, m_backend.defaultSampler());
  resources.pagedTextureDescriptor->updateTexture(3, volume, m_backend.defaultSampler());
  resources.pagedTextureDescriptor->updateTexture(4, colormap, m_backend.defaultSampler());

  const uint32_t devCap = deviceLevelCap(m_backend.device());
  const uint32_t levelCount = static_cast<uint32_t>(std::min<size_t>(image.numLevels(), devCap));
  auto pageData = buildPageDataBuffer(image, channel, zeToScreenPixelVoxelSize, levelCount);
  if (!resources.pageDataBuffer || resources.pageDataCapacity < pageData.size()) {
    resources.pageDataBuffer = m_backend.device().createBuffer(pageData.size(),
                                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  CHECK(resources.pageDataBuffer) << "Failed to allocate Vulkan slice paging uniform buffer.";
  resources.pageDataBuffer->copyData(pageData.data(), pageData.size());
  resources.pageDataCapacity = pageData.size();

  if (!resources.pageDescriptor) {
    resources.pageDescriptor = m_backend.allocateOverrideDescriptorSet(**m_slicePageSetLayout);
  }
  CHECK(resources.pageDescriptor != nullptr) << "Slice page params: override descriptor allocation failed (fatal)";
  resources.pageDescriptor->updateUniformBuffer(2, *resources.pageDataBuffer);

  resources.levelCount = levelCount;
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
    const auto depthUse =
      vulkan::resolveExternalImageUse(ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth);
    m_mergeDescriptor->updateTexture(1,
                                     *depthArray,
                                     m_backend.defaultSampler(),
                                     depthUse.layout,
                                     depthUse.descriptorAspect);
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

void ZVulkanImgSlicePipelineContext::ensureBlockIdCompactionPipeline()
{
  auto& device = m_backend.device().context().device();
  const BlockIdCompactionSource source = vkBlockIdCompactionSource();
  static const std::string shaderBase = nim::ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  auto sourceTag = [](BlockIdCompactionSource s) -> const char* {
    switch (s) {
      case BlockIdCompactionSource::Buffer:
        return "buffer";
      case BlockIdCompactionSource::Storage:
        return "storage";
      case BlockIdCompactionSource::Sampled:
        return "sampled";
    }
    return "<unknown>";
  };

  bool created = false;

  auto ensureBuffer = [&]() {
    if (m_blockIdCompactPipelineBufferAppend) {
      return;
    }
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute}
    };
    if (!m_blockIdCompactSetLayoutBuffer) {
      m_blockIdCompactSetLayoutBuffer.emplace(
        device,
        vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()});
    }
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 5};
    if (!m_blockIdCompactPipelineLayoutBuffer) {
      m_blockIdCompactPipelineLayoutBuffer.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                     .pSetLayouts = &**m_blockIdCompactSetLayoutBuffer,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc});
    }

    const std::string compPath = shaderBase + "block_id_compact_buffer_append.comp.spv";
    auto spirv = readSpirvFile(compPath);
    vk::raii::ShaderModule compModule(
      device,
      vk::ShaderModuleCreateInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()});
    vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                            .module = *compModule,
                                            .pName = "main"};
    m_blockIdCompactPipelineBufferAppend.emplace(
      device,
      nullptr,
      vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_blockIdCompactPipelineLayoutBuffer});
    created = true;
  };

  auto ensureStorage = [&]() {
    if (m_blockIdCompactPipelineStorage) {
      return;
    }
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eStorageImage,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute}
    };
    if (!m_blockIdCompactSetLayoutStorage) {
      m_blockIdCompactSetLayoutStorage.emplace(
        device,
        vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()});
    }
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 5};
    if (!m_blockIdCompactPipelineLayoutStorage) {
      m_blockIdCompactPipelineLayoutStorage.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                     .pSetLayouts = &**m_blockIdCompactSetLayoutStorage,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc});
    }

    const std::string compPath = shaderBase + "block_id_compact_storage_append.comp.spv";
    auto spirv = readSpirvFile(compPath);
    vk::raii::ShaderModule compModule(
      device,
      vk::ShaderModuleCreateInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()});
    vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                            .module = *compModule,
                                            .pName = "main"};
    m_blockIdCompactPipelineStorage.emplace(
      device,
      nullptr,
      vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_blockIdCompactPipelineLayoutStorage});
    created = true;
  };

  auto ensureSampled = [&]() {
    if (m_blockIdCompactPipelineSampled) {
      return;
    }
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute}
    };
    if (!m_blockIdCompactSetLayoutSampled) {
      m_blockIdCompactSetLayoutSampled.emplace(
        device,
        vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()});
    }
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 5};
    if (!m_blockIdCompactPipelineLayoutSampled) {
      m_blockIdCompactPipelineLayoutSampled.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                     .pSetLayouts = &**m_blockIdCompactSetLayoutSampled,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc});
    }

    const std::string compPath = shaderBase + "block_id_compact_append.comp.spv";
    auto spirv = readSpirvFile(compPath);
    vk::raii::ShaderModule compModule(
      device,
      vk::ShaderModuleCreateInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()});
    vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                            .module = *compModule,
                                            .pName = "main"};
    m_blockIdCompactPipelineSampled.emplace(
      device,
      nullptr,
      vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_blockIdCompactPipelineLayoutSampled});
    created = true;
  };

  switch (source) {
    case BlockIdCompactionSource::Buffer:
      ensureBuffer();
      break;
    case BlockIdCompactionSource::Storage:
      ensureStorage();
      break;
    case BlockIdCompactionSource::Sampled:
      ensureSampled();
      break;
  }

  if (created && VLOG_IS_ON(1)) {
    const std::string compPath =
      (source == BlockIdCompactionSource::Buffer)    ? (shaderBase + "block_id_compact_buffer_append.comp.spv")
      : (source == BlockIdCompactionSource::Storage) ? (shaderBase + "block_id_compact_storage_append.comp.spv")
                                                     : (shaderBase + "block_id_compact_append.comp.spv");
    VLOG(1) << fmt::format("ensureSliceBlockIdCompactionPipeline: source={} shader='{}'", sourceTag(source), compPath);
  }
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
