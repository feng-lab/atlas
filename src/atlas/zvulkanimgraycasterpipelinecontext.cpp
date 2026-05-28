#include "zvulkanimgraycasterpipelinecontext.h"
#include "zcommandlineflags.h"

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
#include "zabslflagtypes.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zsysteminfo.h"
#include <fstream>
#include "z3drenderervulkanbackend.h"
#include "zvulkanpagedimageblockuploader.h"
#include "zcancellation.h"
#include "zrenderthreadexecutor_tls.h"
#include "zvulkanresidencymanager.h"

// Safe instantiation of capturing coroutine callables.
#include <folly/coro/Invoke.h>
#include <folly/executors/GlobalExecutor.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>
#include <cstring>
#include <cmath>

// Coroutine scheduling for post-frame work on the render thread.
#include <folly/coro/Task.h>

// Debug: save entry/exit textures after they are rendered (Vulkan only)
ABSL_FLAG(bool,
          atlas_debug_save_entry_exit,
          false,
          "Save Vulkan entry/exit textures (RGBA32F) to TIF files after rendering.");
ABSL_FLAG(std::string,
          atlas_debug_save_dir,
          "",
          "Directory to write debug images (default: current working directory)");
ABSL_FLAG(bool,
          atlas_debug_save_raycaster_layers,
          false,
          "Save Vulkan raycaster layered outputs (color + depth, one TIF per layer) after rendering.");
ABSL_FLAG(bool,
          atlas_debug_save_raycaster_merge_out,
          false,
          "Save Vulkan raycaster merged output (first color attachment) after merge.");
ABSL_FLAG(bool,
          atlas_debug_save_slice_layers,
          false,
          "Save Vulkan slice layered outputs (color + depth, one TIF per layer) after rendering.");
ABSL_FLAG(bool,
          atlas_debug_save_slice_merge_out,
          false,
          "Save Vulkan slice merged output (first color attachment) after merge.");

// Debug: CPU-side count of non-zero block IDs written by the Block-ID pass (per attachment)
ABSL_FLAG(bool,
          atlas_vk_debug_blockid_count,
          false,
          "After Block-ID draw, count non-zero IDs per attachment via CPU readback");

namespace nim {

inline constexpr std::array<AbslEnumFlagValue<VulkanBlockIdCompactionMethod>, 5>
  kVulkanBlockIdCompactionMethodFlagValues{
    {
     {"append_storage_parallel_flush", VulkanBlockIdCompactionMethod::AppendStorageParallelFlush},
     {"append_storage_parallel_flush_gpu_unique", VulkanBlockIdCompactionMethod::AppendStorageParallelFlushGpuUnique},
     {"append_sampled_parallel_flush", VulkanBlockIdCompactionMethod::AppendSampledParallelFlush},
     {"dense_bitset_readback", VulkanBlockIdCompactionMethod::DenseBitsetReadback},
     {"dense_bitset_flags_readback", VulkanBlockIdCompactionMethod::DenseBitsetFlagsReadback},
     }
};

inline bool AbslParseFlag(absl::string_view text, VulkanBlockIdCompactionMethod* value, std::string* error)
{
  return parseAbslEnumFlag(text,
                           value,
                           error,
                           "VulkanBlockIdCompactionMethod",
                           kVulkanBlockIdCompactionMethodFlagValues);
}

inline std::string AbslUnparseFlag(VulkanBlockIdCompactionMethod value)
{
  return unparseAbslEnumFlag(value, kVulkanBlockIdCompactionMethodFlagValues);
}

} // namespace nim

ABSL_FLAG(nim::VulkanBlockIdCompactionMethod,
          atlas_vk_blockid_compaction_method,
          nim::VulkanBlockIdCompactionMethod::AppendStorageParallelFlushGpuUnique,
          "Block-ID compaction method: 'append_storage_parallel_flush_gpu_unique' (default), "
          "'append_storage_parallel_flush', 'append_sampled_parallel_flush', 'dense_bitset_readback', or "
          "'dense_bitset_flags_readback'");
ABSL_FLAG(bool,
          atlas_benchmark_vk_blockid_compaction,
          false,
          "Replay real Vulkan raycaster block-ID attachments through all implemented compaction methods, "
          "validate identical IDs, and log per-method readback/parse metrics.");

// Debug dump of Vulkan raycaster inputs before dispatch (CPU-side only)
ABSL_FLAG(bool,
          atlas_vk_debug_raycaster_dump,
          false,
          "Dump Vulkan raycaster inputs (specializations, push constants, page data, bindings)");
ABSL_FLAG(int32_t,
          atlas_vk_debug_raycaster_dump_levels,
          8,
          "Max page levels to print when dumping Vulkan raycaster inputs");

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

struct RaycasterFastVolumePushConstants
{
  float sampling_rate = 1.0f;
  float iso_value = 0.5f;
  float local_MIP_threshold = 0.8f;
  float ze_to_zw_a = 0.0f;
  float ze_to_zw_b = 0.0f;
  uint32_t ray_entry_exit_tex_coord = 0u;
  uint32_t volume_1 = 0u;
  uint32_t transfer_function_1 = 0u;
};

static_assert(sizeof(RaycasterFastVolumePushConstants) == 32u,
              "Raycaster fast volume push constants must match GLSL packing (5 floats + 3 uints)");

struct RaycasterProgressivePushConstants
{
  float sampling_rate = 1.0f;
  float iso_value = 0.5f;
  float local_MIP_threshold = 0.8f;
  float ze_to_zw_a = 0.0f;
  float ze_to_zw_b = 0.0f;

  uint32_t page_directory = 0u;
  uint32_t page_table_cache = 0u;
  uint32_t image_cache = 0u;
  uint32_t volume = 0u;
  uint32_t transfer_function = 0u;
  uint32_t ray_entry_exit_tex_coord = 0u;
  uint32_t last_ray_depth_tex = 0u;
  uint32_t last_color_tex = 0u;
};

static_assert(sizeof(RaycasterProgressivePushConstants) == 52u,
              "Raycaster progressive push constants must match GLSL packing (5 floats + 8 uints)");

struct RaycasterCopyMergePushConstants
{
  uint32_t color_texture = 0u;
  uint32_t depth_texture = 0u;
};

static_assert(sizeof(RaycasterCopyMergePushConstants) == 8u,
              "Copy/merge push constants must match GLSL packing (2 uints)");

struct RaycasterSingleChannelBindlessUBOStd140
{
  uint32_t volume_1 = 0u;
  uint32_t transfer_function_1 = 0u;
  uint32_t _pad0 = 0u;
  uint32_t _pad1 = 0u;
  uint32_t _pad2 = 0u;
  uint32_t _pad3 = 0u;
  uint32_t _pad4 = 0u;
  uint32_t _pad5 = 0u;
};

static_assert(sizeof(RaycasterSingleChannelBindlessUBOStd140) == 32u,
              "Bindless indices UBO must match backend-shared imgIndices descriptor range (8 uints / 32B)");
static_assert(kVulkanAnalyticRaySetupMaxClipPlanes == kZ3DAnalyticRaySetupMaxClipPlanes);

ImgRaySetupUBOStd140 buildRaySetupUBO(const Z3DAnalyticRaySetup& setup)
{
  ImgRaySetupUBOStd140 ubo{};
  ubo.ndc_to_tex = setup.ndcToTex;
  ubo.ndc_to_eye = setup.ndcToEye;
  ubo.box_min_tex = glm::vec4(setup.boxMinTex, 0.0f);
  ubo.box_max_tex = glm::vec4(setup.boxMaxTex, 0.0f);
  ubo.ndc_z_range = glm::vec4(setup.ndcZRange, 0.0f, 0.0f);
  ubo.clip_params = glm::uvec4(setup.clipPlaneCount, 0u, 0u, setup.enabled ? 1u : 0u);
  for (size_t i = 0; i < kVulkanAnalyticRaySetupMaxClipPlanes; ++i) {
    ubo.clip_planes[i] = setup.clipPlanes[i];
  }
  return ubo;
}

// Block-ID compaction table parameters (must match GLSL)
constexpr uint32_t kEmptyBlockID = 0xFFFFFFFFu;
constexpr uint32_t kBlockIdCompactionHeaderWords = 1u + 8u; // [count][counts[8]]

