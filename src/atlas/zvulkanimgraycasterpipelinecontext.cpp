#include "zvulkanimgraycasterpipelinecontext.h"

#include "z3dimg.h"
#include "z3drendererbase.h"
#include "z3drendererstates.h"
#include "z3dtransferfunction.h"
#include "z3drenderglobalstate.h"
#include "z3dscratchresourcepool.h"
#include "z3dimgraycasterrenderer.h"
#include "zlog.h"
#include "zbenchtimer.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkanbuffer.h"
#include "zvulkanlututils.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanresourcemetadata.h"
#include "zvulkanuniforms.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zsysteminfo.h"
#include <fstream>
#include "z3drenderervulkanbackend.h"
#include "zvulkanpagedimageblockuploader.h"
#include "zcancellation.h"
#include "zrenderthreadexecutor_tls.h"

#include <folly/executors/GlobalExecutor.h>

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>
#include <utility>
#include <cstring>
#include <cmath>

// Coroutine scheduling for post-frame work on the render thread.
#include <folly/coro/Task.h>

// Debug: save entry/exit textures after they are rendered (Vulkan only)
DEFINE_bool(atlas_debug_save_entry_exit,
            false,
            "Save Vulkan entry/exit textures (RGBA32F) to TIF files after rendering.");
DEFINE_string(atlas_debug_save_dir, "", "Directory to write debug images (default: current working directory)");
DEFINE_bool(atlas_debug_save_raycaster_layers,
            false,
            "Save Vulkan raycaster layered outputs (color + depth, one TIF per layer) after rendering.");
DEFINE_bool(atlas_debug_save_raycaster_merge_out,
            false,
            "Save Vulkan raycaster merged output (first color attachment) after merge.");
DEFINE_bool(atlas_debug_save_slice_layers,
            false,
            "Save Vulkan slice layered outputs (color + depth, one TIF per layer) after rendering.");
DEFINE_bool(atlas_debug_save_slice_merge_out,
            false,
            "Save Vulkan slice merged output (first color attachment) after merge.");

// Debug: CPU-side count of non-zero block IDs written by the Block-ID pass (per attachment)
DEFINE_bool(atlas_vk_debug_blockid_count,
            false,
            "After Block-ID draw, count non-zero IDs per attachment via CPU readback");

// Compaction mode:
// 0 = Workgroup-local dedupe + global hash insert (SSBO table of 4Mi entries)
// 1 = Workgroup-local dedupe + global append buffer (SSBO with atomic counter + array)
DEFINE_int32(atlas_vk_blockid_compaction_mode, 1, "Deprecated; append-only; ignored");

// Compaction read source override (append-only)
DEFINE_string(atlas_vk_blockid_compaction_source,
              "buffer",
              "Block-ID compaction read source: 'buffer' (default), 'storage', or 'sampled' (append-only)");

// Debug dump of Vulkan raycaster inputs before dispatch (CPU-side only)
DEFINE_bool(atlas_vk_debug_raycaster_dump,
            false,
            "Dump Vulkan raycaster inputs (specializations, push constants, page data, bindings)");
DEFINE_int32(atlas_vk_debug_raycaster_dump_levels, 8, "Max page levels to print when dumping Vulkan raycaster inputs");

// Force page UBO to use per-draw override descriptor every frame instead of
// binding/updating a persistent descriptor set. Useful to rule out any
// persistence/lifetime issues when diagnosing paging-related timeouts.
DEFINE_bool(atlas_vk_force_page_ubo_override,
            false,
            "Raycaster: always bind page UBO via override descriptor (no persistent set)");

// Allocate fresh override descriptor sets per draw for dynamic/page/static bindings to
// avoid any chance of rewriting a set already bound earlier in the same command buffer.
// Default is false to preserve current behavior; enable to rule out descriptor-lifetime issues.
DEFINE_bool(atlas_vk_force_fresh_override_sets,
            false,
            "Raycaster: allocate fresh override descriptor sets per draw (dynamic/page/static)");

namespace nim {

namespace {

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

ImgCompositingMode sanitizeMode(ImgCompositingMode mode)
{
  switch (mode) {
    case ImgCompositingMode::DirectVolumeRendering:
    case ImgCompositingMode::MaximumIntensityProjection:
    case ImgCompositingMode::LocalMIP:
    case ImgCompositingMode::IsoSurface:
    case ImgCompositingMode::XRay:
    case ImgCompositingMode::MIPOpaque:
    case ImgCompositingMode::LocalMIPOpaque:
      return mode;
    default:
      return ImgCompositingMode::DirectVolumeRendering;
  }
}

struct RayParamsData
{
  float samplingRate = 1.0f;
  float isoValue = 0.5f;
  float localMIPThreshold = 0.8f;
  float zeToZWA = 0.0f;
  float zeToZWB = 1.0f;
  glm::vec3 volumeDimensions{1.0f};
  float padding = 0.0f;
};

// Block-ID compaction table parameters (must match GLSL)
constexpr uint32_t kEmptyBlockID = 0xFFFFFFFFu;

uint32_t rayModeConstant(ImgCompositingMode mode)
{
  switch (mode) {
    case ImgCompositingMode::MaximumIntensityProjection:
    case ImgCompositingMode::MIPOpaque:
      return 1u;
    case ImgCompositingMode::IsoSurface:
      return 2u;
    case ImgCompositingMode::XRay:
      return 3u;
    default:
      return 0u;
  }
}

bool usesLocalMip(ImgCompositingMode mode)
{
  return mode == ImgCompositingMode::LocalMIP || mode == ImgCompositingMode::LocalMIPOpaque;
}

bool resultsOpaque(ImgCompositingMode mode)
{
  return mode == ImgCompositingMode::MIPOpaque || mode == ImgCompositingMode::LocalMIPOpaque;
}

bool requiresMaxProjectionMerge(ImgCompositingMode mode)
{
  switch (mode) {
    case ImgCompositingMode::MaximumIntensityProjection:
    case ImgCompositingMode::LocalMIP:
    case ImgCompositingMode::MIPOpaque:
    case ImgCompositingMode::LocalMIPOpaque:
      return true;
    default:
      return false;
  }
}

CompositingConfig evaluateCompositing(ImgCompositingMode rawMode)
{
  CompositingConfig cfg;
  cfg.mode = sanitizeMode(rawMode);
  cfg.resultOpaque = resultsOpaque(cfg.mode);
  cfg.localMip = usesLocalMip(cfg.mode);
  cfg.maxProjectionMerge = requiresMaxProjectionMerge(cfg.mode);
  return cfg;
}

// Std140-friendly packing helpers for UBO data. We write vec3 as vec4 with padding,
// and scalar floats as a vec4 with value in x and zeros elsewhere to maintain 16-byte strides.
inline void appendStd140Vec4(std::vector<uint8_t>& data, const glm::vec4& v)
{
  const size_t offset = data.size();
  data.resize(offset + sizeof(glm::vec4));
  std::memcpy(data.data() + offset, &v, sizeof(glm::vec4));
}

inline void appendStd140Uvec4(std::vector<uint8_t>& data, const glm::uvec4& v)
{
  const size_t offset = data.size();
  data.resize(offset + sizeof(glm::uvec4));
  std::memcpy(data.data() + offset, &v, sizeof(glm::uvec4));
}

inline void appendUvec3(std::vector<uint8_t>& data, const glm::uvec3& value)
{
  appendStd140Uvec4(data, glm::uvec4(value, 0u));
}

inline void appendVec3(std::vector<uint8_t>& data, const glm::vec3& value)
{
  appendStd140Vec4(data, glm::vec4(value, 0.0f));
}

inline void appendScalar(std::vector<uint8_t>& data, float value)
{
  appendStd140Vec4(data, glm::vec4(value, 0.0f, 0.0f, 0.0f));
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
  // Caller supplies clamped levelCount; only validate here.
  CHECK_GT(levelCount, 0u) << "Image has zero paging levels (incomplete setup)";

  std::vector<uint8_t> data;
  data.reserve(256);

  CHECK_LT(channel, image.numChannels()) << "Channel index out of range in buildPageDataBuffer: channel=" << channel
                                         << " channels=" << image.numChannels();

  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("buildPageDataBuffer: begin channel={} levels={} ze2px={:.6f}",
                           channel,
                           levelCount,
                           zeToScreenPixelVoxelSize);
  }

  const auto& pageDirectoryBases = image.pageDirectoryBases();
  const auto& posToBlockIDs = image.posToBlockIDsLevels();
  const auto& imageDimensions = image.imageDimensionsLevels();
  const auto& voxelWorldSizes = image.voxelWorldSizesLevels();

  CHECK_GE(pageDirectoryBases.size(), levelCount);
  CHECK_GE(posToBlockIDs.size(), levelCount);
  CHECK_GE(imageDimensions.size(), levelCount);
  CHECK_GE(voxelWorldSizes.size(), levelCount);

  // Validate invariants before packing
  const glm::uvec3 ptb = image.pageTableBlockSize();
  CHECK(ptb.x > 0u && ptb.y > 0u && ptb.z > 0u)
    << "Invalid pageTableBlockSize: (" << ptb.x << ", " << ptb.y << ", " << ptb.z << ")";
  const glm::uvec3 ibs = image.imageBlockSize();
  CHECK(ibs.x > 0u && ibs.y > 0u && ibs.z > 0u)
    << "Invalid imageBlockSize: (" << ibs.x << ", " << ibs.y << ", " << ibs.z << ")";

  // Fixed-size fields first
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "buildPageDataBuffer: append pageTableBlockSize";
  }
  appendUvec3(data, ptb);

  if (VLOG_IS_ON(2)) {
    VLOG(2) << "buildPageDataBuffer: append imageBlockSize";
  }
  appendUvec3(data, ibs);

  // Compute address->normalized coordinate scale without relying on GL textures.
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "buildPageDataBuffer: compute addr->norm from imageCacheSize";
  }
  const glm::uvec3 cacheSize = image.imageCacheSize();
  CHECK(cacheSize.x > 0u && cacheSize.y > 0u && cacheSize.z > 0u)
    << "Invalid image cache size: " << cacheSize.x << ", " << cacheSize.y << ", " << cacheSize.z;
  const glm::vec3 addrNorm = 1.0f / glm::vec3(cacheSize);
  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("buildPageDataBuffer: addrNorm=({}, {}, {})", addrNorm.x, addrNorm.y, addrNorm.z);
  }
  appendVec3(data, addrNorm);
  appendScalar(data, zeToScreenPixelVoxelSize);

  // Levels packed group-by-level: dirBase, dims, posToBlockIDs, voxelWorldSize
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "buildPageDataBuffer: append per-level data groups";
  }
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
    if (FLAGS_atlas_vk_debug_raycaster_dump) {
      const int maxLevels = std::max(0, FLAGS_atlas_vk_debug_raycaster_dump_levels);
      if (static_cast<int>(level) < maxLevels) {
        VLOG(2) << fmt::format(
          "Page level[{}]: dirBase=({}, {}, {}) dims=({}, {}, {}) posToIDs=({}, {}, {}) voxelWorld={:.6f}",
          level,
          pageDirectoryBases[level].x,
          pageDirectoryBases[level].y,
          pageDirectoryBases[level].z,
          imageDimensions[level].x,
          imageDimensions[level].y,
          imageDimensions[level].z,
          posToBlockIDs[level].x,
          posToBlockIDs[level].y,
          posToBlockIDs[level].z,
          voxelWorldSizes[level]);
      }
    }
  }

  // Sanity: std140 pack size should be 64B header + 64B per level
  const size_t expectedBytes = 64u + static_cast<size_t>(levelCount) * 64u;
  CHECK_EQ(data.size(), expectedBytes) << "Unexpected PageData UBO size: got=" << data.size()
                                       << " expected=" << expectedBytes << " (levels=" << levelCount << ")";

  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("buildPageDataBuffer: end size={} bytes", data.size());
  }
  if (FLAGS_atlas_vk_debug_raycaster_dump) {
    VLOG(2) << fmt::format(
      "Page UBO summary: pageTableBlockSize=({}, {}, {}) imageBlockSize=({}, {}, {}) cacheSize=({}, {}, {}) addrNorm=({}, {}, {}) ze2px={:.6f}",
      ptb.x,
      ptb.y,
      ptb.z,
      ibs.x,
      ibs.y,
      ibs.z,
      cacheSize.x,
      cacheSize.y,
      cacheSize.z,
      (1.0f / glm::vec3(cacheSize)).x,
      (1.0f / glm::vec3(cacheSize)).y,
      (1.0f / glm::vec3(cacheSize)).z,
      zeToScreenPixelVoxelSize);
  }
  return data;
}

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

} // namespace

ZVulkanImgRaycasterPipelineContext::ZVulkanImgRaycasterPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanImgRaycasterPipelineContext::~ZVulkanImgRaycasterPipelineContext() = default;

void ZVulkanImgRaycasterPipelineContext::ensureUploader()
{
  if (m_imageBlockUploader) {
    return;
  }
  // Construct the uploader with the active device. This must be called only
  // after the backend has ensured the device via beginRender/ensureDevice.
  m_imageBlockUploader = std::make_unique<ZVulkanPagedImageBlockUploader>(m_backend.device());
}

void ZVulkanImgRaycasterPipelineContext::resetFrame()
{
  resetDescriptors();
  m_progressivePrep.reset();
  m_skipStagesThisFrame.clear();
  m_depthClearedThisFrame.clear();
  m_entryGeometryUploadedThisFrame = false;
  m_entryGeometryVertexCountThisFrame = 0u;
  m_entryGeometryIndexCountThisFrame = 0u;
  // Note: do NOT clear m_pendingFinalization / m_deferredProgressive here.
  // These can be set by frame-completion safe-point hooks (post-fence) and must
  // survive across beginRender() calls until the backend consumes them via
  // takePendingFinalization() / ensurePreparedProgressiveRound().
}

std::optional<ZVulkanImgRaycasterPipelineContext::Finalization>
ZVulkanImgRaycasterPipelineContext::takePendingFinalization()
{
  auto ret = m_pendingFinalization;
  m_pendingFinalization.reset();
  return ret;
}

void ZVulkanImgRaycasterPipelineContext::resetDescriptors()
{
  for (auto& channel : m_channelResources) {
    channel.fastDescriptor = nullptr;
    channel.image2DDescriptor = nullptr;
    channel.sliceDescriptor = nullptr;
    channel.pagedDescriptor = nullptr; // dynamic (per-frame)
    channel.staticDescriptor = nullptr; // override for first frame only
    channel.pageDescriptor = nullptr; // override for first frame only
    channel.blockIdDescriptor.reset();
  }
  m_emptyDescriptor.reset();
  m_entryTransformDescriptor = nullptr;
  m_copyDescriptor = nullptr;
  m_mergeDescriptor = nullptr;
  // Do not reset m_descriptorPool here: persistent per-channel descriptor sets
  // (e.g., RayParams) are allocated from this pool and must survive across frames.
}

