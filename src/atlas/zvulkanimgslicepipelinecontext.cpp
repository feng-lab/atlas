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
#include "zrenderthreadexecutor_tls.h"

#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>
#include <folly/executors/GlobalExecutor.h>
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

DECLARE_bool(atlas_debug_save_slice_layers);
DECLARE_bool(atlas_debug_save_slice_merge_out);
DECLARE_string(atlas_debug_save_dir);
DECLARE_string(atlas_vk_blockid_compaction_source);

namespace nim {

namespace {

constexpr float kQuadDepth = 0.0f;
constexpr uint32_t kInvalidBlockID = 0u;
constexpr uint32_t kUnmappedBlockID = 0xFFFFFFFFu;
constexpr uint32_t kBlockIdCompactionHeaderWords = 1u + 8u; // [count][counts[8]]

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
  CHECK_GE(pageDirectoryBases.size(), levelCount)
    << "Insufficient pageDirectoryBases for requested levels: have=" << pageDirectoryBases.size()
    << " need=" << levelCount;
  CHECK_GE(imageDimensions.size(), levelCount)
    << "Insufficient imageDimensionsLevels for requested levels: have=" << imageDimensions.size()
    << " need=" << levelCount;
  CHECK_GE(voxelWorldSizes.size(), levelCount)
    << "Insufficient voxelWorldSizesLevels for requested levels: have=" << voxelWorldSizes.size()
    << " need=" << levelCount;
  CHECK_GE(posToBlockIDs.size(), levelCount)
    << "Insufficient posToBlockIDsLevels for requested levels: have=" << posToBlockIDs.size() << " need=" << levelCount;

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

struct SliceSingleChannelBindlessUBOStd140
{
  uint32_t volume_1 = 0u;
  uint32_t colormap_1 = 0u;
  uint32_t _pad0 = 0u;
  uint32_t _pad1 = 0u;
  uint32_t _pad2 = 0u;
  uint32_t _pad3 = 0u;
  uint32_t _pad4 = 0u;
  uint32_t _pad5 = 0u;
};

static_assert(sizeof(SliceSingleChannelBindlessUBOStd140) == 32u,
              "Slice bindless indices UBO must match backend-shared imgIndices descriptor range (8 uints / 32B)");

// Paged-slice bindless texture indices (matches shader std140 packing).
struct SlicePagedBindlessUBOStd140
{
  uint32_t page_directory = 0u;
  uint32_t page_table_cache = 0u;
  uint32_t image_cache = 0u;
  uint32_t volume = 0u;
  uint32_t colormap = 0u;
  uint32_t _pad0 = 0u;
  uint32_t _pad1 = 0u;
  uint32_t _pad2 = 0u;
};

static_assert(sizeof(SlicePagedBindlessUBOStd140) == 32u,
              "Slice paged bindless indices UBO must match GLSL std140 packing (5 uints + padding)");

struct Image2DArrayCompositorPushConstants
{
  uint32_t color_texture = 0u;
  uint32_t depth_texture = 0u;
};

static_assert(sizeof(Image2DArrayCompositorPushConstants) == 8u,
              "Image2DArrayCompositor push constants must match GLSL packing (2 uints)");

} // namespace

ZVulkanImgSlicePipelineContext::ZVulkanImgSlicePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanImgSlicePipelineContext::~ZVulkanImgSlicePipelineContext() = default;

void ZVulkanImgSlicePipelineContext::preRecordBindlessWarmup(const BindlessWarmupDesc& desc)
{
  CHECK(!m_backend.isRecording()) << "Slice preRecordBindlessWarmup called while recording";
  if (!desc.image) {
    return;
  }
  if (desc.channels.empty()) {
    return;
  }

  auto& image = *desc.image;
  const size_t channelCount = image.numChannels();
  if (channelCount == 0u) {
    return;
  }
  m_channelResources.resize(channelCount);

  if (desc.wantsPaging) {
    if (!m_imageBlockUploader) {
      m_imageBlockUploader = std::make_unique<ZVulkanPagedImageBlockUploader>(m_backend.device());
    }
    CHECK(m_imageBlockUploader != nullptr) << "Slice paging warmup requested but image block uploader is missing";
    m_imageBlockUploader->bindToImage(image);
  }

  for (size_t channelIndex : desc.channels) {
    CHECK_LT(channelIndex, channelCount) << "Slice bindless warmup: channel index out of range";
    ChannelResources& resources = m_channelResources[channelIndex];

    if (desc.wantsVolume3D) {
      auto channelImage = image.channelImageShared(channelIndex);
      CHECK(channelImage != nullptr) << "Slice bindless warmup: missing channel image";
      const uint64_t generation = image.volumeGeneration(channelIndex);
      ZVulkanTexture& vol = ensureVolumeTexture(image, channelIndex, generation, channelImage);
      (void)m_backend.bindlessRegisterSampledImageAuto(vol, "slice_volume");
    }

    if (desc.wantsColormap) {
      CHECK(desc.colormaps != nullptr) << "Slice bindless warmup requested colormap but payload colormaps is null";
      CHECK_LT(channelIndex, desc.colormaps->size()) << "Slice bindless warmup: colormaps size < channel index";
      const ZColorMap* colorMap = (*desc.colormaps)[channelIndex];
      CHECK(colorMap != nullptr) << "Slice bindless warmup: null ZColorMap for channel " << channelIndex;
      ZVulkanTexture& colormap = ensureColormapTexture(channelIndex, colorMap, resources);
      (void)m_backend.bindlessRegisterSampledImageAuto(colormap, "slice_colormap");
    }

    if (desc.wantsPaging) {
      CHECK(m_imageBlockUploader != nullptr) << "Slice bindless warmup: paging uploader missing";
      ZVulkanTexture* imageCache = m_imageBlockUploader->imageCacheTexture(image, channelIndex);
      ZVulkanTexture* pageDirectory = m_imageBlockUploader->pageDirectoryTexture(image, channelIndex);
      ZVulkanTexture* pageTable = m_imageBlockUploader->pageTableTexture(image, channelIndex);
      CHECK(pageDirectory && pageTable && imageCache)
        << "Slice bindless warmup: paging textures missing for channel " << channelIndex;
      (void)m_backend.bindlessRegisterSampledImageAuto(*pageDirectory, "slice_page_directory");
      (void)m_backend.bindlessRegisterSampledImageAuto(*pageTable, "slice_page_table");
      (void)m_backend.bindlessRegisterSampledImageAuto(*imageCache, "slice_image_cache");
    }
  }
}

void ZVulkanImgSlicePipelineContext::preRecordPrimeBlockIdCompaction(
  const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& blockIdLease,
  uint32_t sliceCount,
  uint32_t sliceIndex)
{
  CHECK(!m_backend.isRecording()) << "Slice preRecordPrimeBlockIdCompaction called while recording";
  if (!blockIdLease || !blockIdLease->hasVulkanImage()) {
    return;
  }

  CHECK_GT(sliceCount, 0u) << "Slice preRecordPrimeBlockIdCompaction called with sliceCount=0";
  CHECK_LT(sliceIndex, sliceCount) << "Slice preRecordPrimeBlockIdCompaction sliceIndex out of range";

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Slice preRecordPrimeBlockIdCompaction requires an active Vulkan frame-slot key";

  ZVulkanTexture* blockColor = blockIdLease->colorAttachment(0);
  CHECK(blockColor != nullptr) << "Slice preRecordPrimeBlockIdCompaction missing blockIdLease color attachment";

  const vk::Extent3D extent = blockColor->extent();
  const uint32_t imgW = std::max<uint32_t>(1u, extent.width);
  const uint32_t imgH = std::max<uint32_t>(1u, extent.height);
  const uint32_t capacityIDs = std::max<uint32_t>(1u, imgW * imgH * 4u);
  const size_t bytesPerSlice = static_cast<size_t>(kBlockIdCompactionHeaderWords + capacityIDs) * sizeof(uint32_t);

  // Ensure device-local output buffer exists for this slice index.
  auto& frameOutputs = m_blockIdOutputsByFrame[frameKey];
  if (frameOutputs.bytesPerSlice != bytesPerSlice) {
    frameOutputs.bytesPerSlice = bytesPerSlice;
    frameOutputs.sliceOutputs.clear();
  }
  if (frameOutputs.sliceOutputs.size() < static_cast<size_t>(sliceCount)) {
    frameOutputs.sliceOutputs.resize(static_cast<size_t>(sliceCount));
  }
  if (!frameOutputs.sliceOutputs[static_cast<size_t>(sliceIndex)]) {
    frameOutputs.sliceOutputs[static_cast<size_t>(sliceIndex)] =
      m_backend.device().createBuffer(bytesPerSlice,
                                      vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                                        vk::BufferUsageFlagBits::eTransferDst,
                                      vk::MemoryPropertyFlagBits::eDeviceLocal);
    CHECK(frameOutputs.sliceOutputs[static_cast<size_t>(sliceIndex)] != nullptr)
      << "Slice block-ID compaction: failed to allocate output buffer";
  }

  ZVulkanBuffer* outBuffer = frameOutputs.sliceOutputs[static_cast<size_t>(sliceIndex)].get();
  CHECK(outBuffer != nullptr);

  // Ensure compaction pipeline/layout exists for current source.
  ensureBlockIdCompactionPipeline();

  if (m_blockIdCompactDescriptorSliceCount != sliceCount || m_blockIdCompactDescriptorBytesPerSlice != bytesPerSlice) {
    m_blockIdCompactDescriptors.clear();
    m_blockIdCompactDescriptorSliceCount = sliceCount;
    m_blockIdCompactDescriptorBytesPerSlice = bytesPerSlice;
  }
  if (m_blockIdCompactDescriptors.size() < static_cast<size_t>(sliceCount)) {
    m_blockIdCompactDescriptors.resize(static_cast<size_t>(sliceCount));
  }

  const BlockIdCompactionSource compSource = vkBlockIdCompactionSource();
  const bool storageRead = (compSource == BlockIdCompactionSource::Storage);
  const bool bufferRead = (compSource == BlockIdCompactionSource::Buffer);

  if (bufferRead) {
    CHECK(m_blockIdCompactSetLayoutBuffer.has_value()) << "Slice block-ID buffer compaction set layout missing";
    const size_t needed = static_cast<size_t>(imgW) * imgH * sizeof(uint32_t) * 4ull;
    if (!m_blockIdPixelBuffer || m_blockIdPixelBufferCapacity < needed) {
      m_blockIdPixelBuffer =
        m_backend.device().createBuffer(needed,
                                        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
                                        vk::MemoryPropertyFlagBits::eDeviceLocal);
      m_blockIdPixelBufferCapacity = needed;
    }
    CHECK(m_blockIdPixelBuffer != nullptr) << "Slice block-ID buffer compaction: failed to allocate pixel buffer";

    auto ds = m_backend.allocateFrameDescriptorSet(**m_blockIdCompactSetLayoutBuffer);
    CHECK(ds != nullptr) << "Slice block-ID buffer compaction: failed to allocate descriptor set";
    // binding 0 = output BlockList SSBO, binding 1 = input Texels SSBO
    ds->updateStorageBuffer(0, *outBuffer);
    ds->updateStorageBuffer(1, *m_blockIdPixelBuffer);
    m_blockIdCompactDescriptors[static_cast<size_t>(sliceIndex)] = std::move(ds);
    return;
  }

  if (storageRead) {
    CHECK(m_blockIdCompactSetLayoutStorage.has_value()) << "Slice block-ID storage compaction set layout missing";
    blockColor->setDescriptorLayout(vk::ImageLayout::eGeneral);

    auto ds = m_backend.allocateFrameDescriptorSet(**m_blockIdCompactSetLayoutStorage);
    CHECK(ds != nullptr) << "Slice block-ID storage compaction: failed to allocate descriptor set";
    // binding 0 = output BlockList SSBO, binding 1 = input uimage2D
    ds->updateStorageBuffer(0, *outBuffer);
    ds->updateStorageImage(1, *blockColor, vk::ImageLayout::eGeneral, vk::ImageAspectFlagBits::eColor);
    m_blockIdCompactDescriptors[static_cast<size_t>(sliceIndex)] = std::move(ds);
    return;
  }

  // Sampled (bindless) input: texelFetch(usampler2D) from set=0 bindless table.
  CHECK(m_blockIdCompactSetLayoutSampled.has_value()) << "Slice block-ID sampled compaction set layout missing";
  blockColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  (void)m_backend.bindlessRegisterSampledImageAuto(*blockColor, "slice_blockid_compact_input");

  auto ds = m_backend.allocateFrameDescriptorSet(**m_blockIdCompactSetLayoutSampled);
  CHECK(ds != nullptr) << "Slice block-ID sampled compaction: failed to allocate descriptor set";
  // binding 0 = output BlockList SSBO
  ds->updateStorageBuffer(0, *outBuffer);
  m_blockIdCompactDescriptors[static_cast<size_t>(sliceIndex)] = std::move(ds);
}

void ZVulkanImgSlicePipelineContext::resetFrame()
{
  // Per-frame descriptor sets used by slice/raycaster helpers are owned by the
  // backend and persist for the lifetime of the frame-slot. Only reset the
  // per-submission block-ID compaction descriptor state here.
  m_blockIdCompactDescriptors.clear();
  m_blockIdCompactDescriptorSliceCount = 0u;
  m_blockIdCompactDescriptorBytesPerSlice = 0u;
}

std::optional<ZVulkanImgSlicePipelineContext::Finalization> ZVulkanImgSlicePipelineContext::takePendingFinalization()
{
  auto ret = m_pendingFinalization;
  m_pendingFinalization.reset();
  return ret;
}

void ZVulkanImgSlicePipelineContext::record(Z3DRendererBase& renderer,
                                            const RenderBatch& batch,
                                            const ImgSlicePayload& payload,
                                            const vk::Viewport& viewport,
                                            const vk::Rect2D& scissor,
                                            vk::raii::CommandBuffer& cmd)
{
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  maybeCancel(cancellationToken);

  if (!payload.image || payload.slices.empty()) {
    return;
  }

  const ImgSlicePayload::Stage stage = payload.stage;
  CHECK(stage != ImgSlicePayload::Stage::Unspecified) << "Slice payload missing stage";

  const bool isCompute = (batch.pass.kind == BackendPassDesc::Kind::Compute);
  const bool isRaster = (batch.pass.kind == BackendPassDesc::Kind::Raster);
  CHECK(isCompute || isRaster) << "Slice pipeline received an unknown backend pass kind";

  const bool usePaging = !payload.fastPathOnly && payload.image->isVolumeDownsampled();
  if (usePaging) {
    CHECK(payload.streamKey != 0u) << "Vulkan slice paging requires a non-zero streamKey";
    if (!m_imageBlockUploader) {
      m_imageBlockUploader = std::make_unique<ZVulkanPagedImageBlockUploader>(m_backend.device());
    }
    CHECK(m_imageBlockUploader != nullptr);
    m_imageBlockUploader->bindToImage(*payload.image);
  }

  const size_t channelCount = payload.image->numChannels();
  if (channelCount == 0u) {
    return;
  }
  m_channelResources.resize(channelCount);

  ensureQuadVertexBuffer();

  glm::uvec2 outputSize = payload.outputSize;
  if (outputSize.x == 0u || outputSize.y == 0u) {
    CHECK(batch.pass.viewport.extent.x > 0.0f && batch.pass.viewport.extent.y > 0.0f)
      << "Slice payload missing outputSize and batch viewport is empty";
    outputSize = glm::uvec2(static_cast<uint32_t>(batch.pass.viewport.extent.x),
                            static_cast<uint32_t>(batch.pass.viewport.extent.y));
  }

  float zeToScreenPixelVoxelSize = 0.0f;
  if (usePaging) {
    const auto& viewState = renderer.viewState();
    const auto& sceneState = renderer.sceneState();
    const auto& monoEyeState = viewState.eyes[static_cast<size_t>(Z3DEye::MonoEye)];
    const float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
    const glm::vec2 pixelEyeSpaceSize =
      monoEyeState.frustumNearPlaneSize / glm::vec2(std::max(1u, outputSize.x), std::max(1u, outputSize.y));
    zeToScreenPixelVoxelSize =
      -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / nearClip * sceneState.devicePixelRatio;
  }

  auto ensureChannelInputs = [&](size_t channelIndex) -> ChannelInputs {
    ChannelInputs inputs{};
    CHECK(payload.colormaps != nullptr) << "Slice payload missing colormaps vector";
    CHECK(channelIndex < channelCount) << "Slice channelIndex out of range";
    CHECK(channelIndex < payload.colormaps->size()) << "Slice payload colormaps size < channelCount";

    const ZColorMap* colorMap = (*payload.colormaps)[channelIndex];
    CHECK(colorMap != nullptr) << "Slice payload has null ZColorMap at channel " << channelIndex;

    auto channelImage = payload.image->channelImageShared(channelIndex);
    CHECK(channelImage != nullptr) << "Slice payload missing channel image";
    const uint64_t generation = payload.image->volumeGeneration(channelIndex);
    auto& resources = m_channelResources[channelIndex];
    inputs.volume = &ensureVolumeTexture(*payload.image, channelIndex, generation, channelImage);
    inputs.colormap = &ensureColormapTexture(channelIndex, colorMap, resources);

    if (usePaging) {
      CHECK(m_imageBlockUploader != nullptr) << "Slice paging expected block uploader";
      inputs.imageCache = m_imageBlockUploader->imageCacheTexture(*payload.image, channelIndex);
      inputs.pageDirectory = m_imageBlockUploader->pageDirectoryTexture(*payload.image, channelIndex);
      inputs.pageTable = m_imageBlockUploader->pageTableTexture(*payload.image, channelIndex);
      CHECK(inputs.pageDirectory && inputs.pageTable && inputs.imageCache)
        << "Slice paging missing page directory/table for channel " << channelIndex;
    }

    return inputs;
  };

  auto ensureSliceGeometry = [&]() {
    const bool changed =
      (m_geometryStreamKey != payload.streamKey) || (m_geometrySignature != payload.slicesSignature);
    if (!changed && !m_sliceDrawRanges.empty()) {
      return;
    }
    uploadSliceGeometry(payload.slices);
    m_geometryStreamKey = payload.streamKey;
    m_geometrySignature = payload.slicesSignature;
  };

  auto channelIndexFromLayerAttachment = [&]() -> size_t {
    CHECK(!batch.pass.colorAttachments.empty()) << "Slice layered pass missing color attachments";
    const uint32_t layerIndex = batch.pass.colorAttachments.front().handle.index;
    CHECK(layerIndex < channelCount) << "Slice layer index out of range: " << layerIndex << " >= " << channelCount;
    return static_cast<size_t>(layerIndex);
  };

  ZVulkanPipelineCommandRecorder recorder(cmd);

  // ---------------------------------------------------------------------------
  // Block-ID discovery (paging): per-slice raster + per-slice compute compaction.
  // ---------------------------------------------------------------------------
  if (stage == ImgSlicePayload::Stage::BlockIdDiscovery) {
    if (!usePaging || payload.streamKey == 0u) {
      return;
    }

    const int32_t rawIdx = payload.channelIndexRaw;
    const int32_t rawRound = payload.roundIndexRaw;
    if (rawIdx < 0 || rawRound != 0) {
      return;
    }

    const size_t channelIndex = static_cast<size_t>(rawIdx);
    CHECK(channelIndex < channelCount) << "Slice BlockIdDiscovery channel out of range";

    ChannelInputs inputs = ensureChannelInputs(channelIndex);
    auto& resources = m_channelResources[channelIndex];

    if (isRaster) {
      CHECK(payload.blockIdLease && payload.blockIdLease->hasVulkanImage())
        << "Slice BlockIdDiscovery stage missing blockIdLease";
      CHECK_GE(payload.blockIdSliceIndexRaw, 0) << "Slice BlockIdDiscovery raster requires blockIdSliceIndexRaw >= 0";

      ensureSliceGeometry();
      if (m_vertexCount == 0) {
        return;
      }
      CHECK(static_cast<size_t>(payload.blockIdSliceIndexRaw) < m_sliceDrawRanges.size())
        << "Slice BlockIdDiscovery slice index out of range: " << payload.blockIdSliceIndexRaw;
      const auto& range = m_sliceDrawRanges[static_cast<size_t>(payload.blockIdSliceIndexRaw)];

      CHECK(inputs.pageDirectory && inputs.pageTable) << "Slice BlockIdDiscovery paging tables missing";

      CHECK(m_imageBlockUploader != nullptr) << "Slice BlockIdDiscovery expected image block uploader";
      {
        ZBenchTimer uploadTimer("vulkan_upload_page_caches(slice_blockid)");
        m_imageBlockUploader->uploadPageCaches(*payload.image, channelIndex, uploadTimer);
      }

      const vk::DescriptorSet dsIndices = m_backend.sharedImgIndicesDescriptorSet();
      const vk::DescriptorSet dsPageData = m_backend.sharedImgPageDataDescriptorSet();
      CHECK(dsIndices && dsPageData)
        << "Slice BlockIdDiscovery missing backend-shared descriptor sets (indices/page-data)";

      const auto formats = vulkan::extractAttachmentFormats(batch);
      CHECK(!formats.colorFormats.empty()) << "Slice BlockIdDiscovery batch missing color attachment formats";
      const vk::Format blockFormat = formats.colorFormats.front();

      const uint32_t devCap = deviceLevelCap(m_backend.device());
      const uint32_t levelCount = static_cast<uint32_t>(std::min<size_t>(payload.image->numLevels(), devCap));
      CHECK_GT(levelCount, 0u) << "Slice BlockIdDiscovery has no paging levels";
      resources.levelCount = levelCount;

      const std::vector<uint8_t> pageData =
        buildPageDataBuffer(*payload.image, channelIndex, zeToScreenPixelVoxelSize, levelCount);
      const auto pageSlice = m_backend.suballocateUniformFor(payload, pageData.size());
      CHECK(pageSlice.mapped != nullptr) << "Slice page-data uniform slice mapping missing";
      std::memcpy(pageSlice.mapped, pageData.data(), pageData.size());
      CHECK(pageSlice.offset <= std::numeric_limits<uint32_t>::max())
        << "Slice page-data dynamic offset exceeds uint32 range: " << pageSlice.offset;
      const uint32_t pageDynOffset = static_cast<uint32_t>(pageSlice.offset);

      BlockIdPipelineKey blockKey{levelCount, blockFormat};
      auto& blockPipeline = ensureBlockIdPipeline(blockKey, blockFormat);

      SlicePagedBindlessUBOStd140 bindless{};
      bindless.page_directory =
        m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.pageDirectory, "slice_blockid_page_directory");
      bindless.page_table_cache =
        m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.pageTable, "slice_blockid_page_table");
      const auto indicesSlice = m_backend.suballocateUniformFor(payload, sizeof(bindless));
      CHECK(indicesSlice.mapped != nullptr) << "Slice BlockIdDiscovery indices uniform slice mapping missing";
      std::memcpy(indicesSlice.mapped, &bindless, sizeof(bindless));
      CHECK(indicesSlice.offset <= std::numeric_limits<uint32_t>::max())
        << "Slice BlockIdDiscovery indices dynamic offset exceeds uint32 range: " << indicesSlice.offset;
      const uint32_t indicesDynOffset = static_cast<uint32_t>(indicesSlice.offset);

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

      ZVulkanGraphicsDrawSpec drawSpec{};
      drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
      drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
      drawSpec.pipelineHandle = blockPipeline.pipeline->pipelineHandle();
      drawSpec.pipelineLayoutHandle = blockPipeline.pipeline->pipelineLayoutHandle();
      drawSpec.descriptorSetFirst = 0;
      const std::array<vk::DescriptorSet, 3> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(),
                                                            dsIndices,
                                                            dsPageData};
      const std::array<uint32_t, 2> dynamicOffsets{indicesDynOffset, pageDynOffset};
      drawSpec.descriptorSets = descriptorSets;
      drawSpec.dynamicOffsets = dynamicOffsets;
      drawSpec.expectedDescriptorSetCount = 3;