uint32_t rayModeConstant(ImgCompositingMode mode)
{
  switch (mode) {
    case ImgCompositingMode::LocalMIP:
    case ImgCompositingMode::LocalMIPOpaque:
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
    if (absl::GetFlag(FLAGS_atlas_vk_debug_raycaster_dump)) {
      const int maxLevels = std::max(0, absl::GetFlag(FLAGS_atlas_vk_debug_raycaster_dump_levels));
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
  if (absl::GetFlag(FLAGS_atlas_vk_debug_raycaster_dump)) {
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

} // namespace

[[nodiscard]] VulkanBlockIdCompactionMethod vkBlockIdCompactionMethod();
[[nodiscard]] bool benchmarkVkBlockIdCompaction();
[[nodiscard]] std::vector<VulkanBlockIdCompactionMethod>
blockIdCompactionMethodsToRecord(VulkanBlockIdCompactionMethod productionMethod);
[[nodiscard]] size_t denseBitsetWordCount(uint32_t maxBlockId);
[[nodiscard]] bool blockIdCompactionMethodReadsBackDenseBitset(VulkanBlockIdCompactionMethod method);
[[nodiscard]] bool blockIdCompactionMethodUsesAppendGpuUnique(VulkanBlockIdCompactionMethod method);

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

void ZVulkanImgRaycasterPipelineContext::preRecordBindlessWarmup(const BindlessWarmupDesc& desc)
{
  CHECK(!m_backend.isRecording()) << "Raycaster preRecordBindlessWarmup called while recording";
  if (!desc.image) {
    return;
  }
  if (desc.channels.empty()) {
    return;
  }

  auto& image = *desc.image;

  if (desc.wantsPaging) {
    ensureUploader();
    CHECK(m_imageBlockUploader != nullptr) << "Raycaster paging requested but image block uploader is missing";
    m_imageBlockUploader->bindToImage(image);
  }

  for (size_t channelIndex : desc.channels) {
    CHECK_LT(channelIndex, image.numChannels()) << "Raycaster bindless warmup: channel index out of range";
    ChannelResources& resources = ensureChannelResources(channelIndex);

    auto channelImage = image.channelImageShared(channelIndex);
    CHECK(channelImage != nullptr) << "Raycaster bindless warmup: missing channel image";
    const uint64_t generation = image.volumeGeneration(channelIndex);

    if (desc.wants2D) {
      ZVulkanTexture& img2d = ensureImage2DTexture(image, channelIndex, generation, channelImage);
      (void)m_backend.bindlessRegisterSampledImageAuto(img2d, "raycaster_image2d");
    }

    if (desc.wantsVolume3D) {
      ZVulkanTexture& vol = ensureVolumeTexture(image, channelIndex, generation, channelImage);
      (void)m_backend.bindlessRegisterSampledImageAuto(vol, "raycaster_volume");
    }

    if (desc.transferFunctions) {
      CHECK_LT(channelIndex, desc.transferFunctions->size())
        << "Raycaster bindless warmup: transferFunctions size < channel index";
      auto* tf = (*desc.transferFunctions)[channelIndex];
      if (tf != nullptr) {
        ZVulkanTexture& transfer = ensureTransferTexture(resources, *tf);
        (void)m_backend.bindlessRegisterSampledImageAuto(transfer, "raycaster_transfer");
      }
    }

    if (desc.wantsPaging) {
      CHECK(m_imageBlockUploader != nullptr) << "Raycaster bindless warmup: paging uploader missing";
      ZVulkanTexture* imageCache = m_imageBlockUploader->imageCacheTexture(image, channelIndex);
      ZVulkanTexture* pageDirectory = m_imageBlockUploader->pageDirectoryTexture(image, channelIndex);
      ZVulkanTexture* pageTable = m_imageBlockUploader->pageTableTexture(image, channelIndex);
      CHECK(pageDirectory && pageTable && imageCache)
        << "Raycaster bindless warmup: paging textures missing for channel " << channelIndex;
      (void)m_backend.bindlessRegisterSampledImageAuto(*pageDirectory, "raycaster_page_directory");
      (void)m_backend.bindlessRegisterSampledImageAuto(*pageTable, "raycaster_page_table");
      (void)m_backend.bindlessRegisterSampledImageAuto(*imageCache, "raycaster_image_cache");
    }
  }
}

void ZVulkanImgRaycasterPipelineContext::preRecordPrimeBlockIdCompaction(
  const std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease>& blockIdLease,
  uint32_t effectiveAttachmentCount,
  uint32_t maxBlockId)
{
  CHECK(!m_backend.isRecording()) << "Raycaster preRecordPrimeBlockIdCompaction called while recording";
  if (!blockIdLease || !blockIdLease->hasVulkanImage()) {
    return;
  }

  ZVulkanTexture* firstBlock = blockIdLease->colorAttachment(0);
  if (!firstBlock) {
    return;
  }

  const uint32_t imgW = firstBlock->width();
  const uint32_t imgH = firstBlock->height();
  if (imgW == 0u || imgH == 0u) {
    return;
  }

  // Determine how many attachments to compact (max 8 supported by header).
  const uint32_t attachmentCount = std::max<uint32_t>(1u, blockIdLease->attachments);
  uint32_t effectiveCount = attachmentCount;
  if (effectiveAttachmentCount != 0u) {
    effectiveCount = std::min<uint32_t>(attachmentCount, std::max<uint32_t>(1u, effectiveAttachmentCount));
  }
  effectiveCount = std::min<uint32_t>(effectiveCount, 8u);

  // Ensure pipelines/layouts exist for the active compaction method.
  ensureBlockIdCompactionPipeline();

  const uint32_t capacityIDs = std::max<uint32_t>(1u, imgW * imgH * 4u);
  const VulkanBlockIdCompactionMethod productionMethod = vkBlockIdCompactionMethod();
  const std::vector<VulkanBlockIdCompactionMethod> methods = blockIdCompactionMethodsToRecord(productionMethod);

  for (const VulkanBlockIdCompactionMethod method : methods) {
    const size_t bitsetWords = denseBitsetWordCount(maxBlockId);
    const size_t payloadWords =
      blockIdCompactionMethodUsesAppendGpuUnique(method)
        ? bitsetWords + static_cast<size_t>(maxBlockId)
        : (blockIdCompactionMethodReadsBackDenseBitset(method) ? bitsetWords : static_cast<size_t>(capacityIDs));
    const size_t bytes = (static_cast<size_t>(kBlockIdCompactionHeaderWords) + payloadWords) * sizeof(uint32_t);
    auto& out = ensureBlockIdCompactOutput(method, bytes);
    CHECK(out.buffer != nullptr) << "Raycaster block-ID compaction: missing output buffer";
    ZVulkanBuffer* outBuffer = out.buffer.get();
    CHECK(outBuffer != nullptr);

    if (blockIdCompactionMethodUsesStorage(method)) {
      CHECK(m_blockIdCompactSetLayoutStorage.has_value()) << "Raycaster block-ID storage compaction layout missing";
      auto& storageByMethod = blockIdCompactionMethodIsDense(method)
                                ? m_blockIdCompactDescriptorDenseStorageByMethodAndLease
                                : m_blockIdCompactDescriptorStorageByMethodAndLease;
      auto& storageMap = storageByMethod[method];
      BlockIdStorageDescriptorPack& pack = storageMap[blockIdLease.get()];
      if (pack.perAttachment.size() < effectiveCount) {
        pack.perAttachment.resize(effectiveCount);
      }

      for (uint32_t att = 0; att < effectiveCount; ++att) {
        ZVulkanTexture* blockTex = blockIdLease->colorAttachment(att);
        if (!blockTex) {
          continue;
        }
        blockTex->setDescriptorLayout(vk::ImageLayout::eGeneral);

        if (!pack.perAttachment[att]) {
          pack.perAttachment[att] = m_backend.allocateFrameDescriptorSet(**m_blockIdCompactSetLayoutStorage);
        }
        CHECK(pack.perAttachment[att] != nullptr)
          << "Raycaster block-ID storage compaction: failed to allocate descriptor set (method="
          << blockIdCompactionMethodName(method) << " att=" << att << ")";
        pack.perAttachment[att]->updateStorageBuffer(0, *outBuffer);
        pack.perAttachment[att]->updateStorageImage(1,
                                                    *blockTex,
                                                    vk::ImageLayout::eGeneral,
                                                    vk::ImageAspectFlagBits::eColor);
      }
      continue;
    }

    CHECK(blockIdCompactionMethodUsesSampled(method));
    CHECK(m_blockIdCompactSetLayoutSampled.has_value()) << "Raycaster block-ID sampled compaction layout missing";

    for (uint32_t att = 0; att < effectiveCount; ++att) {
      ZVulkanTexture* blockTex = blockIdLease->colorAttachment(att);
      if (!blockTex) {
        continue;
      }
      blockTex->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
      (void)m_backend.bindlessRegisterSampledImageAuto(*blockTex, "ray_blockid_compact_input");
    }

    auto& sampledDescriptor = m_blockIdCompactDescriptorSampledByMethod[method];
    sampledDescriptor = m_backend.allocateFrameDescriptorSet(**m_blockIdCompactSetLayoutSampled);
    CHECK(sampledDescriptor != nullptr) << "Raycaster block-ID sampled compaction: failed to allocate descriptor set";
    sampledDescriptor->updateStorageBuffer(0, *outBuffer);
  }
}

void ZVulkanImgRaycasterPipelineContext::resetFrame()
{
  // Descriptor sets for indices/page-data are backend-shared per frame-slot and
  // persist across submissions. Only reset per-submission descriptor state
  // allocated from the frame descriptor arena here.
  m_blockIdCompactDescriptorSampledByMethod.clear();
  m_blockIdCompactDescriptorStorageByMethodAndLease.clear();
  m_blockIdCompactDescriptorDenseStorageByMethodAndLease.clear();
  m_progressivePrep.reset();
  m_lastDebugDumpBlockIdKey.reset();
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
  maybeCancel(cancellationToken);

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

  ensureQuadVertexBuffer();
  const vk::DescriptorSet dsPageData = m_backend.sharedImgPageDataDescriptorSet();
  const vk::DescriptorSet dsRaySetup = m_backend.sharedImgRaySetupDescriptorSet();
  CHECK(dsPageData && dsRaySetup)
    << "Raycaster progressive path missing backend-shared ray-setup/page-data descriptor sets (unexpected)";

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

  CHECK(!payload.planarGeometry) << "Progressive raycaster path only supports volumetric rendering.";

  if (!payload.analyticRaySetup.enabled) {
    CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
      << "Vulkan raycaster progressive path missing entry/exit lease.";
  }
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

  auto channelImage = payload.image->channelImageShared(prep.channelIndex);
  CHECK(channelImage != nullptr) << "Raycaster progressive path missing channel image";
  const uint64_t volGen = payload.image->volumeGeneration(prep.channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(*payload.image, prep.channelIndex, volGen, channelImage);

  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster progressive path: payload missing transferFunctions vector (fatal)";
  const auto& transferList = *payload.transferFunctions;
  CHECK(prep.channelIndex < transferList.size() && transferList[prep.channelIndex] != nullptr)
    << "Vulkan raycaster missing transfer function for channel " << prep.channelIndex;
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferList[prep.channelIndex]);
  prep.volumeTexture = &volumeTex;
  prep.transferTexture = &transferTex;

  const ImgRaySetupUBOStd140 raySetup = buildRaySetupUBO(payload.analyticRaySetup);
  const auto raySetupSlice = m_backend.suballocateUniformFor(payload, sizeof(raySetup));
  CHECK(raySetupSlice.mapped != nullptr) << "Raycaster analytic ray-setup uniform slice mapping missing";
  std::memcpy(raySetupSlice.mapped, &raySetup, sizeof(raySetup));
  CHECK(raySetupSlice.offset <= std::numeric_limits<uint32_t>::max())
    << "Raycaster analytic ray-setup dynamic offset exceeds uint32 range: " << raySetupSlice.offset;
  prep.raySetupDynOffset = static_cast<uint32_t>(raySetupSlice.offset);

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
  CHECK((payload.analyticRaySetup.enabled || prep.entryTexture != nullptr) && prep.lastColor && prep.lastDepth &&
        prep.currentColor && prep.currentDepth)
    << "Vulkan raycaster progressive path missing required textures.";

  prep.imageCache = m_imageBlockUploader->imageCacheTexture(*payload.image, prep.channelIndex);
  prep.pageDirectory = m_imageBlockUploader->pageDirectoryTexture(*payload.image, prep.channelIndex);
  prep.pageTable = m_imageBlockUploader->pageTableTexture(*payload.image, prep.channelIndex);
  CHECK(prep.pageDirectory && prep.pageTable && prep.imageCache)
    << "Raycaster progressive path missing paging textures for channel " << prep.channelIndex;

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

  const uint32_t devCap = deviceLevelCap(m_backend.device());
  const uint32_t levelCount = static_cast<uint32_t>(std::min<size_t>(payload.image->numLevels(), devCap));
  CHECK_GT(levelCount, 0u) << "Raycaster progressive path has no paging levels";

  // Validate that paging metadata arrays have at least levelCount entries so
  // shader-side indexing stays in-bounds.
  {
    const auto& pdb = payload.image->pageDirectoryBases();
    const auto& p2b = payload.image->posToBlockIDsLevels();
    const auto& dims = payload.image->imageDimensionsLevels();
    const auto& vox = payload.image->voxelWorldSizesLevels();
    CHECK_GE(pdb.size(), levelCount) << "Insufficient pageDirectoryBases for requested levels: have=" << pdb.size()
                                     << " need=" << levelCount;
    CHECK_GE(p2b.size(), levelCount) << "Insufficient posToBlockIDsLevels for requested levels: have=" << p2b.size()
                                     << " need=" << levelCount;
    CHECK_GE(dims.size(), levelCount) << "Insufficient imageDimensionsLevels for requested levels: have=" << dims.size()
                                      << " need=" << levelCount;
    CHECK_GE(vox.size(), levelCount) << "Insufficient voxelWorldSizesLevels for requested levels: have=" << vox.size()
                                     << " need=" << levelCount;
  }

  prep.levelCount = levelCount;
  resources.levelCount = levelCount;

  const std::vector<uint8_t> pageData =
    buildPageDataBuffer(*payload.image, prep.channelIndex, prep.zeToScreenPixelVoxelSize, levelCount);
  const auto pageSlice = m_backend.suballocateUniformFor(payload, pageData.size());
  CHECK(pageSlice.mapped != nullptr) << "Raycaster page-data uniform slice mapping missing";
  std::memcpy(pageSlice.mapped, pageData.data(), pageData.size());
  CHECK(pageSlice.offset <= std::numeric_limits<uint32_t>::max())
    << "Raycaster page-data dynamic offset exceeds uint32 range: " << pageSlice.offset;
  prep.pageDataDynOffset = static_cast<uint32_t>(pageSlice.offset);

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

  const bool planarGeometry = payload.planarGeometry;
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
  if (absl::GetFlag(FLAGS_atlas_debug_save_entry_exit) && layerIndex == 1u) {
    auto leaseRef = payload.entryExitLease;
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend && leaseRef && leaseRef->hasVulkanImage()) {
      ZVulkanTexture* tex = leaseRef->colorAttachment(0);
      if (!tex) {
        LOG(ERROR) << "Entry/exit debug save: color attachment missing";
        return;
      }

      QString dir = QString::fromStdString(absl::GetFlag(FLAGS_atlas_debug_save_dir));
      if (!dir.isEmpty() && !dir.endsWith('/')) {
        dir += '/';
      }

      struct SaveJob
      {
        QString filename;
        Z3DRendererVulkanBackend::EndOfFrameHostImageReadbackTicket ticket;
      };
      std::vector<SaveJob> jobs;
      jobs.reserve(2u);

      auto enqueueLayer = [&](uint32_t layer, const QString& suffix) {
        jobs.push_back(
          SaveJob{dir + QString("entry_exit_%1_%2x%3.tif").arg(suffix).arg(tex->width()).arg(tex->height()),
                  backend->requestEndOfFrameImageReadbackToHostTicket(*tex,
                                                                      batch.eye,
                                                                      layer,
                                                                      vk::ImageAspectFlagBits::eColor,
                                                                      leaseRef,
                                                                      "VK debug save entry/exit")});
      };

      enqueueLayer(0u, QStringLiteral("front"));
      if (tex->arrayLayers() > 1u) {
        enqueueLayer(1u, QStringLiteral("back"));
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
                LOG(ERROR) << "Entry/exit debug save failed for " << job.filename;
              }
            }
            co_return;
          }),
          "VK debug save entry/exit");
      }
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

  const bool planarGeometry = payload.planarGeometry;
  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);

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
    if (!payload.analyticRaySetup.enabled) {
      CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
        << "Raycaster fast volume stage missing entry/exit lease.";
    }
    auto* entryTexture = payload.entryExitLease ? payload.entryExitLease->colorAttachment(0) : nullptr;
    CHECK(payload.analyticRaySetup.enabled || entryTexture != nullptr) << "Entry/exit texture unavailable";

    ChannelResources& resources = ensureChannelResources(channelIndex);
    auto channelImage = payload.image->channelImageShared(channelIndex);
    CHECK(channelImage != nullptr) << "Raycaster fast volume stage missing channel image";
    const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& volumeTex = ensureVolumeTexture(*payload.image, channelIndex, volGen, channelImage);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
    const ImgRaySetupUBOStd140 raySetup = buildRaySetupUBO(payload.analyticRaySetup);
    const auto raySetupSlice = m_backend.suballocateUniformFor(payload, sizeof(raySetup));
    CHECK(raySetupSlice.mapped != nullptr) << "Raycaster fast volume ray-setup uniform slice mapping missing";
    std::memcpy(raySetupSlice.mapped, &raySetup, sizeof(raySetup));
    CHECK(raySetupSlice.offset <= std::numeric_limits<uint32_t>::max())
      << "Raycaster fast volume ray-setup dynamic offset exceeds uint32 range: " << raySetupSlice.offset;
    const uint32_t raySetupDynOffset = static_cast<uint32_t>(raySetupSlice.offset);

    const auto& viewState = renderer.viewState();
    const float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
    const float farClip = viewState.farClip;
    const float zeToZW_a = farClip * nearClip / (farClip - nearClip);
    const float zeToZW_b = 0.5f * (farClip + nearClip) / (farClip - nearClip) + 0.5f;

    RaycasterFastVolumePushConstants pc{};
    pc.sampling_rate = payload.samplingRate;
    pc.iso_value = payload.isoValue;
    pc.local_MIP_threshold = payload.localMIPThreshold;
    pc.ze_to_zw_a = zeToZW_a;
    pc.ze_to_zw_b = zeToZW_b;
    pc.ray_entry_exit_tex_coord =
      entryTexture ? m_backend.bindlessLookupSampledImageAutoOrCrash(*entryTexture, "ray_fast_entry_exit") : 0u;
    pc.volume_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(volumeTex, "ray_fast_volume");
    pc.transfer_function_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(transferTex, "ray_fast_transfer");

    FastPipelineKey pipelineKey;
    pipelineKey.variant = FastPipelineVariant::Volume;
    pipelineKey.mode = composite.mode;
    pipelineKey.resultOpaque = composite.resultOpaque;
    pipelineKey.depthEnabled = formats.depthFormat.has_value();
    pipelineKey.colorFormats = formats.colorFormats;
    pipelineKey.depthFormat = formats.depthFormat;
    PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

    const vk::DescriptorSet dsRaySetup = m_backend.sharedImgRaySetupDescriptorSet();
    CHECK(dsRaySetup) << "Raycaster fast volume stage missing backend-shared ray-setup descriptor set";
    const std::array<vk::DescriptorSet, 2> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(), dsRaySetup};
    const std::array<uint32_t, 1> dynamicOffsets{raySetupDynOffset};

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    drawSpec.descriptorSets = descriptorSets;
    drawSpec.dynamicOffsets = dynamicOffsets;
    drawSpec.descriptorSetFirst = 0;
    drawSpec.expectedDescriptorSetCount = 2;
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
  const vk::DescriptorSet dsIndices = m_backend.sharedImgIndicesDescriptorSet();
  CHECK(dsIndices) << "Raycaster planar stages missing backend-shared indices descriptor set (unexpected)";

  const size_t eyeIndex = std::min<size_t>(static_cast<size_t>(batch.eye), renderer.viewState().eyes.size() - 1);
  const auto& eyeState = renderer.viewState().eyes[eyeIndex];
  const glm::mat4 projectionView = eyeState.projectionMatrix * eyeState.viewMatrix;

  const uint32_t vertexCount = static_cast<uint32_t>(payload.entryPositions.size());
  CHECK_GT(vertexCount, 0u) << "Raycaster fast planar stage missing vertex data";

  if (payload.image->is2DData()) {
    ChannelResources& resources = ensureChannelResources(channelIndex);
    auto channelImage = payload.image->channelImageShared(channelIndex);
    CHECK(channelImage != nullptr) << "Raycaster fast planar stage missing channel image";
    const uint64_t imgGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& imageTex = ensureImage2DTexture(*payload.image, channelIndex, imgGen, channelImage);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);

    RaycasterSingleChannelBindlessUBOStd140 ubo{};
    ubo.volume_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(imageTex, "ray_fast_image2d");
    ubo.transfer_function_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(transferTex, "ray_fast_image2d_transfer");

    const auto slice = m_backend.suballocateUniformFor(payload, sizeof(ubo));
    CHECK(slice.mapped != nullptr) << "Raycaster indices uniform slice mapping missing";
    std::memcpy(slice.mapped, &ubo, sizeof(ubo));
    CHECK(slice.offset <= std::numeric_limits<uint32_t>::max())
      << "Raycaster indices dynamic offset exceeds uint32 range: " << slice.offset;
    const uint32_t dynOffset = static_cast<uint32_t>(slice.offset);

    FastPipelineKey pipelineKey;
    pipelineKey.variant = FastPipelineVariant::Image2D;
    pipelineKey.mode = composite.mode;
    pipelineKey.resultOpaque = composite.resultOpaque;
    pipelineKey.depthEnabled = formats.depthFormat.has_value();
    pipelineKey.colorFormats = formats.colorFormats;
    pipelineKey.depthFormat = formats.depthFormat;
    PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

    const std::array<vk::DescriptorSet, 2> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(), dsIndices};
    const std::array<uint32_t, 1> dynamicOffsets{dynOffset};

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
    drawSpec.dynamicOffsets = dynamicOffsets;
    drawSpec.descriptorSetFirst = 0;
    drawSpec.expectedDescriptorSetCount = 2;
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
  auto channelImage = payload.image->channelImageShared(channelIndex);
  CHECK(channelImage != nullptr) << "Raycaster fast slice stage missing channel image";
  const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(*payload.image, channelIndex, volGen, channelImage);
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);

  RaycasterSingleChannelBindlessUBOStd140 ubo{};
  ubo.volume_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(volumeTex, "ray_fast_slice_volume");
  ubo.transfer_function_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(transferTex, "ray_fast_slice_transfer");

  const auto slice = m_backend.suballocateUniformFor(payload, sizeof(ubo));
  CHECK(slice.mapped != nullptr) << "Raycaster indices uniform slice mapping missing";
  std::memcpy(slice.mapped, &ubo, sizeof(ubo));
  CHECK(slice.offset <= std::numeric_limits<uint32_t>::max())
    << "Raycaster indices dynamic offset exceeds uint32 range: " << slice.offset;
  const uint32_t dynOffset = static_cast<uint32_t>(slice.offset);

  FastPipelineKey pipelineKey;
  pipelineKey.variant = FastPipelineVariant::Slice2D;
  pipelineKey.mode = composite.mode;
  pipelineKey.resultOpaque = composite.resultOpaque;
  pipelineKey.depthEnabled = formats.depthFormat.has_value();
  pipelineKey.colorFormats = formats.colorFormats;
  pipelineKey.depthFormat = formats.depthFormat;
  PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

  const std::array<vk::DescriptorSet, 2> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(), dsIndices};
  const std::array<uint32_t, 1> dynamicOffsets{dynOffset};

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
  drawSpec.dynamicOffsets = dynamicOffsets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 2;
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

  ensureQuadVertexBuffer();

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);
  const bool planarGeometry = payload.planarGeometry;
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

  RaycasterProgressivePushConstants pc{};
  pc.sampling_rate = payload.samplingRate;
  pc.iso_value = payload.isoValue;
  pc.local_MIP_threshold = payload.localMIPThreshold;
  pc.ze_to_zw_a = prep.zeToZW_a;
  pc.ze_to_zw_b = prep.zeToZW_b;
  pc.page_directory = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.pageDirectory, "ray_prog_blockid_page_dir");
  pc.page_table_cache = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.pageTable, "ray_prog_blockid_page_table");
  pc.image_cache = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.imageCache, "ray_prog_blockid_image_cache");
  pc.volume = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.volumeTexture, "ray_prog_blockid_volume");
  pc.transfer_function =
    m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.transferTexture, "ray_prog_blockid_transfer");
  pc.ray_entry_exit_tex_coord =
    prep.entryTexture
      ? m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.entryTexture, "ray_prog_blockid_entry_exit")
      : 0u;
  pc.last_ray_depth_tex =
    m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.lastDepth, "ray_prog_blockid_last_depth");
  pc.last_color_tex = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.lastColor, "ray_prog_blockid_last_color");

  if (absl::GetFlag(FLAGS_atlas_vk_debug_raycaster_dump)) {
    if (!m_lastDebugDumpBlockIdKey || !(*m_lastDebugDumpBlockIdKey == prep.key)) {
      m_lastDebugDumpBlockIdKey = prep.key;

      LOG(INFO) << fmt::format(
        "VK raycaster dump (Block-ID): streamKey={} eye={} progGen={} channelIndexRaw={} roundIndexRaw={} "
        "channelIndex={} levelCount={} atts={} blockFmt={} pageDataDynOffset={} ze2px={:.6f}",
        prep.key.streamKey,
        static_cast<int>(prep.key.eye),
        prep.key.progressiveGeneration,
        prep.key.channelIndexRaw,
        prep.key.roundIndexRaw,
        prep.channelIndex,
        prep.levelCount,
        blockAttachmentCount,
        enumOrUnderlying(blockFormat, 16),
        prep.pageDataDynOffset,
        prep.zeToScreenPixelVoxelSize);

      LOG(INFO) << fmt::format(
        "VK raycaster dump (Block-ID) push constants: samplingRate={:.6f} iso={:.6f} localMipThr={:.6f} "
        "ze_to_zw_a={:.6f} ze_to_zw_b={:.6f} pageDir={} pageTable={} imageCache={} volume={} transfer={} entryExit={} "
        "lastDepth={} lastColor={}",
        pc.sampling_rate,
        pc.iso_value,
        pc.local_MIP_threshold,
        pc.ze_to_zw_a,
        pc.ze_to_zw_b,
        pc.page_directory,
        pc.page_table_cache,
        pc.image_cache,
        pc.volume,
        pc.transfer_function,
        pc.ray_entry_exit_tex_coord,
        pc.last_ray_depth_tex,
        pc.last_color_tex);

      if (prep.pageDirectory) {
        m_backend.debugDumpBindlessSampledImageEntry(*prep.pageDirectory, "ray_prog_blockid_page_dir");
      }
      if (prep.pageTable) {
        m_backend.debugDumpBindlessSampledImageEntry(*prep.pageTable, "ray_prog_blockid_page_table");
      }
      if (prep.entryTexture) {
        m_backend.debugDumpBindlessSampledImageEntry(*prep.entryTexture, "ray_prog_blockid_entry_exit");
      }
      if (prep.lastDepth) {
        m_backend.debugDumpBindlessSampledImageEntry(*prep.lastDepth, "ray_prog_blockid_last_depth");
      }
      if (prep.lastColor) {
        m_backend.debugDumpBindlessSampledImageEntry(*prep.lastColor, "ray_prog_blockid_last_color");
      }
      if (prep.imageCache) {
        m_backend.debugDumpBindlessSampledImageEntry(*prep.imageCache, "ray_prog_blockid_image_cache");
      }
      if (prep.volumeTexture) {
        m_backend.debugDumpBindlessSampledImageEntry(*prep.volumeTexture, "ray_prog_blockid_volume");
      }
      if (prep.transferTexture) {
        m_backend.debugDumpBindlessSampledImageEntry(*prep.transferTexture, "ray_prog_blockid_transfer");
      }
    }
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);
  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = blockPipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = blockPipeline.pipeline->pipelineLayoutHandle();

  const vk::DescriptorSet dsRaySetup = m_backend.sharedImgRaySetupDescriptorSet();
  const vk::DescriptorSet dsPageData = m_backend.sharedImgPageDataDescriptorSet();
  CHECK(dsRaySetup && dsPageData)
    << "Vulkan raycaster progressive Block-ID stage missing shared ray-setup/page-data descriptor sets";
  const std::array<vk::DescriptorSet, 3> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(),
                                                        dsRaySetup,
                                                        dsPageData};
  const std::array<uint32_t, 2> dynamicOffsets{prep.raySetupDynOffset, prep.pageDataDynOffset};
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.dynamicOffsets = dynamicOffsets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 3;
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

  RaycasterProgressivePushConstants pc{};
  pc.sampling_rate = payload.samplingRate;
  pc.iso_value = payload.isoValue;
  pc.local_MIP_threshold = payload.localMIPThreshold;
  pc.ze_to_zw_a = prep.zeToZW_a;
  pc.ze_to_zw_b = prep.zeToZW_b;
  pc.page_directory = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.pageDirectory, "ray_prog_raycast_page_dir");
  pc.page_table_cache = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.pageTable, "ray_prog_raycast_page_table");
  pc.image_cache = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.imageCache, "ray_prog_raycast_image_cache");
  pc.volume = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.volumeTexture, "ray_prog_raycast_volume");
  pc.transfer_function =
    m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.transferTexture, "ray_prog_raycast_transfer");
  pc.ray_entry_exit_tex_coord =
    prep.entryTexture
      ? m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.entryTexture, "ray_prog_raycast_entry_exit")
      : 0u;
  pc.last_ray_depth_tex =
    m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.lastDepth, "ray_prog_raycast_last_depth");
  pc.last_color_tex = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.lastColor, "ray_prog_raycast_last_color");

  ZVulkanPipelineCommandRecorder recorder(cmd);
  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = progressivePipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = progressivePipeline.pipeline->pipelineLayoutHandle();

  const vk::DescriptorSet dsRaySetup = m_backend.sharedImgRaySetupDescriptorSet();
  const vk::DescriptorSet dsPageData = m_backend.sharedImgPageDataDescriptorSet();
  CHECK(dsRaySetup && dsPageData)
    << "Vulkan raycaster progressive raycast stage missing shared ray-setup/page-data descriptor sets";
  const std::array<vk::DescriptorSet, 3> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(),
                                                        dsRaySetup,
                                                        dsPageData};
  const std::array<uint32_t, 2> dynamicOffsets{prep.raySetupDynOffset, prep.pageDataDynOffset};
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.dynamicOffsets = dynamicOffsets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 3;
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

  RaycasterCopyMergePushConstants pc{};
  pc.color_texture = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.currentColor, "ray_prog_copy_src_color");
  pc.depth_texture = m_backend.bindlessLookupSampledImageAutoOrCrash(*prep.currentDepth, "ray_prog_copy_src_depth");

  const std::array<vk::DescriptorSet, 1> descriptorSets{m_backend.bindlessSampledImageDescriptorSet()};

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
  drawSpec.pushConstantsData = &pc;
  drawSpec.pushConstantsSize = sizeof(pc);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;

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
  CHECK_GT(channelCount, 0u) << "recordFastVolumeLayersOnly requires at least one visible channel";
  CHECK(payload.image) << "Vulkan img raycaster missing image context.";
  if (!payload.analyticRaySetup.enabled) {
    CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
      << "Raycaster fast layers stage missing entry/exit lease.";
  }

  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster fast path: payload missing transferFunctions vector (fatal)";
  const auto& transferFunctions = *payload.transferFunctions;

  auto* entryTexture = payload.entryExitLease ? payload.entryExitLease->colorAttachment(0) : nullptr;
  CHECK(payload.analyticRaySetup.enabled || entryTexture) << "Entry/exit texture unavailable.";
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
  auto channelImage = payload.image->channelImageShared(channelIndex);
  CHECK(channelImage != nullptr) << "Raycaster fast layers stage missing channel image";
  const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(*payload.image, channelIndex, volGen, channelImage);
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
  const ImgRaySetupUBOStd140 raySetup = buildRaySetupUBO(payload.analyticRaySetup);
  const auto raySetupSlice = m_backend.suballocateUniformFor(payload, sizeof(raySetup));
  CHECK(raySetupSlice.mapped != nullptr) << "Raycaster fast layers ray-setup uniform slice mapping missing";
  std::memcpy(raySetupSlice.mapped, &raySetup, sizeof(raySetup));
  CHECK(raySetupSlice.offset <= std::numeric_limits<uint32_t>::max())
    << "Raycaster fast layers ray-setup dynamic offset exceeds uint32 range: " << raySetupSlice.offset;
  const uint32_t raySetupDynOffset = static_cast<uint32_t>(raySetupSlice.offset);

  RaycasterFastVolumePushConstants pcL{};
  pcL.sampling_rate = payload.samplingRate;
  pcL.iso_value = payload.isoValue;
  pcL.local_MIP_threshold = payload.localMIPThreshold;
  pcL.ze_to_zw_a = zeToZW_a;
  pcL.ze_to_zw_b = zeToZW_b;
  pcL.ray_entry_exit_tex_coord =
    entryTexture ? m_backend.bindlessLookupSampledImageAutoOrCrash(*entryTexture, "ray_fast_layers_entry_exit") : 0u;
  pcL.volume_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(volumeTex, "ray_fast_layers_volume");
  pcL.transfer_function_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(transferTex, "ray_fast_layers_transfer");

  const auto layerFormats = vulkan::extractAttachmentFormats(batch);

  FastPipelineKey layerKey;
  layerKey.variant = FastPipelineVariant::Volume;
  layerKey.mode = composite.mode;
  layerKey.resultOpaque = composite.resultOpaque;
  layerKey.depthEnabled = layerFormats.depthFormat.has_value();
  layerKey.colorFormats = layerFormats.colorFormats;
  layerKey.depthFormat = layerFormats.depthFormat;
  PipelineInstance& layerPipeline = ensureFastPipeline(layerKey);

  const vk::DescriptorSet dsRaySetup = m_backend.sharedImgRaySetupDescriptorSet();
  CHECK(dsRaySetup) << "Raycaster fast layers stage missing backend-shared ray-setup descriptor set";
  const std::array<vk::DescriptorSet, 2> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(), dsRaySetup};
  const std::array<uint32_t, 1> dynamicOffsets{raySetupDynOffset};

  ZVulkanGraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = layerPipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = layerPipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.dynamicOffsets = dynamicOffsets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 2;
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
  CHECK_GT(channelCount, 0u) << "recordFastPlanarLayersOnly requires at least one visible channel";
  CHECK(payload.image) << "Vulkan img raycaster missing image context.";
  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster fast path: payload missing transferFunctions vector (fatal)";
  const auto& transferFunctions = *payload.transferFunctions;

  CHECK(!batch.pass.colorAttachments.empty()) << "FastLayers stage requires an active color attachment";
  const uint32_t order = batch.pass.colorAttachments.front().handle.index;
  CHECK(order < channelCount) << "FastLayers stage layer index out of range: " << order << " >= " << channelCount;
  const size_t channelIndex = payload.visibleChannels[static_cast<size_t>(order)];
  CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
    << "Missing transfer function for channel " << channelIndex;

  // Planar geometry uses the uploaded entry vertex buffer (expanded quads/slices).
  ensureEntryGeometryUploadedThisFrame(payload);
  CHECK(m_entryVertexBuffer != nullptr) << "Raycaster fast planar layers stage missing entry vertex buffer";
  const vk::DescriptorSet dsIndices = m_backend.sharedImgIndicesDescriptorSet();
  CHECK(dsIndices) << "Raycaster planar stages missing backend-shared indices descriptor set (unexpected)";

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
    auto channelImage = payload.image->channelImageShared(channelIndex);
    CHECK(channelImage != nullptr) << "Raycaster fast planar layers stage missing channel image";
    const uint64_t imgGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& imageTex = ensureImage2DTexture(*payload.image, channelIndex, imgGen, channelImage);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);

    RaycasterSingleChannelBindlessUBOStd140 ubo{};
    ubo.volume_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(imageTex, "ray_fast_layers_image2d");
    ubo.transfer_function_1 =
      m_backend.bindlessLookupSampledImageAutoOrCrash(transferTex, "ray_fast_layers_image2d_transfer");

    const auto slice = m_backend.suballocateUniformFor(payload, sizeof(ubo));
    CHECK(slice.mapped != nullptr) << "Raycaster indices uniform slice mapping missing";
    std::memcpy(slice.mapped, &ubo, sizeof(ubo));
    CHECK(slice.offset <= std::numeric_limits<uint32_t>::max())
      << "Raycaster indices dynamic offset exceeds uint32 range: " << slice.offset;
    const uint32_t dynOffset = static_cast<uint32_t>(slice.offset);

    FastPipelineKey pipelineKey;
    pipelineKey.variant = FastPipelineVariant::Image2D;
    pipelineKey.mode = composite.mode;
    pipelineKey.resultOpaque = composite.resultOpaque;
    pipelineKey.depthEnabled = formats.depthFormat.has_value();
    pipelineKey.colorFormats = formats.colorFormats;
    pipelineKey.depthFormat = formats.depthFormat;
    PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

    const std::array<vk::DescriptorSet, 2> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(), dsIndices};
    const std::array<uint32_t, 1> dynamicOffsets{dynOffset};

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
    drawSpec.dynamicOffsets = dynamicOffsets;
    drawSpec.descriptorSetFirst = 0;
    drawSpec.expectedDescriptorSetCount = 2;
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
  auto channelImage = payload.image->channelImageShared(channelIndex);
  CHECK(channelImage != nullptr) << "Raycaster fast layers slice stage missing channel image";
  const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(*payload.image, channelIndex, volGen, channelImage);
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);

  RaycasterSingleChannelBindlessUBOStd140 ubo{};
  ubo.volume_1 = m_backend.bindlessLookupSampledImageAutoOrCrash(volumeTex, "ray_fast_layers_slice_volume");
  ubo.transfer_function_1 =
    m_backend.bindlessLookupSampledImageAutoOrCrash(transferTex, "ray_fast_layers_slice_transfer");

  const auto slice = m_backend.suballocateUniformFor(payload, sizeof(ubo));
  CHECK(slice.mapped != nullptr) << "Raycaster indices uniform slice mapping missing";
  std::memcpy(slice.mapped, &ubo, sizeof(ubo));
  CHECK(slice.offset <= std::numeric_limits<uint32_t>::max())
    << "Raycaster indices dynamic offset exceeds uint32 range: " << slice.offset;
  const uint32_t dynOffset = static_cast<uint32_t>(slice.offset);

  FastPipelineKey pipelineKey;
  pipelineKey.variant = FastPipelineVariant::Slice2D;
  pipelineKey.mode = composite.mode;
  pipelineKey.resultOpaque = composite.resultOpaque;
  pipelineKey.depthEnabled = formats.depthFormat.has_value();
  pipelineKey.colorFormats = formats.colorFormats;
  pipelineKey.depthFormat = formats.depthFormat;
  PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

  const std::array<vk::DescriptorSet, 2> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(), dsIndices};
  const std::array<uint32_t, 1> dynamicOffsets{dynOffset};

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
  drawSpec.dynamicOffsets = dynamicOffsets;
  drawSpec.descriptorSetFirst = 0;
  drawSpec.expectedDescriptorSetCount = 2;
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
  ensureQuadVertexBuffer();

  // Segment-managed merge: backend owns vkCmdBeginRendering/vkCmdEndRendering and all attachment transitions.
  // This helper only emits draw commands that sample the layer array textures and write into the active surface.
  const vulkan::AttachmentFormats finalFormats = vulkan::extractAttachmentFormats(batch);
  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = composite.maxProjectionMerge;
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;

  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);
  RaycasterCopyMergePushConstants pc{};
  pc.color_texture = m_backend.bindlessLookupSampledImageAutoOrCrash(layerColor, "ray_merge_layer_color");
  pc.depth_texture = layerDepth ? m_backend.bindlessLookupSampledImageAutoOrCrash(*layerDepth, "ray_merge_layer_depth")
                                : 0u; // bindless placeholder index 0 (2DArray)
  const std::array<vk::DescriptorSet, 1> descriptorSets{m_backend.bindlessSampledImageDescriptorSet()};

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
  drawSpec.pushConstantsData = &pc;
  drawSpec.pushConstantsSize = sizeof(pc);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;

  recorder.recordGraphicsDraw(drawSpec);
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