void ZVulkanImgRaycasterPipelineContext::record(Z3DRendererBase& renderer,
                                                const RenderBatch& batch,
                                                const ImgRaycasterPayload& payload,
                                                const vk::Viewport& viewport,
                                                const vk::Rect2D& scissor,
                                                vk::raii::CommandBuffer& cmd)
{
  // Ensure uploader availability after beginRender() when device is valid.
  ensureUploader();
  // Cooperative cancellation: mirror GL by polling UI events and
  // throwing when a cancel is requested.
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  processEventsAndMaybeCancel(cancellationToken);

  CHECK(payload.stage != ImgRaycasterPayload::Stage::Unspecified) << "Raycaster payload missing stage";

  switch (payload.stage) {
    case ImgRaycasterPayload::Stage::Unspecified:
      CHECK(false) << "Raycaster payload stage is Unspecified";
      break;
    case ImgRaycasterPayload::Stage::EntryExit:
      recordStageEntryExit(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::FastDirect:
      recordStageFastDirect(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::FastLayers:
      recordStageFastLayers(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::FastMerge:
      recordStageFastMerge(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::ProgressivePreviewLayers:
      recordStageProgressivePreviewLayers(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::ProgressiveBlockId:
      recordStageProgressiveBlockId(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::ProgressiveCompaction:
      recordStageProgressiveCompaction(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::ProgressiveRaycast:
      recordStageProgressiveRaycast(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::ProgressiveCopyToLayers:
      recordStageProgressiveCopyToLayers(renderer, batch, payload, viewport, scissor, cmd);
      break;
    case ImgRaycasterPayload::Stage::ProgressiveMerge:
      recordStageProgressiveMerge(renderer, batch, payload, viewport, scissor, cmd);
      break;
  }
}

bool ZVulkanImgRaycasterPipelineContext::stageSkippedThisFrame(uint64_t streamKey, Z3DEye eye) const
{
  if (streamKey == 0u) {
    return false;
  }
  return m_skipStagesThisFrame.contains(StreamEyeKey{streamKey, eye});
}

void ZVulkanImgRaycasterPipelineContext::markStageSkippedThisFrame(uint64_t streamKey, Z3DEye eye)
{
  if (streamKey == 0u) {
    return;
  }
  m_skipStagesThisFrame.insert(StreamEyeKey{streamKey, eye});
}

std::optional<ZVulkanImgRaycasterPipelineContext::ProgressivePrepState>
ZVulkanImgRaycasterPipelineContext::ensurePreparedProgressiveRound(Z3DRendererBase& renderer,
                                                                   const RenderBatch& batch,
                                                                   const ImgRaycasterPayload& payload,
                                                                   const vk::Viewport& viewport,
                                                                   const vk::Rect2D& scissor,
                                                                   vk::raii::CommandBuffer& cmd,
                                                                   const CompositingConfig& composite)
{
  (void)viewport;
  (void)scissor;
  (void)cmd;
  (void)composite;

  const uint64_t streamKey = payload.streamKey;
  const Z3DEye eye = batch.eye;
  if (stageSkippedThisFrame(streamKey, eye)) {
    return std::nullopt;
  }

  // If a finalization from the previous frame is pending (and no deferred
  // progressive-only round is required), skip recording any progressive work
  // for this stream/eye now. The backend will consume the finalization after
  // this stage returns.
  std::optional<DeferredProgressive> deferredRound;
  if (streamKey != 0u && m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
      m_deferredProgressive->eye == eye &&
      m_deferredProgressive->progressiveGeneration == payload.progressiveGeneration) {
    deferredRound = m_deferredProgressive;
  }
  if (!deferredRound && streamKey != 0u && m_pendingFinalization && m_pendingFinalization->streamKey == streamKey &&
      m_pendingFinalization->eye == eye) {
    markStageSkippedThisFrame(streamKey, eye);
    return std::nullopt;
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureEmptyDescriptor();
  ensureQuadVertexBuffer();

  ProgressivePrepKey key;
  key.streamKey = streamKey;
  key.eye = eye;
  key.progressiveGeneration = payload.progressiveGeneration;
  key.channelIndexRaw = payload.channelIndexRaw;
  key.roundIndexRaw = payload.roundIndexRaw;

  if (m_progressivePrep && m_progressivePrep->key == key) {
    return m_progressivePrep;
  }

  ProgressivePrepState prep;
  prep.key = key;
  prep.channelCount = static_cast<uint32_t>(payload.visibleChannels.size());
  if (prep.channelCount == 0u) {
    m_progressivePrep = prep;
    return prep;
  }

  CHECK(!payload.fastPathOnly) << "ensurePreparedProgressiveRound called for fast-only payload";
  CHECK(payload.image) << "Raycaster progressive path: payload missing image pointer.";

  const bool hasIndices = payload.entryHasIndices && !payload.entryIndices.empty();
  const bool planarGeometry = !hasIndices;
  CHECK(!planarGeometry) << "Progressive raycaster path only supports volumetric rendering.";

  CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
    << "Vulkan raycaster progressive path missing entry/exit lease.";
  CHECK(payload.lastAccumLease && payload.currentAccumLease)
    << "Vulkan raycaster progressive path missing accum leases.";

  CHECK(m_imageBlockUploader) << "Vulkan raycaster progressive path missing image block uploader.";
  m_imageBlockUploader->bindToImage(*payload.image);

  const int32_t rawIdx = payload.channelIndexRaw;
  CHECK_GE(rawIdx, 0) << "Negative channelIndexRaw (preview) not expected in progressive preparation.";
  CHECK_LT(static_cast<uint32_t>(rawIdx), prep.channelCount) << "channelIndexRaw out of range for visibleChannels.";
  prep.activeChannelIndex = static_cast<uint32_t>(rawIdx);
  CHECK_LT(prep.activeChannelIndex, payload.visibleChannels.size());
  prep.channelIndex = payload.visibleChannels[prep.activeChannelIndex];

  ChannelResources& resources = ensureChannelResources(prep.channelIndex);

  const ZImg& channelImage = *payload.image->channelImageShared(prep.channelIndex);
  const uint64_t volGen = payload.image->volumeGeneration(prep.channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, prep.channelIndex, volGen);

  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster progressive path: payload missing transferFunctions vector (fatal)";
  const auto& transferList = *payload.transferFunctions;
  CHECK(prep.channelIndex < transferList.size() && transferList[prep.channelIndex] != nullptr)
    << "Vulkan raycaster missing transfer function for channel " << prep.channelIndex;
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferList[prep.channelIndex]);

  // Ensure paging caches (page directory + page table) are uploaded before Block-ID / raycast.
  {
    ZBenchTimer uploadTimer("vulkan_upload_page_caches");
    if (auto* statsSink = payload.image->readStatsSink()) {
      CHECK_GE(payload.roundIndexRaw, 0) << "Negative roundIndexRaw not expected in progressive path.";
      const auto statsContext =
        ZImgReadStatsContext{static_cast<uint32_t>(prep.channelIndex), static_cast<uint32_t>(payload.roundIndexRaw)};
      const uint64_t pageDirectoryBytes =
        static_cast<uint64_t>(payload.image->pageDirectoryView(prep.channelIndex).size_bytes());
      const uint64_t pageTableBytes =
        static_cast<uint64_t>(payload.image->pageTableCacheView(prep.channelIndex).size_bytes());
      statsSink->onGpuUploadBytes(statsContext, ZImgGpuUploadKind::PageDirectory, pageDirectoryBytes, /*blocks=*/0);
      statsSink->onGpuUploadBytes(statsContext, ZImgGpuUploadKind::PageTableCache, pageTableBytes, /*blocks=*/0);
    }
    m_imageBlockUploader->uploadPageCaches(*payload.image, prep.channelIndex, uploadTimer);
  }

  prep.entryTexture = payload.entryExitLease ? payload.entryExitLease->colorAttachment(0) : nullptr;
  prep.lastColor = payload.lastAccumLease->colorAttachment(0);
  prep.lastDepth = payload.lastAccumLease->colorAttachment(1);
  prep.currentColor = payload.currentAccumLease->colorAttachment(0);
  prep.currentDepth = payload.currentAccumLease->colorAttachment(1);
  CHECK(prep.entryTexture && prep.lastColor && prep.lastDepth && prep.currentColor && prep.currentDepth)
    << "Vulkan raycaster progressive path missing required textures.";

  // NOTE: Keep barriers out of dynamic rendering.
  // - Round-0 clear of last accum is emitted by the linear-script stage recorder
  //   (Z3DImgRaycasterRenderer::recordVulkanStagesToScript) as a commands() node
  //   that runs outside vkCmdBeginRendering.
  // - Sampling layouts for entry/exit + last accum are ensured via batch metadata
  //   (BackendPassDesc::externalImageUses + AttachmentDesc::finalUse).

  const glm::uvec2 outputSize = payload.outputSize;
  CHECK(outputSize.x > 0u && outputSize.y > 0u) << "Vulkan raycaster progressive path requires non-zero output size.";

  const auto& viewState = renderer.viewState();
  const auto& sceneState = renderer.sceneState();
  const auto& monoEyeState = viewState.eyes[static_cast<size_t>(Z3DEye::MonoEye)];
  const float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
  const float farClip = viewState.farClip;
  const glm::vec2 pixelEyeSpaceSize =
    monoEyeState.frustumNearPlaneSize / glm::vec2(std::max(1u, outputSize.x), std::max(1u, outputSize.y));
  prep.zeToScreenPixelVoxelSize =
    -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / nearClip * sceneState.devicePixelRatio;
  prep.zeToZW_a = farClip * nearClip / (farClip - nearClip);
  prep.zeToZW_b = 0.5f * (farClip + nearClip) / (farClip - nearClip) + 0.5f;

  updateChannelFastDescriptors(resources,
                               payload,
                               prep.channelIndex,
                               *prep.entryTexture,
                               volumeTex,
                               transferTex,
                               prep.zeToZW_a,
                               prep.zeToZW_b,
                               glm::vec3(static_cast<float>(channelImage.width()),
                                         static_cast<float>(channelImage.height()),
                                         static_cast<float>(channelImage.depth())));

  if (!updatePageDescriptors(resources,
                             payload,
                             *prep.entryTexture,
                             *prep.lastDepth,
                             *prep.lastColor,
                             volumeTex,
                             transferTex,
                             *payload.image,
                             prep.channelIndex,
                             prep.zeToScreenPixelVoxelSize,
                             /*freshOverrideDescriptors=*/FLAGS_atlas_vk_force_fresh_override_sets)) {
    prep.ready = false;
    m_progressivePrep = prep;
    return prep;
  }

  // Prefer per-draw override static descriptors (GL parity / avoid stale sets).
  const bool preferOverrideStaticGlobal = true;
  if (preferOverrideStaticGlobal) {
    if (FLAGS_atlas_vk_force_fresh_override_sets || !resources.staticDescriptor) {
      resources.staticDescriptor = m_backend.allocateOverrideDescriptorSet(**m_progressiveStaticSetLayout);
    }
    if (resources.staticDescriptor) {
      CHECK(resources.boundPageDirectoryTex && resources.boundPageTableTex && resources.boundImageCacheTex &&
            resources.boundVolumeTex && resources.boundTransferTex)
        << "Missing bound paging/volume/transfer textures for override static DS";
      resources.staticDescriptor->updateTexture(0, *resources.boundPageDirectoryTex, m_backend.nearestClampSampler());
      resources.staticDescriptor->updateTexture(1, *resources.boundPageTableTex, m_backend.nearestClampSampler());
      resources.staticDescriptor->updateTexture(2, *resources.boundImageCacheTex, m_backend.defaultSampler());
      resources.staticDescriptor->updateTexture(3, *resources.boundVolumeTex, m_backend.defaultSampler());
      resources.staticDescriptor->updateTexture(4, *resources.boundTransferTex, m_backend.defaultSampler());
    }
  }

  prep.ready = true;

  if (deferredRound) {
    prep.skipBlockIdPass = true;
    prep.finalizeAfterProgressive =
      Finalization{.streamKey = streamKey, .eye = eye, .lastRound = true, .channelCount = deferredRound->channelCount};
  }

  m_progressivePrep = prep;
  return prep;
}

void ZVulkanImgRaycasterPipelineContext::recordStageEntryExit(Z3DRendererBase& renderer,
                                                              const RenderBatch& batch,
                                                              const ImgRaycasterPayload& payload,
                                                              const vk::Viewport& viewport,
                                                              const vk::Rect2D& scissor,
                                                              vk::raii::CommandBuffer& cmd)
{
  if (payload.visibleChannels.empty()) {
    return;
  }

  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }
  const bool deferredRound =
    (streamKey != 0u && m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
     m_deferredProgressive->eye == batch.eye &&
     m_deferredProgressive->progressiveGeneration == payload.progressiveGeneration);
  if (!deferredRound && streamKey != 0u && m_pendingFinalization && m_pendingFinalization->streamKey == streamKey &&
      m_pendingFinalization->eye == batch.eye) {
    markStageSkippedThisFrame(streamKey, batch.eye);
    return;
  }

  const bool hasIndices = payload.entryHasIndices && !payload.entryIndices.empty();
  const bool planarGeometry = !hasIndices;
  const bool needsEntryExit = !planarGeometry;
  if (!needsEntryExit) {
    return;
  }

  CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
    << "Vulkan raycaster EntryExit stage missing entry/exit lease.";

  CHECK(!batch.pass.colorAttachments.empty()) << "EntryExit stage requires an active color attachment";
  const uint32_t layerIndex = batch.pass.colorAttachments.front().handle.index;
  CHECK(layerIndex < 2u) << "EntryExit stage expects layer index 0/1, got " << layerIndex;

  // Segment-managed entry/exit: each batch targets exactly one array layer via AttachmentHandle.index,
  // and the backend owns vkCmdBeginRendering/vkCmdEndRendering and layout transitions.
  ZVulkanTexture* outTex = payload.entryExitLease->colorAttachment(0);
  CHECK(outTex != nullptr) << "Entry/exit lease missing color attachment";

  ensureEntryPipelines(outTex->format());

  const size_t eyeIndex = std::min<size_t>(static_cast<size_t>(batch.eye), renderer.viewState().eyes.size() - 1);
  const auto& eyeState = renderer.viewState().eyes[eyeIndex];

  struct EntryPushConstant
  {
    glm::mat4 projectionView;
    glm::mat4 view;
  } pushConstant{eyeState.projectionMatrix * eyeState.viewMatrix, eyeState.viewMatrix};

  const size_t vertexCount = payload.entryPositions.size();
  CHECK_GT(vertexCount, 0u) << "EntryExit stage missing entry vertex data";

  ensureEntryGeometryUploadedThisFrame(payload);

  const bool flipped = payload.entryFlipped;
  PipelineInstance* pipeline = nullptr;
  if (layerIndex == 0u) {
    pipeline = flipped ? &m_entryBackPipeline : &m_entryFrontPipeline;
  } else if (layerIndex == 1u) {
    pipeline = flipped ? &m_entryFrontPipeline : &m_entryBackPipeline;
  } else {
    CHECK(false) << "Unhandled entry/exit layer index " << layerIndex;
  }

  std::optional<size_t> gpuScope;
  if (layerIndex == 0u) {
    gpuScope = m_backend.beginGpuScope("ray_entry_exit(front)");
  } else {
    gpuScope = m_backend.beginGpuScope("ray_entry_exit(back)");
  }
  auto guard = folly::makeGuard([&]() {
    if (gpuScope) {
      m_backend.endGpuScope(*gpuScope);
    }
  });

  CHECK(m_entryVertexBuffer != nullptr) << "EntryExit stage missing entry vertex buffer";

  ZVulkanPipelineCommandRecorder recorder(cmd);
  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline->pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline->pipeline->pipelineLayoutHandle();

  const std::array<vk::Buffer, 1> vertexBuffers{m_entryVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.pushConstantsData = &pushConstant;
  drawSpec.pushConstantsSize = sizeof(pushConstant);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
  drawSpec.requirePushConstants = true;
  drawSpec.instanceCount = 1;

  if (payload.entryHasIndices && m_entryIndexBuffer && !payload.entryIndices.empty()) {
    drawSpec.indexBuffer = m_entryIndexBuffer->buffer();
    drawSpec.indexOffset = 0;
    drawSpec.indexType = vk::IndexType::eUint32;
    drawSpec.indexCount = static_cast<uint32_t>(payload.entryIndices.size());
    drawSpec.firstIndex = 0;
    drawSpec.vertexOffset = 0;
    drawSpec.firstInstance = 0;
  } else {
    drawSpec.vertexCount = static_cast<uint32_t>(vertexCount);
    drawSpec.firstVertex = 0;
    drawSpec.firstInstance = 0;
  }

  recorder.recordGraphicsDraw(drawSpec);

  // Optional debug dump of entry/exit layers to TIF (RGBA32F). Trigger once after the second layer.
  if (FLAGS_atlas_debug_save_entry_exit && layerIndex == 1u) {
    auto leaseRef = payload.entryExitLease;
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend && leaseRef && leaseRef->hasVulkanImage()) {
      ZVulkanTexture* tex = leaseRef->colorAttachment(0);
      if (!tex) {
        LOG(ERROR) << "Entry/exit debug save: color attachment missing";
        return;
      }

      QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
      if (!dir.isEmpty() && !dir.endsWith('/')) {
        dir += '/';
      }

      struct SaveJob
      {
        QString filename;
        Z3DRendererVulkanBackend::EndOfFrameColorReadbackTicket ticket;
      };
      std::vector<SaveJob> jobs;
      jobs.reserve(2u);

      auto enqueueLayer = [&](uint32_t layer, QString suffix) {
        jobs.push_back(
          SaveJob{dir + QString("entry_exit_%1_%2x%3.tif").arg(suffix).arg(tex->width()).arg(tex->height()),
                  backend->requestEndOfFrameImageReadbackTicket(*tex,
                                                                batch.eye,
                                                                layer,
                                                                vk::ImageAspectFlagBits::eColor,
                                                                "VK debug save entry/exit")});
      };

      enqueueLayer(0u, QStringLiteral("front"));
      if (tex->arrayLayers() > 1u) {
        enqueueLayer(1u, QStringLiteral("back"));
      }

      backend->spawnDetachedTask(
        folly::getGlobalCPUExecutor(),
        [jobs = std::move(jobs), leaseRef]() mutable -> folly::coro::Task<void> {
          (void)leaseRef;
          for (auto& job : jobs) {
            std::vector<uint8_t> owned = co_await job.ticket.awaitOwnedBytes();

            if (!ZVulkanTexture::saveReadbackToImage(job.filename,
                                                     job.ticket.format,
                                                     job.ticket.size.x,
                                                     job.ticket.size.y,
                                                     owned.data(),
                                                     owned.size(),
                                                     /*flipY=*/true)) {
              LOG(ERROR) << "Entry/exit debug save failed for " << job.filename.toStdString();
            }
          }
          co_return;
        }(),
        "VK debug save entry/exit");
    }
  }
}

void ZVulkanImgRaycasterPipelineContext::recordStageFastDirect(Z3DRendererBase& renderer,
                                                               const RenderBatch& batch,
                                                               const ImgRaycasterPayload& payload,
                                                               const vk::Viewport& viewport,
                                                               const vk::Rect2D& scissor,
                                                               vk::raii::CommandBuffer& cmd)
{
  if (payload.visibleChannels.empty()) {
    return;
  }

  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }
  const bool deferredRound =
    (streamKey != 0u && m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
     m_deferredProgressive->eye == batch.eye &&
     m_deferredProgressive->progressiveGeneration == payload.progressiveGeneration);
  if (!deferredRound && streamKey != 0u && m_pendingFinalization && m_pendingFinalization->streamKey == streamKey &&
      m_pendingFinalization->eye == batch.eye) {
    markStageSkippedThisFrame(streamKey, batch.eye);
    return;
  }

  const bool hasIndices = payload.entryHasIndices && !payload.entryIndices.empty();
  const bool planarGeometry = !hasIndices;
  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureEmptyDescriptor();
  ensureQuadVertexBuffer();

  CHECK(payload.image) << "Vulkan img raycaster missing image context.";
  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster fast path: payload missing transferFunctions vector (fatal)";

  const size_t channelCount = payload.visibleChannels.size();
  CHECK_EQ(channelCount, 1u) << "FastDirect stage expects a single-channel payload";
  const size_t channelIndex = payload.visibleChannels.front();

  const auto& transferFunctions = *payload.transferFunctions;
  CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
    << "Missing transfer function for channel " << channelIndex;

  // Segment-managed fast direct draw: backend owns dynamic rendering segments and attachment transitions.
  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);
  if (formats.colorFormats.empty()) {
    return;
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);

  auto gpuScope = m_backend.beginGpuScope("ray_fast_direct");
  auto guard = folly::makeGuard([&]() {
    if (gpuScope) {
      m_backend.endGpuScope(*gpuScope);
    }
  });

  if (!planarGeometry) {
    CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
      << "Raycaster fast volume stage missing entry/exit lease.";
    auto* entryTexture = payload.entryExitLease->colorAttachment(0);
    CHECK(entryTexture != nullptr) << "Entry/exit texture unavailable";

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);

    const auto& viewState = renderer.viewState();
    const float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
    const float farClip = viewState.farClip;
    const float zeToZW_a = farClip * nearClip / (farClip - nearClip);
    const float zeToZW_b = 0.5f * (farClip + nearClip) / (farClip - nearClip) + 0.5f;

    updateChannelFastDescriptors(resources,
                                 payload,
                                 channelIndex,
                                 *entryTexture,
                                 volumeTex,
                                 transferTex,
                                 zeToZW_a,
                                 zeToZW_b,
                                 glm::vec3(static_cast<float>(channelImage.width()),
                                           static_cast<float>(channelImage.height()),
                                           static_cast<float>(channelImage.depth())));

    FastPipelineKey pipelineKey;
    pipelineKey.variant = FastPipelineVariant::Volume;
    pipelineKey.mode = composite.mode;
    pipelineKey.resultOpaque = composite.resultOpaque;
    pipelineKey.depthEnabled = formats.depthFormat.has_value();
    pipelineKey.colorFormats = formats.colorFormats;
    pipelineKey.depthFormat = formats.depthFormat;
    PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

    CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast volume stage missing descriptor";
    const std::array<vk::DescriptorSet, 1> descriptorSets{resources.fastDescriptor->descriptorSet()};

    struct RayPC
    {
      float s;
      float i;
      float l;
      float a;
      float b;
    } pc{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    drawSpec.descriptorSets = descriptorSets;
    drawSpec.descriptorSetFirst = 0;
    drawSpec.expectedDescriptorSetCount = 1;
    const std::array<vk::Buffer, 1> vertexBuffers{m_quadVertexBuffer->buffer()};
    const std::array<vk::DeviceSize, 1> vertexOffsets{0};
    drawSpec.vertexBuffers = vertexBuffers;
    drawSpec.vertexOffsets = vertexOffsets;
    drawSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
    drawSpec.instanceCount = 1;
    drawSpec.pushConstantsData = &pc;
    drawSpec.pushConstantsSize = sizeof(pc);
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    drawSpec.requirePushConstants = true;
    recorder.recordGraphicsDraw(drawSpec);
    return;
  }

  // Planar geometry fast direct: draw the entry quad(s) using either the 2D image shader
  // or the slice shader depending on the source data shape.
  ensureEntryGeometryUploadedThisFrame(payload);
  CHECK(m_entryVertexBuffer != nullptr) << "Raycaster fast planar stage missing entry vertex buffer";

  const size_t eyeIndex = std::min<size_t>(static_cast<size_t>(batch.eye), renderer.viewState().eyes.size() - 1);
  const auto& eyeState = renderer.viewState().eyes[eyeIndex];
  const glm::mat4 projectionView = eyeState.projectionMatrix * eyeState.viewMatrix;

  const uint32_t vertexCount = static_cast<uint32_t>(payload.entryPositions.size());
  CHECK_GT(vertexCount, 0u) << "Raycaster fast planar stage missing vertex data";

  if (payload.image->is2DData()) {
    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t imgGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& imageTex = ensureImage2DTexture(resources, channelImage, imgGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
    updateChannelImage2DDescriptors(resources, imageTex, transferTex);

    FastPipelineKey pipelineKey;
    pipelineKey.variant = FastPipelineVariant::Image2D;
    pipelineKey.mode = composite.mode;
    pipelineKey.resultOpaque = composite.resultOpaque;
    // GL parity: 2D image rendering participates in scene depth (and exports depth
    // for later compositing). If we don't write depth here, the layer depth stays
    // at the clear value (typically 1.0), and downstream depth-based composition
    // will only show the bounding box overlay that writes depth.
    pipelineKey.depthEnabled = formats.depthFormat.has_value();
    pipelineKey.colorFormats = formats.colorFormats;
    pipelineKey.depthFormat = formats.depthFormat;
    PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

    CHECK(resources.image2DDescriptor != nullptr) << "Raycaster fast image2D stage missing descriptor";
    const std::array<vk::DescriptorSet, 1> descriptorSets{resources.image2DDescriptor->descriptorSet()};

    struct Image2DPush
    {
      glm::mat4 projectionView;
    } pc{projectionView};

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    drawSpec.descriptorSets = descriptorSets;
    drawSpec.descriptorSetFirst = 0;
    drawSpec.expectedDescriptorSetCount = 1;
    const std::array<vk::Buffer, 1> vertexBuffers{m_entryVertexBuffer->buffer()};
    const std::array<vk::DeviceSize, 1> vertexOffsets{0};
    drawSpec.vertexBuffers = vertexBuffers;
    drawSpec.vertexOffsets = vertexOffsets;
    drawSpec.pushConstantsData = &pc;
    drawSpec.pushConstantsSize = sizeof(pc);
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    drawSpec.requirePushConstants = true;
    drawSpec.instanceCount = 1;

    if (payload.entryHasIndices && m_entryIndexBuffer && !payload.entryIndices.empty()) {
      drawSpec.indexBuffer = m_entryIndexBuffer->buffer();
      drawSpec.indexOffset = 0;
      drawSpec.indexType = vk::IndexType::eUint32;
      drawSpec.indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      drawSpec.firstIndex = 0;
      drawSpec.vertexOffset = 0;
      drawSpec.firstInstance = 0;
    } else {
      drawSpec.vertexCount = vertexCount;
      drawSpec.firstVertex = 0;
      drawSpec.firstInstance = 0;
    }

    recorder.recordGraphicsDraw(drawSpec);
    return;
  }

  const glm::mat4 viewMatrix = eyeState.viewMatrix;
  ChannelResources& resources = ensureChannelResources(channelIndex);
  const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
  const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
  updateChannelSliceDescriptors(resources, volumeTex, transferTex);

  FastPipelineKey pipelineKey;
  pipelineKey.variant = FastPipelineVariant::Slice2D;
  pipelineKey.mode = composite.mode;
  pipelineKey.resultOpaque = composite.resultOpaque;
  // GL parity: slices are depth-tested and must write depth for later composition.
  pipelineKey.depthEnabled = formats.depthFormat.has_value();
  pipelineKey.colorFormats = formats.colorFormats;
  pipelineKey.depthFormat = formats.depthFormat;
  PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

  CHECK(resources.sliceDescriptor != nullptr) << "Raycaster fast slice stage missing descriptor";
  const std::array<vk::DescriptorSet, 1> descriptorSets{resources.sliceDescriptor->descriptorSet()};

  struct SlicePush
  {
    glm::mat4 projectionView;
    glm::mat4 view;
  } pc{projectionView, viewMatrix};

  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 1;
  const std::array<vk::Buffer, 1> vertexBuffers{m_entryVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.pushConstantsData = &pc;
  drawSpec.pushConstantsSize = sizeof(pc);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
  drawSpec.requirePushConstants = true;
  drawSpec.instanceCount = 1;

  if (payload.entryHasIndices && m_entryIndexBuffer && !payload.entryIndices.empty()) {
    drawSpec.indexBuffer = m_entryIndexBuffer->buffer();
    drawSpec.indexOffset = 0;
    drawSpec.indexType = vk::IndexType::eUint32;
    drawSpec.indexCount = static_cast<uint32_t>(payload.entryIndices.size());
    drawSpec.firstIndex = 0;
    drawSpec.vertexOffset = 0;
    drawSpec.firstInstance = 0;
  } else {
    drawSpec.vertexCount = vertexCount;
    drawSpec.firstVertex = 0;
    drawSpec.firstInstance = 0;
  }

  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanImgRaycasterPipelineContext::recordStageFastLayers(Z3DRendererBase& renderer,
                                                               const RenderBatch& batch,
                                                               const ImgRaycasterPayload& payload,
                                                               const vk::Viewport& viewport,
                                                               const vk::Rect2D& scissor,
                                                               vk::raii::CommandBuffer& cmd)
{
  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }
  const bool deferredRound =
    (streamKey != 0u && m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
     m_deferredProgressive->eye == batch.eye &&
     m_deferredProgressive->progressiveGeneration == payload.progressiveGeneration);
  if (!deferredRound && streamKey != 0u && m_pendingFinalization && m_pendingFinalization->streamKey == streamKey &&
      m_pendingFinalization->eye == batch.eye) {
    markStageSkippedThisFrame(streamKey, batch.eye);
    return;
  }

  CHECK(payload.fastPathOnly) << "FastLayers stage requires fastPathOnly=true";
  CHECK(payload.visibleChannels.size() > 1u) << "FastLayers stage requires multi-channel payload";
  CHECK(payload.channelLayerLease && payload.channelLayerLease->hasVulkanImage())
    << "FastLayers stage requires a Vulkan layer lease for multi-channel fast rendering";

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureEmptyDescriptor();
  ensureQuadVertexBuffer();

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);
  const bool hasIndices = payload.entryHasIndices && !payload.entryIndices.empty();
  const bool planarGeometry = !hasIndices;
  if (auto t = m_backend.beginGpuScope("ray_fast_layers")) {
    if (planarGeometry) {
      recordFastPlanarLayersOnly(renderer, batch, payload, viewport, scissor, cmd, composite);
    } else {
      recordFastVolumeLayersOnly(renderer, batch, payload, viewport, scissor, cmd, composite);
    }
    m_backend.endGpuScope(*t);
  } else {
    if (planarGeometry) {
      recordFastPlanarLayersOnly(renderer, batch, payload, viewport, scissor, cmd, composite);
    } else {
      recordFastVolumeLayersOnly(renderer, batch, payload, viewport, scissor, cmd, composite);
    }
  }
}

void ZVulkanImgRaycasterPipelineContext::recordStageFastMerge(Z3DRendererBase& renderer,
                                                              const RenderBatch& batch,
                                                              const ImgRaycasterPayload& payload,
                                                              const vk::Viewport& viewport,
                                                              const vk::Rect2D& scissor,
                                                              vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }
  const bool deferredRound =
    (streamKey != 0u && m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
     m_deferredProgressive->eye == batch.eye &&
     m_deferredProgressive->progressiveGeneration == payload.progressiveGeneration);
  if (!deferredRound && streamKey != 0u && m_pendingFinalization && m_pendingFinalization->streamKey == streamKey &&
      m_pendingFinalization->eye == batch.eye) {
    markStageSkippedThisFrame(streamKey, batch.eye);
    return;
  }

  CHECK(payload.fastPathOnly) << "FastMerge stage requires fastPathOnly=true";
  CHECK(payload.visibleChannels.size() > 1u) << "FastMerge stage requires multi-channel payload";
  CHECK(payload.channelLayerLease && payload.channelLayerLease->hasVulkanImage())
    << "FastMerge stage requires a Vulkan layer lease for multi-channel fast rendering";

  auto* layerColor = payload.channelLayerLease->colorAttachment(0);
  auto* layerDepth = payload.channelLayerLease->depthAttachmentTexture();
  CHECK(layerColor != nullptr) << "FastMerge stage missing layer color attachment";

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureEmptyDescriptor();
  ensureQuadVertexBuffer();

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);
  if (auto t = m_backend.beginGpuScope("ray_fast_merge")) {
    recordMergeFromLayers(batch,
                          viewport,
                          scissor,
                          cmd,
                          composite,
                          *layerColor,
                          layerDepth,
                          static_cast<uint32_t>(payload.visibleChannels.size()));
    m_backend.endGpuScope(*t);
  } else {
    recordMergeFromLayers(batch,
                          viewport,
                          scissor,
                          cmd,
                          composite,
                          *layerColor,
                          layerDepth,
                          static_cast<uint32_t>(payload.visibleChannels.size()));
  }
}

void ZVulkanImgRaycasterPipelineContext::recordStageProgressivePreviewLayers(Z3DRendererBase& renderer,
                                                                             const RenderBatch& batch,
                                                                             const ImgRaycasterPayload& payload,
                                                                             const vk::Viewport& viewport,
                                                                             const vk::Rect2D& scissor,
                                                                             vk::raii::CommandBuffer& cmd)
{
  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }

  // Starting a new progressive cycle (preview frame): drop any stale deferred
  // bookkeeping from a previous cycle so we don't accidentally skip Block-ID
  // discovery / finalize the wrong channel.
  if (streamKey != 0u) {
    if (m_pendingFinalization && m_pendingFinalization->streamKey == streamKey &&
        m_pendingFinalization->eye == batch.eye) {
      m_pendingFinalization.reset();
    }
    if (m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
        m_deferredProgressive->eye == batch.eye) {
      m_deferredProgressive.reset();
    }
  }

  const bool deferredRound =
    (streamKey != 0u && m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
     m_deferredProgressive->eye == batch.eye &&
     m_deferredProgressive->progressiveGeneration == payload.progressiveGeneration);
  if (!deferredRound && streamKey != 0u && m_pendingFinalization && m_pendingFinalization->streamKey == streamKey &&
      m_pendingFinalization->eye == batch.eye) {
    markStageSkippedThisFrame(streamKey, batch.eye);
    return;
  }

  CHECK(!payload.fastPathOnly) << "ProgressivePreviewLayers stage requires fastPathOnly=false";
  CHECK(payload.channelIndexRaw < 0) << "ProgressivePreviewLayers stage requires channelIndexRaw < 0";

  const uint32_t channelCount = static_cast<uint32_t>(payload.visibleChannels.size());
  if (channelCount == 0u) {
    return;
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureEmptyDescriptor();
  ensureQuadVertexBuffer();

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);
  recordFastVolumeLayersOnly(renderer, batch, payload, viewport, scissor, cmd, composite);
}

void ZVulkanImgRaycasterPipelineContext::recordStageProgressiveBlockId(Z3DRendererBase& renderer,
                                                                       const RenderBatch& batch,
                                                                       const ImgRaycasterPayload& payload,
                                                                       const vk::Viewport& viewport,
                                                                       const vk::Rect2D& scissor,
                                                                       vk::raii::CommandBuffer& cmd)
{
  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }

  if (payload.channelIndexRaw < 0) {
    // Preview frame does not run Block-ID.
    return;
  }

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);
  auto prepOpt = ensurePreparedProgressiveRound(renderer, batch, payload, viewport, scissor, cmd, composite);
  if (!prepOpt || !prepOpt->ready) {
    return;
  }
  const ProgressivePrepState prep = *prepOpt;
  if (prep.skipBlockIdPass) {
    return;
  }

  CHECK(payload.blockIdLease && payload.blockIdLease->attachments != 0)
    << "Vulkan raycaster progressive Block-ID stage missing block-ID lease.";

  ChannelResources& resources = ensureChannelResources(prep.channelIndex);

  // Segment-managed Block-ID: the backend owns vkCmdBeginRendering/vkCmdEndRendering and
  // all attachment/layout transitions. This stage records draw commands only.
  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);
  CHECK(!formats.colorFormats.empty()) << "Vulkan raycaster Block-ID stage requires color attachments";
  const uint32_t blockAttachmentCount = static_cast<uint32_t>(formats.colorFormats.size());
  CHECK_EQ(blockAttachmentCount, payload.blockIdLease->attachments)
    << "Vulkan raycaster Block-ID stage attachment count mismatch: batch=" << blockAttachmentCount
    << " lease=" << payload.blockIdLease->attachments;

  const vk::Format blockFormat = formats.colorFormats.front();
  BlockIdPipelineKey blockKey{resources.levelCount, blockAttachmentCount, blockFormat};
  auto& blockPipeline = ensureBlockIdPipeline(blockKey, blockFormat);

  const bool preferOverrideStaticGlobal = true;
  std::vector<vk::DescriptorSet> blockDescriptorSets =
    collectProgressiveDescriptorSets(resources, preferOverrideStaticGlobal);
  struct RayPC
  {
    float s, i, l, a, b;
  } pcB{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, prep.zeToZW_a, prep.zeToZW_b};

  ZVulkanPipelineCommandRecorder recorder(cmd);
  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = blockPipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = blockPipeline.pipeline->pipelineLayoutHandle();

  const std::array<vk::DescriptorSet, 3> descriptorSets{blockDescriptorSets[0],
                                                        blockDescriptorSets[1],
                                                        blockDescriptorSets[2]};
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 3;
  const std::array<vk::Buffer, 1> vertexBuffers{m_quadVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  drawSpec.instanceCount = 1;
  drawSpec.pushConstantsData = &pcB;
  drawSpec.pushConstantsSize = sizeof(pcB);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;

  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanImgRaycasterPipelineContext::recordStageProgressiveCompaction(Z3DRendererBase& renderer,
                                                                          const RenderBatch& batch,
                                                                          const ImgRaycasterPayload& payload,
                                                                          const vk::Viewport& viewport,
                                                                          const vk::Rect2D& scissor,
                                                                          vk::raii::CommandBuffer& cmd)
{
  (void)viewport;
  (void)scissor;

  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }

  if (payload.channelIndexRaw < 0) {
    // Preview frame does not run compaction.
    return;
  }

  const bool skipBlockIdPass =
    (streamKey != 0u && m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
     m_deferredProgressive->eye == batch.eye &&
     m_deferredProgressive->progressiveGeneration == payload.progressiveGeneration);
  if (skipBlockIdPass) {
    return;
  }

  recordBlockIdCompaction(renderer, batch, payload, cmd);
}

void ZVulkanImgRaycasterPipelineContext::recordStageProgressiveRaycast(Z3DRendererBase& renderer,
                                                                       const RenderBatch& batch,
                                                                       const ImgRaycasterPayload& payload,
                                                                       const vk::Viewport& viewport,
                                                                       const vk::Rect2D& scissor,
                                                                       vk::raii::CommandBuffer& cmd)
{
  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }

  if (payload.channelIndexRaw < 0) {
    // Preview frame uses fast path (handled by ProgressivePreviewLayers + ProgressiveMerge).
    return;
  }

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);
  auto prepOpt = ensurePreparedProgressiveRound(renderer, batch, payload, viewport, scissor, cmd, composite);
  if (!prepOpt || !prepOpt->ready) {
    return;
  }
  const ProgressivePrepState prep = *prepOpt;

  ChannelResources& resources = ensureChannelResources(prep.channelIndex);

  // Segment-managed progressive raycast: the backend owns dynamic rendering and attachment transitions.
  // This stage records draw commands only.
  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);
  CHECK_EQ(formats.colorFormats.size(), 2u) << "Progressive raycast stage requires exactly 2 color attachments";

  ProgressivePipelineKey progressiveKey{formats.colorFormats[0],
                                        formats.colorFormats[1],
                                        composite.mode,
                                        composite.localMip,
                                        composite.resultOpaque};
  progressiveKey.levelCount = resources.levelCount;
  auto& progressivePipeline = ensureProgressivePipeline(progressiveKey, formats);

  const bool preferOverrideStaticGlobal = true;
  std::vector<vk::DescriptorSet> progressiveSets =
    collectProgressiveDescriptorSets(resources, preferOverrideStaticGlobal);
  CHECK_EQ(progressiveSets.size(), 3u) << "Progressive raycaster requires exactly 3 descriptor sets";

  struct RayPC2
  {
    float s, i, l, a, b;
  } pcP{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, prep.zeToZW_a, prep.zeToZW_b};

  ZVulkanPipelineCommandRecorder recorder(cmd);
  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = progressivePipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = progressivePipeline.pipeline->pipelineLayoutHandle();

  const std::array<vk::DescriptorSet, 3> descriptorSets{progressiveSets[0], progressiveSets[1], progressiveSets[2]};
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 3;
  const std::array<vk::Buffer, 1> vertexBuffers{m_quadVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  drawSpec.instanceCount = 1;
  drawSpec.pushConstantsData = &pcP;
  drawSpec.pushConstantsSize = sizeof(pcP);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;

  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanImgRaycasterPipelineContext::recordStageProgressiveCopyToLayers(Z3DRendererBase& renderer,
                                                                            const RenderBatch& batch,
                                                                            const ImgRaycasterPayload& payload,
                                                                            const vk::Viewport& viewport,
                                                                            const vk::Rect2D& scissor,
                                                                            vk::raii::CommandBuffer& cmd)
{
  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }

  if (payload.channelIndexRaw < 0) {
    // Preview frame does not run copy; it draws directly into the layer arrays.
    return;
  }

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);
  auto prepOpt = ensurePreparedProgressiveRound(renderer, batch, payload, viewport, scissor, cmd, composite);
  if (!prepOpt || !prepOpt->ready) {
    return;
  }
  const ProgressivePrepState prep = *prepOpt;

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureQuadVertexBuffer();

  // Segment-managed copy: batch targets exactly one array layer via AttachmentHandle.index,
  // and the backend owns dynamic rendering + attachment transitions.
  CHECK(payload.channelLayerLease && payload.channelLayerLease->hasVulkanImage())
    << "Vulkan raycaster progressive copy stage missing Vulkan layer lease";
  CHECK(!batch.pass.colorAttachments.empty()) << "Vulkan raycaster progressive copy stage requires a color attachment";
  CHECK(payload.channelIndexRaw >= 0) << "Vulkan raycaster progressive copy stage requires channelIndexRaw >= 0";
  CHECK_EQ(batch.pass.colorAttachments.front().handle.index, static_cast<uint32_t>(payload.channelIndexRaw))
    << "Vulkan raycaster progressive copy stage expects batch layer index to match channelIndexRaw";

  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);
  CHECK_EQ(formats.colorFormats.size(), 1u) << "Progressive copy stage requires exactly one color attachment";
  CHECK(formats.depthFormat.has_value()) << "Progressive copy stage requires a depth attachment";

  CopyPipelineKey layerCopyKey{formats.colorFormats, formats.depthFormat};
  auto& layerCopyPipeline = ensureCopyPipeline(layerCopyKey, formats);

  if (!m_copyDescriptor) {
    m_copyDescriptor = m_backend.allocateOverrideDescriptorSet(**m_copySetLayout);
  }
  CHECK(m_copyDescriptor != nullptr) << "Raycaster layer copy: override descriptor allocation failed (fatal)";
  m_copyDescriptor->updateTexture(0, *prep.currentColor, m_backend.defaultSampler());
  m_copyDescriptor->updateTexture(1,
                                  *prep.currentDepth,
                                  m_backend.defaultSampler(),
                                  vk::ImageLayout::eShaderReadOnlyOptimal,
                                  vk::ImageAspectFlags{});

  const std::array<vk::DescriptorSet, 1> descriptorSets{m_copyDescriptor->descriptorSet()};

  ZVulkanPipelineCommandRecorder recorder(cmd);
  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = layerCopyPipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = layerCopyPipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 1;
  const std::array<vk::Buffer, 1> vertexBuffers{m_quadVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  drawSpec.instanceCount = 1;

  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanImgRaycasterPipelineContext::recordStageProgressiveMerge(Z3DRendererBase& renderer,
                                                                     const RenderBatch& batch,
                                                                     const ImgRaycasterPayload& payload,
                                                                     const vk::Viewport& viewport,
                                                                     const vk::Rect2D& scissor,
                                                                     vk::raii::CommandBuffer& cmd)
{
  const uint64_t streamKey = payload.streamKey;
  if (stageSkippedThisFrame(streamKey, batch.eye)) {
    return;
  }

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);

  CHECK(payload.channelLayerLease && payload.channelLayerLease->hasVulkanImage())
    << "Vulkan raycaster merge stage missing Vulkan layer lease";
  auto* layerColor = payload.channelLayerLease->colorAttachment(0);
  auto* layerDepth = payload.channelLayerLease->depthAttachmentTexture();
  CHECK(layerColor != nullptr) << "Vulkan raycaster merge stage missing layer color attachment";

  uint32_t channelCount = static_cast<uint32_t>(payload.visibleChannels.size());
  const bool wantsFinalization = payload.requestFinalization;
  bool setFinalization = false;
  bool consumeDeferredRound = false;
  Finalization fin{};

  if (payload.channelIndexRaw < 0) {
    // Preview merge: request renderer to flip channelIdx from -1 to 0 (GL parity).
    if (wantsFinalization && streamKey != 0u) {
      setFinalization = true;
      fin.streamKey = streamKey;
      fin.eye = batch.eye;
      fin.lastRound = false;
      fin.channelCount = channelCount;
    }
  } else {
    if (wantsFinalization) {
      auto prepOpt = ensurePreparedProgressiveRound(renderer, batch, payload, viewport, scissor, cmd, composite);
      if (!prepOpt || !prepOpt->ready) {
        return;
      }
      const ProgressivePrepState prep = *prepOpt;
      channelCount = prep.channelCount;

      if (streamKey != 0u) {
        setFinalization = true;
        fin.streamKey = streamKey;
        fin.eye = batch.eye;
        if (prep.finalizeAfterProgressive) {
          fin.lastRound = prep.finalizeAfterProgressive->lastRound;
          fin.channelCount = prep.finalizeAfterProgressive->channelCount;
          consumeDeferredRound = true;
        } else {
          fin.lastRound = false;
          fin.channelCount = channelCount;
        }
      }
    }
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureQuadVertexBuffer();

  recordMergeFromLayers(batch, viewport, scissor, cmd, composite, *layerColor, layerDepth, channelCount);

  if (setFinalization) {
    m_pendingFinalization = fin;
    if (consumeDeferredRound && m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
        m_deferredProgressive->eye == batch.eye &&
        m_deferredProgressive->progressiveGeneration == payload.progressiveGeneration) {
      m_deferredProgressive.reset();
    }
  }
}

void ZVulkanImgRaycasterPipelineContext::recordFastVolumeLayersOnly(Z3DRendererBase& renderer,
                                                                    const RenderBatch& batch,
                                                                    const ImgRaycasterPayload& payload,
                                                                    const vk::Viewport& viewport,
                                                                    const vk::Rect2D& scissor,
                                                                    vk::raii::CommandBuffer& cmd,
                                                                    const CompositingConfig& composite)
{
  const size_t channelCount = payload.visibleChannels.size();
  CHECK_GT(channelCount, 1u) << "recordFastVolumeLayersOnly requires multi-channel payload";
  CHECK(payload.image) << "Vulkan img raycaster missing image context.";
  CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
    << "Raycaster fast layers stage missing entry/exit lease.";

  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster fast path: payload missing transferFunctions vector (fatal)";
  const auto& transferFunctions = *payload.transferFunctions;

  auto* entryTexture = payload.entryExitLease->colorAttachment(0);
  CHECK(entryTexture) << "Entry/exit texture unavailable.";
  // Entry/exit is produced by the EntryExit stage and must be transitioned by the backend
  // via attachment finalUse + externalImageUses metadata (no barriers inside dynamic rendering).

  const auto& viewState = renderer.viewState();
  const float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
  const float farClip = viewState.farClip;
  const float zeToZW_a = farClip * nearClip / (farClip - nearClip);
  const float zeToZW_b = 0.5f * (farClip + nearClip) / (farClip - nearClip) + 0.5f;

  // Segment-managed layered rendering: this batch targets exactly one array layer via
  // AttachmentHandle.index set by the stage recorder (Z3DImgRaycasterRenderer::recordVulkanStagesToScript).
  // Use that index to select which channel to render into the active layer target.
  CHECK(!batch.pass.colorAttachments.empty()) << "FastLayers stage requires an active color attachment";
  const uint32_t order = batch.pass.colorAttachments.front().handle.index;
  CHECK(order < channelCount) << "FastLayers stage layer index out of range: " << order << " >= " << channelCount;

  const size_t channelIndex = payload.visibleChannels[static_cast<size_t>(order)];
  CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
    << "Missing transfer function for channel " << channelIndex;

  ChannelResources& resources = ensureChannelResources(channelIndex);
  const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
  const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);

  updateChannelFastDescriptors(resources,
                               payload,
                               channelIndex,
                               *entryTexture,
                               volumeTex,
                               transferTex,
                               zeToZW_a,
                               zeToZW_b,
                               glm::vec3(static_cast<float>(channelImage.width()),
                                         static_cast<float>(channelImage.height()),
                                         static_cast<float>(channelImage.depth())));

  const auto layerFormats = vulkan::extractAttachmentFormats(batch);

  FastPipelineKey layerKey;
  layerKey.variant = FastPipelineVariant::Volume;
  layerKey.mode = composite.mode;
  layerKey.resultOpaque = composite.resultOpaque;
  layerKey.depthEnabled = layerFormats.depthFormat.has_value();
  layerKey.colorFormats = layerFormats.colorFormats;
  layerKey.depthFormat = layerFormats.depthFormat;
  PipelineInstance& layerPipeline = ensureFastPipeline(layerKey);

  CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path missing fast descriptor (layered)";
  const std::array<vk::DescriptorSet, 1> descriptorSets{resources.fastDescriptor->descriptorSet()};

  struct RayPC
  {
    float s;
    float i;
    float l;
    float a;
    float b;
  } pcL{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};

  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = layerPipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = layerPipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 1;
  const std::array<vk::Buffer, 1> vertexBuffers{m_quadVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  drawSpec.instanceCount = 1;
  drawSpec.pushConstantsData = &pcL;
  drawSpec.pushConstantsSize = sizeof(pcL);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;

  ZVulkanPipelineCommandRecorder recorder(cmd);
  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanImgRaycasterPipelineContext::recordFastPlanarLayersOnly(Z3DRendererBase& renderer,
                                                                    const RenderBatch& batch,
                                                                    const ImgRaycasterPayload& payload,
                                                                    const vk::Viewport& viewport,
                                                                    const vk::Rect2D& scissor,
                                                                    vk::raii::CommandBuffer& cmd,
                                                                    const CompositingConfig& composite)
{
  const size_t channelCount = payload.visibleChannels.size();
  CHECK_GT(channelCount, 1u) << "recordFastPlanarLayersOnly requires multi-channel payload";
  CHECK(payload.image) << "Vulkan img raycaster missing image context.";
  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster fast path: payload missing transferFunctions vector (fatal)";
  const auto& transferFunctions = *payload.transferFunctions;

  // Segment-managed layered rendering: this batch targets exactly one array layer via
  // AttachmentHandle.index set by the stage recorder (Z3DImgRaycasterRenderer::recordVulkanStagesToScript).
  // Use that index to select which channel to render into the active layer target.
  CHECK(!batch.pass.colorAttachments.empty()) << "FastLayers stage requires an active color attachment";
  const uint32_t order = batch.pass.colorAttachments.front().handle.index;
  CHECK(order < channelCount) << "FastLayers stage layer index out of range: " << order << " >= " << channelCount;
  const size_t channelIndex = payload.visibleChannels[static_cast<size_t>(order)];
  CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
    << "Missing transfer function for channel " << channelIndex;

  // Planar geometry uses the uploaded entry vertex buffer (expanded quads/slices).
  ensureEntryGeometryUploadedThisFrame(payload);
  CHECK(m_entryVertexBuffer != nullptr) << "Raycaster fast planar layers stage missing entry vertex buffer";

  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);
  if (formats.colorFormats.empty()) {
    return;
  }

  const size_t eyeIndex = std::min<size_t>(static_cast<size_t>(batch.eye), renderer.viewState().eyes.size() - 1);
  const auto& eyeState = renderer.viewState().eyes[eyeIndex];
  const glm::mat4 projectionView = eyeState.projectionMatrix * eyeState.viewMatrix;

  const uint32_t vertexCount = static_cast<uint32_t>(payload.entryPositions.size());
  CHECK_GT(vertexCount, 0u) << "Raycaster fast planar layers stage missing vertex data";

  ZVulkanPipelineCommandRecorder recorder(cmd);

  if (payload.image->is2DData()) {
    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t imgGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& imageTex = ensureImage2DTexture(resources, channelImage, imgGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
    updateChannelImage2DDescriptors(resources, imageTex, transferTex);

    FastPipelineKey pipelineKey;
    pipelineKey.variant = FastPipelineVariant::Image2D;
    pipelineKey.mode = composite.mode;
    pipelineKey.resultOpaque = composite.resultOpaque;
    pipelineKey.depthEnabled = formats.depthFormat.has_value();
    pipelineKey.colorFormats = formats.colorFormats;
    pipelineKey.depthFormat = formats.depthFormat;
    PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

    CHECK(resources.image2DDescriptor != nullptr) << "Raycaster fast planar layers: missing image2D descriptor";
    const std::array<vk::DescriptorSet, 1> descriptorSets{resources.image2DDescriptor->descriptorSet()};

    struct Image2DPush
    {
      glm::mat4 projectionView;
    } pc{projectionView};

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    drawSpec.descriptorSets = descriptorSets;
    drawSpec.descriptorSetFirst = 0;
    drawSpec.expectedDescriptorSetCount = 1;
    const std::array<vk::Buffer, 1> vertexBuffers{m_entryVertexBuffer->buffer()};
    const std::array<vk::DeviceSize, 1> vertexOffsets{0};
    drawSpec.vertexBuffers = vertexBuffers;
    drawSpec.vertexOffsets = vertexOffsets;
    drawSpec.pushConstantsData = &pc;
    drawSpec.pushConstantsSize = sizeof(pc);
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    drawSpec.requirePushConstants = true;
    drawSpec.instanceCount = 1;

    if (payload.entryHasIndices && m_entryIndexBuffer && !payload.entryIndices.empty()) {
      drawSpec.indexBuffer = m_entryIndexBuffer->buffer();
      drawSpec.indexOffset = 0;
      drawSpec.indexType = vk::IndexType::eUint32;
      drawSpec.indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      drawSpec.firstIndex = 0;
      drawSpec.vertexOffset = 0;
      drawSpec.firstInstance = 0;
    } else {
      drawSpec.vertexCount = vertexCount;
      drawSpec.firstVertex = 0;
      drawSpec.firstInstance = 0;
    }

    recorder.recordGraphicsDraw(drawSpec);
    return;
  }

  const glm::mat4 viewMatrix = eyeState.viewMatrix;
  ChannelResources& resources = ensureChannelResources(channelIndex);
  const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
  const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
  updateChannelSliceDescriptors(resources, volumeTex, transferTex);

  FastPipelineKey pipelineKey;
  pipelineKey.variant = FastPipelineVariant::Slice2D;
  pipelineKey.mode = composite.mode;
  pipelineKey.resultOpaque = composite.resultOpaque;
  pipelineKey.depthEnabled = formats.depthFormat.has_value();
  pipelineKey.colorFormats = formats.colorFormats;
  pipelineKey.depthFormat = formats.depthFormat;
  PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

  CHECK(resources.sliceDescriptor != nullptr) << "Raycaster fast planar layers: missing slice descriptor";
  const std::array<vk::DescriptorSet, 1> descriptorSets{resources.sliceDescriptor->descriptorSet()};

  struct SlicePush
  {
    glm::mat4 projectionView;
    glm::mat4 view;
  } pc{projectionView, viewMatrix};

  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 1;
  const std::array<vk::Buffer, 1> vertexBuffers{m_entryVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.pushConstantsData = &pc;
  drawSpec.pushConstantsSize = sizeof(pc);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
  drawSpec.requirePushConstants = true;
  drawSpec.instanceCount = 1;

  if (payload.entryHasIndices && m_entryIndexBuffer && !payload.entryIndices.empty()) {
    drawSpec.indexBuffer = m_entryIndexBuffer->buffer();
    drawSpec.indexOffset = 0;
    drawSpec.indexType = vk::IndexType::eUint32;
    drawSpec.indexCount = static_cast<uint32_t>(payload.entryIndices.size());
    drawSpec.firstIndex = 0;
    drawSpec.vertexOffset = 0;
    drawSpec.firstInstance = 0;
  } else {
    drawSpec.vertexCount = vertexCount;
    drawSpec.firstVertex = 0;
    drawSpec.firstInstance = 0;
  }

  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanImgRaycasterPipelineContext::recordMergeFromLayers(const RenderBatch& batch,
                                                               const vk::Viewport& viewport,
                                                               const vk::Rect2D& scissor,
                                                               vk::raii::CommandBuffer& cmd,
                                                               const CompositingConfig& composite,
                                                               ZVulkanTexture& layerColor,
                                                               /*nullable*/ ZVulkanTexture* layerDepth,
                                                               uint32_t channelCount)
{
  // Segment-managed merge: backend owns vkCmdBeginRendering/vkCmdEndRendering and all attachment transitions.
  // This helper only emits draw commands that sample the layer array textures and write into the active surface.
  const vulkan::AttachmentFormats finalFormats = vulkan::extractAttachmentFormats(batch);
  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = composite.maxProjectionMerge;
  mergeKey.resultOpaque = composite.resultOpaque;
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;

  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);
  bindMergeDescriptor(layerColor, layerDepth);

  CHECK(m_mergeDescriptor != nullptr) << "Raycaster merge requires descriptor set";
  const std::array<vk::DescriptorSet, 1> descriptorSets{m_mergeDescriptor->descriptorSet()};

  ZVulkanPipelineCommandRecorder recorder(cmd);
  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = mergePipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = mergePipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 1;
  const std::array<vk::Buffer, 1> vertexBuffers{m_quadVertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{0};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  drawSpec.instanceCount = 1;

  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanImgRaycasterPipelineContext::ensureDescriptorPool()
{
  if (!m_descriptorPool) {
    m_descriptorPool = m_backend.device().createDescriptorPool();
  }
}

void ZVulkanImgRaycasterPipelineContext::ensureDescriptorLayouts()
{
  auto& device = m_backend.device().context().device();

  if (!m_entrySetLayout) {
    vk::DescriptorSetLayoutCreateInfo info{};
    m_entrySetLayout.emplace(device, info);
  }

  if (!m_fastSetLayout) {
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_fastSetLayout.emplace(device, info);
  }

  if (!m_image2DSetLayout) {
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
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_image2DSetLayout.emplace(device, info);
  }

  if (!m_sliceFastSetLayout) {
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
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_sliceFastSetLayout.emplace(device, info);
  }

  // Progressive static textures: page_directory, page_table_cache, image_cache, volume, transfer
  if (!m_progressiveStaticSetLayout) {
    // Immutable samplers per binding:
    //  - binding 0: page_directory (nearest clamp)
    //  - binding 1: page_table_cache (nearest clamp)
    //  - binding 2: image_cache (linear/default)
    //  - binding 3: volume (linear/default)
    //  - binding 4: transfer (linear/default)
    const vk::Sampler immNearest = m_backend.nearestClampSampler();
    const vk::Sampler immLinear = m_backend.defaultSampler();
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
      if (i == 0 || i == 1) {
        bindings[i].pImmutableSamplers = &immNearest;
      } else {
        bindings[i].pImmutableSamplers = &immLinear;
      }
    }
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_progressiveStaticSetLayout.emplace(device, info);
  }

  // Progressive dynamic textures: entry/exit, last_depth, last_color
  if (!m_progressiveDynamicSetLayout) {
    // Dynamic set: default sampler is acceptable; ray textures are float/normalized.
    vk::Sampler imm = m_backend.defaultSampler();
    std::array<vk::Sampler, 3> imms{imm, imm, imm};
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
      bindings[i].pImmutableSamplers = &imms[i];
    }
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_progressiveDynamicSetLayout.emplace(device, info);
  }

  if (!m_pageSetLayout) {
    vk::DescriptorSetLayoutBinding binding{.binding = 2,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_pageSetLayout.emplace(device, info);
  }

  if (!m_transformSetLayout) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags =
                                             vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_transformSetLayout.emplace(device, info);
  }

  if (!m_copySetLayout) {
    // Immutable samplers to avoid sampler writes
    vk::Sampler imm = m_backend.defaultSampler();
    std::array<vk::Sampler, 2> imms{imm, imm};
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
      bindings[i].pImmutableSamplers = &imms[i];
    }
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_copySetLayout.emplace(device, info);
  }

  if (!m_mergeSetLayout) {
    // Immutable samplers to avoid sampler writes
    vk::Sampler imm = m_backend.defaultSampler();
    std::array<vk::Sampler, 2> imms{imm, imm};
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
      bindings[i].pImmutableSamplers = &imms[i];
    }
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_mergeSetLayout.emplace(device, info);
  }

  if (!m_emptySetLayout) {
    vk::DescriptorSetLayoutCreateInfo info{};
    m_emptySetLayout.emplace(device, info);
  }
}

void ZVulkanImgRaycasterPipelineContext::ensureEmptyDescriptor()
{
  if (m_emptyDescriptor) {
    return;
  }
  m_emptyDescriptor = m_backend.allocateFrameDescriptorSet(**m_emptySetLayout);
  CHECK(m_emptyDescriptor != nullptr) << "Raycaster: failed to allocate empty descriptor set";
}

void ZVulkanImgRaycasterPipelineContext::ensureEntryVertexCapacity(size_t vertexCount, size_t indexCount)
{
  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Raycaster entry geometry upload requires an active Vulkan frame";
  EntryGeometryBuffers& buffers = m_entryGeometryBuffers[frameKey];

  auto& device = m_backend.device();
  if (vertexCount > buffers.vertexCapacity) {
    buffers.vertexCapacity = vertexCount;
    buffers.vertexBuffer =
      device.createBuffer(buffers.vertexCapacity * sizeof(EntryVertex),
                          vk::BufferUsageFlagBits::eVertexBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    CHECK(buffers.vertexBuffer != nullptr)
      << "Failed to allocate entry vertex buffer, count=" << buffers.vertexCapacity;
  }

  if (indexCount > buffers.indexCapacity) {
    buffers.indexCapacity = indexCount;
    if (buffers.indexCapacity == 0) {
      buffers.indexBuffer.reset();
    } else {
      buffers.indexBuffer =
        device.createBuffer(buffers.indexCapacity * sizeof(uint32_t),
                            vk::BufferUsageFlagBits::eIndexBuffer,
                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
      CHECK(buffers.indexBuffer != nullptr) << "Failed to allocate entry index buffer, count=" << buffers.indexCapacity;
    }
  }

  m_entryVertexBuffer = buffers.vertexBuffer.get();
  m_entryIndexBuffer = buffers.indexBuffer.get();
}

void ZVulkanImgRaycasterPipelineContext::ensureQuadVertexBuffer()
{
  if (m_quadVertexBuffer && m_quadVertexCount == 4) {
    return;
  }

  auto& device = m_backend.device();
  std::array<glm::vec3, 4> quad = {
    glm::vec3{-1.f, -1.f, 0.f},
    glm::vec3{-1.f, 1.f,  0.f},
    glm::vec3{1.f,  -1.f, 0.f},
    glm::vec3{1.f,  1.f,  0.f}
  };
  m_quadVertexCount = quad.size();
  m_quadVertexBuffer =
    device.createBuffer(quad.size() * sizeof(glm::vec3),
                        vk::BufferUsageFlagBits::eVertexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  CHECK(m_quadVertexBuffer != nullptr) << "Failed to allocate raycaster quad vertex buffer";
  m_quadVertexBuffer->copyData(quad.data(), quad.size() * sizeof(glm::vec3));
}

void ZVulkanImgRaycasterPipelineContext::uploadEntryGeometry(const ImgRaycasterPayload& payload)
{
  const size_t indexCount = payload.entryHasIndices ? payload.entryIndices.size() : 0u;
  ensureEntryVertexCapacity(payload.entryPositions.size(), indexCount);
  if (m_entryVertexBuffer == nullptr) {
    return;
  }

  std::vector<EntryVertex> vertices(payload.entryPositions.size());
  for (size_t i = 0; i < payload.entryPositions.size(); ++i) {
    vertices[i].position = payload.entryPositions[i];
    if (i < payload.entryTexCoords.size()) {
      vertices[i].texCoord = payload.entryTexCoords[i];
    }
  }
  const size_t vbBytes = vertices.size() * sizeof(EntryVertex);
  const size_t ibBytes = indexCount * sizeof(uint32_t);

  // Entry geometry uploads must be safe inside dynamic rendering. Use host-visible
  // per-frame-slot buffers (keyed by activeFrameKey) and memcpy the data.
  if (vbBytes > 0) {
    m_entryVertexBuffer->copyData(vertices.data(), vbBytes);
  }
  if (ibBytes > 0) {
    CHECK(m_entryIndexBuffer != nullptr) << "Entry geometry has indices but index buffer is missing";
    m_entryIndexBuffer->copyData(payload.entryIndices.data(), ibBytes);
  }
}

void ZVulkanImgRaycasterPipelineContext::ensureEntryGeometryUploadedThisFrame(const ImgRaycasterPayload& payload)
{
  const size_t vertexCount = payload.entryPositions.size();
  CHECK_GT(vertexCount, 0u) << "Entry geometry upload requires non-empty vertex data";

  if (!m_entryGeometryUploadedThisFrame) {
    uploadEntryGeometry(payload);
    m_entryGeometryUploadedThisFrame = true;
    m_entryGeometryVertexCountThisFrame = vertexCount;
    m_entryGeometryIndexCountThisFrame = payload.entryIndices.size();
    return;
  }

  CHECK(m_entryGeometryVertexCountThisFrame == vertexCount &&
        m_entryGeometryIndexCountThisFrame == payload.entryIndices.size())
    << "Entry geometry changed within one frame; stage payload splitting should share a common geometry blob";
}

void ZVulkanImgRaycasterPipelineContext::ensureEntryTransformResources(Z3DRendererBase& renderer,
                                                                       const RenderBatch& batch,
                                                                       const ImgRaycasterPayload& /*payload*/)
{
  auto& device = m_backend.device();
  if (!m_entryTransformBuffer) {
    m_entryTransformBuffer =
      device.createBuffer(sizeof(TransformsUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    CHECK(m_entryTransformBuffer != nullptr) << "Raycaster entry: failed to allocate transform UBO";
  }

  const size_t eyeIndex = std::min<size_t>(static_cast<size_t>(batch.eye), renderer.viewState().eyes.size() - 1);
  const auto& eyeState = renderer.viewState().eyes[eyeIndex];

  TransformsUBOStd140 transforms{};
  transforms.projection_view_matrix = eyeState.projectionMatrix * eyeState.viewMatrix;
  transforms.view_matrix = eyeState.viewMatrix;
  transforms.pos_transform = glm::mat4(1.0f);
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(glm::mat3(1.0f));
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  transforms.parameters = glm::vec4(1.0f, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);

  m_entryTransformBuffer->copyData(&transforms, sizeof(transforms));

  if (!m_entryTransformDescriptor) {
    m_entryTransformDescriptor = m_backend.allocateOverrideDescriptorSet(**m_transformSetLayout);
    CHECK(m_entryTransformDescriptor != nullptr) << "Raycaster entry: failed to allocate transform descriptor set";
  }
  m_entryTransformDescriptor->updateUniformBuffer(0, *m_entryTransformBuffer);
}

namespace {
inline bool vkBlockIdUseStorage()
{
  const std::string v = FLAGS_atlas_vk_blockid_compaction_source;
  return v == "storage" || v == "Storage" || v == "STORAGE";
}
inline bool vkBlockIdUseBuffer()
{
  const std::string v = FLAGS_atlas_vk_blockid_compaction_source;
  return v == "buffer" || v == "Buffer" || v == "BUFFER";
}
} // namespace

void ZVulkanImgRaycasterPipelineContext::ensureBlockIdCompactionPipeline(uint32_t attachmentCount, int /*mode*/)
{
  (void)attachmentCount; // current compaction uses first attachment only
  // Append-only compaction; rebuild per call to keep compatible with current source
  auto& device = m_backend.device().context().device();
  const bool storage = vkBlockIdUseStorage();
  const bool buffer = vkBlockIdUseBuffer();
  const std::string shaderBase = nim::ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";
  const std::string compPath = buffer ? (shaderBase + "block_id_compact_buffer_append.comp.spv")
                                      : (storage ? (shaderBase + "block_id_compact_storage_append.comp.spv")
                                                 : (shaderBase + "block_id_compact_append.comp.spv"));

  if (buffer) {
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
    m_blockIdCompactSetLayoutBuffer.emplace(
      device,
      vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                        .pBindings = bindings.data()});
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 5};
    m_blockIdCompactPipelineLayoutBuffer.emplace(
      device,
      vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                   .pSetLayouts = &**m_blockIdCompactSetLayoutBuffer,
                                   .pushConstantRangeCount = 1,
                                   .pPushConstantRanges = &pc});
    auto spirv = readSpirvFile(compPath);
    vk::raii::ShaderModule compModule(
      device,
      vk::ShaderModuleCreateInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()});
    vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                            .module = *compModule,
                                            .pName = "main"};
    m_blockIdCompactPipelineBufferAppend.reset();
    m_blockIdCompactPipelineBufferAppend.emplace(
      device,
      nullptr,
      vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_blockIdCompactPipelineLayoutBuffer});
  } else if (storage) {
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
    m_blockIdCompactSetLayoutStorage.emplace(
      device,
      vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                        .pBindings = bindings.data()});
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 5};
    m_blockIdCompactPipelineLayoutStorage.emplace(
      device,
      vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                   .pSetLayouts = &**m_blockIdCompactSetLayoutStorage,
                                   .pushConstantRangeCount = 1,
                                   .pPushConstantRanges = &pc});
    auto spirv = readSpirvFile(compPath);
    vk::raii::ShaderModule compModule(
      device,
      vk::ShaderModuleCreateInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()});
    vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                            .module = *compModule,
                                            .pName = "main"};
    m_blockIdCompactPipelineStorage.reset();
    m_blockIdCompactPipelineStorage.emplace(
      device,
      nullptr,
      vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_blockIdCompactPipelineLayoutStorage});
  } else {
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
    m_blockIdCompactSetLayoutSampled.emplace(
      device,
      vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                        .pBindings = bindings.data()});
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 5};
    m_blockIdCompactPipelineLayoutSampled.emplace(
      device,
      vk::PipelineLayoutCreateInfo{.setLayoutCount = 1,
                                   .pSetLayouts = &**m_blockIdCompactSetLayoutSampled,
                                   .pushConstantRangeCount = 1,
                                   .pPushConstantRanges = &pc});
    auto spirv = readSpirvFile(compPath);
    vk::raii::ShaderModule compModule(
      device,
      vk::ShaderModuleCreateInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()});
    vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                            .module = *compModule,
                                            .pName = "main"};
    m_blockIdCompactPipelineSampled.reset();
    m_blockIdCompactPipelineSampled.emplace(
      device,
      nullptr,
      vk::ComputePipelineCreateInfo{.stage = stage, .layout = **m_blockIdCompactPipelineLayoutSampled});
  }

  if (VLOG_IS_ON(1)) {
    VLOG(1) << fmt::format("ensureBlockIdCompactionPipeline: mode={} source={} shader='{}'",
                           1,
                           buffer ? "buffer" : (storage ? "storage" : "sampled"),
                           compPath);
  }
}