      const std::array<vk::Buffer, 1> vertexBuffers{m_vertexBuffer->buffer()};
      const std::array<vk::DeviceSize, 1> vertexOffsets{0};
      drawSpec.vertexBuffers = vertexBuffers;
      drawSpec.vertexOffsets = vertexOffsets;
      CHECK((range.vertexOffsetBytes % sizeof(SliceVertex)) == 0u)
        << "Slice vertex offset not aligned to vertex stride";
      drawSpec.vertexCount = range.vertexCount;
      drawSpec.firstVertex = static_cast<uint32_t>(range.vertexOffsetBytes / sizeof(SliceVertex));
      drawSpec.instanceCount = 1;
      drawSpec.pushConstantsData = &pc;
      drawSpec.pushConstantsSize = sizeof(pc);
      drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
      drawSpec.requirePushConstants = true;
      recorder.recordGraphicsDraw(drawSpec);
      return;
    }

    CHECK(isCompute);
    CHECK(payload.blockIdLease && payload.blockIdLease->hasVulkanImage())
      << "Slice BlockIdDiscovery missing blockIdLease";
    CHECK_GE(payload.blockIdSliceIndexRaw, 0) << "Slice BlockIdDiscovery compute requires blockIdSliceIndexRaw >= 0";

    ZVulkanTexture* blockColor = payload.blockIdLease->colorAttachment(0);
    CHECK(blockColor != nullptr) << "Slice BlockIdDiscovery missing blockId color attachment texture";

    const vk::Extent3D extent = blockColor->extent();
    const uint32_t imgW = std::max<uint32_t>(1u, extent.width);
    const uint32_t imgH = std::max<uint32_t>(1u, extent.height);
    const uint32_t capacityIDs = std::max<uint32_t>(1u, imgW * imgH * 4u);
    const size_t bytesPerSlice = static_cast<size_t>(kBlockIdCompactionHeaderWords + capacityIDs) * sizeof(uint32_t);
    const size_t headerBytes = static_cast<size_t>(kBlockIdCompactionHeaderWords) * sizeof(uint32_t);

    void* frameKey = m_backend.activeFrameKey();
    CHECK(frameKey != nullptr) << "Slice block-ID compaction requested with no active Vulkan frame";

    const size_t sliceCount = payload.slices.size();
    CHECK_GT(sliceCount, 0u) << "Slice BlockIdDiscovery missing slices span";
    CHECK(static_cast<size_t>(payload.blockIdSliceIndexRaw) < sliceCount)
      << "Slice BlockIdDiscovery slice index out of range: " << payload.blockIdSliceIndexRaw << " >= " << sliceCount;
    const size_t sliceIndex = static_cast<size_t>(payload.blockIdSliceIndexRaw);

    auto itOutputs = m_blockIdOutputsByFrame.find(frameKey);
    CHECK(itOutputs != m_blockIdOutputsByFrame.end())
      << "Slice block-ID compaction requires pre-record primed per-slice outputs";
    auto& frameOutputs = itOutputs->second;
    CHECK(frameOutputs.bytesPerSlice == bytesPerSlice)
      << "Slice block-ID compaction output size mismatch; missing pre-record priming";
    CHECK(frameOutputs.sliceOutputs.size() >= sliceCount)
      << "Slice block-ID compaction output vector too small; missing pre-record priming";
    CHECK(frameOutputs.sliceOutputs[sliceIndex] != nullptr)
      << "Slice block-ID compaction missing output buffer for sliceIndex=" << sliceIndex
      << "; missing pre-record priming";

    ZVulkanBuffer* outBuffer = frameOutputs.sliceOutputs[sliceIndex].get();
    CHECK(outBuffer != nullptr) << "Slice block-ID compaction output buffer pointer missing";

    // Zero the output header before dispatch (count + counts[8]).
    CHECK((outBuffer->usage() & vk::BufferUsageFlagBits::eTransferDst) != vk::BufferUsageFlags{})
      << "Slice Block-ID compaction output must support vkCmdFillBuffer (eTransferDst usage missing)";
    cmd.fillBuffer(outBuffer->buffer(), /*dstOffset=*/0, headerBytes, /*data=*/0u);
    // Ensure the filled header is visible to the compaction compute shader.
    {
      vk::BufferMemoryBarrier2 bb{};
      bb.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
      bb.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
      bb.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
      bb.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
      bb.buffer = outBuffer->buffer();
      bb.offset = 0;
      bb.size = headerBytes;
      vk::DependencyInfo dep{};
      dep.bufferMemoryBarrierCount = 1;
      dep.pBufferMemoryBarriers = &bb;
      cmd.pipelineBarrier2(dep);
    }

    ensureBlockIdCompactionPipeline();

    const BlockIdCompactionSource compSource = vkBlockIdCompactionSource();
    const bool storageRead = (compSource == BlockIdCompactionSource::Storage);
    const bool bufferRead = (compSource == BlockIdCompactionSource::Buffer);

    if (bufferRead) {
      const size_t needed = static_cast<size_t>(imgW) * imgH * sizeof(uint32_t) * 4ull;
      CHECK(m_blockIdPixelBuffer != nullptr && m_blockIdPixelBufferCapacity >= needed)
        << "Slice block-ID buffer compaction requires pre-record primed pixel buffer (needed=" << needed << "B)";

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

      // Layout transition to eTransferSrcOptimal is performed by the backend via externalImageUses metadata.
      cmd.copyImageToBuffer(blockColor->image(),
                            vk::ImageLayout::eTransferSrcOptimal,
                            m_blockIdPixelBuffer->buffer(),
                            region);

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
    compSpec.expectedDescriptorSetCount = 2;
    compSpec.groupX = (imgW + 15) / 16;
    compSpec.groupY = (imgH + 15) / 16;
    compSpec.groupZ = 1;

    CHECK(m_blockIdCompactDescriptorSliceCount == static_cast<uint32_t>(sliceCount) &&
          m_blockIdCompactDescriptorBytesPerSlice == bytesPerSlice)
      << "Slice block-ID compaction descriptor state mismatch; missing pre-record priming";
    CHECK(m_blockIdCompactDescriptors.size() >= sliceCount)
      << "Slice block-ID compaction descriptor vector too small; missing pre-record priming";

    auto* ds = m_blockIdCompactDescriptors[sliceIndex].get();
    CHECK(ds != nullptr) << "Slice block-ID compaction missing pre-record descriptor for sliceIndex=" << sliceIndex;

    const vk::DescriptorSet bindlessSet = m_backend.bindlessSampledImageDescriptorSet();
    const std::array<vk::DescriptorSet, 2> descriptorSets{bindlessSet, ds->descriptorSet()};
    compSpec.descriptorSets = descriptorSets;

    // Push constants: width, height, stride, capacity, att index (+ optional bindless image index).
    //
    // IMPORTANT: keep the backing storage alive until recordComputePass() pushes
    // the constants. Do not take the address of a block-scoped local and use it
    // after leaving the scope.
    std::array<uint32_t, 6> pcWords{imgW, imgH, imgW, capacityIDs, 0u, 0u};
    if (storageRead || bufferRead) {
      if (storageRead) {
        CHECK(blockColor->layout() == vk::ImageLayout::eGeneral)
          << "Slice block-ID storage compaction requires blockColor in VK_IMAGE_LAYOUT_GENERAL";
      }
      compSpec.pushConstantsSize = sizeof(uint32_t) * 5u;
    } else {
      CHECK(blockColor->layout() == vk::ImageLayout::eShaderReadOnlyOptimal)
        << "Slice block-ID sampled compaction requires blockColor in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL";
      pcWords[5] = m_backend.bindlessLookupSampledImageAutoOrCrash(*blockColor, "slice_blockid_compact_input");
      compSpec.pushConstantsSize = sizeof(uint32_t) * 6u;
    }
    compSpec.pushConstantsData = pcWords.data();
    compSpec.pushConstantsStages = vk::ShaderStageFlagBits::eCompute;

    recorder.recordComputePass(compSpec);

    if (sliceIndex + 1u == sliceCount) {
      const uint32_t roundIndexU32 = static_cast<uint32_t>(payload.roundIndexRaw);
      const std::string_view debugLabel = "VK slice blockid compaction parse";
      const std::string_view readbackLabel = "slice_block_id_compact_readback";

      // Request only the fixed-size header first. The payload range is copied
      // later only when the header reports IDs, avoiding full-capacity
      // readbacks on tiles with no missing blocks.
      std::vector<Z3DRendererVulkanBackend::EndOfFrameBufferReadbackTicket> readbackTickets;
      readbackTickets.reserve(frameOutputs.sliceOutputs.size());
      std::vector<ZVulkanBuffer*> outputBuffers;
      outputBuffers.reserve(frameOutputs.sliceOutputs.size());
      for (auto& bufOwner : frameOutputs.sliceOutputs) {
        if (!bufOwner) {
          continue;
        }
        CHECK((bufOwner->usage() & vk::BufferUsageFlagBits::eTransferSrc) != vk::BufferUsageFlags{})
          << "Slice Block-ID compaction output must support end-of-frame buffer readback (eTransferSrc usage missing)";
        CHECK_LE(headerBytes, bufOwner->size()) << "Slice Block-ID compaction header exceeds output buffer size";
        outputBuffers.push_back(bufOwner.get());
        readbackTickets.emplace_back(
          m_backend.requestEndOfFrameBufferReadbackTicket(*bufOwner, /*srcOffset=*/0, headerBytes, readbackLabel));
      }

      // Block-ID discovery cache upload is a CPU control-flow boundary: force the
      // backend to wait until the completion safe point so this hook runs before
      // client code records the next progressive stage.
      m_backend.requireCompletionSafePointWaitForActiveSubmission(debugLabel);
      m_backend.registerAfterCurrentFrameCompletionHook(
        currentRenderThreadExecutorKeepAlive(debugLabel),
        [rendererPtr = &renderer,
         bytesPerSlice,
         capacityIDs,
         imgW,
         imgH,
         tickets = std::move(readbackTickets),
         outputBuffers = std::move(outputBuffers),
         streamKey = payload.streamKey,
         eye = batch.eye,
         channelCountU32 = static_cast<uint32_t>(channelCount),
         channelIndexSize = channelIndex,
         roundIndexU32,
         cancellationToken,
         imagePtr = payload.image](Z3DRendererVulkanBackend& backend) mutable -> folly::coro::Task<void> {
          if (!imagePtr) {
            // Still release any staging slots we acquired.
            for (auto& ticket : tickets) {
              co_await ticket.awaitAndDiscard();
            }
            co_return;
          }

          std::unordered_set<uint32_t> unique;
          unique.reserve(1024);

          CHECK_GT(bytesPerSlice, 0u);
          CHECK((bytesPerSlice % sizeof(uint32_t)) == 0u) << "Slice block-ID readback bytes not u32-aligned";
          CHECK_EQ(tickets.size(), outputBuffers.size()) << "Slice block-ID readback ticket/source count mismatch";

          for (size_t i = 0; i < tickets.size(); ++i) {
            if (cancellationToken.isCancellationRequested()) {
              // Drop pending CPU work on cancellation, but still release staging
              // slots to avoid starving future readbacks.
              for (; i < tickets.size(); ++i) {
                co_await tickets[i].awaitAndDiscard();
              }
              co_return;
            }

            auto& ticket = tickets[i];
            std::array<uint32_t, kBlockIdCompactionHeaderWords> headerWords{};
            co_await ticket.awaitCopyTo(headerWords.data(), headerWords.size() * sizeof(uint32_t));
            if (cancellationToken.isCancellationRequested()) {
              for (size_t j = i + 1; j < tickets.size(); ++j) {
                co_await tickets[j].awaitAndDiscard();
              }
              co_return;
            }

            // Unified format: [count][counts[8]][ids...]
            const uint32_t count = headerWords[0];
            CHECK_LE(count, capacityIDs) << fmt::format(
              "Slice block-ID compaction count exceeds capacity (would truncate): {}x{} cap={} count={}",
              imgW,
              imgH,
              capacityIDs,
              count);
            std::vector<uint32_t> idWords;
            if (count > 0u) {
              ZVulkanBuffer* outputBuffer = outputBuffers[i];
              CHECK(outputBuffer != nullptr) << "Slice block-ID compaction payload source is null";
              const size_t idBytes = static_cast<size_t>(count) * sizeof(uint32_t);
              auto payloadBytes = backend.readBufferRangeAfterCompletion(
                *outputBuffer,
                static_cast<vk::DeviceSize>(kBlockIdCompactionHeaderWords * sizeof(uint32_t)),
                idBytes,
                "slice_block_id_compact_payload_readback");
              CHECK_EQ(payloadBytes.size(), idBytes) << "Slice block-ID compaction payload readback size mismatch";
              idWords.resize(count);
              std::memcpy(idWords.data(), payloadBytes.data(), idBytes);
            }
            for (uint32_t id = 0; id < count; ++id) {
              const uint32_t v = idWords[id];
              if (v != kInvalidBlockID && v != kUnmappedBlockID) {
                unique.insert(v);
              }
            }
          }

          std::vector<uint32_t> missingBlocks;
          missingBlocks.reserve(unique.size());
          missingBlocks.insert(missingBlocks.end(), unique.begin(), unique.end());
          std::ranges::sort(missingBlocks);

          if (!missingBlocks.empty()) {
            ZBenchTimer timer(fmt::format("vulkan_slice_channel_{}", channelIndexSize));
            imagePtr->updateAndUploadPageDirectoryCaches(missingBlocks,
                                                         channelIndexSize,
                                                         cancellationToken,
                                                         timer,
                                                         roundIndexU32);
          }

          // Advance to the next progressive stage (round++).
          if (streamKey != 0u && rendererPtr != nullptr) {
            finalizeImgSliceRoundByKey(*rendererPtr,
                                       streamKey,
                                       eye,
                                       /*lastRound=*/false,
                                       /*channelCount=*/channelCountU32);
          }
          co_return;
        },
        debugLabel);
    }
    return;
  }

  // ---------------------------------------------------------------------------
  // Slice draw (fast or paged).
  // ---------------------------------------------------------------------------
  if (stage == ImgSlicePayload::Stage::DrawLayers) {
    if (!isRaster) {
      return;
    }

    ensureSliceGeometry();
    if (m_vertexCount == 0) {
      return;
    }

    const auto formats = vulkan::extractAttachmentFormats(batch);
    if (formats.colorFormats.empty() && !formats.depthFormat) {
      return;
    }

    const bool layered = (payload.layerLease != nullptr);
    const size_t channelIndex = layered ? channelIndexFromLayerAttachment() : 0u;

    bool pagingDraw = false;
    bool setFinalization = false;
    bool finalizationLastRound = false;

    if (usePaging && payload.streamKey != 0u) {
      const int32_t rawIdx = payload.channelIndexRaw;
      const int32_t rawRound = payload.roundIndexRaw;
      CHECK_GE(rawRound, 0);

      if (channelCount == 1u) {
        if (rawIdx < 0) {
          // Preview: render fast slice, then request renderer to flip channelIdx from -1 -> 0.
          pagingDraw = false;
          setFinalization = true;
          finalizationLastRound = false;
        } else {
          CHECK_EQ(rawIdx, 0) << "Slice single-channel paging expects channelIndexRaw==0";
          if (rawRound == 0) {
            // Round 0: cache discovery/upload happens in BlockIdDiscovery; keep presenting fast preview here.
            pagingDraw = false;
          } else {
            // Round 1+: paged draw using the current cache state, then complete.
            pagingDraw = true;
            setFinalization = true;
            finalizationLastRound = true;
          }
        }
      } else {
        if (rawIdx < 0) {
          // Preview: draw all channels via fast path (the stage recorder emits one batch per layer).
          pagingDraw = false;
        } else {
          // Round 0 should not emit DrawLayers (renderer emits BlockIdDiscovery+MergeLayers).
          if (rawRound == 0) {
            return;
          }
          pagingDraw = true;
          CHECK_EQ(static_cast<uint32_t>(rawIdx), batch.pass.colorAttachments.front().handle.index)
            << "Slice paging stage mismatch: payload.channelIndexRaw != batch layer index";
        }
      }
    }

    ChannelInputs inputs = ensureChannelInputs(channelIndex);
    auto& resources = m_channelResources[channelIndex];

    SlicePipelineKey sliceKey;
    sliceKey.validInput = true;
    sliceKey.colorFormats = formats.colorFormats;
    sliceKey.depthFormat = formats.depthFormat;

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

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.descriptorSetFirst = 0;

    const std::array<vk::Buffer, 1> vertexBuffers{m_vertexBuffer->buffer()};
    const std::array<vk::DeviceSize, 1> vertexOffsets{0};
    drawSpec.vertexBuffers = vertexBuffers;
    drawSpec.vertexOffsets = vertexOffsets;
    drawSpec.vertexCount = static_cast<uint32_t>(m_vertexCount);
    drawSpec.instanceCount = 1;
    drawSpec.pushConstantsData = &pc;
    drawSpec.pushConstantsSize = sizeof(pc);
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    drawSpec.requirePushConstants = true;
    if (pagingDraw) {
      CHECK(usePaging);
      CHECK(inputs.pageDirectory && inputs.pageTable && inputs.imageCache)
        << "Slice paging missing paging textures for channel " << channelIndex;
      CHECK(m_imageBlockUploader != nullptr) << "Slice paging expected image block uploader";
      {
        ZBenchTimer uploadTimer("vulkan_upload_page_caches(slice_draw)");
        m_imageBlockUploader->uploadPageCaches(*payload.image, channelIndex, uploadTimer);
      }

      const vk::DescriptorSet dsIndices = m_backend.sharedImgIndicesDescriptorSet();
      const vk::DescriptorSet dsPageData = m_backend.sharedImgPageDataDescriptorSet();
      CHECK(dsIndices && dsPageData) << "Slice paging draw missing backend-shared descriptor sets (indices/page-data)";

      const uint32_t devCap = deviceLevelCap(m_backend.device());
      const uint32_t levelCount = static_cast<uint32_t>(std::min<size_t>(payload.image->numLevels(), devCap));
      CHECK_GT(levelCount, 0u) << "Slice paging draw has no paging levels";
      resources.levelCount = levelCount;

      const std::vector<uint8_t> pageData =
        buildPageDataBuffer(*payload.image, channelIndex, zeToScreenPixelVoxelSize, levelCount);
      const auto pageSlice = m_backend.suballocateUniformFor(payload, pageData.size());
      CHECK(pageSlice.mapped != nullptr) << "Slice page-data uniform slice mapping missing";
      std::memcpy(pageSlice.mapped, pageData.data(), pageData.size());
      CHECK(pageSlice.offset <= std::numeric_limits<uint32_t>::max())
        << "Slice page-data dynamic offset exceeds uint32 range: " << pageSlice.offset;
      const uint32_t pageDynOffset = static_cast<uint32_t>(pageSlice.offset);

      sliceKey.levelCount = levelCount;
      PipelineInstance& pipeline = ensureSlicePipeline(sliceKey, formats);
      drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
      drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();

      SlicePagedBindlessUBOStd140 bindless{};
      bindless.page_directory =
        m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.pageDirectory, "slice_paged_page_directory");
      bindless.page_table_cache =
        m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.pageTable, "slice_paged_page_table");
      bindless.image_cache =
        m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.imageCache, "slice_paged_image_cache");
      bindless.volume = m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.volume, "slice_paged_volume");
      bindless.colormap = m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.colormap, "slice_paged_colormap");
      const auto indicesSlice = m_backend.suballocateUniformFor(payload, sizeof(bindless));
      CHECK(indicesSlice.mapped != nullptr) << "Slice paging indices uniform slice mapping missing";
      std::memcpy(indicesSlice.mapped, &bindless, sizeof(bindless));
      CHECK(indicesSlice.offset <= std::numeric_limits<uint32_t>::max())
        << "Slice paging indices dynamic offset exceeds uint32 range: " << indicesSlice.offset;
      const uint32_t indicesDynOffset = static_cast<uint32_t>(indicesSlice.offset);

      const std::array<vk::DescriptorSet, 3> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(),
                                                            dsIndices,
                                                            dsPageData};
      const std::array<uint32_t, 2> dynamicOffsets{indicesDynOffset, pageDynOffset};
      drawSpec.descriptorSets = descriptorSets;
      drawSpec.dynamicOffsets = dynamicOffsets;
      drawSpec.expectedDescriptorSetCount = 3;
      recorder.recordGraphicsDraw(drawSpec);
    } else {
      const vk::DescriptorSet dsIndices = m_backend.sharedImgIndicesDescriptorSet();
      CHECK(dsIndices) << "Slice fast draw missing backend-shared indices descriptor set (unexpected)";
      CHECK(inputs.volume && inputs.colormap) << "Slice fast draw missing textures";

      sliceKey.levelCount = 1u;
      PipelineInstance& pipeline = ensureSlicePipeline(sliceKey, formats);
      drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
      drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();

      SliceSingleChannelBindlessUBOStd140 ubo{};
      ubo.volume_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.volume, "slice_fast_volume");
      ubo.colormap_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(*inputs.colormap, "slice_fast_colormap");

      const auto slice = m_backend.suballocateUniformFor(payload, sizeof(ubo));
      CHECK(slice.mapped != nullptr) << "Slice indices uniform slice mapping missing";
      std::memcpy(slice.mapped, &ubo, sizeof(ubo));
      CHECK(slice.offset <= std::numeric_limits<uint32_t>::max())
        << "Slice indices dynamic offset exceeds uint32 range: " << slice.offset;
      const uint32_t dynOffset = static_cast<uint32_t>(slice.offset);

      const std::array<vk::DescriptorSet, 2> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(), dsIndices};
      const std::array<uint32_t, 1> dynamicOffsets{dynOffset};
      drawSpec.descriptorSets = descriptorSets;
      drawSpec.dynamicOffsets = dynamicOffsets;
      drawSpec.expectedDescriptorSetCount = 2;
      recorder.recordGraphicsDraw(drawSpec);
    }

    if (setFinalization && payload.streamKey != 0u) {
      m_pendingFinalization = Finalization{.streamKey = payload.streamKey,
                                           .eye = batch.eye,
                                           .lastRound = finalizationLastRound,
                                           .channelCount = static_cast<uint32_t>(channelCount)};
    }
    return;
  }

  // ---------------------------------------------------------------------------
  // Merge layer array into the active surface.
  // ---------------------------------------------------------------------------
  CHECK(stage == ImgSlicePayload::Stage::MergeLayers) << "Unhandled ImgSlicePayload stage";
  if (!isRaster) {
    return;
  }

  CHECK(payload.layerLease && payload.layerLease->hasVulkanImage())
    << "Vulkan slice MergeLayers stage requires a Vulkan layerLease";

  ZVulkanTexture* layerColor = payload.layerLease->colorAttachment(0);
  ZVulkanTexture* layerDepth = payload.layerLease->depthAttachmentTexture();
  CHECK(layerColor) << "Layer array render target is missing color attachments for Vulkan slice pipeline.";
  CHECK(layerDepth) << "Layer array render target is missing depth attachments for Vulkan slice pipeline.";

  const auto formats = vulkan::extractAttachmentFormats(batch);
  if (formats.colorFormats.empty() && !formats.depthFormat) {
    return;
  }

  MergePipelineKey mergeKey;
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = payload.maxProjectionMerge;
  mergeKey.colorFormats = formats.colorFormats;
  mergeKey.depthFormat = formats.depthFormat;
  auto& mergePipeline = ensureMergePipeline(mergeKey, formats);

  Image2DArrayCompositorPushConstants pc{};
  pc.color_texture = m_backend.bindlessLookupSampledImageAutoOrCrash(*layerColor, "slice_merge_layer_color");
  pc.depth_texture = m_backend.bindlessLookupSampledImageAutoOrCrash(*layerDepth, "slice_merge_layer_depth");
  const std::array<vk::DescriptorSet, 1> mergeSets{m_backend.bindlessSampledImageDescriptorSet()};

  ZVulkanGraphicsDrawSpec mergeDraw{};
  mergeDraw.viewports = std::span<const vk::Viewport>(&viewport, 1);
  mergeDraw.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  mergeDraw.pipelineHandle = mergePipeline.pipeline->pipelineHandle();
  mergeDraw.pipelineLayoutHandle = mergePipeline.pipeline->pipelineLayoutHandle();
  mergeDraw.descriptorSetFirst = 0;
  mergeDraw.descriptorSets = mergeSets;
  mergeDraw.expectedDescriptorSetCount = 1;
  mergeDraw.pushConstantsData = &pc;
  mergeDraw.pushConstantsSize = sizeof(pc);
  mergeDraw.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  mergeDraw.requirePushConstants = true;

  const std::array<vk::Buffer, 1> vertexBuffers{m_quadVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  mergeDraw.vertexBuffers = vertexBuffers;
  mergeDraw.vertexOffsets = vertexOffsets;
  mergeDraw.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  mergeDraw.instanceCount = 1;
  recorder.recordGraphicsDraw(mergeDraw);
  maybeCancel(cancellationToken);

  if (FLAGS_atlas_debug_save_slice_layers) {
    auto leaseRef = payload.layerLease;
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend && leaseRef && leaseRef->hasVulkanImage()) {
      const uint32_t saveLayerCount = static_cast<uint32_t>(channelCount);
      ZVulkanTexture* tex = leaseRef->colorAttachment(0);
      ZVulkanTexture* dtex = leaseRef->depthAttachmentTexture();

      QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
      if (!dir.isEmpty() && !dir.endsWith('/')) {
        dir += '/';
      }

      struct SaveJob
      {
        QString filename;
        Z3DRendererVulkanBackend::EndOfFrameHostImageReadbackTicket ticket;
      };
      std::vector<SaveJob> jobs;

      const uint32_t colorLayers = tex ? std::min<uint32_t>(saveLayerCount, tex->arrayLayers()) : 0u;
      const uint32_t depthLayers = dtex ? std::min<uint32_t>(saveLayerCount, dtex->arrayLayers()) : 0u;
      jobs.reserve(static_cast<size_t>(colorLayers) + static_cast<size_t>(depthLayers));

      if (tex) {
        for (uint32_t layer = 0; layer < colorLayers; ++layer) {
          const QString filename =
            dir + QString("slice_layer_%1_%2x%3.tif").arg(layer).arg(tex->width()).arg(tex->height());
          jobs.push_back(SaveJob{filename,
                                 backend->requestEndOfFrameImageReadbackToHostTicket(*tex,
                                                                                     batch.eye,
                                                                                     layer,
                                                                                     vk::ImageAspectFlagBits::eColor,
                                                                                     leaseRef,
                                                                                     "VK debug save slice layers")});
        }
      }

      if (dtex) {
        for (uint32_t layer = 0; layer < depthLayers; ++layer) {
          const QString filename =
            dir + QString("slice_layer_depth_%1_%2x%3.tif").arg(layer).arg(dtex->width()).arg(dtex->height());
          jobs.push_back(
            SaveJob{filename,
                    backend->requestEndOfFrameImageReadbackToHostTicket(*dtex,
                                                                        batch.eye,
                                                                        layer,
                                                                        vk::ImageAspectFlagBits::eDepth,
                                                                        leaseRef,
                                                                        "VK debug save slice layers depth")});
        }
      }

      if (!jobs.empty()) {
        backend->spawnDetachedTask(
          folly::getGlobalCPUExecutor(),
          folly::coro::co_invoke([jobs = std::move(jobs)]() mutable -> folly::coro::Task<void> {
            for (auto& job : jobs) {
              co_await job.ticket.awaitReady();

              if (!ZVulkanTexture::saveReadbackToImage(job.filename,
                                                       job.ticket.format,
                                                       job.ticket.size.x,
                                                       job.ticket.size.y,
                                                       job.ticket.data(),
                                                       job.ticket.dataBytes(),
                                                       /*flipY=*/true)) {
                LOG(ERROR) << "Slice layer debug save failed for " << job.filename;
              }
            }
            co_return;
          }),
          "VK debug save slice layers");
      }
    }
  }

  if (usePaging && payload.streamKey != 0u) {
    const int32_t rawIdx = payload.channelIndexRaw;
    const int32_t rawRound = payload.roundIndexRaw;
    CHECK_GE(rawRound, 0);

    if (rawIdx < 0) {
      if (!m_pendingFinalization) {
        m_pendingFinalization = Finalization{.streamKey = payload.streamKey,
                                             .eye = batch.eye,
                                             .lastRound = false,
                                             .channelCount = static_cast<uint32_t>(channelCount)};
      }
    } else if (rawRound > 0) {
      if (!m_pendingFinalization) {
        m_pendingFinalization = Finalization{.streamKey = payload.streamKey,
                                             .eye = batch.eye,
                                             .lastRound = true,
                                             .channelCount = static_cast<uint32_t>(channelCount)};
      }
    }
  }

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
      const AttachmentHandle& handle = colorHandles.front();
      CHECK(handle.valid() && handle.backend == RenderBackend::Vulkan)
        << "Slice merge debug save: invalid color attachment handle";

      QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
      if (!dir.isEmpty() && !dir.endsWith('/')) {
        dir += '/';
      }

      struct SaveJob
      {
        QString filename;
        Z3DRendererVulkanBackend::EndOfFrameHostImageReadbackTicket ticket;
      };
      std::vector<SaveJob> jobs;
      jobs.reserve(depthHandle ? 2u : 1u);

      auto& tex = vulkan::textureFromHandle(handle, m_backend.device(), "slice merge debug");
      jobs.push_back(SaveJob{dir + QString("slice_merge_%1x%2.tif").arg(tex.width()).arg(tex.height()),
                             backend->requestEndOfFrameImageReadbackToHostTicket(tex,
                                                                                 batch.eye,
                                                                                 /*arrayLayer=*/0u,
                                                                                 vk::ImageAspectFlagBits::eColor,
                                                                                 "VK debug save slice merge output")});

      if (depthHandle && depthHandle->valid() && depthHandle->backend == RenderBackend::Vulkan) {
        auto& dtex = vulkan::textureFromHandle(*depthHandle, m_backend.device(), "slice merge depth debug");
        jobs.push_back(
          SaveJob{dir + QString("slice_merge_depth_%1x%2.tif").arg(dtex.width()).arg(dtex.height()),
                  backend->requestEndOfFrameImageReadbackToHostTicket(dtex,
                                                                      batch.eye,
                                                                      /*arrayLayer=*/0u,
                                                                      vk::ImageAspectFlagBits::eDepth,
                                                                      "VK debug save slice merge output depth")});
      }

      backend->spawnDetachedTask(folly::getGlobalCPUExecutor(),
                                 folly::coro::co_invoke([jobs = std::move(jobs)]() mutable -> folly::coro::Task<void> {
                                   if (jobs.empty()) {
                                     co_return;
                                   }
                                   for (auto& job : jobs) {
                                     co_await job.ticket.awaitReady();

                                     if (!ZVulkanTexture::saveReadbackToImage(job.filename,
                                                                              job.ticket.format,
                                                                              job.ticket.size.x,
                                                                              job.ticket.size.y,
                                                                              job.ticket.data(),
                                                                              job.ticket.dataBytes(),
                                                                              /*flipY=*/true)) {
                                       LOG(ERROR) << "Slice merge debug save failed for " << job.filename;
                                     }
                                   }
                                   co_return;
                                 }),
                                 "VK debug save slice merge output");
    }
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