constexpr uint32_t kDenseBitsetInvalidBlockIdFlag = 0x80000000u;

inline VulkanBlockIdCompactionMethod vkBlockIdCompactionMethod()
{
  return absl::GetFlag(FLAGS_atlas_vk_blockid_compaction_method);
}

inline bool benchmarkVkBlockIdCompaction()
{
  return absl::GetFlag(FLAGS_atlas_benchmark_vk_blockid_compaction);
}

std::vector<VulkanBlockIdCompactionMethod>
blockIdCompactionMethodsToRecord(VulkanBlockIdCompactionMethod productionMethod)
{
  std::vector<VulkanBlockIdCompactionMethod> methods;
  methods.push_back(productionMethod);
  if (!benchmarkVkBlockIdCompaction()) {
    return methods;
  }

  constexpr std::array<VulkanBlockIdCompactionMethod, 5> kBenchmarkMethods{
    VulkanBlockIdCompactionMethod::AppendSampledParallelFlush,
    VulkanBlockIdCompactionMethod::AppendStorageParallelFlush,
    VulkanBlockIdCompactionMethod::AppendStorageParallelFlushGpuUnique,
    VulkanBlockIdCompactionMethod::DenseBitsetReadback,
    VulkanBlockIdCompactionMethod::DenseBitsetFlagsReadback};
  for (const VulkanBlockIdCompactionMethod method : kBenchmarkMethods) {
    if (method != productionMethod) {
      methods.push_back(method);
    }
  }
  return methods;
}