// (probe pipelines removed)

ZVulkanImgRaycasterPipelineContext::BlockIdCompactionOutput&
ZVulkanImgRaycasterPipelineContext::ensureBlockIdCompactOutput(size_t bytes)
{
  CHECK(bytes > 0) << "ensureBlockIdCompactOutput called with zero bytes";
  void* key = m_backend.activeFrameKey();
  CHECK(key != nullptr) << "ensureBlockIdCompactOutput called with no active frame";

  // The executor may rebuild its frame slots when maxFramesInFlight changes or
  // when the shared Vulkan device is replaced. Frame keys become invalid across
  // such rebuilds, so clear any per-slot buffers when those properties change.
  auto& dev = m_backend.device();
  const uint32_t maxFrames = dev.frameExecutor().maxFramesInFlight();
  if (m_blockIdCompactDevice != &dev || m_blockIdCompactMaxFramesInFlight != maxFrames) {
    m_blockIdCompactOutputs.clear();
    m_blockIdCompactDevice = &dev;
    m_blockIdCompactMaxFramesInFlight = maxFrames;
  }

  auto& out = m_blockIdCompactOutputs[key];
  if (out.buffer && out.capacity >= bytes) {
    return out;
  }

  // Allocate a device-local SSBO for compaction output.
  //
  // Why device-local:
  // - The compaction shader performs a large number of atomic writes. Writing
  //   directly into host-coherent memory is extremely slow on MoltenVK/Metal
  //   and can trigger GPU timeouts at full resolution.
  // - CPU parsing is performed via the backend's end-of-frame readback API
  //   (buffer->staging copy + mapped ring), which requires eTransferSrc usage.
  //
  // Why eTransferDst:
  // - We clear the header (count + counts[8]) via vkCmdFillBuffer (GPU-side),
  //   which is a transfer write.
  auto unique = dev.createBuffer(bytes,
                                 vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                                   vk::BufferUsageFlagBits::eTransferDst,
                                 vk::MemoryPropertyFlagBits::eDeviceLocal);
  out.buffer = std::shared_ptr<ZVulkanBuffer>(std::move(unique));
  out.capacity = bytes;
  return out;
}