ZVulkanTexture& ZVulkanImgSlicePipelineContext::ensureVolumeTexture(Z3DImg& owner,
                                                                    size_t channel,
                                                                    uint64_t generation,
                                                                    std::shared_ptr<const ZImg> image)
{
  CHECK(channel <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    << "Slice channel index exceeds Vulkan residency key range";
  ZVulkanTexture* texture = m_backend.device().residencyManager().denseVolumeTexture(owner,
                                                                                     static_cast<uint32_t>(channel),
                                                                                     std::move(image),
                                                                                     generation);
  CHECK(texture != nullptr) << "Slice: managed 3D volume texture missing";
  m_backend.pinTextureForActiveSubmission(texture);
  return *texture;
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

ZVulkanImgSlicePipelineContext::PipelineInstance&
ZVulkanImgSlicePipelineContext::ensureSlicePipeline(const SlicePipelineKey& key,
                                                    const vulkan::AttachmentFormats& formats)
{
  auto it = m_slicePipelines.find(key);
  if (it != m_slicePipelines.end()) {
    return it->second;
  }

  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Slice pipeline requires backend bindless descriptor set layout";

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
    const vk::DescriptorSetLayout indicesLayout = m_backend.imgIndicesDescriptorSetLayout();
    CHECK(indicesLayout) << "Slice paged pipeline requires backend indices descriptor set layout";
    const vk::DescriptorSetLayout pageDataLayout = m_backend.imgPageDataDescriptorSetLayout();
    CHECK(pageDataLayout) << "Slice paged pipeline requires backend page-data descriptor set layout";
    instance.pipeline->setDescriptorSetLayouts({bindlessLayout, indicesLayout, pageDataLayout});
  } else {
    const vk::DescriptorSetLayout indicesLayout = m_backend.imgIndicesDescriptorSetLayout();
    CHECK(indicesLayout) << "Slice fast pipeline requires backend indices descriptor set layout";
    instance.pipeline->setDescriptorSetLayouts({bindlessLayout, indicesLayout});
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
  // Paged slice shaders read bindless indices from a UBO (set=1) to avoid
  // overlapping push-constant ranges across stages.
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
  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Slice merge pipeline requires backend bindless descriptor set layout";
  instance.pipeline->setDescriptorSetLayouts({bindlessLayout});
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

  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(Image2DArrayCompositorPushConstants))};
  instance.pipeline->setPushConstantRanges({pcRange});

  std::array<vk::SpecializationMapEntry, 2> entries{
    vk::SpecializationMapEntry{.constantID = 70, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 71, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };
  std::vector<vk::SpecializationMapEntry> entryVec(entries.begin(), entries.end());
  std::array<uint32_t, 2> values{static_cast<uint32_t>(std::max(1, key.numVolumes)), key.maxProjectionMerge ? 1u : 0u};
  std::vector<uint8_t> data(sizeof(values));
  std::memcpy(data.data(), values.data(), sizeof(values));
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, entryVec, data);

  instance.pipeline->create();

  auto [inserted, _] = m_mergePipelines.emplace(key, std::move(instance));
  return inserted->second;
}