size_t denseBitsetWordCount(uint32_t maxBlockId)
{
  return std::max<size_t>(1u, (static_cast<size_t>(maxBlockId) + 32u) / 32u);
}

bool blockIdCompactionMethodReadsBackDenseBitset(VulkanBlockIdCompactionMethod method)
{
  return method == VulkanBlockIdCompactionMethod::DenseBitsetReadback ||
         method == VulkanBlockIdCompactionMethod::DenseBitsetFlagsReadback;
}

bool blockIdCompactionMethodUsesAppendGpuUnique(VulkanBlockIdCompactionMethod method)
{
  return method == VulkanBlockIdCompactionMethod::AppendStorageParallelFlushGpuUnique;
}

void ZVulkanImgRaycasterPipelineContext::ensureBlockIdCompactionPipeline()
{
  auto& device = m_backend.device().context().device();
  const std::string shaderBase = nim::ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";
  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(static_cast<VkDescriptorSetLayout>(bindlessLayout) != VK_NULL_HANDLE)
    << "Block-ID compaction requires backend bindless descriptor set layout";

  auto loadComputePipeline =
    [&](const std::string& compPath, vk::PipelineLayout layout, std::optional<vk::raii::Pipeline>& pipeline) {
      auto spirv = readSpirvFile(compPath);
      vk::raii::ShaderModule compModule(
        device,
        vk::ShaderModuleCreateInfo{.codeSize = spirv.size() * sizeof(uint32_t), .pCode = spirv.data()});
      vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute,
                                              .module = *compModule,
                                              .pName = "main"};
      pipeline.emplace(device, nullptr, vk::ComputePipelineCreateInfo{.stage = stage, .layout = layout});
    };

  auto ensureStorageParallelFlush = [&]() {
    if (m_blockIdCompactPipelineStorageParallelFlush) {
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
    loadComputePipeline(
      shaderBase + std::string(blockIdCompactionShaderFile(VulkanBlockIdCompactionMethod::AppendStorageParallelFlush)),
      **m_blockIdCompactPipelineLayoutStorage,
      m_blockIdCompactPipelineStorageParallelFlush);
    if (VLOG_IS_ON(1)) {
      VLOG(1) << "ensureBlockIdCompactionPipeline: method=append_storage_parallel_flush";
    }
  };

  auto ensureSampledParallelFlush = [&]() {
    if (m_blockIdCompactPipelineSampledParallelFlush) {
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
    loadComputePipeline(
      shaderBase + std::string(blockIdCompactionShaderFile(VulkanBlockIdCompactionMethod::AppendSampledParallelFlush)),
      **m_blockIdCompactPipelineLayoutSampled,
      m_blockIdCompactPipelineSampledParallelFlush);
    if (VLOG_IS_ON(1)) {
      VLOG(1) << "ensureBlockIdCompactionPipeline: method=append_sampled_parallel_flush";
    }
  };

  auto ensureDenseBitsetStorage = [&]() {
    if (m_blockIdCompactPipelineDenseBitsetStorage) {
      return;
    }

    if (!m_blockIdCompactSetLayoutStorage) {
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
      m_blockIdCompactSetLayoutStorage.emplace(
        device,
        vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()});
    }
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{bindlessLayout, **m_blockIdCompactSetLayoutStorage};
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 6};
    if (!m_blockIdCompactPipelineLayoutDenseBitsetStorage) {
      m_blockIdCompactPipelineLayoutDenseBitsetStorage.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                     .pSetLayouts = setLayouts.data(),
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc});
    }
    loadComputePipeline(shaderBase +
                          std::string(blockIdCompactionShaderFile(VulkanBlockIdCompactionMethod::DenseBitsetReadback)),
                        **m_blockIdCompactPipelineLayoutDenseBitsetStorage,
                        m_blockIdCompactPipelineDenseBitsetStorage);
    if (VLOG_IS_ON(1)) {
      VLOG(1) << "ensureBlockIdCompactionPipeline: method=dense_bitset_readback";
    }
  };

  auto ensureDenseBitsetFlagsStorage = [&]() {
    if (m_blockIdCompactPipelineDenseBitsetFlagsStorage) {
      return;
    }

    if (!m_blockIdCompactSetLayoutStorage) {
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
      m_blockIdCompactSetLayoutStorage.emplace(
        device,
        vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()});
    }
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{bindlessLayout, **m_blockIdCompactSetLayoutStorage};
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 6};
    if (!m_blockIdCompactPipelineLayoutDenseBitsetStorage) {
      m_blockIdCompactPipelineLayoutDenseBitsetStorage.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                     .pSetLayouts = setLayouts.data(),
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc});
    }
    loadComputePipeline(
      shaderBase + std::string(blockIdCompactionShaderFile(VulkanBlockIdCompactionMethod::DenseBitsetFlagsReadback)),
      **m_blockIdCompactPipelineLayoutDenseBitsetStorage,
      m_blockIdCompactPipelineDenseBitsetFlagsStorage);
    if (VLOG_IS_ON(1)) {
      VLOG(1) << "ensureBlockIdCompactionPipeline: method=dense_bitset_flags_readback";
    }
  };

  auto ensureAppendStorageGpuUnique = [&]() {
    if (m_blockIdCompactPipelineAppendStorageGpuUniqueMark && m_blockIdCompactPipelineAppendGpuUniqueEmit) {
      return;
    }

    if (!m_blockIdCompactSetLayoutStorage) {
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
      m_blockIdCompactSetLayoutStorage.emplace(
        device,
        vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()});
    }
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{bindlessLayout, **m_blockIdCompactSetLayoutStorage};
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                             .offset = 0,
                             .size = sizeof(uint32_t) * 6};
    if (!m_blockIdCompactPipelineLayoutDenseBitsetStorage) {
      m_blockIdCompactPipelineLayoutDenseBitsetStorage.emplace(
        device,
        vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
                                     .pSetLayouts = setLayouts.data(),
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc});
    }
    if (!m_blockIdCompactPipelineAppendStorageGpuUniqueMark) {
      loadComputePipeline(shaderBase + std::string(blockIdCompactionShaderFile(
                                         VulkanBlockIdCompactionMethod::AppendStorageParallelFlushGpuUnique)),
                          **m_blockIdCompactPipelineLayoutDenseBitsetStorage,
                          m_blockIdCompactPipelineAppendStorageGpuUniqueMark);
    }
    if (!m_blockIdCompactPipelineAppendGpuUniqueEmit) {
      loadComputePipeline(shaderBase + "block_id_compact_append_gpu_unique_emit.comp.spv",
                          **m_blockIdCompactPipelineLayoutDenseBitsetStorage,
                          m_blockIdCompactPipelineAppendGpuUniqueEmit);
    }
    if (VLOG_IS_ON(1)) {
      VLOG(1) << "ensureBlockIdCompactionPipeline: method=append_storage_parallel_flush_gpu_unique";
    }
  };

  const std::vector<VulkanBlockIdCompactionMethod> methods =
    blockIdCompactionMethodsToRecord(vkBlockIdCompactionMethod());
  for (const VulkanBlockIdCompactionMethod method : methods) {
    switch (method) {
      case VulkanBlockIdCompactionMethod::AppendStorageParallelFlush:
        ensureStorageParallelFlush();
        break;
      case VulkanBlockIdCompactionMethod::AppendStorageParallelFlushGpuUnique:
        ensureAppendStorageGpuUnique();
        break;
      case VulkanBlockIdCompactionMethod::AppendSampledParallelFlush:
        ensureSampledParallelFlush();
        break;
      case VulkanBlockIdCompactionMethod::DenseBitsetReadback:
        ensureDenseBitsetStorage();
        break;
      case VulkanBlockIdCompactionMethod::DenseBitsetFlagsReadback:
        ensureDenseBitsetFlagsStorage();
        break;
    }
  }
}