// counts snapshot removed; counts are embedded in unified compact buffer header

// (probe pipelines removed)

// (probe pipelines removed)

void ZVulkanImgRaycasterPipelineContext::recordBlockIdCompaction(Z3DRendererBase& renderer,
                                                                 const RenderBatch& batch,
                                                                 const ImgRaycasterPayload& payload,
                                                                 vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  const folly::CancellationToken cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  processEventsAndMaybeCancel(cancellationToken);
  (void)batch;
  if (!payload.blockIdLease || !payload.blockIdLease->hasVulkanImage()) {
    return;
  }
  // Ensure pipeline and output buffer
  const int mode = FLAGS_atlas_vk_blockid_compaction_mode;
  // Use all block-ID attachments (parity with GL): compact each attachment sequentially into the same output
  ZVulkanTexture* firstBlock = payload.blockIdLease->colorAttachment(0);
  if (!firstBlock) {
    return;
  }
  const uint32_t attachmentCount = std::max<uint32_t>(1u, payload.blockIdLease->attachments);
  CHECK_LE(attachmentCount, 8u) << "Block-ID compaction supports up to 8 attachments";
  uint32_t effectiveAttachmentCount = attachmentCount;
  if (payload.blockIdEffectiveAttachmentCount != 0u) {
    effectiveAttachmentCount =
      std::min<uint32_t>(attachmentCount, std::max<uint32_t>(1u, payload.blockIdEffectiveAttachmentCount));
  }
  CHECK_LE(effectiveAttachmentCount, 8u) << "Unified BlockList header supports up to 8 attachments";
  ensureBlockIdCompactionPipeline(effectiveAttachmentCount, mode);
  uint32_t imgW = firstBlock->width();
  uint32_t imgH = firstBlock->height();
  if (imgW == 0u || imgH == 0u) {
    VLOG(1) << fmt::format("BlockID compaction skipped: empty attachment size {}x{}", imgW, imgH);
    return;
  }
  // Optional: dual probe (sampled + storage) and skip normal compaction
  // (probes removed)
  std::shared_ptr<ZVulkanBuffer> compactOutput;
  size_t compactOutputBytes = 0;
  // Append-only compaction (drop hash variants): allocate append buffer
  {
    // Unified output buffer:
    //   [count (1x u32)] + [counts[8] (8x u32)] + ids[capacity]
    const uint32_t capacityIDs = std::max<uint32_t>(1u, imgW * imgH * 4u);
    const uint32_t headerWords = 1u + 8u; // count + counts[8]
    const size_t bytes = static_cast<size_t>(headerWords + capacityIDs) * sizeof(uint32_t);
    auto& out = ensureBlockIdCompactOutput(bytes);
    CHECK(out.buffer != nullptr);
    compactOutput = out.buffer;
    compactOutputBytes = bytes;
    // Zero the header (count + counts[8]) on GPU. The buffer is device-local so
    // it is not CPU-mappable.
    const size_t headerBytes = static_cast<size_t>(headerWords) * sizeof(uint32_t);
    CHECK((compactOutput->usage() & vk::BufferUsageFlagBits::eTransferDst) != vk::BufferUsageFlags{})
      << "Block-ID compaction output must support vkCmdFillBuffer (eTransferDst usage missing)";
    cmd.fillBuffer(compactOutput->buffer(), /*dstOffset=*/0, headerBytes, /*data=*/0u);
    // Ensure the filled header is visible to the compaction compute shader.
    {
      vk::BufferMemoryBarrier2 bb{};
      bb.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
      bb.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
      bb.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
      bb.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
      bb.buffer = compactOutput->buffer();
      bb.offset = 0;
      bb.size = headerBytes;
      vk::DependencyInfo dep{};
      dep.bufferMemoryBarrierCount = 1;
      dep.pBufferMemoryBarriers = &bb;
      cmd.pipelineBarrier2(dep);
    }
    if (VLOG_IS_ON(1)) {
      VLOG(1) << fmt::format("BlockID compaction (append): output capacity={} bytes (header[9] + ids)", bytes);
    }
  }
  CHECK(compactOutput != nullptr);
  CHECK_GT(compactOutputBytes, 0u);

  // Record compute dispatch
  ZVulkanPipelineCommandRecorder recorder(cmd);
  auto gpuScope = m_backend.beginGpuScope("block_id_compact_append");
  const bool storageRead = vkBlockIdUseStorage();
  const bool bufferRead = vkBlockIdUseBuffer();
  ZVulkanComputePassSpec spec{};
  if (bufferRead) {
    spec.pipeline = &*m_blockIdCompactPipelineBufferAppend;
    spec.pipelineLayout = &*m_blockIdCompactPipelineLayoutBuffer;
  } else if (storageRead) {
    spec.pipeline = &*m_blockIdCompactPipelineStorage;
    spec.pipelineLayout = &*m_blockIdCompactPipelineLayoutStorage;
  } else {
    spec.pipeline = &*m_blockIdCompactPipelineSampled;
    spec.pipelineLayout = &*m_blockIdCompactPipelineLayoutSampled;
  }
  spec.descriptorSetFirst = 0;
  spec.expectedDescriptorSetCount = 1;
  // Workgroup size assumed 16x16 in shader
  spec.groupX = (imgW + 15) / 16;
  spec.groupY = (imgH + 15) / 16;
  spec.groupZ = 1;
  VLOG(2)
    << fmt::format("record compute: groupX={} groupY={} width={} height={}", spec.groupX, spec.groupY, imgW, imgH);
  // Iterate every Block-ID attachment and dispatch compaction on each.
  // Allocate a fresh override descriptor set per attachment to avoid updating a bound set.

  for (uint32_t att = 0; att < effectiveAttachmentCount; ++att) {
    processEventsAndMaybeCancel(cancellationToken);
    ZVulkanTexture* blockTex = payload.blockIdLease->colorAttachment(att);
    if (!blockTex) {
      continue;
    }
    // Validate dimensions match the first attachment
    if (blockTex->width() != imgW || blockTex->height() != imgH) {
      LOG(WARNING) << "Block-ID attachment size mismatch; skipping att=" << att;
      continue;
    }
    // Barrier + set writes depend on read source
    if (bufferRead) {
      // Ensure pixel buffer capacity and record image->buffer copy
      const size_t needed = static_cast<size_t>(imgW) * imgH * sizeof(uint32_t) * 4ull;
      if (!m_blockIdPixelBuffer || m_blockIdPixelBufferCapacity < needed) {
        m_blockIdPixelBuffer = m_backend.device().createBuffer(needed,
                                                               vk::BufferUsageFlagBits::eTransferDst |
                                                                 vk::BufferUsageFlagBits::eStorageBuffer,
                                                               vk::MemoryPropertyFlagBits::eDeviceLocal);
        m_blockIdPixelBufferCapacity = needed;
      }
      // Transition image to transfer src via helper to keep layout tracking consistent
      blockTex->transitionLayout(cmd,
                                 blockTex->layout(),
                                 vk::ImageLayout::eTransferSrcOptimal,
                                 vk::ImageAspectFlagBits::eColor);
      {
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
        cmd.copyImageToBuffer(blockTex->image(),
                              vk::ImageLayout::eTransferSrcOptimal,
                              m_blockIdPixelBuffer->buffer(),
                              region);
      }
      // Restore original stable layout for downstream readbacks
      blockTex->transitionLayout(cmd,
                                 vk::ImageLayout::eTransferSrcOptimal,
                                 vk::ImageLayout::eShaderReadOnlyOptimal,
                                 vk::ImageAspectFlagBits::eColor);
      blockTex->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
      // Barrier for buffer: transfer write -> compute read
      {
        vk::BufferMemoryBarrier2 bb{};
        bb.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        bb.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        bb.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        bb.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
        bb.buffer = m_blockIdPixelBuffer->buffer();
        bb.offset = 0;
        bb.size = VK_WHOLE_SIZE;
        vk::DependencyInfo dep{};
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &bb;
        cmd.pipelineBarrier2(dep);
      }
    } else if (storageRead) {
      // Transition to GENERAL for storage image reads
      if (blockTex->layout() != vk::ImageLayout::eGeneral) {
        blockTex->transitionLayout(cmd, blockTex->layout(), vk::ImageLayout::eGeneral, vk::ImageAspectFlagBits::eColor);
      }
      vk::ImageMemoryBarrier2 barrier{};
      barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
      barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
      barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
      barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
      barrier.oldLayout = vk::ImageLayout::eGeneral;
      barrier.newLayout = vk::ImageLayout::eGeneral;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = blockTex->image();
      barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
      vk::DependencyInfo dep{};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &barrier;
      cmd.pipelineBarrier2(dep);
    } else {
      // Keep sampled layout; enforce a memory barrier
      if (blockTex->layout() != vk::ImageLayout::eShaderReadOnlyOptimal) {
        blockTex->transitionLayout(cmd,
                                   blockTex->layout(),
                                   vk::ImageLayout::eShaderReadOnlyOptimal,
                                   vk::ImageAspectFlagBits::eColor);
      }
      vk::ImageMemoryBarrier2 barrier{};
      barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
      barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
      barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
      barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
      barrier.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
      barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = blockTex->image();
      barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
      vk::DependencyInfo dep{};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &barrier;
      cmd.pipelineBarrier2(dep);
    }

    // Allocate and write a transient override set for this attachment
    auto* ds = bufferRead    ? m_backend.allocateOverrideDescriptorSet(**m_blockIdCompactSetLayoutBuffer)
               : storageRead ? m_backend.allocateOverrideDescriptorSet(**m_blockIdCompactSetLayoutStorage)
                             : m_backend.allocateOverrideDescriptorSet(**m_blockIdCompactSetLayoutSampled);
    CHECK(ds) << "Failed to allocate override DS for block-id compaction";
    if (VLOG_IS_ON(2)) {
      VLOG(2) << fmt::format("Compaction DS: set=0x{:x} att={} img=0x{:x} fmt={} layout={} descLayout={}",
                             reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(ds->descriptorSet())),
                             att,
                             reinterpret_cast<uintptr_t>(static_cast<VkImage>(blockTex->image())),
                             enumOrUnderlying(blockTex->format(), 16),
                             enumOrUnderlying(blockTex->layout(), 16),
                             enumOrUnderlying(blockTex->descriptorLayout(), 16));
    }
    if (bufferRead) {
      // binding 0 = SSBO texels; binding 1 = output buffer
      ds->updateStorageBuffer(0, *m_blockIdPixelBuffer);
    } else if (storageRead) {
      blockTex->setDescriptorLayout(vk::ImageLayout::eGeneral);
      ds->updateStorageImage(0, *blockTex, vk::ImageLayout::eGeneral, vk::ImageAspectFlagBits::eColor);
    } else {
      blockTex->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
      ds->updateTexture(0,
                        *blockTex,
                        m_backend.nearestClampSampler(),
                        vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::ImageAspectFlags{});
    }
    if (VLOG_IS_ON(2)) {
      CHECK(compactOutput != nullptr);
      VLOG(2) << fmt::format("Compaction DS storage: set=0x{:x} buf=0x{:x} size={}B",
                             reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(ds->descriptorSet())),
                             reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(compactOutput->buffer())),
                             compactOutput->size());
    }
    ds->updateStorageBuffer(1, *compactOutput);

    const std::array<vk::DescriptorSet, 1> descriptorSets{ds->descriptorSet()};
    spec.descriptorSets = descriptorSets;
    // Push constants: width, height, stride, capacity, att index
    struct PC
    {
      uint32_t width;
      uint32_t height;
      uint32_t stride;
      uint32_t capacity;
      uint32_t att;
    } pc{imgW, imgH, imgW, static_cast<uint32_t>(imgW * imgH * 4u), att};
    spec.pushConstantsData = &pc;
    spec.pushConstantsSize = sizeof(pc);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eCompute;
    recorder.recordComputePass(spec);
  }
  if (gpuScope) {
    m_backend.endGpuScope(*gpuScope);
  }

  // Resolve channel index from raw booking for post-frame work
  {
    const int32_t rawIdx = payload.channelIndexRaw;
    CHECK_GE(rawIdx, 0);
    CHECK_LT(static_cast<size_t>(rawIdx), payload.visibleChannels.size());
  }
  {
    const int32_t rawRound = payload.roundIndexRaw;
    CHECK_GE(rawRound, 0);
  }
  const size_t resolvedChannelIndex = payload.visibleChannels[static_cast<size_t>(payload.channelIndexRaw)];

  // After submission completion: parse compacted buffer and update caches; determine if this round is complete.
  //
  // Note: this work can throw cancellation exceptions (and can trigger additional uploads).
  // To keep teardown + residency pin lifetimes robust, gate this on "current frame completion"
  // (the backend's frame completion safe point: applyPendingArenaReset) rather than the raw submission fence.
  // This mirrors other readback consumers and ensures fence-gated completion callbacks (e.g. residency unpins)
  // have already run before we process the readback.
  CHECK(compactOutput != nullptr);
  CHECK((compactOutput->usage() & vk::BufferUsageFlagBits::eTransferSrc) != vk::BufferUsageFlags{})
    << "Block-ID compaction output must be readable via end-of-frame buffer readback (eTransferSrc usage missing)";

  // Block-ID compaction is a CPU control-flow boundary: the cache upload it
  // triggers is a hard dependency for the next progressive stage. Force the
  // backend to synchronously wait until the frame completion safe point so the
  // hook below runs before we return to client code.
  m_backend.requireCompletionSafePointWaitForActiveSubmission("block_id_compact_readback");

  auto readbackTicket = m_backend.requestEndOfFrameBufferReadbackTicket(*compactOutput,
                                                                        /*srcOffset=*/0,
                                                                        compactOutputBytes,
                                                                        "block_id_compact_readback");

  const std::string_view debugLabel = "VK raycaster compaction output parse";
  m_backend.registerAfterCurrentFrameCompletionHook(
    currentRenderThreadExecutorKeepAlive(debugLabel),
    [this,
     rendererPtr = &renderer,
     ticket = std::move(readbackTicket),
     cancellationToken,
     imgW,
     imgH,
     streamKey = payload.streamKey,
     eye = batch.eye,
     channelCount = static_cast<uint32_t>(payload.visibleChannels.size()),
     progressiveGeneration = payload.progressiveGeneration,
     attCount = effectiveAttachmentCount,
     channelIndex = resolvedChannelIndex,
     channelIndexRaw = static_cast<uint32_t>(payload.channelIndexRaw),
     roundIndex = static_cast<uint32_t>(payload.roundIndexRaw),
     imagePtr = payload.image](Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
      if (cancellationToken.isCancellationRequested()) {
        // Drop pending CPU work on cancellation, but still release the staging
        // slot to avoid starving future readbacks.
        co_await ticket.awaitAndDiscard();
        co_return;
      }

      CHECK_GT(imgW, 0u);
      CHECK_GT(imgH, 0u);
      CHECK(imagePtr != nullptr);
      const uint32_t capacityIDs = imgW * imgH * 4u;
      const uint32_t idsOffsetWords = 1u + 8u; // count + counts[8]
      CHECK_LE(attCount, 8u) << "Unified header supports up to 8 attachments";
      const uint32_t mapWords = idsOffsetWords + capacityIDs;
      const size_t mapBytes = static_cast<size_t>(mapWords) * sizeof(uint32_t);

      std::vector<uint32_t> words;
      words.resize(static_cast<size_t>(mapWords));
      co_await ticket.awaitCopyTo(words.data(), mapBytes);
      if (cancellationToken.isCancellationRequested()) {
        co_return;
      }

      std::vector<uint32_t> missingBlocks;
      // Unified format: [count][counts[8]][ids...]
      const uint32_t count = words[0];
      const uint32_t clamped = std::min(count, capacityIDs);
      std::array<uint32_t, 8> counts{};
      for (uint32_t att = 0; att < attCount; ++att) {
        counts[att] = words[1 + att];
      }

      missingBlocks.reserve(clamped);
      for (uint32_t i = 0; i < clamped; ++i) {
        uint32_t v = words[idsOffsetWords + i];
        if (v != kEmptyBlockID && v != 0u) {
          missingBlocks.push_back(v);
        }
      }
      // CPU-side dedupe (ids may repeat across workgroups)
      std::unordered_set<uint32_t> uniq;
      uniq.reserve(missingBlocks.size());
      std::vector<uint32_t> deduped;
      deduped.reserve(missingBlocks.size());
      for (uint32_t v : missingBlocks) {
        if (uniq.insert(v).second) {
          deduped.push_back(v);
        }
      }
      missingBlocks.swap(deduped);

      VLOG(1) << fmt::format("compaction output parsed: keys={} (non-empty)", missingBlocks.size());

      // Upload missing blocks (if any).
      //
      // This runs at the backend's "frame completion safe point"
      // (Z3DRendererVulkanBackend::applyPendingArenaReset()). That safe point is
      // responsible for draining all deferred work and waking awaiters even if
      // one callback fails; it will rethrow after cleanup so the render loop can
      // handle cancellation (or crash on real invariant violations).
      if (!missingBlocks.empty()) {
        ZBenchTimer timer("vulkan_raycaster_blockid_compaction");
        imagePtr->updateAndUploadPageDirectoryCaches(missingBlocks, channelIndex, cancellationToken, timer, roundIndex);
        VLOG(1) << fmt::format("cache uploads dispatched: blocks={}", missingBlocks.size());
      }

      // Track attachments that rendered zero block IDs so we can mirror GL's
      // "last round" and "skip progressive" heuristics.
      bool anyZeroAttachment = false;
      bool allZeroAttachments = (attCount != 0);
      uint32_t zeroAtt = std::numeric_limits<uint32_t>::max();

      for (uint32_t att = 0; att < attCount; ++att) {
        const bool zero = counts[att] == 0u;
        if (zero) {
          if (!anyZeroAttachment) {
            zeroAtt = att;
          }
          anyZeroAttachment = true;
          break;
        } else {
          allZeroAttachments = false;
        }
      }
      if (VLOG_IS_ON(2)) {
        std::string s;
        s.reserve(attCount * 6);
        for (uint32_t att = 0; att < attCount; ++att) {
          if (att) {
            s += ",";
          }
          s += fmt::format("{}", counts[att]);
        }
        VLOG(2) << fmt::format("compaction per-attachment counts: [{}] (anyZero={} allZero={} firstZeroAtt={})",
                               s,
                               anyZeroAttachment ? 1 : 0,
                               allZeroAttachments ? 1 : 0,
                               (zeroAtt == std::numeric_limits<uint32_t>::max() ? -1 : static_cast<int>(zeroAtt)));
      }

      // Last-round decision: require an all-zero attachment AND union fits cache
      const size_t cacheCapacity = imagePtr->numCachedImages(channelIndex);
      const bool fitsCache = missingBlocks.size() <= cacheCapacity;
      if (VLOG_IS_ON(2)) {
        VLOG(2) << fmt::format(
          "compaction last-round check: anyZero={} allZero={} fitsCache={} unionSize={} capacity={}",
          anyZeroAttachment ? 1 : 0,
          allZeroAttachments ? 1 : 0,
          fitsCache ? 1 : 0,
          missingBlocks.size(),
          cacheCapacity);
      }
      if (allZeroAttachments) {
        CHECK(missingBlocks.size() == 0);
        m_deferredProgressive.reset();
        if (streamKey != 0u && rendererPtr != nullptr) {
          finalizeImgRaycasterRoundByKey(*rendererPtr,
                                         streamKey,
                                         eye,
                                         /*lastRound=*/true,
                                         /*channelCount=*/channelCount);
        }
        VLOG(1) << fmt::format(
          "compaction: skip progressive round (all attachments zero, channelIndex={} (raw={}) unionSize={} capacity={})",
          channelIndex,
          channelIndexRaw,
          missingBlocks.size(),
          cacheCapacity);
        co_return;
      }

      if (anyZeroAttachment && fitsCache) {
        m_deferredProgressive = DeferredProgressive{.streamKey = streamKey,
                                                    .eye = eye,
                                                    .channelCount = channelCount,
                                                    .progressiveGeneration = progressiveGeneration};
        VLOG(1) << fmt::format(
          "compaction: schedule progressive-only round (channelIndex={} (raw={}) firstZeroAtt={} unionSize={} capacity={})",
          channelIndex,
          channelIndexRaw,
          (zeroAtt == std::numeric_limits<uint32_t>::max() ? -1 : static_cast<int>(zeroAtt)),
          missingBlocks.size(),
          cacheCapacity);
      } else if (m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
                 m_deferredProgressive->eye == eye &&
                 m_deferredProgressive->progressiveGeneration == progressiveGeneration) {
        m_deferredProgressive.reset();
      }

      co_return;
    },
    debugLabel);
  // recordBlockIdCompaction()
}