ZVulkanImgSlicePipelineContext::PipelineInstance&
ZVulkanImgSlicePipelineContext::ensureBlockIdPipeline(const BlockIdPipelineKey& key, vk::Format colorFormat)
{
  auto it = m_blockIdPipelines.find(key);
  if (it != m_blockIdPipelines.end()) {
    return it->second;
  }

  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Slice Block-ID pipeline requires backend bindless descriptor set layout";
  const vk::DescriptorSetLayout indicesLayout = m_backend.imgIndicesDescriptorSetLayout();
  CHECK(indicesLayout) << "Slice Block-ID pipeline requires backend indices descriptor set layout";
  const vk::DescriptorSetLayout pageDataLayout = m_backend.imgPageDataDescriptorSetLayout();
  CHECK(pageDataLayout) << "Slice Block-ID pipeline requires backend page-data descriptor set layout";

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "transform_with_3dtexture_and_eye_coordinate.vert.spv",
                                                    shaderBase + "image3d_slice_with_transfun_blockID.frag.spv",
                                                    std::nullopt);

  auto vertexState = makeSliceVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexState, vk::PrimitiveTopology::eTriangleList);
  instance.pipeline->setDescriptorSetLayouts({bindlessLayout, indicesLayout, pageDataLayout});
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
  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(static_cast<VkDescriptorSetLayout>(bindlessLayout) != VK_NULL_HANDLE)
    << "Slice block-ID compaction requires backend bindless descriptor set layout";

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
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{bindlessLayout, **m_blockIdCompactSetLayoutBuffer};
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 5};
    if (!m_blockIdCompactPipelineLayoutBuffer) {
      m_blockIdCompactPipelineLayoutBuffer.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                     .pSetLayouts = setLayouts.data(),
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
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageImage,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute}
    };
    if (!m_blockIdCompactSetLayoutStorage) {
      m_blockIdCompactSetLayoutStorage.emplace(
        device,
        vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()});
    }
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{bindlessLayout, **m_blockIdCompactSetLayoutStorage};
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 5};
    if (!m_blockIdCompactPipelineLayoutStorage) {
      m_blockIdCompactPipelineLayoutStorage.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                     .pSetLayouts = setLayouts.data(),
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
    std::array<vk::DescriptorSetLayoutBinding, 1> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
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
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{bindlessLayout, **m_blockIdCompactSetLayoutSampled};
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 6};
    if (!m_blockIdCompactPipelineLayoutSampled) {
      m_blockIdCompactPipelineLayoutSampled.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                     .pSetLayouts = setLayouts.data(),
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

} // namespace nim