// (probe pipelines removed)

ZVulkanImgRaycasterPipelineContext::BlockIdCompactionOutput&
ZVulkanImgRaycasterPipelineContext::ensureBlockIdCompactOutput(VulkanBlockIdCompactionMethod method, size_t bytes)
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

  auto& out = m_blockIdCompactOutputs[key][method];
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
  maybeCancel(cancellationToken);
  (void)batch;
  if (!payload.blockIdLease || !payload.blockIdLease->hasVulkanImage()) {
    return;
  }
  // Ensure pipeline and output buffer
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
  ensureBlockIdCompactionPipeline();
  uint32_t imgW = firstBlock->width();
  uint32_t imgH = firstBlock->height();
  if (imgW == 0u || imgH == 0u) {
    VLOG(1) << fmt::format("BlockID compaction skipped: empty attachment size {}x{}", imgW, imgH);
    return;
  }
  CHECK(payload.image != nullptr);
  const VulkanBlockIdCompactionMethod productionMethod = vkBlockIdCompactionMethod();
  const bool benchmarkCompaction = benchmarkVkBlockIdCompaction();
  const std::vector<VulkanBlockIdCompactionMethod> methods = blockIdCompactionMethodsToRecord(productionMethod);
  const uint32_t capacityIDs = std::max<uint32_t>(1u, imgW * imgH * 4u);
  const uint32_t maxBlockId = payload.image->maxPagedBlockID();
  const size_t bitsetWordCount = denseBitsetWordCount(maxBlockId);
  CHECK_LE(bitsetWordCount + kBlockIdCompactionHeaderWords, static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    << "Block-ID bitset output offsets exceed uint32 push-constant range";
  const size_t headerBytes = static_cast<size_t>(kBlockIdCompactionHeaderWords) * sizeof(uint32_t);

  struct RecordedCompaction
  {
    VulkanBlockIdCompactionMethod method = VulkanBlockIdCompactionMethod::AppendSampledParallelFlush;
    std::shared_ptr<ZVulkanBuffer> output;
    size_t outputBytes = 0;
    size_t payloadWords = 0;
    size_t bitsetWords = 0;
    uint32_t payloadReadbackOffsetWords = kBlockIdCompactionHeaderWords;
    uint32_t payloadReadbackCapacityWords = 0;
  };
  std::vector<RecordedCompaction> recordedCompactions;
  recordedCompactions.reserve(methods.size());

  ZVulkanPipelineCommandRecorder recorder(cmd);
  const vk::DescriptorSet bindlessSet = m_backend.bindlessSampledImageDescriptorSet();

  for (const VulkanBlockIdCompactionMethod method : methods) {
    const bool denseReadback = blockIdCompactionMethodReadsBackDenseBitset(method);
    const bool appendGpuUnique = blockIdCompactionMethodUsesAppendGpuUnique(method);
    const bool bitsetPayload = denseReadback || appendGpuUnique;
    const bool storageRead = blockIdCompactionMethodUsesStorage(method);
    const bool sampledRead = blockIdCompactionMethodUsesSampled(method);
    const size_t payloadWords = appendGpuUnique ? bitsetWordCount + static_cast<size_t>(maxBlockId)
                                                : (denseReadback ? bitsetWordCount : static_cast<size_t>(capacityIDs));
    const uint32_t payloadReadbackOffsetWords =
      appendGpuUnique ? static_cast<uint32_t>(kBlockIdCompactionHeaderWords + bitsetWordCount)
                      : kBlockIdCompactionHeaderWords;
    const uint32_t payloadReadbackCapacityWords = appendGpuUnique ? maxBlockId : static_cast<uint32_t>(payloadWords);
    const size_t outputBytes = (static_cast<size_t>(kBlockIdCompactionHeaderWords) + payloadWords) * sizeof(uint32_t);
    auto& out = ensureBlockIdCompactOutput(method, outputBytes);
    CHECK(out.buffer != nullptr);
    auto compactOutput = out.buffer;
    CHECK((compactOutput->usage() & vk::BufferUsageFlagBits::eTransferDst) != vk::BufferUsageFlags{})
      << "Block-ID compaction output must support vkCmdFillBuffer (eTransferDst usage missing)";
    const size_t clearBytes = bitsetPayload ? (headerBytes + bitsetWordCount * sizeof(uint32_t)) : headerBytes;
    cmd.fillBuffer(compactOutput->buffer(), /*dstOffset=*/0, clearBytes, /*data=*/0u);
    {
      vk::BufferMemoryBarrier2 bb{};
      bb.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
      bb.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
      bb.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
      bb.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
      bb.buffer = compactOutput->buffer();
      bb.offset = 0;
      bb.size = clearBytes;
      vk::DependencyInfo dep{};
      dep.bufferMemoryBarrierCount = 1;
      dep.pBufferMemoryBarriers = &bb;
      cmd.pipelineBarrier2(dep);
    }

    ZVulkanComputePassSpec spec{};
    if (method == VulkanBlockIdCompactionMethod::AppendStorageParallelFlush) {
      spec.pipeline = &*m_blockIdCompactPipelineStorageParallelFlush;
      spec.pipelineLayout = &*m_blockIdCompactPipelineLayoutStorage;
    } else if (method == VulkanBlockIdCompactionMethod::AppendStorageParallelFlushGpuUnique) {
      spec.pipeline = &*m_blockIdCompactPipelineAppendStorageGpuUniqueMark;
      spec.pipelineLayout = &*m_blockIdCompactPipelineLayoutDenseBitsetStorage;
    } else if (method == VulkanBlockIdCompactionMethod::AppendSampledParallelFlush) {
      spec.pipeline = &*m_blockIdCompactPipelineSampledParallelFlush;
      spec.pipelineLayout = &*m_blockIdCompactPipelineLayoutSampled;
    } else if (method == VulkanBlockIdCompactionMethod::DenseBitsetReadback) {
      spec.pipeline = &*m_blockIdCompactPipelineDenseBitsetStorage;
      spec.pipelineLayout = &*m_blockIdCompactPipelineLayoutDenseBitsetStorage;
    } else if (method == VulkanBlockIdCompactionMethod::DenseBitsetFlagsReadback) {
      spec.pipeline = &*m_blockIdCompactPipelineDenseBitsetFlagsStorage;
      spec.pipelineLayout = &*m_blockIdCompactPipelineLayoutDenseBitsetStorage;
    }
    spec.descriptorSetFirst = 0;
    spec.expectedDescriptorSetCount = 2;
    spec.groupX = (imgW + 15) / 16;
    spec.groupY = (imgH + 15) / 16;
    spec.groupZ = 1;

    ZVulkanDescriptorSet* sampledDescriptor = nullptr;
    const BlockIdStorageDescriptorPack* storagePack = nullptr;
    if (storageRead) {
      const auto& storageByMethod = denseReadback ? m_blockIdCompactDescriptorDenseStorageByMethodAndLease
                                                  : m_blockIdCompactDescriptorStorageByMethodAndLease;
      auto methodStorageIt = storageByMethod.find(method);
      CHECK(methodStorageIt != storageByMethod.end())
        << "Raycaster block-ID storage compaction requires pre-record primed descriptor sets for method="
        << blockIdCompactionMethodName(method);
      const auto& storageMap = methodStorageIt->second;
      auto it = storageMap.find(payload.blockIdLease.get());
      CHECK(it != storageMap.end())
        << "Raycaster block-ID storage compaction requires pre-record primed descriptor sets";
      storagePack = &it->second;
      CHECK(storagePack->perAttachment.size() >= effectiveAttachmentCount)
        << "Raycaster block-ID storage compaction descriptor pack too small: have=" << storagePack->perAttachment.size()
        << " need=" << effectiveAttachmentCount;
    } else {
      CHECK(sampledRead);
      auto sampledIt = m_blockIdCompactDescriptorSampledByMethod.find(method);
      CHECK(sampledIt != m_blockIdCompactDescriptorSampledByMethod.end())
        << "Raycaster block-ID sampled compaction requires pre-record primed descriptor set for method="
        << blockIdCompactionMethodName(method);
      sampledDescriptor = sampledIt->second.get();
      CHECK(sampledDescriptor != nullptr)
        << "Raycaster block-ID compaction requires pre-record primed sampled descriptor set";
    }

    auto gpuScope = m_backend.beginGpuScope(fmt::format("block_id_compact_{}", blockIdCompactionMethodName(method)));
    for (uint32_t att = 0; att < effectiveAttachmentCount; ++att) {
      maybeCancel(cancellationToken);
      ZVulkanTexture* blockTex = payload.blockIdLease->colorAttachment(att);
      if (!blockTex) {
        continue;
      }
      if (blockTex->width() != imgW || blockTex->height() != imgH) {
        LOG(WARNING) << "Block-ID attachment size mismatch; skipping att=" << att;
        continue;
      }

      if (storageRead) {
        if (blockTex->layout() != vk::ImageLayout::eGeneral) {
          blockTex->transitionLayout(cmd,
                                     blockTex->layout(),
                                     vk::ImageLayout::eGeneral,
                                     vk::ImageAspectFlagBits::eColor);
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
        if (blockTex->layout() != vk::ImageLayout::eShaderReadOnlyOptimal) {
          blockTex->transitionLayout(cmd,
                                     blockTex->layout(),
                                     vk::ImageLayout::eShaderReadOnlyOptimal,
                                     vk::ImageAspectFlagBits::eColor);
        }
        blockTex->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
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

      ZVulkanDescriptorSet* ds = nullptr;
      if (storageRead) {
        CHECK(storagePack != nullptr);
        ds = storagePack->perAttachment[att].get();
      } else {
        ds = sampledDescriptor;
      }
      CHECK(ds != nullptr) << "Block-ID compaction missing descriptor set for method="
                           << blockIdCompactionMethodName(method) << " att=" << att;

      const std::array<vk::DescriptorSet, 2> descriptorSets{bindlessSet, ds->descriptorSet()};
      spec.descriptorSets = descriptorSets;
      uint32_t payloadLimitWords = capacityIDs;
      if (bitsetPayload) {
        CHECK_LE(bitsetWordCount, static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
        payloadLimitWords = static_cast<uint32_t>(bitsetWordCount);
      }
      std::array<uint32_t, 6> pcWords{imgW, imgH, imgW, payloadLimitWords, att, 0u};
      if (bitsetPayload) {
        pcWords[5] = maxBlockId;
        spec.pushConstantsSize = sizeof(uint32_t) * 6u;
      } else if (sampledRead) {
        pcWords[5] = m_backend.bindlessLookupSampledImageAutoOrCrash(*blockTex, "ray_blockid_compact_input");
        spec.pushConstantsSize = sizeof(uint32_t) * 6u;
      } else {
        spec.pushConstantsSize = sizeof(uint32_t) * 5u;
      }
      spec.pushConstantsData = pcWords.data();
      spec.pushConstantsStages = vk::ShaderStageFlagBits::eCompute;
      recorder.recordComputePass(spec);
    }

    if (appendGpuUnique) {
      CHECK(storagePack != nullptr);
      CHECK(!storagePack->perAttachment.empty() && storagePack->perAttachment.front() != nullptr)
        << "Append GPU-unique compaction requires at least one storage descriptor set";
      {
        vk::BufferMemoryBarrier2 bb{};
        bb.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        bb.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
        bb.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
        bb.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
        bb.buffer = compactOutput->buffer();
        bb.offset = 0;
        bb.size = sizeof(uint32_t);
        vk::DependencyInfo dep{};
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &bb;
        cmd.pipelineBarrier2(dep);
      }
      cmd.fillBuffer(compactOutput->buffer(), /*dstOffset=*/0, sizeof(uint32_t), /*data=*/0u);
      {
        vk::BufferMemoryBarrier2 bb{};
        bb.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
        bb.srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
        bb.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        bb.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
        bb.buffer = compactOutput->buffer();
        bb.offset = 0;
        bb.size = outputBytes;
        vk::DependencyInfo dep{};
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &bb;
        cmd.pipelineBarrier2(dep);
      }

      ZVulkanComputePassSpec emitSpec{};
      emitSpec.pipeline = &*m_blockIdCompactPipelineAppendGpuUniqueEmit;
      emitSpec.pipelineLayout = &*m_blockIdCompactPipelineLayoutDenseBitsetStorage;
      emitSpec.descriptorSetFirst = 0;
      emitSpec.expectedDescriptorSetCount = 2;
      const std::array<vk::DescriptorSet, 2> descriptorSets{bindlessSet,
                                                            storagePack->perAttachment.front()->descriptorSet()};
      emitSpec.descriptorSets = descriptorSets;
      emitSpec.groupX = static_cast<uint32_t>((bitsetWordCount + 255u) / 256u);
      emitSpec.groupY = 1;
      emitSpec.groupZ = 1;
      CHECK_LE(bitsetWordCount, static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
      std::array<uint32_t, 4> emitPcWords{static_cast<uint32_t>(bitsetWordCount),
                                          maxBlockId,
                                          static_cast<uint32_t>(bitsetWordCount),
                                          maxBlockId};
      emitSpec.pushConstantsData = emitPcWords.data();
      emitSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(uint32_t) * emitPcWords.size());
      emitSpec.pushConstantsStages = vk::ShaderStageFlagBits::eCompute;
      recorder.recordComputePass(emitSpec);
    }

    if (gpuScope) {
      m_backend.endGpuScope(*gpuScope);
    }

    recordedCompactions.push_back(RecordedCompaction{.method = method,
                                                     .output = compactOutput,
                                                     .outputBytes = outputBytes,
                                                     .payloadWords = payloadWords,
                                                     .bitsetWords = bitsetWordCount,
                                                     .payloadReadbackOffsetWords = payloadReadbackOffsetWords,
                                                     .payloadReadbackCapacityWords = payloadReadbackCapacityWords});
  }
  CHECK(!recordedCompactions.empty()) << "No Vulkan block-ID compaction methods were recorded";

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
  // Block-ID compaction is a CPU control-flow boundary: the cache upload it
  // triggers is a hard dependency for the next progressive stage. Force the
  // backend to synchronously wait until the frame completion safe point so the
  // hook below runs before we return to client code.
  m_backend.requireCompletionSafePointWaitForActiveSubmission("block_id_compact_readback");

  std::vector<Z3DRendererVulkanBackend::EndOfFrameBufferReadbackTicket> headerTickets;
  headerTickets.reserve(recordedCompactions.size());
  for (const auto& recorded : recordedCompactions) {
    CHECK(recorded.output != nullptr) << "Recorded block-ID compaction missing output buffer";
    CHECK((recorded.output->usage() & vk::BufferUsageFlagBits::eTransferSrc) != vk::BufferUsageFlags{})
      << "Block-ID compaction output must be readable via end-of-frame buffer readback (eTransferSrc usage missing)";
    CHECK_LE(headerBytes, recorded.outputBytes) << "Block-ID compaction header exceeds output buffer size";
    headerTickets.emplace_back(m_backend.requestEndOfFrameBufferReadbackTicket(*recorded.output,
                                                                               /*srcOffset=*/0,
                                                                               headerBytes,
                                                                               "block_id_compact_header_readback"));
  }

  const std::string_view debugLabel = "VK raycaster compaction output parse";
  m_backend.registerAfterCurrentFrameCompletionHook(
    currentRenderThreadExecutorKeepAlive(debugLabel),
    [this,
     rendererPtr = &renderer,
     tickets = std::move(headerTickets),
     recordedCompactions = std::move(recordedCompactions),
     productionMethod,
     benchmarkCompaction,
     cancellationToken,
     imgW,
     imgH,
     maxBlockId,
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
        for (auto& ticket : tickets) {
          co_await ticket.awaitAndDiscard();
        }
        co_return;
      }

      CHECK_GT(imgW, 0u);
      CHECK_GT(imgH, 0u);
      CHECK(imagePtr != nullptr);
      CHECK_LE(attCount, 8u) << "Unified header supports up to 8 attachments";
      CHECK_EQ(tickets.size(), recordedCompactions.size()) << "Compaction ticket/output count mismatch";

      struct ParsedCompaction
      {
        VulkanBlockIdCompactionMethod method = VulkanBlockIdCompactionMethod::AppendSampledParallelFlush;
        uint32_t rawCount = 0;
        size_t payloadReadbackBytes = 0;
        double payloadReadbackMs = 0.0;
        double cpuParseMs = 0.0;
        std::array<uint32_t, 8> counts{};
        std::vector<uint32_t> blockIds;
      };
      std::vector<ParsedCompaction> parsed;
      parsed.reserve(recordedCompactions.size());

      for (size_t i = 0; i < recordedCompactions.size(); ++i) {
        if (cancellationToken.isCancellationRequested()) {
          for (; i < tickets.size(); ++i) {
            co_await tickets[i].awaitAndDiscard();
          }
          co_return;
        }

        const auto& recorded = recordedCompactions[i];
        std::array<uint32_t, kBlockIdCompactionHeaderWords> headerWords{};
        co_await tickets[i].awaitCopyTo(headerWords.data(), headerWords.size() * sizeof(uint32_t));
        if (cancellationToken.isCancellationRequested()) {
          for (size_t j = i + 1; j < tickets.size(); ++j) {
            co_await tickets[j].awaitAndDiscard();
          }
          co_return;
        }

        ParsedCompaction result{};
        result.method = recorded.method;
        const uint32_t headerCount = headerWords[0];
        for (uint32_t att = 0; att < attCount; ++att) {
          result.counts[att] = headerWords[1 + att];
        }
        if (blockIdCompactionMethodUsesAppendGpuUnique(recorded.method)) {
          CHECK_EQ(result.counts[0] & kDenseBitsetInvalidBlockIdFlag, 0u)
            << "Append GPU-unique compaction saw block ID outside maxPagedBlockID";
          result.counts[0] &= ~kDenseBitsetInvalidBlockIdFlag;
          uint64_t rawSum = 0;
          for (uint32_t att = 0; att < attCount; ++att) {
            rawSum += result.counts[att];
          }
          CHECK_LE(rawSum, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
            << "Append GPU-unique raw count exceeds uint32 range";
          result.rawCount = static_cast<uint32_t>(rawSum);
        } else {
          result.rawCount = headerCount;
        }

        auto cpuParseStart = std::chrono::steady_clock::now();
        if (blockIdCompactionMethodReadsBackDenseBitset(recorded.method)) {
          CHECK_EQ(result.rawCount & kDenseBitsetInvalidBlockIdFlag, 0u)
            << "Dense bitset compaction saw block ID outside maxPagedBlockID";
          result.rawCount &= ~kDenseBitsetInvalidBlockIdFlag;
          const size_t bitsetBytes = recorded.bitsetWords * sizeof(uint32_t);
          if (result.rawCount > 0u && bitsetBytes > 0u) {
            const auto payloadReadbackStart = std::chrono::steady_clock::now();
            auto payloadBytes = m_backend.readBufferRangeAfterCompletion(
              *recorded.output,
              static_cast<vk::DeviceSize>(recorded.payloadReadbackOffsetWords * sizeof(uint32_t)),
              bitsetBytes,
              "block_id_compact_dense_bitset_readback");
            result.payloadReadbackMs +=
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - payloadReadbackStart)
                .count();
            CHECK_EQ(payloadBytes.size(), bitsetBytes) << "Dense bitset compaction payload readback size mismatch";
            result.payloadReadbackBytes = bitsetBytes;
            cpuParseStart = std::chrono::steady_clock::now();
            std::vector<uint32_t> bitWords(recorded.bitsetWords);
            std::memcpy(bitWords.data(), payloadBytes.data(), bitsetBytes);
            result.blockIds.reserve(std::min<size_t>(result.rawCount, maxBlockId));
            for (size_t wordIndex = 0; wordIndex < bitWords.size(); ++wordIndex) {
              uint32_t word = bitWords[wordIndex];
              while (word != 0u) {
                const uint32_t bit = static_cast<uint32_t>(std::countr_zero(word));
                const uint32_t id = static_cast<uint32_t>(wordIndex * 32u + bit);
                if (id != 0u && id <= maxBlockId) {
                  result.blockIds.push_back(id);
                }
                word &= (word - 1u);
              }
            }
          }
        } else {
          const bool appendGpuUnique = blockIdCompactionMethodUsesAppendGpuUnique(recorded.method);
          const uint32_t count = appendGpuUnique ? headerCount : result.rawCount;
          CHECK_LE(count, recorded.payloadReadbackCapacityWords)
            << fmt::format("Block-ID compaction count exceeds capacity (would truncate): {}x{} cap={} count={}",
                           imgW,
                           imgH,
                           recorded.payloadReadbackCapacityWords,
                           count);
          std::vector<uint32_t> idWords;
          if (count > 0u) {
            const size_t idBytes = static_cast<size_t>(count) * sizeof(uint32_t);
            const auto payloadReadbackStart = std::chrono::steady_clock::now();
            auto payloadBytes = m_backend.readBufferRangeAfterCompletion(
              *recorded.output,
              static_cast<vk::DeviceSize>(recorded.payloadReadbackOffsetWords * sizeof(uint32_t)),
              idBytes,
              "block_id_compact_payload_readback");
            result.payloadReadbackMs +=
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - payloadReadbackStart)
                .count();
            CHECK_EQ(payloadBytes.size(), idBytes) << "Block-ID compaction payload readback size mismatch";
            result.payloadReadbackBytes = idBytes;
            cpuParseStart = std::chrono::steady_clock::now();
            idWords.resize(count);
            std::memcpy(idWords.data(), payloadBytes.data(), idBytes);
          }
          result.blockIds.reserve(count);
          if (appendGpuUnique) {
            for (uint32_t j = 0; j < count; ++j) {
              const uint32_t v = idWords[j];
              if (v != kEmptyBlockID && v != 0u) {
                CHECK_LE(v, maxBlockId) << "Append GPU-unique compaction emitted ID outside maxPagedBlockID";
                result.blockIds.push_back(v);
              }
            }
          } else {
            std::vector<uint32_t> seen(denseBitsetWordCount(maxBlockId), 0u);
            for (uint32_t j = 0; j < count; ++j) {
              const uint32_t v = idWords[j];
              if (v != kEmptyBlockID && v != 0u) {
                CHECK_LE(v, maxBlockId) << "Append compaction emitted ID outside maxPagedBlockID";
                uint32_t& word = seen[v >> 5u];
                const uint32_t mask = 1u << (v & 31u);
                if ((word & mask) == 0u) {
                  word |= mask;
                  result.blockIds.push_back(v);
                }
              }
            }
          }
        }

        const size_t cacheCapacity = imagePtr->numCachedImages(channelIndex);
        const bool hasEnoughMissingIDs = result.blockIds.size() > cacheCapacity;
        if ((roundIndex % 2u == 1u) && hasEnoughMissingIDs) {
          std::ranges::sort(result.blockIds, std::ranges::greater{});
        } else {
          std::ranges::sort(result.blockIds);
        }
        result.cpuParseMs =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuParseStart).count();
        std::string countsText;
        for (uint32_t att = 0; att < attCount; ++att) {
          if (!countsText.empty()) {
            countsText += ',';
          }
          countsText += fmt::format("{}", result.counts[att]);
        }
        const std::string parseMessage =
          fmt::format("VK Block-ID compaction parsed method={} raw={} unique={} payload_readback={}B "
                      "payload_readback_ms={:.3f} cpu_parse_ms={:.3f} counts=[{}]",
                      blockIdCompactionMethodName(result.method),
                      result.rawCount,
                      result.blockIds.size(),
                      result.payloadReadbackBytes,
                      result.payloadReadbackMs,
                      result.cpuParseMs,
                      countsText);
        if (benchmarkCompaction) {
          LOG(INFO) << parseMessage;
        } else {
          VLOG(1) << parseMessage;
        }
        parsed.push_back(std::move(result));
      }

      CHECK(!parsed.empty()) << "No parsed Vulkan block-ID compaction outputs";
      auto productionIt = std::ranges::find_if(parsed, [&](const ParsedCompaction& result) {
        return result.method == productionMethod;
      });
      CHECK(productionIt != parsed.end()) << "Production Vulkan block-ID compaction output was not parsed";
      for (const auto& result : parsed) {
        if (result.method == productionMethod) {
          continue;
        }
        CHECK(result.blockIds == productionIt->blockIds)
          << "Vulkan block-ID compaction benchmark mismatch: production="
          << blockIdCompactionMethodName(productionMethod)
          << " candidate=" << blockIdCompactionMethodName(result.method)
          << " production_unique=" << productionIt->blockIds.size() << " candidate_unique=" << result.blockIds.size();
      }

      std::vector<uint32_t> missingBlocks = productionIt->blockIds;
      const std::array<uint32_t, 8> counts = productionIt->counts;

      const size_t cacheCapacity = imagePtr->numCachedImages(channelIndex);
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

  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Block-ID pipeline requires backend bindless descriptor set layout";
  const vk::DescriptorSetLayout raySetupLayout = m_backend.imgRaySetupDescriptorSetLayout();
  CHECK(raySetupLayout) << "Block-ID pipeline requires backend ray-setup descriptor set layout";
  const vk::DescriptorSetLayout pageDataLayout = m_backend.imgPageDataDescriptorSetLayout();
  CHECK(pageDataLayout) << "Block-ID pipeline requires backend page-data descriptor set layout";

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
  instance.pipeline->setDescriptorSetLayouts({bindlessLayout, raySetupLayout, pageDataLayout});
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

  // Push constants for ray params + bindless texture indices.
  // Must be configured before create() so pipeline layout contains the range.
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(RaycasterProgressivePushConstants))};
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

  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Progressive raycaster pipeline requires backend bindless descriptor set layout";
  const vk::DescriptorSetLayout raySetupLayout = m_backend.imgRaySetupDescriptorSetLayout();
  CHECK(raySetupLayout) << "Progressive raycaster pipeline requires backend ray-setup descriptor set layout";
  const vk::DescriptorSetLayout pageDataLayout = m_backend.imgPageDataDescriptorSetLayout();
  CHECK(pageDataLayout) << "Progressive raycaster pipeline requires backend page-data descriptor set layout";

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
  instance.pipeline->setDescriptorSetLayouts({bindlessLayout, raySetupLayout, pageDataLayout});
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

  // Fragment push constants: ray params + bindless texture indices.
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(RaycasterProgressivePushConstants))};
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

  if (absl::GetFlag(FLAGS_atlas_vk_debug_raycaster_dump)) {
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

  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Copy pipeline requires backend bindless descriptor set layout";

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
  instance.pipeline->setDescriptorSetLayouts({bindlessLayout});
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

  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(RaycasterCopyMergePushConstants))};
  instance.pipeline->setPushConstantRanges({pcRange});

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

  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Merge pipeline requires backend bindless descriptor set layout";

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
  instance.pipeline->setDescriptorSetLayouts({bindlessLayout});
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
  // 70: NUM_VOLUMES, 71: MAX_PROJ_MERGE
  std::array<vk::SpecializationMapEntry, 2> entries{
    vk::SpecializationMapEntry{.constantID = 70, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 71, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };
  std::array<uint32_t, 2> values{static_cast<uint32_t>(std::max(1, key.numVolumes)), key.maxProjectionMerge ? 1u : 0u};
  std::vector<uint8_t> data(sizeof(values));
  std::memcpy(data.data(), values.data(), sizeof(values));
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                              std::vector(entries.begin(), entries.end()),
                                              data);

  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(RaycasterCopyMergePushConstants))};
  instance.pipeline->setPushConstantRanges({pcRange});

  instance.pipeline->create();
  VLOG(2) << fmt::format(
    "Raycaster Merge Pipeline: depthTest=0 depthWrite=0 colorFmt0={} depthFmt={} volumes={} maxProj={}",
    formats.colorFormats.empty() ? -1 : static_cast<int>(formats.colorFormats.front()),
    formats.depthFormat ? static_cast<int>(*formats.depthFormat) : -1,
    key.numVolumes,
    key.maxProjectionMerge);
  VLOG(1) << "Merge pipeline created: depthTest=1 depthWrite=1 compareOp="
          << static_cast<int>(vk::CompareOp::eLessOrEqual)
          << " depthFmt=" << (formats.depthFormat ? static_cast<int>(*formats.depthFormat) : -1)
          << " color0Fmt=" << (formats.colorFormats.empty() ? -1 : static_cast<int>(formats.colorFormats.front()))
          << " [spec] volumes=" << key.numVolumes << " maxProj=" << (key.maxProjectionMerge ? 1 : 0);

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