void ZVulkanImgRaycasterPipelineContext::ensureEntryPipelines(vk::Format colorFormat)
{
  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  auto buildPipeline = [&](PipelineInstance& instance, vk::CullModeFlagBits cullMode) {
    const bool needsRebuild =
      !instance.pipeline || instance.colorFormats.size() != 1 || instance.colorFormats.front() != colorFormat;
    if (!needsRebuild) {
      return;
    }

    if (!instance.shader) {
      instance.shader =
        std::make_unique<ZVulkanShader>(device,
                                        shaderBase + "transform_with_3dtexture_and_eye_coordinate.vert.spv",
                                        shaderBase + "render_3dtexture_coordinate_and_eye_coordinate.frag.spv",
                                        std::nullopt);
    }

    vk::VertexInputBindingDescription binding{.binding = 0,
                                              .stride = sizeof(EntryVertex),
                                              .inputRate = vk::VertexInputRate::eVertex};
    std::array<vk::VertexInputAttributeDescription, 2> attrs{
      vk::VertexInputAttributeDescription{.location = 0,
                                          .binding = 0,
                                          .format = vk::Format::eR32G32B32Sfloat,
                                          .offset = offsetof(EntryVertex, position)},
      vk::VertexInputAttributeDescription{.location = 1,
                                          .binding = 0,
                                          .format = vk::Format::eR32G32B32Sfloat,
                                          .offset = offsetof(EntryVertex, texCoord)}
    };
    vk::PipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions = attrs.data();

    instance.pipeline.reset();
    instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
    // No descriptor sets; entry shader uses push constants for transforms.
    instance.pipeline->setDescriptorSetLayouts({});
    instance.pipeline->setAttachmentFormats({colorFormat}, std::nullopt);
    vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eVertex,
                                .offset = 0,
                                .size = sizeof(glm::mat4) * 2};
    instance.pipeline->setPushConstantRanges({range});
    instance.pipeline->setCullMode(cullMode);
    instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
    instance.pipeline->setDepthTestEnable(false);
    instance.pipeline->setDepthWriteEnable(false);
    instance.pipeline->setColorBlendAttachment(vk::PipelineColorBlendAttachmentState{
      .blendEnable = VK_FALSE,
      .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA});
    instance.pipeline->create();
    instance.colorFormats = {colorFormat};
    instance.depthFormat.reset();
  };

  buildPipeline(m_entryFrontPipeline, vk::CullModeFlagBits::eFront);
  buildPipeline(m_entryBackPipeline, vk::CullModeFlagBits::eBack);
}

ZVulkanImgRaycasterPipelineContext::PipelineInstance&
ZVulkanImgRaycasterPipelineContext::ensureBlockIdPipeline(const BlockIdPipelineKey& key, vk::Format colorFormat)
{
  auto it = m_blockIdPipelines.find(key);
  if (it != m_blockIdPipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "image3d_raycaster_blockID.frag.spv",
                                                    std::nullopt);

  vk::VertexInputBindingDescription binding{.binding = 0,
                                            .stride = sizeof(glm::vec3),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32B32Sfloat,
                                           .offset = 0};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts(
    {**m_progressiveStaticSetLayout, **m_progressiveDynamicSetLayout, **m_pageSetLayout});
  std::vector<vk::Format> colorFormats(std::max(1u, key.attachmentCount), colorFormat);
  instance.pipeline->setAttachmentFormats(colorFormats, std::nullopt);
  // Disable blending for block-ID outputs (integer formats are not blendable).
  {
    vk::PipelineColorBlendAttachmentState att{};
    att.blendEnable = VK_FALSE;
    att.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    std::vector<vk::PipelineColorBlendAttachmentState> atts(colorFormats.size(), att);
    instance.pipeline->setColorBlendAttachments(std::move(atts));
  }
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);

  std::array<vk::SpecializationMapEntry, 1> entries{
    vk::SpecializationMapEntry{.constantID = 70, .offset = 0, .size = sizeof(uint32_t)}
  };
  uint32_t levelCount = std::max(1u, key.levelCount);
  std::vector<uint8_t> data(sizeof(uint32_t));
  std::memcpy(data.data(), &levelCount, sizeof(uint32_t));
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                              std::vector(entries.begin(), entries.end()),
                                              data);
  if (VLOG_IS_ON(1)) {
    VLOG(1) << fmt::format("ensureBlockIdPipeline: LEVEL_COUNT specialization = {}", levelCount);
  }

  // Push constants for ray params used by block-ID shader (5 floats)
  // Must be configured before create() so pipeline layout contains the range.
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = sizeof(float) * 5};
  instance.pipeline->setPushConstantRanges({pcRange});

  instance.pipeline->create();

  // (no-op debug in block-id pipeline)
  instance.colorFormats = std::move(colorFormats);
  instance.depthFormat.reset();

  auto [inserted, _] = m_blockIdPipelines.emplace(key, std::move(instance));
  return inserted->second;
}

ZVulkanImgRaycasterPipelineContext::PipelineInstance&
ZVulkanImgRaycasterPipelineContext::ensureProgressivePipeline(const ProgressivePipelineKey& key,
                                                              const vulkan::AttachmentFormats& formats)
{
  auto it = m_progressivePipelines.find(key);
  if (it != m_progressivePipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "image3d_raycaster.frag.spv",
                                                    std::nullopt);

  vk::VertexInputBindingDescription binding{.binding = 0,
                                            .stride = sizeof(glm::vec3),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32B32Sfloat,
                                           .offset = 0};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts(
    {**m_progressiveStaticSetLayout, **m_progressiveDynamicSetLayout, **m_pageSetLayout});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.colorFormats = formats.colorFormats;
  instance.depthFormat = formats.depthFormat;
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);

  vk::PipelineColorBlendAttachmentState colorBlend{};
  colorBlend.blendEnable = true;
  colorBlend.srcColorBlendFactor = vk::BlendFactor::eOne;
  colorBlend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  colorBlend.colorBlendOp = vk::BlendOp::eAdd;
  colorBlend.srcAlphaBlendFactor = vk::BlendFactor::eOne;
  colorBlend.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  colorBlend.alphaBlendOp = vk::BlendOp::eAdd;
  colorBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  std::vector<vk::PipelineColorBlendAttachmentState> blends(formats.colorFormats.size(), colorBlend);
  instance.pipeline->setColorBlendAttachments(std::move(blends));

  // Fragment push constants for ray params (5 floats)
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = sizeof(float) * 5};
  instance.pipeline->setPushConstantRanges({pcRange});

  std::array<vk::SpecializationMapEntry, 4> entries{
    vk::SpecializationMapEntry{.constantID = 80, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 81, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 51, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 70, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };
  std::array<uint32_t, 4> values{rayModeConstant(key.mode),
                                 key.localMip ? 1u : 0u,
                                 key.resultOpaque ? 1u : 0u,
                                 key.levelCount};
  std::vector<uint8_t> data(sizeof(values));
  std::memcpy(data.data(), values.data(), sizeof(values));
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                              std::vector(entries.begin(), entries.end()),
                                              data);

  instance.pipeline->create();

  if (FLAGS_atlas_vk_debug_raycaster_dump) {
    VLOG(2) << fmt::format("Raycaster specializations: mode={} localMip={} opaque={} levels={}",
                           static_cast<int>(key.mode),
                           key.localMip,
                           key.resultOpaque,
                           key.levelCount);
    VLOG(2) << fmt::format("Raycaster attachments: colors={} depthPresent={} c0Fmt={} dFmt={}",
                           formats.colorFormats.size(),
                           formats.depthFormat.has_value(),
                           formats.colorFormats.empty() ? -1 : static_cast<int>(formats.colorFormats.front()),
                           formats.depthFormat ? static_cast<int>(*formats.depthFormat) : -1);
  }

  auto [inserted, _] = m_progressivePipelines.emplace(key, std::move(instance));
  return inserted->second;
}

ZVulkanImgRaycasterPipelineContext::PipelineInstance&
ZVulkanImgRaycasterPipelineContext::ensureCopyPipeline(const CopyPipelineKey& key,
                                                       const vulkan::AttachmentFormats& formats)
{
  auto it = m_copyPipelines.find(key);
  if (it != m_copyPipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "copy_raycaster_image.frag.spv",
                                                    std::nullopt);

  vk::VertexInputBindingDescription binding{.binding = 0,
                                            .stride = sizeof(glm::vec3),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32B32Sfloat,
                                           .offset = 0};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_copySetLayout});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.colorFormats = formats.colorFormats;
  instance.depthFormat = formats.depthFormat;
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  // Copy pipeline uses the same depth state as before; no debug toggles here.
  // Overwrite semantics: always pass depth and write updated depth, no color blending
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthWriteEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE; // no blending; full overwrite of destination color
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blend);

  instance.pipeline->create();

  auto [inserted, _] = m_copyPipelines.emplace(key, std::move(instance));
  return inserted->second;
}

ZVulkanImgRaycasterPipelineContext::PipelineInstance&
ZVulkanImgRaycasterPipelineContext::ensureMergePipeline(const MergePipelineKey& key,
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

  vk::VertexInputBindingDescription binding{.binding = 0,
                                            .stride = sizeof(glm::vec3),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32B32Sfloat,
                                           .offset = 0};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_mergeSetLayout});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.colorFormats = formats.colorFormats;
  instance.depthFormat = formats.depthFormat;
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
  std::vector<vk::PipelineColorBlendAttachmentState> blends(formats.colorFormats.size(), blend);
  instance.pipeline->setColorBlendAttachments(std::move(blends));

  // Match constant IDs with image2d_array_compositor.frag
  // 70: NUM_VOLUMES, 71: MAX_PROJ_MERGE, 51: RESULT_OPAQUE
  std::array<vk::SpecializationMapEntry, 3> entries{
    vk::SpecializationMapEntry{.constantID = 70, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 71, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 51, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };
  std::array<uint32_t, 3> values{static_cast<uint32_t>(std::max(1, key.numVolumes)),
                                 key.maxProjectionMerge ? 1u : 0u,
                                 key.resultOpaque ? 1u : 0u};
  std::vector<uint8_t> data(sizeof(values));
  std::memcpy(data.data(), values.data(), sizeof(values));
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                              std::vector(entries.begin(), entries.end()),
                                              data);

  instance.pipeline->create();
  VLOG(2) << fmt::format(
    "Raycaster Merge Pipeline: depthTest=0 depthWrite=0 colorFmt0={} depthFmt={} volumes={} maxProj={} opaque={}",
    formats.colorFormats.empty() ? -1 : static_cast<int>(formats.colorFormats.front()),
    formats.depthFormat ? static_cast<int>(*formats.depthFormat) : -1,
    key.numVolumes,
    key.maxProjectionMerge,
    key.resultOpaque);
  VLOG(1) << "Merge pipeline created: depthTest=1 depthWrite=1 compareOp="
          << static_cast<int>(vk::CompareOp::eLessOrEqual)
          << " depthFmt=" << (formats.depthFormat ? static_cast<int>(*formats.depthFormat) : -1)
          << " color0Fmt=" << (formats.colorFormats.empty() ? -1 : static_cast<int>(formats.colorFormats.front()))
          << " [spec] volumes=" << key.numVolumes << " maxProj=" << (key.maxProjectionMerge ? 1 : 0)
          << " opaque=" << (key.resultOpaque ? 1 : 0);

  auto [inserted, _] = m_mergePipelines.emplace(key, std::move(instance));
  return inserted->second;
}