ZVulkanImgRaycasterPipelineContext::ChannelResources&
ZVulkanImgRaycasterPipelineContext::ensureChannelResources(size_t channelIndex)
{
  if (channelIndex >= m_channelResources.size()) {
    m_channelResources.resize(channelIndex + 1);
  }
  return m_channelResources[channelIndex];
}

ZVulkanTexture& ZVulkanImgRaycasterPipelineContext::ensureVolumeTexture(Z3DImg& owner,
                                                                        size_t channelIndex,
                                                                        uint64_t generation,
                                                                        std::shared_ptr<const ZImg> image)
{
  CHECK(channelIndex <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    << "Raycaster channel index exceeds Vulkan residency key range";
  ZVulkanTexture* texture =
    m_backend.device().residencyManager().denseVolumeTexture(owner,
                                                             static_cast<uint32_t>(channelIndex),
                                                             std::move(image),
                                                             generation);
  CHECK(texture != nullptr) << "Raycaster: managed 3D volume texture missing";
  m_backend.pinTextureForActiveSubmission(texture);
  return *texture;
}

ZVulkanTexture& ZVulkanImgRaycasterPipelineContext::ensureImage2DTexture(Z3DImg& owner,
                                                                         size_t channelIndex,
                                                                         uint64_t generation,
                                                                         std::shared_ptr<const ZImg> image)
{
  CHECK(channelIndex <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    << "Raycaster 2D channel index exceeds Vulkan residency key range";
  ZVulkanTexture* texture =
    m_backend.device().residencyManager().denseImage2DTexture(owner,
                                                              static_cast<uint32_t>(channelIndex),
                                                              std::move(image),
                                                              generation);
  CHECK(texture != nullptr) << "Raycaster: managed 2D image texture missing";
  m_backend.pinTextureForActiveSubmission(texture);
  return *texture;
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

ZVulkanImgRaycasterPipelineContext::PipelineInstance&
ZVulkanImgRaycasterPipelineContext::ensureFastPipeline(const FastPipelineKey& key)
{
  auto it = m_fastPipelines.find(key);
  if (it != m_fastPipelines.end()) {
    return it->second;
  }

  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Fast pipeline requires backend bindless descriptor set layout";
  const vk::DescriptorSetLayout indicesLayout = m_backend.imgIndicesDescriptorSetLayout();
  CHECK(indicesLayout) << "Fast pipeline requires backend indices descriptor set layout";
  const vk::DescriptorSetLayout raySetupLayout = m_backend.imgRaySetupDescriptorSetLayout();
  CHECK(raySetupLayout) << "Fast pipeline requires backend ray-setup descriptor set layout";

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;

  auto setCommonState = [&](vk::PipelineVertexInputStateCreateInfo& vertexInput,
                            std::span<vk::PipelineColorBlendAttachmentState const> blends,
                            std::span<vk::PushConstantRange const> pushConstants,
                            const std::vector<vk::Format>& colorFormats,
                            const std::optional<vk::Format>& depthFormat,
                            std::initializer_list<vk::DescriptorSetLayout> layouts,
                            vk::PrimitiveTopology topology,
                            bool depthEnabled) {
    instance.pipeline = device.createPipeline(*instance.shader, vertexInput, topology);
    if (!layouts.size()) {
      instance.pipeline->setDescriptorSetLayouts({});
    } else {
      instance.pipeline->setDescriptorSetLayouts(std::vector<vk::DescriptorSetLayout>(layouts.begin(), layouts.end()));
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
                                    .size = static_cast<uint32_t>(sizeof(RaycasterFastVolumePushConstants))};

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
                     {bindlessLayout, raySetupLayout},
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
                     {bindlessLayout, indicesLayout},
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
                     {bindlessLayout, indicesLayout},
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