void ZVulkanImgRaycasterPipelineContext::ensureDepthOnlyRampPipeline(vk::Format depthFormat)
{
  // Rebuild if first time or depth format changed
  if (m_depthRampPipeline.pipeline && m_depthRampFormat && *m_depthRampFormat == depthFormat) {
    return;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  m_depthRampPipeline.shader = std::make_unique<ZVulkanShader>(device,
                                                               shaderBase + "pass.vert.spv",
                                                               shaderBase + "depth_ramp.frag.spv",
                                                               std::nullopt);

  // Screen-space quad as triangle strip with vec3 positions (z=0) at location 0
  vk::VertexInputBindingDescription binding{.binding = 0,
                                            .stride = sizeof(glm::vec3),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32B32Sfloat,
                                           .offset = 0};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  m_depthRampPipeline.pipeline =
    device.createPipeline(*m_depthRampPipeline.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  // No descriptor sets
  m_depthRampPipeline.pipeline->setDescriptorSetLayouts({});
  // Depth-only rendering: no color attachments
  m_depthRampPipeline.pipeline->setAttachmentFormats({}, depthFormat);
  m_depthRampPipeline.colorFormats.clear();
  m_depthRampPipeline.depthFormat = depthFormat;
  m_depthRampPipeline.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  m_depthRampPipeline.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  m_depthRampPipeline.pipeline->setDepthTestEnable(true);
  m_depthRampPipeline.pipeline->setDepthWriteEnable(true);
  m_depthRampPipeline.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
  // Push constant: float invHeight for ramp scaling
  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(float)};
  m_depthRampPipeline.pipeline->setPushConstantRanges({range});
  m_depthRampPipeline.pipeline->create();
  m_depthRampFormat = depthFormat;
}

// depthArray is nullable per pointer contract
void ZVulkanImgRaycasterPipelineContext::bindMergeDescriptor(ZVulkanTexture& colorArray,
                                                             /*nullable*/ ZVulkanTexture* depthArray)
{
  if (!m_mergeDescriptor) {
    m_mergeDescriptor = m_backend.allocateOverrideDescriptorSet(**m_mergeSetLayout);
  }
  CHECK(m_mergeDescriptor != nullptr) << "Raycaster merge: override descriptor allocation failed (fatal)";
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

ZVulkanImgRaycasterPipelineContext::ChannelResources&
ZVulkanImgRaycasterPipelineContext::ensureChannelResources(size_t channelIndex)
{
  if (channelIndex >= m_channelResources.size()) {
    m_channelResources.resize(channelIndex + 1);
  }
  return m_channelResources[channelIndex];
}

ZVulkanTexture& ZVulkanImgRaycasterPipelineContext::ensureVolumeTexture(ChannelResources& resources,
                                                                        const ZImg& image,
                                                                        size_t channelIndex,
                                                                        uint64_t generation)
{
  (void)channelIndex;
  const uint32_t width = static_cast<uint32_t>(image.width());
  const uint32_t height = static_cast<uint32_t>(image.height());
  const uint32_t depth = static_cast<uint32_t>(image.depth());
  const size_t byteSize = image.byteNumber();

  CHECK_EQ(image.info().bytesPerVoxel, 1u) << "Vulkan raycaster currently expects 8-bit single-channel volumes.";
  const uint8_t* data = image.channelData<uint8_t>(0);

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
    resources.volumeTexture = m_backend.device().createTexture(info);
    CHECK(resources.volumeTexture != nullptr) << "Raycaster: failed to create 3D volume texture";
  } else if (resources.volumeGeneration == generation && resources.volumeTexture) {
    // Static content unchanged; no upload needed.
    return *resources.volumeTexture;
  }

  if (byteSize > 0 && data) {
    resources.volumeTexture->uploadData(data, byteSize, vk::ImageLayout::eShaderReadOnlyOptimal);
    resources.volumeGeneration = generation;
  }

  return *resources.volumeTexture;
}

ZVulkanTexture& ZVulkanImgRaycasterPipelineContext::ensureImage2DTexture(ChannelResources& resources,
                                                                         const ZImg& image,
                                                                         uint64_t generation)
{
  const uint32_t width = static_cast<uint32_t>(image.width());
  const uint32_t height = static_cast<uint32_t>(image.height());
  const size_t byteSize = static_cast<size_t>(width) * height * image.info().bytesPerVoxel;

  CHECK_EQ(image.info().bytesPerVoxel, 1u) << "Vulkan raycaster expects 8-bit single-channel 2D inputs.";
  const uint8_t* data = image.channelData<uint8_t>(0);

  const bool needsRecreate = !resources.image2DTexture || resources.image2DTexture->extent().width != width ||
                             resources.image2DTexture->extent().height != height;

  if (needsRecreate) {
    auto info =
      ZVulkanTexture::CreateInfo::make2D(width,
                                         height,
                                         vk::Format::eR8Unorm,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal,
                                         1u,
                                         true,
                                         vk::ImageLayout::eShaderReadOnlyOptimal);
    resources.image2DTexture = m_backend.device().createTexture(info);
    CHECK(resources.image2DTexture != nullptr) << "Raycaster: failed to create 2D image texture";
  } else if (resources.image2DGeneration == generation && resources.image2DTexture) {
    return *resources.image2DTexture;
  }

  if (byteSize > 0 && data) {
    resources.image2DTexture->uploadData(data, byteSize, vk::ImageLayout::eShaderReadOnlyOptimal);
    resources.image2DGeneration = generation;
  }

  return *resources.image2DTexture;
}

ZVulkanTexture& ZVulkanImgRaycasterPipelineContext::ensureTransferTexture(ChannelResources& resources,
                                                                          const Z3DTransferFunction& transferFunction)
{
  auto& device = m_backend.device();
  const uint32_t width = static_cast<uint32_t>(transferFunction.dimensions().x);
  const uint64_t gen = transferFunction.generation();

  bool createdOrResized = false;
  if (!resources.transferTexture || resources.transferWidth != width) {
    // Vulkan path prefers RGBA textures; build an RGBA8 LUT and use
    // eR8G8B8A8Unorm to match channel order.
    vulkan::ensure1DLUTTexture(device,
                               resources.transferTexture,
                               width,
                               vk::Format::eR8G8B8A8Unorm,
                               vk::ImageLayout::eShaderReadOnlyOptimal);
    resources.transferWidth = width;
    createdOrResized = true;
  }

  if (createdOrResized || resources.transferGeneration != gen) {
    std::vector<uint8_t> texels;
    transferFunction.buildLUTRGBA8(texels, width);
    if (!texels.empty()) {
      vulkan::uploadLUT(*resources.transferTexture,
                        texels.data(),
                        texels.size(),
                        vk::ImageLayout::eShaderReadOnlyOptimal);
      resources.transferGeneration = gen;
    }
  }

  return *resources.transferTexture;
}

void ZVulkanImgRaycasterPipelineContext::updateChannelFastDescriptors(ChannelResources& resources,
                                                                      const ImgRaycasterPayload& payload,
                                                                      size_t channelIndex,
                                                                      ZVulkanTexture& entryExitTexture,
                                                                      ZVulkanTexture& volume,
                                                                      ZVulkanTexture& transfer,
                                                                      float zeToZW_a,
                                                                      float zeToZW_b,
                                                                      const glm::vec3& volumeDimensions)
{
  (void)channelIndex;
  (void)payload;
  (void)zeToZW_a;
  (void)zeToZW_b;
  (void)volumeDimensions;
  if (!resources.fastDescriptor) {
    resources.fastDescriptor = m_backend.allocateOverrideDescriptorSet(**m_fastSetLayout);
  }
  CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path: override descriptor allocation failed (fatal)";
  resources.fastDescriptor->updateTexture(0, entryExitTexture, m_backend.defaultSampler());
  resources.fastDescriptor->updateTexture(1, volume, m_backend.defaultSampler());
  resources.fastDescriptor->updateTexture(2, transfer, m_backend.defaultSampler());

  // No UBOs; ray params are passed via push constants at draw time.
}

void ZVulkanImgRaycasterPipelineContext::updateChannelImage2DDescriptors(ChannelResources& resources,
                                                                         ZVulkanTexture& imageTexture,
                                                                         ZVulkanTexture& transferTexture)
{
  if (!m_image2DSetLayout) {
    ensureDescriptorLayouts();
  }
  if (!resources.image2DDescriptor) {
    resources.image2DDescriptor = m_backend.allocateOverrideDescriptorSet(**m_image2DSetLayout);
  }
  CHECK(resources.image2DDescriptor != nullptr) << "Raycaster 2D path: override descriptor allocation failed (fatal)";
  resources.image2DDescriptor->updateTexture(0, imageTexture, m_backend.defaultSampler());
  resources.image2DDescriptor->updateTexture(1, transferTexture, m_backend.defaultSampler());
}

void ZVulkanImgRaycasterPipelineContext::updateChannelSliceDescriptors(ChannelResources& resources,
                                                                       ZVulkanTexture& volumeTexture,
                                                                       ZVulkanTexture& transferTexture)
{
  if (!m_sliceFastSetLayout) {
    ensureDescriptorLayouts();
  }
  if (!resources.sliceDescriptor) {
    resources.sliceDescriptor = m_backend.allocateOverrideDescriptorSet(**m_sliceFastSetLayout);
  }
  CHECK(resources.sliceDescriptor != nullptr) << "Raycaster slice path: override descriptor allocation failed (fatal)";
  resources.sliceDescriptor->updateTexture(0, volumeTexture, m_backend.defaultSampler());
  resources.sliceDescriptor->updateTexture(1, transferTexture, m_backend.defaultSampler());
}

bool ZVulkanImgRaycasterPipelineContext::updatePageDescriptors(ChannelResources& resources,
                                                               const ImgRaycasterPayload& payload,
                                                               ZVulkanTexture& entryExit,
                                                               ZVulkanTexture& lastDepth,
                                                               ZVulkanTexture& lastColor,
                                                               ZVulkanTexture& volume,
                                                               ZVulkanTexture& transfer,
                                                               const Z3DImg& image,
                                                               size_t channelIndex,
                                                               float zeToScreenPixelVoxelSize,
                                                               bool freshOverrideDescriptors)
{
  // Uploader must be available in progressive (paged) path
  ensureUploader();
  // Always allocate/update a transient per-draw descriptor for dynamic textures (set=1).
  // If freshOverrideDescriptors (or global flag) is true, allocate a new override set even if one exists
  // to avoid updating a set that might still be bound earlier in this command buffer.
  if (freshOverrideDescriptors || FLAGS_atlas_vk_force_fresh_override_sets || !resources.pagedDescriptor) {
    resources.pagedDescriptor = m_backend.allocateOverrideDescriptorSet(**m_progressiveDynamicSetLayout);
  }
  CHECK(resources.pagedDescriptor) << "Failed to allocate Vulkan raycaster paging descriptor set.";

  auto* pageDirectory = m_imageBlockUploader->pageDirectoryTexture(*payload.image, channelIndex);
  auto* pageTable = m_imageBlockUploader->pageTableTexture(*payload.image, channelIndex);
  auto* imageCache = m_imageBlockUploader->imageCacheTexture(*payload.image, channelIndex);
  CHECK(pageDirectory && pageTable && imageCache)
    << "Paging textures unavailable for Vulkan raycaster channel " << channelIndex;

  // Track currently referenced textures regardless of whether the static
  // descriptor gets rewritten in this call. This allows later lazy-override
  // allocation without touching the persistent static descriptor.
  resources.boundPageDirectoryTex = pageDirectory;
  resources.boundPageTableTex = pageTable;
  resources.boundImageCacheTex = imageCache;
  resources.boundVolumeTex = &volume;
  resources.boundTransferTex = &transfer;
  // Dynamic per-draw set (m_progressiveDynamicSetLayout) only binds: entry/exit, last_depth(color), last_color.
  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("updatePageDescriptors dynamic set: entryExit img=0x{:x} fmt={} layout={} descLayout={}",
                           reinterpret_cast<uintptr_t>(static_cast<VkImage>(entryExit.image())),
                           enumOrUnderlying(entryExit.format(), 16),
                           enumOrUnderlying(entryExit.layout(), 16),
                           enumOrUnderlying(entryExit.descriptorLayout(), 16));
    VLOG(2) << fmt::format(
      "updatePageDescriptors dynamic set: lastDepth(img/color) img=0x{:x} fmt={} layout={} descLayout={} (override layout {})",
      reinterpret_cast<uintptr_t>(static_cast<VkImage>(lastDepth.image())),
      enumOrUnderlying(lastDepth.format(), 16),
      enumOrUnderlying(lastDepth.layout(), 16),
      enumOrUnderlying(lastDepth.descriptorLayout(), 16),
      enumOrUnderlying(vk::ImageLayout::eShaderReadOnlyOptimal, 16));
    VLOG(2) << fmt::format("updatePageDescriptors dynamic set: lastColor img=0x{:x} fmt={} layout={} descLayout={}",
                           reinterpret_cast<uintptr_t>(static_cast<VkImage>(lastColor.image())),
                           enumOrUnderlying(lastColor.format(), 16),
                           enumOrUnderlying(lastColor.layout(), 16),
                           enumOrUnderlying(lastColor.descriptorLayout(), 16));
  }
  resources.pagedDescriptor->updateTexture(0,
                                           entryExit,
                                           m_backend.defaultSampler(),
                                           vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::ImageAspectFlags{});
  resources.pagedDescriptor->updateTexture(1,
                                           lastDepth,
                                           m_backend.defaultSampler(),
                                           vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::ImageAspectFlags{});
  resources.pagedDescriptor->updateTexture(2,
                                           lastColor,
                                           m_backend.defaultSampler(),
                                           vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::ImageAspectFlags{});
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "updatePageDescriptors: after dynamic writes";
  }

  const uint32_t devCap = deviceLevelCap(m_backend.device());
  const uint32_t levelCount = static_cast<uint32_t>(std::min<size_t>(image.numLevels(), devCap));
  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("updatePageDescriptors: building page data, levels={} ze2px={:.6f}",
                           levelCount,
                           zeToScreenPixelVoxelSize);
    // Log source array sizes for early diagnosis of level mismatches.
    const auto& dbg_pdb = image.pageDirectoryBases();
    const auto& dbg_p2b = image.posToBlockIDsLevels();
    const auto& dbg_dims = image.imageDimensionsLevels();
    const auto& dbg_vox = image.voxelWorldSizesLevels();
    VLOG(2) << fmt::format(
      "pageDirectoryBases.size()={} posToBlockIDs.size()={} imageDimensions.size()={} voxelWorldSizes.size()={}",
      dbg_pdb.size(),
      dbg_p2b.size(),
      dbg_dims.size(),
      dbg_vox.size());
  }
  // Enforce levelCount fits backing arrays early with a clear message.
  {
    const auto& pdb = image.pageDirectoryBases();
    const auto& p2b = image.posToBlockIDsLevels();
    const auto& dims = image.imageDimensionsLevels();
    const auto& vox = image.voxelWorldSizesLevels();
    CHECK_GE(pdb.size(), levelCount) << "Insufficient pageDirectoryBases for requested levels: have=" << pdb.size()
                                     << " need=" << levelCount;
    CHECK_GE(p2b.size(), levelCount) << "Insufficient posToBlockIDsLevels for requested levels: have=" << p2b.size()
                                     << " need=" << levelCount;
    CHECK_GE(dims.size(), levelCount) << "Insufficient imageDimensionsLevels for requested levels: have=" << dims.size()
                                      << " need=" << levelCount;
    CHECK_GE(vox.size(), levelCount) << "Insufficient voxelWorldSizesLevels for requested levels: have=" << vox.size()
                                     << " need=" << levelCount;
  }
  auto pageData = buildPageDataBuffer(image, channelIndex, zeToScreenPixelVoxelSize, levelCount);
  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("updatePageDescriptors: page data size={} bytes", pageData.size());
  }

  if (!resources.pageDataBuffer || resources.pageDataCapacity < pageData.size()) {
    auto buf = m_backend.device().createBuffer(pageData.size(),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
    resources.pageDataBuffer = std::shared_ptr<ZVulkanBuffer>(std::move(buf));
    if (VLOG_IS_ON(2)) {
      VLOG(2) << fmt::format("updatePageDescriptors: allocated page UBO buffer size={}", pageData.size());
    }
  }

  CHECK(resources.pageDataBuffer) << "Failed to allocate Vulkan raycaster paging uniform buffer.";

  resources.pageDataBuffer->copyData(pageData.data(), pageData.size());
  resources.pageDataCapacity = pageData.size();
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "updatePageDescriptors: copied page data to UBO";
  }

  // Prepare/update a persistent page descriptor set that binds the page UBO (binding=2).
  if (!FLAGS_atlas_vk_force_page_ubo_override) {
    if (!resources.persistentPageDescriptor) {
      const size_t resIndex = channelIndex;
      auto pageBuf = resources.pageDataBuffer;
      const std::string_view debugLabel = "VK raycaster allocate persistent page descriptor";
      m_backend.registerAfterCurrentFrameCompletionHook(
        currentRenderThreadExecutorKeepAlive(debugLabel),
        [this, resIndex, pageBuf](Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
          if (!pageBuf) {
            co_return;
          }
          ChannelResources& res = ensureChannelResources(resIndex);
          if (res.persistentPageDescriptor) {
            co_return;
          }
          ensureDescriptorPool();
          try {
            vk::DescriptorSet raw = m_descriptorPool->allocateDescriptorSet(**m_pageSetLayout);
            res.persistentPageDescriptor =
              std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), raw, /*isOverrideTransient=*/false);
            res.persistentPageDescriptor->writeUniformBufferOnce(2, *pageBuf);
            res.boundPageDataBuffer = pageBuf;
          }
          catch (const std::exception& e) {
            LOG(ERROR) << "Failed to allocate persistent page descriptor: " << e.what();
          }
          co_return;
        },
        debugLabel);
    } else {
      // If the page buffer object changed since last bind, rewrite the persistent descriptor
      // after frame completion. Also bind an override descriptor for this frame so the current
      // draw sees the correct UBO immediately (avoid using stale persistent set for one frame).
      if (resources.pageDataBuffer != resources.boundPageDataBuffer) {
        auto pageBuf = resources.pageDataBuffer;
        const size_t resIndex2 = channelIndex;
        // Immediate override for this frame
        if (!resources.pageDescriptor) {
          resources.pageDescriptor = m_backend.allocateOverrideDescriptorSet(**m_pageSetLayout);
        }
        CHECK(resources.pageDescriptor)
          << "Failed to allocate Vulkan raycaster page descriptor set (override current).";
        resources.pageDescriptor->updateUniformBuffer(2, *resources.pageDataBuffer);

        // Deferred persistent update after the active submission completes.
        // Capture the buffer to keep it alive while this frame is in flight.
        const std::string_view debugLabel = "VK raycaster update persistent page descriptor";
        m_backend.registerAfterCurrentFrameCompletionHook(
          currentRenderThreadExecutorKeepAlive(debugLabel),
          [this, resIndex2, pageBuf](Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
            if (!pageBuf) {
              co_return;
            }
            ChannelResources& res = ensureChannelResources(resIndex2);
            if (res.persistentPageDescriptor) {
              res.persistentPageDescriptor->updateUniformBuffer(2, *pageBuf);
              res.boundPageDataBuffer = pageBuf;
            }
            co_return;
          },
          debugLabel);
      }
    }
  } else {
    // If the page buffer object changed since last bind, rewrite the persistent descriptor
    // after frame completion. Also bind an override descriptor for this frame so the current
    // draw sees the correct UBO immediately (avoid using stale persistent set for one frame).
    if (resources.pageDataBuffer != resources.boundPageDataBuffer) {
      auto pageBuf = resources.pageDataBuffer;
      const size_t resIndex2 = channelIndex;
      // Immediate override for this frame
      if (!resources.pageDescriptor) {
        resources.pageDescriptor = m_backend.allocateOverrideDescriptorSet(**m_pageSetLayout);
      }
      CHECK(resources.pageDescriptor) << "Failed to allocate Vulkan raycaster page descriptor set (override current).";
      resources.pageDescriptor->updateUniformBuffer(2, *resources.pageDataBuffer);
      resources.boundPageDataBuffer = resources.pageDataBuffer;

      // In force-override mode we never update the persistent descriptor. Still
      // keep the buffer alive until this submission completes.
      const std::string_view debugLabel = "VK raycaster page descriptor override noop";
      m_backend.registerAfterCurrentFrameCompletionHook(
        currentRenderThreadExecutorKeepAlive(debugLabel),
        [resIndex2, pageBuf](Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
          (void)resIndex2;
          (void)pageBuf;
          co_return;
        },
        debugLabel);
    }
  }

  // Prepare/update static texture descriptor set (set=0) persistently; bind an override for first frame.
  if (!resources.persistentStaticDescriptor) {
    // First-frame override
    if (!resources.staticDescriptor) {
      resources.staticDescriptor = m_backend.allocateOverrideDescriptorSet(**m_progressiveStaticSetLayout);
    }
    CHECK(resources.staticDescriptor) << "Failed to allocate Vulkan raycaster static descriptor set (override).";
    // Integer 3D samplers must use nearest sampling on Vulkan/Metal
    resources.staticDescriptor->updateTexture(0, *pageDirectory, m_backend.nearestClampSampler());
    resources.staticDescriptor->updateTexture(1, *pageTable, m_backend.nearestClampSampler());
    resources.staticDescriptor->updateTexture(2, *imageCache, m_backend.defaultSampler());
    resources.staticDescriptor->updateTexture(3, volume, m_backend.defaultSampler());
    resources.staticDescriptor->updateTexture(4, transfer, m_backend.defaultSampler());
    // Track currently bound textures so we can enforce sampled layouts before use.
    resources.boundPageDirectoryTex = pageDirectory;
    resources.boundPageTableTex = pageTable;
    resources.boundImageCacheTex = imageCache;
    resources.boundVolumeTex = &volume;
    resources.boundTransferTex = &transfer;

    // Schedule persistent creation after frame completion.
    const size_t resIndexStatic = channelIndex;
    ZVulkanTexture* pd = pageDirectory;
    ZVulkanTexture* pt = pageTable;
    ZVulkanTexture* ic = imageCache;
    ZVulkanTexture* vol = &volume;
    ZVulkanTexture* tf = &transfer;
    const std::string_view debugLabel = "VK raycaster allocate persistent static descriptor";
    m_backend.registerAfterCurrentFrameCompletionHook(
      currentRenderThreadExecutorKeepAlive(debugLabel),
      [this, resIndexStatic, pd, pt, ic, vol, tf](Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
        ChannelResources& res = ensureChannelResources(resIndexStatic);
        if (res.persistentStaticDescriptor) {
          co_return;
        }
        ensureDescriptorPool();
        try {
          vk::DescriptorSet raw = m_descriptorPool->allocateDescriptorSet(**m_progressiveStaticSetLayout);
          res.persistentStaticDescriptor =
            std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), raw, /*isOverrideTransient=*/false);
          // Integer 3D samplers: use nearest clamp sampler
          res.persistentStaticDescriptor->writeTextureOnce(0, *pd, m_backend.nearestClampSampler());
          res.persistentStaticDescriptor->writeTextureOnce(1, *pt, m_backend.nearestClampSampler());
          res.persistentStaticDescriptor->writeTextureOnce(2, *ic, m_backend.defaultSampler());
          res.persistentStaticDescriptor->writeTextureOnce(3, *vol, m_backend.defaultSampler());
          res.persistentStaticDescriptor->writeTextureOnce(4, *tf, m_backend.defaultSampler());
          res.boundPageDirectoryTex = pd;
          res.boundPageTableTex = pt;
          res.boundImageCacheTex = ic;
          res.boundVolumeTex = vol;
          res.boundTransferTex = tf;
        }
        catch (const std::exception& e) {
          LOG(ERROR) << "Failed to allocate persistent static descriptor: " << e.what();
        }
        co_return;
      },
      debugLabel);
  } else {
    // If any static texture changed (recreated), rewrite persistent descriptor after frame completion.
    ZVulkanTexture* pd = pageDirectory;
    ZVulkanTexture* pt = pageTable;
    ZVulkanTexture* ic = imageCache;
    ZVulkanTexture* vol = &volume;
    ZVulkanTexture* tf = &transfer;
    const bool changed = (pd != resources.boundPageDirectoryTex) || (pt != resources.boundPageTableTex) ||
                         (ic != resources.boundImageCacheTex) || (vol != resources.boundVolumeTex) ||
                         (tf != resources.boundTransferTex);
    if (changed) {
      const size_t resIndexStatic = channelIndex;
      const std::string_view debugLabel = "VK raycaster update persistent static descriptor";
      m_backend.registerAfterCurrentFrameCompletionHook(
        currentRenderThreadExecutorKeepAlive(debugLabel),
        [this, resIndexStatic, pd, pt, ic, vol, tf](Z3DRendererVulkanBackend&) mutable -> folly::coro::Task<void> {
          ChannelResources& res = ensureChannelResources(resIndexStatic);
          if (!res.persistentStaticDescriptor) {
            co_return;
          }
          res.persistentStaticDescriptor->updateTexture(0, *pd, m_backend.nearestClampSampler());
          res.persistentStaticDescriptor->updateTexture(1, *pt, m_backend.nearestClampSampler());
          res.persistentStaticDescriptor->updateTexture(2, *ic, m_backend.defaultSampler());
          res.persistentStaticDescriptor->updateTexture(3, *vol, m_backend.defaultSampler());
          res.persistentStaticDescriptor->updateTexture(4, *tf, m_backend.defaultSampler());
          res.boundPageDirectoryTex = pd;
          res.boundPageTableTex = pt;
          res.boundImageCacheTex = ic;
          res.boundVolumeTex = vol;
          res.boundTransferTex = tf;
          co_return;
        },
        debugLabel);
    }

    // Prefer a per-draw override static descriptor so we never rewrite a
    // persistent set that might still be referenced later in the frame.
    if (freshOverrideDescriptors) {
      if (!resources.staticDescriptor) {
        resources.staticDescriptor = m_backend.allocateOverrideDescriptorSet(**m_progressiveStaticSetLayout);
      }
      if (resources.staticDescriptor) {
        resources.staticDescriptor->updateTexture(0, *pd, m_backend.nearestClampSampler());
        resources.staticDescriptor->updateTexture(1, *pt, m_backend.nearestClampSampler());
        resources.staticDescriptor->updateTexture(2, *ic, m_backend.defaultSampler());
        resources.staticDescriptor->updateTexture(3, volume, m_backend.defaultSampler());
        resources.staticDescriptor->updateTexture(4, transfer, m_backend.defaultSampler());
        // Track bound textures so a later lazy allocation path can construct
        // an override static set without touching persistent descriptors.
        resources.boundPageDirectoryTex = pd;
        resources.boundPageTableTex = pt;
        resources.boundImageCacheTex = ic;
        resources.boundVolumeTex = &volume;
        resources.boundTransferTex = &transfer;
      }
    }
  }

  // Also maintain a per-draw override pageDescriptor for the current frame so
  // the very first frame has a valid set before the persistent one exists.
  if (!resources.persistentPageDescriptor) {
    if (freshOverrideDescriptors || !resources.pageDescriptor) {
      resources.pageDescriptor = m_backend.allocateOverrideDescriptorSet(**m_pageSetLayout);
    }
    CHECK(resources.pageDescriptor) << "Failed to allocate Vulkan raycaster page descriptor set.";
    if (VLOG_IS_ON(2)) {
      VLOG(2) << fmt::format(
        "updatePageDescriptors: writing page UBO into override DS=0x{:x}",
        reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(resources.pageDescriptor->descriptorSet())));
    }
    resources.pageDescriptor->updateUniformBuffer(2, *resources.pageDataBuffer);
    if (VLOG_IS_ON(2)) {
      VLOG(2) << "updatePageDescriptors: wrote page UBO into override descriptor";
    }
  }

  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format(
      "updatePageDescriptors done: levelCount={} staticDS={} dynamicDS={} pageDS={}",
      levelCount,
      reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(
        resources.persistentStaticDescriptor
          ? resources.persistentStaticDescriptor->descriptorSet()
          : (resources.staticDescriptor ? resources.staticDescriptor->descriptorSet() : vk::DescriptorSet{}))),
      reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(
        resources.pagedDescriptor ? resources.pagedDescriptor->descriptorSet() : vk::DescriptorSet{})),
      reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(
        resources.persistentPageDescriptor
          ? resources.persistentPageDescriptor->descriptorSet()
          : (resources.pageDescriptor ? resources.pageDescriptor->descriptorSet() : vk::DescriptorSet{}))));
  }
  resources.levelCount = levelCount;
  return true;
}

std::vector<vk::DescriptorSet>
ZVulkanImgRaycasterPipelineContext::collectProgressiveDescriptorSets(ChannelResources& resources,
                                                                     bool preferOverrideStatic)
{
  CHECK(resources.pagedDescriptor) << "Vulkan raycaster progressive descriptors not initialised.";

  // Allocate a fresh override static descriptor when requested so we avoid
  // mutating a set that may have been bound earlier in the same command buffer.
  vk::DescriptorSet staticSet{};
  if (preferOverrideStatic) {
    CHECK(resources.boundPageDirectoryTex && resources.boundPageTableTex && resources.boundImageCacheTex &&
          resources.boundVolumeTex && resources.boundTransferTex)
      << "Bound paging/volume/transfer textures unavailable for override static DS";
    auto* fresh = m_backend.allocateOverrideDescriptorSet(**m_progressiveStaticSetLayout);
    CHECK(fresh) << "Failed to allocate override static descriptor set (split mode)";
    fresh->updateTexture(0, *resources.boundPageDirectoryTex, m_backend.nearestClampSampler());
    fresh->updateTexture(1, *resources.boundPageTableTex, m_backend.nearestClampSampler());
    fresh->updateTexture(2, *resources.boundImageCacheTex, m_backend.defaultSampler());
    fresh->updateTexture(3, *resources.boundVolumeTex, m_backend.defaultSampler());
    fresh->updateTexture(4, *resources.boundTransferTex, m_backend.defaultSampler());
    staticSet = fresh->descriptorSet();
  } else {
    if (resources.persistentStaticDescriptor) {
      staticSet = resources.persistentStaticDescriptor->descriptorSet();
    } else if (resources.staticDescriptor) {
      staticSet = resources.staticDescriptor->descriptorSet();
    }
  }
  CHECK(staticSet) << "Vulkan raycaster progressive static descriptor missing.";

  // Choose page descriptor set: in force-override mode, always use the override set.
  vk::DescriptorSet pageSet{};
  const bool persistentValid = resources.persistentPageDescriptor != nullptr;
  const bool overrideValid = resources.pageDescriptor != nullptr;
  const bool persistentStale = persistentValid && (resources.boundPageDataBuffer != resources.pageDataBuffer);
  if (FLAGS_atlas_vk_force_page_ubo_override) {
    CHECK(overrideValid) << "Force-override enabled but no override page descriptor set is bound.";
    pageSet = resources.pageDescriptor->descriptorSet();
  } else if (overrideValid && persistentStale) {
    pageSet = resources.pageDescriptor->descriptorSet();
  } else if (persistentValid) {
    pageSet = resources.persistentPageDescriptor->descriptorSet();
  } else if (overrideValid) {
    pageSet = resources.pageDescriptor->descriptorSet();
  }
  CHECK(pageSet) << "Vulkan raycaster progressive page descriptor missing.";

  if (VLOG_IS_ON(2)) {
    const auto dynSet = resources.pagedDescriptor->descriptorSet();
    VLOG(2) << fmt::format("collectProgressiveDescriptorSets: static=0x{:x} dynamic=0x{:x} page=0x{:x}",
                           reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(staticSet)),
                           reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(dynSet)),
                           reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(pageSet)));
  }

  return {staticSet, resources.pagedDescriptor->descriptorSet(), pageSet};
}

ZVulkanImgRaycasterPipelineContext::PipelineInstance&
ZVulkanImgRaycasterPipelineContext::ensureFastPipeline(const FastPipelineKey& key)
{
  auto it = m_fastPipelines.find(key);
  if (it != m_fastPipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;

  auto setCommonState = [&](vk::PipelineVertexInputStateCreateInfo& vertexInput,
                            std::span<vk::PipelineColorBlendAttachmentState const> blends,
                            std::span<vk::PushConstantRange const> pushConstants,
                            const std::vector<vk::Format>& colorFormats,
                            const std::optional<vk::Format>& depthFormat,
                            std::initializer_list<const vk::raii::DescriptorSetLayout*> layouts,
                            vk::PrimitiveTopology topology,
                            bool depthEnabled) {
    instance.pipeline = device.createPipeline(*instance.shader, vertexInput, topology);
    std::vector<vk::DescriptorSetLayout> layoutHandles;
    layoutHandles.reserve(layouts.size());
    for (const auto* layout : layouts) {
      CHECK(layout != nullptr) << "Fast pipeline descriptor layout unavailable";
      layoutHandles.push_back(**layout);
    }
    if (!layoutHandles.empty()) {
      instance.pipeline->setDescriptorSetLayouts(layoutHandles);
    }
    instance.pipeline->setAttachmentFormats(colorFormats, depthFormat);
    instance.colorFormats = colorFormats;
    instance.depthFormat = depthFormat;
    instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
    instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
    instance.pipeline->setDepthTestEnable(depthEnabled);
    instance.pipeline->setDepthWriteEnable(depthEnabled);
    instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);
    if (!blends.empty()) {
      if (blends.size() == 1 && colorFormats.size() <= 1) {
        instance.pipeline->setColorBlendAttachment(blends.front());
      } else {
        std::vector<vk::PipelineColorBlendAttachmentState> att;
        const size_t count = std::max<size_t>(colorFormats.empty() ? 1 : colorFormats.size(), blends.size());
        att.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          att.push_back(blends[std::min(i, blends.size() - 1)]);
        }
        instance.pipeline->setColorBlendAttachments(std::move(att));
      }
    }
    if (!pushConstants.empty()) {
      instance.pipeline->setPushConstantRanges(std::vector(pushConstants.begin(), pushConstants.end()));
    }
  };

  switch (key.variant) {
    case FastPipelineVariant::Volume: {
      instance.shader = std::make_unique<ZVulkanShader>(device,
                                                        shaderBase + "pass.vert.spv",
                                                        shaderBase + "volume_raycaster_single_channel.frag.spv",
                                                        std::nullopt);
      vk::VertexInputBindingDescription binding{.binding = 0,
                                                .stride = sizeof(glm::vec3),
                                                .inputRate = vk::VertexInputRate::eVertex};
      vk::VertexInputAttributeDescription attr{.location = 0,
                                               .binding = 0,
                                               .format = vk::Format::eR32G32B32Sfloat,
                                               .offset = 0};
      vk::PipelineVertexInputStateCreateInfo vertexInput{};
      vertexInput.vertexBindingDescriptionCount = 1;
      vertexInput.pVertexBindingDescriptions = &binding;
      vertexInput.vertexAttributeDescriptionCount = 1;
      vertexInput.pVertexAttributeDescriptions = &attr;

      vk::PipelineColorBlendAttachmentState blend{};
      // Match GL fast path: disable blending and write pre-multiplied color directly.
      blend.blendEnable = VK_FALSE;
      blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

      vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                    .offset = 0,
                                    .size = static_cast<uint32_t>(sizeof(float) * 5)};

      std::array<vk::SpecializationMapEntry, 3> entries{
        vk::SpecializationMapEntry{.constantID = 80, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
        vk::SpecializationMapEntry{.constantID = 81, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
        vk::SpecializationMapEntry{.constantID = 51, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}
      };
      const uint32_t rayMode = rayModeConstant(key.mode);
      const uint32_t localMip = usesLocalMip(key.mode) ? 1u : 0u;
      const uint32_t opaque = key.resultOpaque ? 1u : 0u;
      std::array<uint32_t, 3> values{rayMode, localMip, opaque};
      std::vector<uint8_t> data(sizeof(values));
      std::memcpy(data.data(), values.data(), sizeof(values));
      instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                                  std::vector(entries.begin(), entries.end()),
                                                  data);
      setCommonState(vertexInput,
                     std::span(&blend, 1),
                     std::span(&pcRange, 1),
                     key.colorFormats,
                     key.depthFormat,
                     {&*m_fastSetLayout},
                     vk::PrimitiveTopology::eTriangleStrip,
                     key.depthEnabled);
      instance.pipeline->create();
      VLOG(2) << fmt::format(
        "FastPipeline Volume: depthEnabled={} colorFmt0={} depthFmt={} rayMode={} localMip={} opaque={}",
        key.depthEnabled,
        key.colorFormats.empty() ? -1 : static_cast<int>(key.colorFormats.front()),
        key.depthFormat ? static_cast<int>(*key.depthFormat) : -1,
        values[0],
        values[1],
        values[2]);
      break;
    }

    case FastPipelineVariant::Image2D: {
      instance.shader = std::make_unique<ZVulkanShader>(device,
                                                        shaderBase + "transform_with_2dtexture.vert.spv",
                                                        shaderBase + "image2d_with_transfun_single_channel.frag.spv",
                                                        std::nullopt);

      vk::VertexInputBindingDescription binding{.binding = 0,
                                                .stride = static_cast<uint32_t>(sizeof(EntryVertex)),
                                                .inputRate = vk::VertexInputRate::eVertex};
      std::array<vk::VertexInputAttributeDescription, 2> attrs{
        vk::VertexInputAttributeDescription{.location = 0,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32B32Sfloat,
                                            .offset = static_cast<uint32_t>(offsetof(EntryVertex, position))},
        vk::VertexInputAttributeDescription{.location = 1,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32Sfloat,
                                            .offset = static_cast<uint32_t>(offsetof(EntryVertex, texCoord))}
      };
      vk::PipelineVertexInputStateCreateInfo vertexInput{};
      vertexInput.vertexBindingDescriptionCount = 1;
      vertexInput.pVertexBindingDescriptions = &binding;
      vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
      vertexInput.pVertexAttributeDescriptions = attrs.data();

      vk::PipelineColorBlendAttachmentState blend{};
      blend.blendEnable = VK_FALSE;
      blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

      vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eVertex,
                                    .offset = 0,
                                    .size = static_cast<uint32_t>(sizeof(glm::mat4))};

      vk::SpecializationMapEntry entry{.constantID = 51, .offset = 0, .size = sizeof(uint32_t)};
      uint32_t value = key.resultOpaque ? 1u : 0u;
      std::vector<uint8_t> data(sizeof(uint32_t));
      std::memcpy(data.data(), &value, sizeof(uint32_t));
      instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, std::vector{entry}, data);
      setCommonState(vertexInput,
                     std::span(&blend, 1),
                     std::span(&pcRange, 1),
                     key.colorFormats,
                     key.depthFormat,
                     {&*m_image2DSetLayout},
                     vk::PrimitiveTopology::eTriangleList,
                     key.depthEnabled);
      instance.pipeline->create();
      VLOG(2) << fmt::format("FastPipeline Image2D: depthEnabled={} colorFmt0={} depthFmt={} opaque={}",
                             key.depthEnabled,
                             key.colorFormats.empty() ? -1 : static_cast<int>(key.colorFormats.front()),
                             key.depthFormat ? static_cast<int>(*key.depthFormat) : -1,
                             value);
      break;
    }

    case FastPipelineVariant::Slice2D: {
      instance.shader =
        std::make_unique<ZVulkanShader>(device,
                                        shaderBase + "transform_with_3dtexture_and_eye_coordinate.vert.spv",
                                        shaderBase + "volume_slice_with_transfun_single_channel.frag.spv",
                                        std::nullopt);

      vk::VertexInputBindingDescription binding{.binding = 0,
                                                .stride = static_cast<uint32_t>(sizeof(EntryVertex)),
                                                .inputRate = vk::VertexInputRate::eVertex};
      std::array<vk::VertexInputAttributeDescription, 2> attrs{
        vk::VertexInputAttributeDescription{.location = 0,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32B32Sfloat,
                                            .offset = static_cast<uint32_t>(offsetof(EntryVertex, position))},
        vk::VertexInputAttributeDescription{.location = 1,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32B32Sfloat,
                                            .offset = static_cast<uint32_t>(offsetof(EntryVertex, texCoord))}
      };
      vk::PipelineVertexInputStateCreateInfo vertexInput{};
      vertexInput.vertexBindingDescriptionCount = 1;
      vertexInput.pVertexBindingDescriptions = &binding;
      vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
      vertexInput.pVertexAttributeDescriptions = attrs.data();

      vk::PipelineColorBlendAttachmentState blend{};
      blend.blendEnable = VK_FALSE;
      blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

      vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eVertex,
                                    .offset = 0,
                                    .size = static_cast<uint32_t>(sizeof(glm::mat4) * 2)};

      vk::SpecializationMapEntry entry{.constantID = 51, .offset = 0, .size = sizeof(uint32_t)};
      uint32_t value = key.resultOpaque ? 1u : 0u;
      std::vector<uint8_t> data(sizeof(uint32_t));
      std::memcpy(data.data(), &value, sizeof(uint32_t));
      instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, std::vector{entry}, data);
      setCommonState(vertexInput,
                     std::span(&blend, 1),
                     std::span(&pcRange, 1),
                     key.colorFormats,
                     key.depthFormat,
                     {&*m_sliceFastSetLayout},
                     vk::PrimitiveTopology::eTriangleList,
                     key.depthEnabled);
      instance.pipeline->create();
      VLOG(2) << fmt::format("FastPipeline Slice2D: depthEnabled={} colorFmt0={} depthFmt={} opaque={}",
                             key.depthEnabled,
                             key.colorFormats.empty() ? -1 : static_cast<int>(key.colorFormats.front()),
                             key.depthFormat ? static_cast<int>(*key.depthFormat) : -1,
                             value);
      break;
    }
  }

  auto [inserted, _] = m_fastPipelines.emplace(key, std::move(instance));
  return inserted->second;
}

} // namespace nim
