#include "zvulkanimgraycasterpipelinecontext.h"

#include "z3dimg.h"
#include "z3drendererbase.h"
#include "z3drendererstates.h"
#include "z3dtransferfunction.h"
#include "z3drenderglobalstate.h"
#include "z3dscratchresourcepool.h"
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
#include "zvulkanuniforms.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zsysteminfo.h"
#include <fstream>
#include "z3drenderervulkanbackend.h"
#include "zvulkanpagedimageblockuploader.h"
#include "zcancellation.h"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>
#include <utility>
#include <cstring>

// Forward declaration for local SPIR-V loader
namespace {
std::vector<uint32_t> readSpirvFile(const std::string& path);
}

namespace nim {

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

namespace {

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

constexpr uint32_t kMaxPagingLevels = 16u;

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

std::vector<uint8_t>
buildPageDataBuffer(const Z3DImg& image, size_t channel, float zeToScreenPixelVoxelSize, uint32_t levelCount)
{
  levelCount = std::min<uint32_t>(levelCount, kMaxPagingLevels);

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

  // Fixed-size fields first
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "buildPageDataBuffer: append pageTableBlockSize";
  }
  appendUvec3(data, image.pageTableBlockSize());

  if (VLOG_IS_ON(2)) {
    VLOG(2) << "buildPageDataBuffer: append imageBlockSize";
  }
  appendUvec3(data, image.imageBlockSize());

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
    appendUvec3(data, pageDirectoryBases[level]);
    appendUvec3(data, imageDimensions[level]);
    appendUvec3(data, posToBlockIDs[level]);
    appendScalar(data, voxelWorldSizes[level]);
    if (FLAGS_atlas_vk_debug_raycaster_dump) {
      const int maxLevels = std::max(0, FLAGS_atlas_vk_debug_raycaster_dump_levels);
      if (static_cast<int>(level) < maxLevels) {
        VLOG(1) << fmt::format(
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

  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("buildPageDataBuffer: end size={} bytes", data.size());
  }
  if (FLAGS_atlas_vk_debug_raycaster_dump) {
    const auto ptb = image.pageTableBlockSize();
    const auto ibs = image.imageBlockSize();
    VLOG(1) << fmt::format(
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

// Depth-only layouts/aspects (stencil not used in this pipeline)
std::pair<vk::ImageLayout, vk::ImageAspectFlags> depthReadDescriptorLayoutAndAspect(const ZVulkanTexture& /*texture*/)
{
  return {vk::ImageLayout::eDepthReadOnlyOptimal, vk::ImageAspectFlagBits::eDepth};
}

std::pair<vk::ImageLayout, vk::ImageAspectFlags> depthAttachmentLayoutAndAspect(const ZVulkanTexture& /*texture*/)
{
  return {vk::ImageLayout::eDepthAttachmentOptimal, vk::ImageAspectFlagBits::eDepth};
}

vk::ImageAspectFlags depthReadBarrierAspect(const ZVulkanTexture& /*texture*/)
{
  return vk::ImageAspectFlagBits::eDepth;
}

} // namespace

ZVulkanImgRaycasterPipelineContext::ZVulkanImgRaycasterPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
  , m_imageBlockUploader(std::make_unique<ZVulkanPagedImageBlockUploader>(backend.device()))
{}

ZVulkanImgRaycasterPipelineContext::~ZVulkanImgRaycasterPipelineContext() = default;

void ZVulkanImgRaycasterPipelineContext::resetFrame()
{
  resetDescriptors();
  m_depthClearedThisFrame.clear();
  // Drop any carry-over progressive bookkeeping so a cancelled frame
  // starts clean (GL parity after exception unwinds to next frame).
  m_pendingFinalization.reset();
  m_deferredProgressive.reset();
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
  // Cooperative cancellation: mirror GL by polling UI events and
  // throwing when a cancel is requested.
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  processEventsAndMaybeCancel(cancellationToken);

  VLOG(2) << fmt::format(
    "record begin fastOnly={} channels={} out={}x{} leases: entryExit={} lastAccum={} currentAccum={} blockId={}",
    payload.fastPathOnly,
    payload.visibleChannels.size(),
    static_cast<int>(payload.outputSize.x),
    static_cast<int>(payload.outputSize.y),
    static_cast<bool>(payload.entryExitLease && payload.entryExitLease->hasVulkanImage()),
    static_cast<bool>(payload.lastAccumLease && payload.lastAccumLease->hasVulkanImage()),
    static_cast<bool>(payload.currentAccumLease && payload.currentAccumLease->hasVulkanImage()),
    static_cast<bool>(payload.blockIdLease && payload.blockIdLease->hasVulkanImage()));
  // Do not clear m_pendingFinalization here; it may contain a completion
  // request emitted at end of the previous frame (post-fence). The backend
  // will pull it via takePendingFinalization() after record().
  CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
    << "Vulkan img raycaster missing entry/exit lease.";

  if (payload.visibleChannels.empty()) {
    return;
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureEmptyDescriptor();
  ensureQuadVertexBuffer();

  uploadEntryGeometry(payload);
  VLOG(2) << "after uploadEntryGeometry";

  processEventsAndMaybeCancel(cancellationToken);

  CHECK(payload.image) << "Raycaster payload missing image pointer.";

  // Progressive bookkeeping overview:
  // - m_pendingFinalization carries the round completion that the backend
  //   should report back to the renderer after this submission.
  // - m_deferredProgressive is set when the compaction callback discovers that
  //   the next round only needs the progressive raycast (block-ID pass already
  //   proved the cache is complete). When present, we skip the block-ID stage
  //   below but still emit the progressive pass so GL/Vulkan stay in lockstep.
  std::optional<DeferredProgressive> deferredRound;
  if (m_deferredProgressive && m_deferredProgressive->streamKey == payload.streamKey &&
      m_deferredProgressive->eye == batch.eye) {
    deferredRound = m_deferredProgressive;
  }

  // If a finalization from the previous frame is pending (and no deferred
  // progressive-only round is required), skip recording any progressive work
  // for this stream/eye now. The backend will consume the finalization
  // immediately after record() and advance the channel, avoiding an extra
  // compaction pass on the same channel.
  if (!deferredRound && m_pendingFinalization && m_pendingFinalization->streamKey == payload.streamKey &&
      m_pendingFinalization->eye == batch.eye) {
    VLOG(1) << "Raycaster: skipping progressive work due to pending finalization (will advance channel).";
    return;
  }

  const CompositingConfig composite = evaluateCompositing(payload.compositingMode);
  const bool hasIndices = payload.entryHasIndices && !payload.entryIndices.empty();
  const bool planarGeometry = !hasIndices;
  FastPipelineVariant fastVariant = FastPipelineVariant::Volume;
  if (planarGeometry) {
    if (payload.image->is2DData()) {
      fastVariant = FastPipelineVariant::Image2D;
    } else {
      fastVariant = FastPipelineVariant::Slice2D;
    }
  }

  const bool needsEntryExit = fastVariant == FastPipelineVariant::Volume;
  // GL parity for progressive: render entry/exit exactly once per progressive
  // cycle. Use channelIndexRaw<0 to detect the pre-progressive fast-preview
  // frame; render every frame for fast-only.
  const bool entryExitThisFrame = needsEntryExit && (payload.fastPathOnly || payload.channelIndexRaw < 0);
  if (entryExitThisFrame) {
    if (auto t = m_backend.beginGpuScope("ray_entry_exit")) {
      renderEntryExit(renderer, batch, payload, viewport, scissor, cmd);
      m_backend.endGpuScope(*t);
    } else {
      renderEntryExit(renderer, batch, payload, viewport, scissor, cmd);
    }
    VLOG(2) << "after renderEntryExit";
  } else if (needsEntryExit) {
    VLOG(2) << "skip renderEntryExit (progressive in-flight; reusing existing)";
  }

  processEventsAndMaybeCancel(cancellationToken);

  const bool skipBlockIdThisFrame = deferredRound.has_value();
  std::optional<Finalization> finalizeAfterProgressive;
  if (deferredRound) {
    finalizeAfterProgressive = Finalization{.streamKey = payload.streamKey,
                                            .eye = batch.eye,
                                            .lastRound = true,
                                            .channelCount = deferredRound->channelCount};
  }

  if (payload.fastPathOnly) {
    VLOG(2) << "dispatch fast path";
    processEventsAndMaybeCancel(cancellationToken);
    if (auto t = m_backend.beginGpuScope("ray_fast")) {
      renderFastPath(renderer, batch, payload, viewport, scissor, cmd, fastVariant, composite);
      m_backend.endGpuScope(*t);
    } else {
      renderFastPath(renderer, batch, payload, viewport, scissor, cmd, fastVariant, composite);
    }
  } else {
    CHECK(fastVariant == FastPipelineVariant::Volume)
      << "Progressive raycaster path only supports volumetric rendering.";
    VLOG(2) << "dispatch progressive path";
    processEventsAndMaybeCancel(cancellationToken);
    if (auto t = m_backend.beginGpuScope("ray_progressive")) {
      renderProgressivePath(renderer,
                            batch,
                            payload,
                            viewport,
                            scissor,
                            cmd,
                            composite,
                            skipBlockIdThisFrame,
                            finalizeAfterProgressive);
      m_backend.endGpuScope(*t);
    } else {
      renderProgressivePath(renderer,
                            batch,
                            payload,
                            viewport,
                            scissor,
                            cmd,
                            composite,
                            skipBlockIdThisFrame,
                            finalizeAfterProgressive);
    }
    if (skipBlockIdThisFrame) {
      m_deferredProgressive.reset();
    }
  }
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
    // Use immutable nearest-clamp samplers to ensure integer formats (e.g., RGBA32UI)
    // are sampled without linear filtering in all block-ID paths.
    vk::Sampler imm = m_backend.nearestClampSampler();
    std::array<vk::Sampler, 5> imms{imm, imm, imm, imm, imm};
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
      bindings[i].pImmutableSamplers = &imms[i];
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
  auto& device = m_backend.device();
  if (vertexCount > m_entryVertexCapacity) {
    m_entryVertexCapacity = vertexCount;
    // Device-local VB with transfer dst for staging
    m_entryVertexBuffer =
      device.createBuffer(m_entryVertexCapacity * sizeof(EntryVertex),
                          vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eDeviceLocal);
    CHECK(m_entryVertexBuffer != nullptr) << "Failed to allocate entry vertex buffer, count=" << m_entryVertexCapacity;
  }

  if (indexCount > m_entryIndexCapacity) {
    m_entryIndexCapacity = indexCount;
    if (m_entryIndexCapacity == 0) {
      m_entryIndexBuffer.reset();
    } else {
      // Device-local IB with transfer dst for staging
      m_entryIndexBuffer =
        device.createBuffer(m_entryIndexCapacity * sizeof(uint32_t),
                            vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                            vk::MemoryPropertyFlagBits::eDeviceLocal);
      CHECK(m_entryIndexBuffer != nullptr) << "Failed to allocate entry index buffer, count=" << m_entryIndexCapacity;
    }
  }
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
  ensureEntryVertexCapacity(payload.entryPositions.size(), payload.entryIndices.size());
  if (!m_entryVertexBuffer) {
    return;
  }

  std::vector<EntryVertex> vertices(payload.entryPositions.size());
  for (size_t i = 0; i < payload.entryPositions.size(); ++i) {
    vertices[i].position = payload.entryPositions[i];
    if (i < payload.entryTexCoords.size()) {
      vertices[i].texCoord = payload.entryTexCoords[i];
    }
  }
  // Reserve upload slices to minimize arena growth
  const size_t vbBytes = vertices.size() * sizeof(EntryVertex);
  const size_t ibBytes = payload.entryHasIndices ? payload.entryIndices.size() * sizeof(uint32_t) : 0;
  if (vbBytes > 0 || ibBytes > 0) {
    m_backend.reserveUploadSlices({
      {vbBytes, alignof(EntryVertex)},
      {ibBytes, alignof(uint32_t)   }
    });
  }

  // Stage copy into device-local VB
  if (vbBytes > 0) {
    auto slice = m_backend.suballocateUpload(vbBytes, alignof(EntryVertex));
    if (slice.mapped && slice.size >= vbBytes) {
      std::memcpy(slice.mapped, vertices.data(), vbBytes);
      m_backend.stageCopy(m_entryVertexBuffer->buffer(), 0, slice, /*isIndexBuffer=*/false);
    }
  }

  if (ibBytes > 0 && m_entryIndexBuffer) {
    auto slice = m_backend.suballocateUpload(ibBytes, alignof(uint32_t));
    if (slice.mapped && slice.size >= ibBytes) {
      std::memcpy(slice.mapped, payload.entryIndices.data(), ibBytes);
      m_backend.stageCopy(m_entryIndexBuffer->buffer(), 0, slice, /*isIndexBuffer=*/true);
    }
  }
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
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 2,
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
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eStorageImage,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 2,
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
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{.binding = 2,
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

void ZVulkanImgRaycasterPipelineContext::ensureBlockIdCompactOutput(size_t bytes)
{
  if (m_blockIdCompactOutput && m_blockIdCompactCapacity >= bytes) {
    return;
  }
  // Allocate host-visible, coherent buffer to receive compacted block IDs or bitset
  m_blockIdCompactOutput = m_backend.device().createBuffer(
    bytes,
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_blockIdCompactCapacity = bytes;
}

void ZVulkanImgRaycasterPipelineContext::ensureBlockIdCountSnapshot(uint32_t attachmentCount)
{
  const size_t bytes = static_cast<size_t>(attachmentCount) * sizeof(uint32_t);
  if (m_blockIdCountSnapshot && m_blockIdCountSnapshotCapacity >= bytes) {
    return;
  }
  // Counts are consumed by the compaction compute shader as an SSBO; they must be
  // created with STORAGE_BUFFER usage per Vulkan spec.
  m_blockIdCountSnapshot = m_backend.device().createBuffer(
    bytes,
    vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_blockIdCountSnapshotCapacity = bytes;
}

// (probe pipelines removed)

// (probe pipelines removed)

void ZVulkanImgRaycasterPipelineContext::recordBlockIdCompaction(Z3DRendererBase& renderer,
                                                                 const RenderBatch& batch,
                                                                 const ImgRaycasterPayload& payload,
                                                                 vk::raii::CommandBuffer& cmd)
{
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  processEventsAndMaybeCancel(cancellationToken);
  (void)renderer;
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
  ensureBlockIdCompactionPipeline(attachmentCount, mode);
  uint32_t imgW = firstBlock->width();
  uint32_t imgH = firstBlock->height();
  // Optional: dual probe (sampled + storage) and skip normal compaction
  // (probes removed)
  // Append-only compaction (drop hash variants): allocate append buffer
  {
    // Append buffer: [count (4B)] + ids[capacity]. Use 4 IDs/pixel capacity to mirror GL per-attachment read.
    const uint32_t capacity = std::max<uint32_t>(1u, imgW * imgH * 4u);
    const size_t bytes = sizeof(uint32_t) + static_cast<size_t>(capacity) * sizeof(uint32_t);
    ensureBlockIdCompactOutput(bytes);
    // Zero the count
    if (void* mapped = m_blockIdCompactOutput->map(0, sizeof(uint32_t))) {
      std::memset(mapped, 0x00, sizeof(uint32_t));
      m_blockIdCompactOutput->unmap();
    }
    if (VLOG_IS_ON(1)) {
      VLOG(1) << fmt::format("BlockID compaction (append): output capacity={} bytes (count + ids)", bytes);
    }
  }

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
  // Ensure per-attachment counts buffer exists and zero it before the sequence
  ensureBlockIdCountSnapshot(attachmentCount);
  if (m_blockIdCountSnapshot) {
    void* mapped = m_blockIdCountSnapshot->map(0, m_blockIdCountSnapshotCapacity);
    CHECK(mapped != nullptr);
    std::memset(mapped, 0x00, m_blockIdCountSnapshotCapacity);
    m_blockIdCountSnapshot->unmap();
  }

  for (uint32_t att = 0; att < attachmentCount; ++att) {
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
    // binding 2 = counts buffer
    ensureBlockIdCountSnapshot(attachmentCount);
    CHECK(m_blockIdCountSnapshot != nullptr);
    ds->updateStorageBuffer(2, *m_blockIdCountSnapshot);
    if (VLOG_IS_ON(2)) {
      VLOG(2) << fmt::format("Compaction DS storage: set=0x{:x} buf=0x{:x} size={}B",
                             reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(ds->descriptorSet())),
                             reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_blockIdCompactOutput->buffer())),
                             m_blockIdCompactOutput->size());
    }
    ds->updateStorageBuffer(1, *m_blockIdCompactOutput);
    spec.descriptorSets = {ds->descriptorSet()};
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
  const size_t resolvedChannelIndex = payload.visibleChannels[static_cast<size_t>(payload.channelIndexRaw)];

  // After fence: parse compacted buffer and update caches; determine if this round is complete
  auto bufPtr = m_blockIdCompactOutput.get();
  auto countSnap = m_blockIdCountSnapshot.get();
  m_backend.scheduleAfterActiveSubmissionFence([this,
                                                bufPtr,
                                                countSnap,
                                                imgW,
                                                imgH,
                                                streamKey = payload.streamKey,
                                                eye = batch.eye,
                                                channelCount = static_cast<uint32_t>(payload.visibleChannels.size()),
                                                attCount = attachmentCount,
                                                channelIndex = resolvedChannelIndex,
                                                channelIndexRaw = static_cast<uint32_t>(payload.channelIndexRaw),
                                                imagePtr = payload.image]() {
    CHECK(bufPtr != nullptr);
    CHECK(imagePtr != nullptr);
    const size_t unionBytes = sizeof(uint32_t) + static_cast<size_t>(imgW) * imgH * 4ull * sizeof(uint32_t);
    const void* mapped = bufPtr->map(0, unionBytes);
    if (!mapped) {
      return;
    }
    std::vector<uint32_t> missingBlocks;
    // Append buffer format: [count][ids...]
    const uint32_t* u32 = static_cast<const uint32_t*>(mapped);
    const uint32_t count = u32[0];
    const uint32_t capacity = imgW * imgH * 4u;
    const uint32_t clamped = std::min(count, capacity);

    missingBlocks.reserve(clamped);
    for (uint32_t i = 0; i < clamped; ++i) {
      uint32_t v = u32[1 + i];
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
    bufPtr->unmap();

    VLOG(1) << fmt::format("compaction output parsed: keys={} (non-empty)", missingBlocks.size());

    // Upload missing blocks (if any)
    if (!missingBlocks.empty()) {
      auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
      ZBenchTimer timer("vulkan_raycaster_blockid_compaction");
      imagePtr->updateAndUploadPageDirectoryCaches(missingBlocks, channelIndex, cancellationToken, timer);
      VLOG(1) << fmt::format("cache uploads dispatched: blocks={}", missingBlocks.size());
    }

    // Track attachments that rendered zero block IDs so we can mirror GL's
    // "last round" and "skip progressive" heuristics.
    bool anyZeroAttachment = false;
    bool allZeroAttachments = (attCount != 0);
    uint32_t zeroAtt = std::numeric_limits<uint32_t>::max();
    if (countSnap) {
      const size_t bytes = static_cast<size_t>(attCount) * sizeof(uint32_t);
      const void* mappedCnt = countSnap->map(0, bytes);
      CHECK(mappedCnt != nullptr);
      const uint32_t* counts = static_cast<const uint32_t*>(mappedCnt);
      for (uint32_t att = 0; att < attCount; ++att) {
        const bool zero = counts[att] == 0u;
        if (zero) {
          if (!anyZeroAttachment) {
            zeroAtt = att;
          }
          anyZeroAttachment = true;
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
      countSnap->unmap();
    }

    // Last-round decision: require an all-zero attachment AND union fits cache
    const size_t cacheCapacity = imagePtr->numCachedImages(channelIndex);
    const bool fitsCache = missingBlocks.size() <= cacheCapacity;
    if (VLOG_IS_ON(2)) {
      VLOG(2) << fmt::format("compaction last-round check: anyZero={} allZero={} fitsCache={} unionSize={} capacity={}",
                             anyZeroAttachment ? 1 : 0,
                             allZeroAttachments ? 1 : 0,
                             fitsCache ? 1 : 0,
                             missingBlocks.size(),
                             cacheCapacity);
    }
    if (allZeroAttachments) {
      m_deferredProgressive.reset();
      m_pendingFinalization =
        Finalization{.streamKey = streamKey, .eye = eye, .lastRound = true, .channelCount = channelCount};
      VLOG(1) << fmt::format(
        "compaction: skip progressive round (all attachments zero, channelIndex={} (raw={}) unionSize={} capacity={})",
        channelIndex,
        channelIndexRaw,
        missingBlocks.size(),
        cacheCapacity);
    } else if (anyZeroAttachment && fitsCache) {
      m_deferredProgressive = DeferredProgressive{.streamKey = streamKey, .eye = eye, .channelCount = channelCount};
      VLOG(1) << fmt::format(
        "compaction: schedule progressive-only round (channelIndex={} (raw={}) firstZeroAtt={} unionSize={} capacity={})",
        channelIndex,
        channelIndexRaw,
        (zeroAtt == std::numeric_limits<uint32_t>::max() ? -1 : static_cast<int>(zeroAtt)),
        missingBlocks.size(),
        cacheCapacity);
    } else if (m_deferredProgressive && m_deferredProgressive->streamKey == streamKey &&
               m_deferredProgressive->eye == eye) {
      m_deferredProgressive.reset();
    }
  });
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
    VLOG(1) << fmt::format("Raycaster specializations: mode={} localMip={} opaque={} levels={}",
                           static_cast<int>(key.mode),
                           key.localMip,
                           key.resultOpaque,
                           key.levelCount);
    VLOG(1) << fmt::format("Raycaster attachments: colors={} depthPresent={} c0Fmt={} dFmt={}",
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
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthWriteEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);

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
    const auto [depthLayout, depthAspect] = depthReadDescriptorLayoutAndAspect(*depthArray);
    m_mergeDescriptor->updateTexture(1, *depthArray, m_backend.defaultSampler(), depthLayout, depthAspect);
  } else {
    m_mergeDescriptor->updateTexture(1, colorArray, m_backend.defaultSampler());
  }
}

void ZVulkanImgRaycasterPipelineContext::ensureProgressiveLayerTargets(const glm::uvec2& size,
                                                                       uint32_t layerCount,
                                                                       uint32_t generation,
                                                                       vk::raii::CommandBuffer& cmd)
{
  if (layerCount == 0u || size.x == 0u || size.y == 0u) {
    m_progressiveLayerColor.reset();
    m_progressiveLayerDepth.reset();
    m_progressiveLayerSize = glm::uvec2(0u);
    m_progressiveLayerCount = 0u;
    m_progressiveGeneration = generation;
    return;
  }

  const bool sizeChanged = m_progressiveLayerSize != size;
  const bool layerChanged = m_progressiveLayerCount != layerCount;
  const bool generationChanged = m_progressiveGeneration != generation;

  if (sizeChanged || layerChanged) {
    auto colorInfo =
      ZVulkanTexture::CreateInfo::make2DArray(size.x,
                                              size.y,
                                              layerCount,
                                              vk::Format::eR16G16B16A16Sfloat,
                                              vk::ImageUsageFlagBits::eColorAttachment |
                                                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                              vk::MemoryPropertyFlagBits::eDeviceLocal,
                                              1u,
                                              true,
                                              vk::ImageLayout::eShaderReadOnlyOptimal);
    m_progressiveLayerColor = m_backend.device().createTexture(colorInfo);
    CHECK(m_progressiveLayerColor != nullptr) << "Raycaster: failed to create progressive color layer array";

    auto depthInfo =
      ZVulkanTexture::CreateInfo::make2DArray(size.x,
                                              size.y,
                                              layerCount,
                                              vk::Format::eD32Sfloat,
                                              vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                              vk::MemoryPropertyFlagBits::eDeviceLocal,
                                              1u,
                                              true,
                                              vk::ImageLayout::eShaderReadOnlyOptimal);
    m_progressiveLayerDepth = m_backend.device().createTexture(depthInfo);
    CHECK(m_progressiveLayerDepth != nullptr) << "Raycaster: failed to create progressive depth layer array";

    m_progressiveLayerSize = size;
    m_progressiveLayerCount = layerCount;
  }

  if (!m_progressiveLayerColor || !m_progressiveLayerDepth) {
    return;
  }

  if (sizeChanged || layerChanged || generationChanged) {
    auto clearColor = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
    auto colorRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, layerCount};
    m_progressiveLayerColor->transitionLayout(cmd,
                                              m_progressiveLayerColor->layout(),
                                              vk::ImageLayout::eTransferDstOptimal);
    cmd.clearColorImage(m_progressiveLayerColor->image(), vk::ImageLayout::eTransferDstOptimal, clearColor, colorRange);
    m_progressiveLayerColor->transitionLayout(cmd,
                                              vk::ImageLayout::eTransferDstOptimal,
                                              vk::ImageLayout::eShaderReadOnlyOptimal);
    m_progressiveLayerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

    auto depthRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0u, 1u, 0u, layerCount};
    m_progressiveLayerDepth->transitionLayout(cmd,
                                              m_progressiveLayerDepth->layout(),
                                              vk::ImageLayout::eTransferDstOptimal,
                                              vk::ImageAspectFlagBits::eDepth);
    cmd.clearDepthStencilImage(m_progressiveLayerDepth->image(),
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearDepthStencilValue{1.0f, 0u},
                               depthRange);
    m_progressiveLayerDepth->transitionLayout(cmd,
                                              vk::ImageLayout::eTransferDstOptimal,
                                              vk::ImageLayout::eDepthReadOnlyOptimal,
                                              vk::ImageAspectFlagBits::eDepth);
    m_progressiveLayerDepth->setDescriptorLayout(vk::ImageLayout::eDepthReadOnlyOptimal);
  }

  m_progressiveGeneration = generation;
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
    resources.volumeTexture->uploadData(data, byteSize);
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
    resources.image2DTexture->uploadData(data, byteSize);
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
    vulkan::ensure1DLUTTexture(device, resources.transferTexture, width);
    resources.transferWidth = width;
    createdOrResized = true;
  }

  if (createdOrResized || resources.transferGeneration != gen) {
    std::vector<uint8_t> texels;
    transferFunction.buildLUTRGBA8(texels, width);
    if (!texels.empty()) {
      vulkan::uploadLUT(*resources.transferTexture, texels.data(), texels.size());
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
  // Always allocate/update a transient per-draw descriptor for dynamic textures (set=1).
  // If freshOverrideDescriptors is true, allocate a new override set even if one exists
  // to avoid updating a set that might still be bound earlier in this command buffer.
  if (freshOverrideDescriptors || !resources.pagedDescriptor) {
    resources.pagedDescriptor = m_backend.allocateOverrideDescriptorSet(**m_progressiveDynamicSetLayout);
  }
  CHECK(resources.pagedDescriptor) << "Failed to allocate Vulkan raycaster paging descriptor set.";

  auto* pageDirectory =
    m_imageBlockUploader ? m_imageBlockUploader->pageDirectoryTexture(*payload.image, channelIndex) : nullptr;
  auto* pageTable =
    m_imageBlockUploader ? m_imageBlockUploader->pageTableTexture(*payload.image, channelIndex) : nullptr;
  auto* imageCache =
    m_imageBlockUploader ? m_imageBlockUploader->imageCacheTexture(*payload.image, channelIndex) : nullptr;
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

  const uint32_t levelCount = static_cast<uint32_t>(std::min<size_t>(image.numLevels(), kMaxPagingLevels));
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
    resources.pageDataBuffer = m_backend.device().createBuffer(pageData.size(),
                                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
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
  if (!resources.persistentPageDescriptor) {
    auto* backendPtr = &m_backend;
    auto* pageBufPtr = resources.pageDataBuffer.get();
    const size_t resIndex = channelIndex;
    backendPtr->scheduleAfterCurrentFrameCompletion([this, backendPtr, pageBufPtr, resIndex]() {
      if (!pageBufPtr) {
        return;
      }
      ensureDescriptorPool();
      try {
        vk::DescriptorSet raw = m_descriptorPool->allocateDescriptorSet(**m_pageSetLayout);
        ChannelResources& res = ensureChannelResources(resIndex);
        res.persistentPageDescriptor =
          std::make_unique<ZVulkanDescriptorSet>(backendPtr->device(), raw, /*isOverrideTransient=*/false);
        res.persistentPageDescriptor->writeUniformBufferOnce(2, *pageBufPtr);
        res.boundPageDataBuffer = pageBufPtr;
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Failed to allocate persistent page descriptor: " << e.what();
      }
    });
  } else {
    // If the page buffer object changed since last bind, rewrite the persistent descriptor
    // after frame completion.
    ZVulkanBuffer* curPageBuf = resources.pageDataBuffer.get();
    if (curPageBuf != resources.boundPageDataBuffer) {
      auto* pageBufPtr = curPageBuf;
      const size_t resIndex2 = channelIndex;
      m_backend.scheduleAfterCurrentFrameCompletion([this, resIndex2, pageBufPtr]() {
        ChannelResources& res = ensureChannelResources(resIndex2);
        if (res.persistentPageDescriptor && pageBufPtr) {
          res.persistentPageDescriptor->updateUniformBuffer(2, *pageBufPtr);
          res.boundPageDataBuffer = pageBufPtr;
        }
      });
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

    // Schedule persistent creation after frame completion
    auto* backendPtr = &m_backend;
    ZVulkanTexture* pd = pageDirectory;
    ZVulkanTexture* pt = pageTable;
    ZVulkanTexture* ic = imageCache;
    ZVulkanTexture* vol = &volume;
    ZVulkanTexture* tf = &transfer;
    const size_t resIndexStatic = channelIndex;
    backendPtr->scheduleAfterCurrentFrameCompletion([this, backendPtr, pd, pt, ic, vol, tf, resIndexStatic]() {
      ensureDescriptorPool();
      try {
        vk::DescriptorSet raw = m_descriptorPool->allocateDescriptorSet(**m_progressiveStaticSetLayout);
        ChannelResources& res = ensureChannelResources(resIndexStatic);
        res.persistentStaticDescriptor =
          std::make_unique<ZVulkanDescriptorSet>(backendPtr->device(), raw, /*isOverrideTransient=*/false);
        // Integer 3D samplers: use nearest clamp sampler
        res.persistentStaticDescriptor->writeTextureOnce(0, *pd, backendPtr->nearestClampSampler());
        res.persistentStaticDescriptor->writeTextureOnce(1, *pt, backendPtr->nearestClampSampler());
        res.persistentStaticDescriptor->writeTextureOnce(2, *ic, backendPtr->defaultSampler());
        res.persistentStaticDescriptor->writeTextureOnce(3, *vol, backendPtr->defaultSampler());
        res.persistentStaticDescriptor->writeTextureOnce(4, *tf, backendPtr->defaultSampler());
        res.boundPageDirectoryTex = pd;
        res.boundPageTableTex = pt;
        res.boundImageCacheTex = ic;
        res.boundVolumeTex = vol;
        res.boundTransferTex = tf;
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Failed to allocate persistent static descriptor: " << e.what();
      }
    });
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
      m_backend.scheduleAfterCurrentFrameCompletion([this, resIndexStatic, pd, pt, ic, vol, tf]() {
        ChannelResources& res = ensureChannelResources(resIndexStatic);
        if (res.persistentStaticDescriptor) {
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
        }
      });
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

  const vk::DescriptorSet pageSet =
    resources.persistentPageDescriptor
      ? resources.persistentPageDescriptor->descriptorSet()
      : (resources.pageDescriptor ? resources.pageDescriptor->descriptorSet() : vk::DescriptorSet{});
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

void ZVulkanImgRaycasterPipelineContext::renderEntryExit(Z3DRendererBase& renderer,
                                                         const RenderBatch& batch,
                                                         const ImgRaycasterPayload& payload,
                                                         const vk::Viewport& viewport,
                                                         const vk::Rect2D& scissor,
                                                         vk::raii::CommandBuffer& cmd)
{
  VLOG(2) << "Raycaster::renderEntryExit begin";
  auto* texture = payload.entryExitLease->colorAttachment(0);
  CHECK(texture) << "Entry/exit lease missing color attachment.";
  VLOG(2) << fmt::format("Raycaster::renderEntryExit target tex=0x{:x}", reinterpret_cast<uint64_t>(texture));

  ensureEntryPipelines(texture->format());
  // No transform descriptor needed; entry shader uses push constants.

  const bool flipped = payload.entryFlipped;
  const size_t eyeIndex = std::min<size_t>(static_cast<size_t>(batch.eye), renderer.viewState().eyes.size() - 1);
  const auto& eyeState = renderer.viewState().eyes[eyeIndex];

  struct EntryPushConstant
  {
    glm::mat4 projectionView;
    glm::mat4 view;
  } pushConstant{eyeState.projectionMatrix * eyeState.viewMatrix, eyeState.viewMatrix};

  ZVulkanPipelineCommandRecorder recorder(cmd);

  for (uint32_t layer = 0; layer < 2; ++layer) {
    auto& pipeline = (layer == 0) ? (flipped ? m_entryBackPipeline : m_entryFrontPipeline)
                                  : (flipped ? m_entryFrontPipeline : m_entryBackPipeline);

    vk::ImageView layerView = texture->layerImageView(layer);
    if (layerView == vk::ImageView{}) {
      layerView = texture->imageView();
    }
    if (layerView == vk::ImageView{}) {
      continue;
    }

    VLOG(2) << "VK raycaster entry/exit: layer=" << layer << " flipped=" << flipped
            << " texFmt=" << enumOrUnderlying(texture->format(), 16)
            << " size=" << static_cast<int>(payload.outputSize.x) << "x" << static_cast<int>(payload.outputSize.y);

    ZVulkanAttachmentInfo attachment{};
    attachment.image = texture->image();
    attachment.view = layerView;
    attachment.format = texture->format();
    // Begin from UNDEFINED since loadOp=CLEAR; avoid relying on prior recorded transitions.
    attachment.initialLayout = vk::ImageLayout::eUndefined;
    // End entry/exit color in sampled layout for downstream reads
    attachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    attachment.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f})};
    attachment.loadOp = vk::AttachmentLoadOp::eClear;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;
    attachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    attachment.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    attachment.srcAccess = {};
    attachment.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    attachment.aspect = vk::ImageAspectFlagBits::eColor;
    attachment.trackingTexture = texture;

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = scissor;
    spec.viewports = {viewport};
    spec.scissors = {scissor};
    spec.colorAttachments = {attachment};
    spec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    spec.vertexBuffers = {m_entryVertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.pushConstantsData = &pushConstant;
    spec.pushConstantsSize = sizeof(pushConstant);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    spec.requirePushConstants = true;
    spec.instanceCount = 1;

    if (payload.entryHasIndices && m_entryIndexBuffer) {
      VLOG(2) << "VK raycaster entry/exit drawIndexed: count=" << payload.entryIndices.size();
      spec.indexBuffer = m_entryIndexBuffer->buffer();
      spec.indexOffset = 0;
      spec.indexType = vk::IndexType::eUint32;
      spec.indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      spec.firstIndex = 0;
      spec.vertexOffset = 0;
      spec.firstInstance = 0;
    } else {
      VLOG(2) << "VK raycaster entry/exit draw: verts=" << payload.entryPositions.size();
      spec.vertexCount = static_cast<uint32_t>(payload.entryPositions.size());
      spec.firstVertex = 0;
      spec.firstInstance = 0;
    }

    recorder.recordGraphicsPass(spec);
    VLOG(2) << "Raycaster::renderEntryExit end";
  }

  // Transition for sampling in raycaster path
  texture->transitionLayout(cmd, texture->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  texture->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  // Optional debug dump of entry/exit layers to TIF (RGBA32F)
  if (FLAGS_atlas_debug_save_entry_exit) {
    // Hold a strong ref to the lease to keep the texture alive until we read it back.
    auto leaseRef = payload.entryExitLease;
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend && leaseRef && leaseRef->hasVulkanImage()) {
      backend->scheduleAfterCurrentFrameCompletion([leaseRef]() {
        ZVulkanTexture* tex = leaseRef->colorAttachment(0);
        if (!tex) {
          LOG(ERROR) << "Entry/exit debug save: color attachment missing";
          return;
        }

        QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
        if (!dir.isEmpty() && !dir.endsWith('/')) {
          dir += '/';
        }

        auto saveLayer = [&](uint32_t layer, const QString& label) {
          const QString filename =
            dir + QString("entry_exit_%1_%2x%3.tif").arg(label).arg(tex->width()).arg(tex->height());
          ZVulkanTexture::ImageSaveOptions opts;
          opts.arrayLayer = layer;
          if (!tex->saveToImage(filename, opts)) {
            LOG(ERROR) << "Entry/exit debug save failed for layer " << layer;
          }
        };

        saveLayer(0u, QStringLiteral("front"));
        if (tex->arrayLayers() > 1u) {
          saveLayer(1u, QStringLiteral("back"));
        }
      });
    }
  }
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

void ZVulkanImgRaycasterPipelineContext::renderFastVolume(Z3DRendererBase& renderer,
                                                          const RenderBatch& batch,
                                                          const ImgRaycasterPayload& payload,
                                                          const vk::Viewport& viewport,
                                                          const vk::Rect2D& scissor,
                                                          vk::raii::CommandBuffer& cmd,
                                                          const CompositingConfig& composite)
{
  VLOG(2) << "VK raycaster fast-path: channels=" << payload.visibleChannels.size() << " output=" << payload.outputSize.x
          << "x" << payload.outputSize.y << " mode=" << enumToString(payload.compositingMode)
          << " fastOnly=" << payload.fastPathOnly;
  const size_t channelCount = payload.visibleChannels.size();
  if (channelCount == 0) {
    return;
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);

  CHECK(payload.image) << "Vulkan img raycaster missing image context.";

  CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
    << "Raycaster fast path missing entry/exit lease.";

  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster fast pass color attachment");
    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    // Produce color in sampled layout to avoid follow-up transitions
    info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    info.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                              attachment.clearValue.color.g,
                                                                              attachment.clearValue.color.b,
                                                                              attachment.clearValue.color.a})};
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    info.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    info.srcAccess = {};
    info.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    info.aspect = vk::ImageAspectFlagBits::eColor;
    info.trackingTexture = &texture;
    VLOG(2) << fmt::format("ray fast color: loadOp={} storeOp={} fmt={}",
                           enumOrUnderlying(info.loadOp),
                           enumOrUnderlying(info.storeOp),
                           enumOrUnderlying(texture.format()));
    return info;
  };

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster fast pass depth attachment");
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(texture);
    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    // After this pass, depth will be sampled in downstream composition/copy
    info.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    // Clear-on-first-use: ensure background depth is deterministic when no prior writes.
    // Track by VkImage per frame.
    const VkImage imgHandle = static_cast<VkImage>(texture.image());
    const bool firstUse = m_depthClearedThisFrame.insert(imgHandle).second;
    info.loadOp = firstUse ? vk::AttachmentLoadOp::eClear : vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil = vk::ClearDepthStencilValue(firstUse ? 1.0f : attachment.clearValue.depth,
                                                              firstUse ? 0u : attachment.clearValue.stencil);
    info.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    info.dstStage = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    info.srcAccess = {};
    info.dstAccess =
      vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    info.aspect = attachAspect;
    info.trackingTexture = &texture;
    if (firstUse) {
      VLOG(2) << "Raycaster merge first-use depth clear on image=" << reinterpret_cast<uint64_t>(imgHandle);
    }
    VLOG(2) << fmt::format("ray fast depth: loadOp={} storeOp={} fmt={}",
                           enumOrUnderlying(info.loadOp),
                           enumOrUnderlying(info.storeOp),
                           enumOrUnderlying(texture.format()));
    return info;
  };

  std::vector<ZVulkanAttachmentInfo> finalColorAttachments;
  finalColorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      finalColorAttachments.push_back(*info);
    }
  }

  std::optional<ZVulkanAttachmentInfo> finalDepthAttachment;
  if (batch.pass.depthAttachment) {
    finalDepthAttachment = buildDepthAttachment(*batch.pass.depthAttachment);
  }

  glm::uvec2 outputSize = payload.outputSize;
  if (outputSize.x == 0u || outputSize.y == 0u) {
    const auto& viewportState = renderer.frameState().viewport;
    outputSize = glm::uvec2(std::max<uint32_t>(1u, viewportState.z), std::max<uint32_t>(1u, viewportState.w));
  }

  auto* entryTexture = payload.entryExitLease->colorAttachment(0);
  CHECK(entryTexture) << "Entry/exit texture unavailable.";
  entryTexture->transitionLayout(cmd, entryTexture->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  entryTexture->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  ensureEmptyDescriptor();

  vulkan::AttachmentFormats finalFormats = vulkan::extractAttachmentFormats(batch);

  const auto& viewState = renderer.viewState();
  const float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
  const float farClip = viewState.farClip;
  const float zeToZW_a = farClip * nearClip / (farClip - nearClip);
  const float zeToZW_b = 0.5f * (farClip + nearClip) / (farClip - nearClip) + 0.5f;

  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster fast path: payload missing transferFunctions vector (fatal)";
  const auto& transferFunctions = *payload.transferFunctions;

  if (channelCount == 1) {
    const size_t channelIndex = payload.visibleChannels.front();
    if (channelIndex >= transferFunctions.size() || transferFunctions[channelIndex] == nullptr) {
      LOG(ERROR) << "Missing transfer function for channel " << channelIndex;
      return;
    }

    m_backend.validateFormatsOrCrash(finalFormats, "img raycaster fast path");
    FastPipelineKey fastKey;
    fastKey.variant = FastPipelineVariant::Volume;
    fastKey.mode = composite.mode;
    fastKey.resultOpaque = composite.resultOpaque;
    fastKey.depthEnabled = true;
    fastKey.colorFormats = finalFormats.colorFormats;
    fastKey.depthFormat = finalFormats.depthFormat;
    PipelineInstance& fastPipeline = ensureFastPipeline(fastKey);

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);

    // GL fast path does not run Block-ID; defer paging to the dedicated stage

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

    struct RayPC
    {
      float s;
      float i;
      float l;
      float a;
      float b;
    } pc{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};

    CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path missing fast descriptor";
    std::vector<vk::DescriptorSet> descriptorSets{resources.fastDescriptor->descriptorSet()};

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = scissor;
    spec.viewports = {viewport};
    spec.scissors = {scissor};
    spec.colorAttachments = finalColorAttachments;
    spec.depthStencilAttachment = finalDepthAttachment;
    spec.pipelineHandle = fastPipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = fastPipeline.pipeline->pipelineLayoutHandle();
    spec.descriptorSets = std::move(descriptorSets);
    spec.descriptorSetFirst = 0;
    spec.expectedDescriptorSetCount = 1;
    spec.vertexBuffers = {m_quadVertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
    spec.instanceCount = 1;
    spec.pushConstantsData = &pc;
    spec.pushConstantsSize = sizeof(pc);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    spec.requirePushConstants = true;

    VLOG(2) << "VK raycaster fast-path draw: verts=" << m_quadVertexCount
            << " colorAttCount=" << finalColorAttachments.size()
            << " colorFmt0=" << enumOrUnderlying(finalFormats.colorFormats.front())
            << " depthFmt=" << enumOrUnderlying(finalFormats.depthFormat.value()) << " output=" << outputSize.x << "x"
            << outputSize.y;

    recorder.recordGraphicsPass(spec);
    return;
  }

  // Prefer explicit layer lease if present; otherwise use progressive arrays.
  ZVulkanTexture* layerColor = nullptr;
  ZVulkanTexture* layerDepth = nullptr;
  if (payload.channelLayerLease && payload.channelLayerLease->hasVulkanImage()) {
    Z3DScratchResourcePool::RenderTargetLease* layerLease = payload.channelLayerLease.get();
    layerColor = layerLease->colorAttachment(0);
    layerDepth = layerLease->depthAttachmentTexture();
  } else {
    layerColor = m_progressiveLayerColor.get();
    layerDepth = m_progressiveLayerDepth.get();
  }
  CHECK(layerColor) << "Layer array color attachment unavailable.";
  CHECK(layerDepth) << "Layer array depth attachment unavailable.";

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  layerFormats.depthFormat = layerDepth->format();
  FastPipelineKey layerKey;
  layerKey.variant = FastPipelineVariant::Volume;
  layerKey.mode = composite.mode;
  layerKey.resultOpaque = composite.resultOpaque;
  layerKey.depthEnabled = true;
  layerKey.colorFormats = layerFormats.colorFormats;
  layerKey.depthFormat = layerFormats.depthFormat;
  PipelineInstance& layerPipeline = ensureFastPipeline(layerKey);

  // Derive per-layer viewport/scissor from the actual layer target to avoid offset/extent mismatches.
  // Use the input viewport/scissor from the compositor; avoid overriding with texture extents.
  vk::Viewport layerViewport = viewport;
  vk::Rect2D layerRect = scissor;

  // Rely on per-layer loadOp=Clear for depth to reduce full-array clears.

  auto tLayers = m_backend.beginGpuScope("ray_layers");
  for (size_t order = 0; order < channelCount; ++order) {
    const size_t channelIndex = payload.visibleChannels[order];
    CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
      << "Missing transfer function for channel " << channelIndex;

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);

    // GL fast path does not run Block-ID per channel; skip paging here

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

    auto colorView = layerColor->layerImageView(static_cast<uint32_t>(order));
    if (colorView == vk::ImageView{}) {
      colorView = layerColor->imageView();
    }
    ZVulkanAttachmentInfo colorAttachment{};
    colorAttachment.image = layerColor->image();
    colorAttachment.view = colorView;
    colorAttachment.format = layerColor->format();
    colorAttachment.initialLayout = layerColor->layout();
    colorAttachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    colorAttachment.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    colorAttachment.srcAccess = {};
    colorAttachment.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    colorAttachment.aspect = vk::ImageAspectFlagBits::eColor;
    colorAttachment.trackingTexture = layerColor;

    auto depthView = layerDepth->layerImageView(static_cast<uint32_t>(order));
    if (depthView == vk::ImageView{}) {
      depthView = layerDepth->imageView();
    }
    CHECK(depthView != vk::ImageView{}) << "Layer depth attachment view missing for volumetric layered path.";
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
    ZVulkanAttachmentInfo depthAttachment{};
    depthAttachment.image = layerDepth->image();
    depthAttachment.view = depthView;
    depthAttachment.format = layerDepth->format();
    depthAttachment.initialLayout = layerDepth->layout();
    depthAttachment.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depthAttachment.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
    depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    depthAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    depthAttachment.dstStage =
      vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    depthAttachment.srcAccess = {};
    depthAttachment.dstAccess =
      vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depthAttachment.aspect = attachAspect;
    depthAttachment.trackingTexture = layerDepth;

    CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path missing fast descriptor (layered)";
    std::vector<vk::DescriptorSet> layeredSets{resources.fastDescriptor->descriptorSet()};

    struct RayPC
    {
      float s;
      float i;
      float l;
      float a;
      float b;
    } pcL{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};

    ZVulkanGraphicsPassSpec layerSpec{};
    layerSpec.renderArea = layerRect;
    layerSpec.viewports = {layerViewport};
    layerSpec.scissors = {layerRect};
    layerSpec.colorAttachments = {colorAttachment};
    layerSpec.depthStencilAttachment = depthAttachment;
    layerSpec.pipelineHandle = layerPipeline.pipeline->pipelineHandle();
    layerSpec.pipelineLayoutHandle = layerPipeline.pipeline->pipelineLayoutHandle();
    layerSpec.descriptorSets = std::move(layeredSets);
    layerSpec.descriptorSetFirst = 0;
    layerSpec.expectedDescriptorSetCount = 1;
    layerSpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
    layerSpec.vertexOffsets = {0};
    layerSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
    layerSpec.instanceCount = 1;
    layerSpec.pushConstantsData = &pcL;
    layerSpec.pushConstantsSize = sizeof(pcL);
    layerSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    layerSpec.requirePushConstants = true;

    {
      auto depthFmt = enumOrUnderlying(layerDepth->format(), 16);
      VLOG(2) << "VK raycaster layered draw: order=" << order << " channelIndex=" << channelIndex
              << " verts=" << m_quadVertexCount << " colorFmt=" << enumOrUnderlying(layerColor->format(), 16)
              << " depthFmt=" << depthFmt;
    }
    recorder.recordGraphicsPass(layerSpec);
  }

  // Transition the entire color array once for merge sampling.
  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  // Ensure the entire depth array is in the descriptor sampling layout before merge.
  const auto [readLayout, _descAspect] = depthReadDescriptorLayoutAndAspect(*layerDepth);
  const auto fullBarrierAspect = depthReadBarrierAspect(*layerDepth);
  layerDepth->transitionLayout(cmd, layerDepth->layout(), readLayout, fullBarrierAspect);
  layerDepth->setDescriptorLayout(readLayout);

  // Optional debug dump of layered raycaster color array to TIFs (per-layer)
  if (FLAGS_atlas_debug_save_raycaster_layers) {
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend) {
      const bool hasLease = (payload.channelLayerLease && payload.channelLayerLease->hasVulkanImage());
      if (hasLease) {
        // Preferred: capture the lease to keep images alive until callback finishes
        auto leaseRef = payload.channelLayerLease;
        backend->scheduleAfterCurrentFrameCompletion([leaseRef, channelCount]() {
          ZVulkanTexture* tex = leaseRef->colorAttachment(0);
          if (tex) {
            QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
            if (!dir.isEmpty() && !dir.endsWith('/')) {
              dir += '/';
            }
            const uint32_t totalLayers = std::min<uint32_t>(static_cast<uint32_t>(channelCount), tex->arrayLayers());
            for (uint32_t layer = 0; layer < totalLayers; ++layer) {
              const QString filename =
                dir + QString("raycaster_layer_%1_%2x%3.tif").arg(layer).arg(tex->width()).arg(tex->height());
              ZVulkanTexture::ImageSaveOptions opts;
              opts.arrayLayer = layer;
              (void)tex->saveToImage(filename, opts);
            }
          }
          ZVulkanTexture* dtex = leaseRef->depthAttachmentTexture();
          if (dtex) {
            QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
            if (!dir.isEmpty() && !dir.endsWith('/')) {
              dir += '/';
            }
            const uint32_t totalLayers = std::min<uint32_t>(static_cast<uint32_t>(channelCount), dtex->arrayLayers());
            for (uint32_t layer = 0; layer < totalLayers; ++layer) {
              const QString filename =
                dir + QString("raycaster_layer_depth_%1_%2x%3.tif").arg(layer).arg(dtex->width()).arg(dtex->height());
              ZVulkanTexture::ImageSaveOptions opts;
              opts.arrayLayer = layer;
              opts.aspectMask = vk::ImageAspectFlagBits::eDepth;
              (void)dtex->saveToImage(filename, opts);
            }
          }
        });
      } else if (m_progressiveLayerColor && m_progressiveLayerDepth) {
        // Fallback: capture 'this' to keep context-owned progressive arrays valid
        ZVulkanTexture* tex = m_progressiveLayerColor.get();
        ZVulkanTexture* dtex = m_progressiveLayerDepth.get();
        backend->scheduleAfterCurrentFrameCompletion([tex, dtex, channelCount]() {
          QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!dir.isEmpty() && !dir.endsWith('/')) {
            dir += '/';
          }
          const uint32_t colorLayers = std::min<uint32_t>(channelCount, tex->arrayLayers());
          for (uint32_t layer = 0; layer < colorLayers; ++layer) {
            const QString filename =
              dir + QString("raycaster_layer_%1_%2x%3.tif").arg(layer).arg(tex->width()).arg(tex->height());
            ZVulkanTexture::ImageSaveOptions opts;
            opts.arrayLayer = layer;
            (void)tex->saveToImage(filename, opts);
          }
          const uint32_t depthLayers = std::min<uint32_t>(channelCount, dtex->arrayLayers());
          for (uint32_t layer = 0; layer < depthLayers; ++layer) {
            const QString filename =
              dir + QString("raycaster_layer_depth_%1_%2x%3.tif").arg(layer).arg(dtex->width()).arg(dtex->height());
            ZVulkanTexture::ImageSaveOptions dopts;
            dopts.arrayLayer = layer;
            dopts.aspectMask = vk::ImageAspectFlagBits::eDepth;
            (void)dtex->saveToImage(filename, dopts);
          }
        });
      }
    }
  }

  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = composite.maxProjectionMerge;
  mergeKey.resultOpaque = composite.resultOpaque;
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;

  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);
  bindMergeDescriptor(*layerColor, layerDepth);

  if (tLayers) {
    m_backend.endGpuScope(*tLayers);
  }

  auto tMerge = m_backend.beginGpuScope("ray_merge");
  CHECK(m_mergeDescriptor != nullptr) << "Raycaster merge requires descriptor set";
  std::vector<vk::DescriptorSet> mergeSets{m_mergeDescriptor->descriptorSet()};

  VLOG(2) << "VK raycaster merge: colors=" << finalColorAttachments.size()
          << " depth=" << (finalDepthAttachment ? 1 : 0)
          << " color0Fmt=" << enumOrUnderlying(finalFormats.colorFormats.front())
          << " depthFmt=" << enumOrUnderlying(finalFormats.depthFormat.value());

  ZVulkanGraphicsPassSpec mergeSpec{};
  mergeSpec.renderArea = scissor;
  mergeSpec.viewports = {viewport};
  mergeSpec.scissors = {scissor};
  mergeSpec.colorAttachments = finalColorAttachments;
  mergeSpec.depthStencilAttachment = finalDepthAttachment;
  mergeSpec.pipelineHandle = mergePipeline.pipeline->pipelineHandle();
  mergeSpec.pipelineLayoutHandle = mergePipeline.pipeline->pipelineLayoutHandle();
  mergeSpec.descriptorSets = std::move(mergeSets);
  mergeSpec.descriptorSetFirst = 0;
  mergeSpec.expectedDescriptorSetCount = 1;
  mergeSpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
  mergeSpec.vertexOffsets = {0};
  mergeSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  mergeSpec.instanceCount = 1;

  recorder.recordGraphicsPass(mergeSpec);
  if (tMerge) {
    m_backend.endGpuScope(*tMerge);
  }
  // end merge scope handled at pass-level if enabled

  // Optional debug: save merged output (first color attachment of the active surface)
  if (FLAGS_atlas_debug_save_raycaster_merge_out) {
    // Snapshot the handles from the batch's pass to read back after frame completion
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
          << "Raycaster merge debug save: invalid color attachment handle";

        auto& tex = vulkan::textureFromHandle(handle, m_backend.device(), "img raycaster merge debug");

        QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
        if (!dir.isEmpty() && !dir.endsWith('/')) {
          dir += '/';
        }
        const QString filename = dir + QString("raycaster_merge_%1x%2.tif").arg(tex.width()).arg(tex.height());
        ZVulkanTexture::ImageSaveOptions colorOpts;
        if (!tex.saveToImage(filename, colorOpts)) {
          LOG(ERROR) << "Raycaster merge debug save failed for color attachment";
        }

        // Save depth if available
        if (depthHandle && depthHandle->valid() && depthHandle->backend == AttachmentBackend::Vulkan) {
          auto& dtex = vulkan::textureFromHandle(*depthHandle, m_backend.device(), "img raycaster merge depth debug");
          QString ddir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!ddir.isEmpty() && !ddir.endsWith('/')) {
            ddir += '/';
          }
          const QString dname = ddir + QString("raycaster_merge_depth_%1x%2.tif").arg(dtex.width()).arg(dtex.height());
          ZVulkanTexture::ImageSaveOptions depthOpts;
          depthOpts.aspectMask = vk::ImageAspectFlagBits::eDepth;
          if (!dtex.saveToImage(dname, depthOpts)) {
            LOG(ERROR) << "Raycaster merge depth save failed";
          }
        }
      });
    }
  }
}

void ZVulkanImgRaycasterPipelineContext::renderFastPath(Z3DRendererBase& renderer,
                                                        const RenderBatch& batch,
                                                        const ImgRaycasterPayload& payload,
                                                        const vk::Viewport& viewport,
                                                        const vk::Rect2D& scissor,
                                                        vk::raii::CommandBuffer& cmd,
                                                        FastPipelineVariant variant,
                                                        const CompositingConfig& composite)
{
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  processEventsAndMaybeCancel(cancellationToken);
  switch (variant) {
    case FastPipelineVariant::Volume:
      renderFastVolume(renderer, batch, payload, viewport, scissor, cmd, composite);
      break;
    case FastPipelineVariant::Image2D:
      renderFastImage2D(renderer, batch, payload, viewport, scissor, cmd, composite);
      break;
    case FastPipelineVariant::Slice2D:
      renderFastSlice2D(renderer, batch, payload, viewport, scissor, cmd, composite);
      break;
  }
}

void ZVulkanImgRaycasterPipelineContext::renderFastImage2D(Z3DRendererBase& renderer,
                                                           const RenderBatch& batch,
                                                           const ImgRaycasterPayload& payload,
                                                           const vk::Viewport& viewport,
                                                           const vk::Rect2D& scissor,
                                                           vk::raii::CommandBuffer& cmd,
                                                           const CompositingConfig& composite)
{
  const size_t channelCount = payload.visibleChannels.size();
  if (channelCount == 0) {
    return;
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);

  const ImgCompositingMode sanitizedMode = composite.mode;
  const bool resultOpaque = composite.resultOpaque;

  CHECK(payload.image) << "Vulkan img raycaster missing image context.";
  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster 2D fast path: payload missing transferFunctions vector (fatal)";
  const auto& transferFunctions = *payload.transferFunctions;

  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster 2D fast color attachment");
    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    info.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                              attachment.clearValue.color.g,
                                                                              attachment.clearValue.color.b,
                                                                              attachment.clearValue.color.a})};
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

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster 2D fast depth attachment");
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(texture);
    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    info.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil =
      vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
    info.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    info.dstStage = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    info.srcAccess = {};
    info.dstAccess =
      vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    info.aspect = attachAspect;
    info.trackingTexture = &texture;
    return info;
  };

  std::vector<ZVulkanAttachmentInfo> finalColorAttachments;
  finalColorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      finalColorAttachments.push_back(*info);
    }
  }

  std::optional<ZVulkanAttachmentInfo> finalDepthAttachment;
  if (batch.pass.depthAttachment) {
    finalDepthAttachment = buildDepthAttachment(*batch.pass.depthAttachment);
  }

  glm::uvec2 outputSize = payload.outputSize;
  if (outputSize.x == 0u || outputSize.y == 0u) {
    const auto& viewportState = renderer.frameState().viewport;
    outputSize = glm::uvec2(std::max<uint32_t>(1u, viewportState.z), std::max<uint32_t>(1u, viewportState.w));
  }

  const auto& viewState = renderer.viewState();
  const size_t eyeIndex = std::min<size_t>(static_cast<size_t>(batch.eye), viewState.eyes.size() - 1);
  const auto& eyeState = viewState.eyes[eyeIndex];
  const glm::mat4 projectionView = eyeState.projectionMatrix * eyeState.viewMatrix;

  vulkan::AttachmentFormats finalFormats = vulkan::extractAttachmentFormats(batch);

  const uint32_t vertexCount = static_cast<uint32_t>(payload.entryPositions.size());
  CHECK_GT(vertexCount, 0u) << "Raycaster 2D fast path missing vertex data.";

  if (channelCount == 1) {
    const size_t channelIndex = payload.visibleChannels.front();
    CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
      << "Missing transfer function for channel " << channelIndex;

    m_backend.validateFormatsOrCrash(finalFormats, "img raycaster 2d fast path");

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t imgGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& imageTex = ensureImage2DTexture(resources, channelImage, imgGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
    updateChannelImage2DDescriptors(resources, imageTex, transferTex);

    FastPipelineKey pipelineKey;
    pipelineKey.variant = FastPipelineVariant::Image2D;
    pipelineKey.mode = sanitizedMode;
    pipelineKey.resultOpaque = resultOpaque;
    pipelineKey.depthEnabled = false;
    pipelineKey.colorFormats = finalFormats.colorFormats;
    pipelineKey.depthFormat = finalFormats.depthFormat;
    PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

    CHECK(resources.image2DDescriptor != nullptr) << "Raycaster 2D fast path missing descriptor";
    std::vector<vk::DescriptorSet> descriptorSets{resources.image2DDescriptor->descriptorSet()};

    struct Image2DPush
    {
      glm::mat4 projectionView;
    } pc{projectionView};

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = scissor;
    spec.viewports = {viewport};
    spec.scissors = {scissor};
    spec.colorAttachments = finalColorAttachments;
    spec.depthStencilAttachment = finalDepthAttachment;
    spec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    spec.descriptorSets = std::move(descriptorSets);
    spec.descriptorSetFirst = 0;
    spec.expectedDescriptorSetCount = 1;
    spec.vertexBuffers = {m_entryVertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.pushConstantsData = &pc;
    spec.pushConstantsSize = sizeof(pc);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    spec.requirePushConstants = true;
    spec.instanceCount = 1;
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      const uint32_t indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      spec.indexBuffer = m_entryIndexBuffer->buffer();
      spec.indexOffset = 0;
      spec.indexType = vk::IndexType::eUint32;
      spec.indexCount = indexCount;
      spec.firstIndex = 0;
      spec.vertexOffset = 0;
      spec.firstInstance = 0;
    } else {
      spec.vertexCount = vertexCount;
      spec.firstVertex = 0;
      spec.firstInstance = 0;
    }

    recorder.recordGraphicsPass(spec);
    return;
  }

  auto* layerLease = payload.channelLayerLease ? payload.channelLayerLease.get() : nullptr;
  CHECK(layerLease && layerLease->hasVulkanImage())
    << "2D fast path requires layer array lease for multi-channel output.";

  ZVulkanTexture* layerColor = layerLease->colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease->depthAttachmentTexture();
  CHECK(layerColor) << "Layer array color attachment unavailable for 2D fast path.";
  CHECK(layerDepth) << "Layer array depth attachment unavailable for 2D fast path.";

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  layerFormats.depthFormat = layerDepth->format();

  FastPipelineKey layerKey;
  layerKey.variant = FastPipelineVariant::Image2D;
  layerKey.mode = sanitizedMode;
  layerKey.resultOpaque = resultOpaque;
  layerKey.depthEnabled = false;
  layerKey.colorFormats = layerFormats.colorFormats;
  layerKey.depthFormat = layerFormats.depthFormat;
  PipelineInstance& layerPipeline = ensureFastPipeline(layerKey);

  vk::Viewport layerViewport = viewport;
  vk::Rect2D layerRect = scissor;

  auto tLayers = m_backend.beginGpuScope("ray2d_layers");
  for (size_t order = 0; order < channelCount; ++order) {
    const size_t channelIndex = payload.visibleChannels[order];
    CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
      << "Missing transfer function for channel " << channelIndex;

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t imgGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& imageTex = ensureImage2DTexture(resources, channelImage, imgGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
    updateChannelImage2DDescriptors(resources, imageTex, transferTex);

    auto colorView = layerColor->layerImageView(static_cast<uint32_t>(order));
    if (colorView == vk::ImageView{}) {
      colorView = layerColor->imageView();
    }
    ZVulkanAttachmentInfo colorAttachment{};
    colorAttachment.image = layerColor->image();
    colorAttachment.view = colorView;
    colorAttachment.format = layerColor->format();
    colorAttachment.initialLayout = layerColor->layout();
    colorAttachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    colorAttachment.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    colorAttachment.srcAccess = {};
    colorAttachment.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    colorAttachment.aspect = vk::ImageAspectFlagBits::eColor;
    colorAttachment.trackingTexture = layerColor;

    auto depthView = layerDepth->layerImageView(static_cast<uint32_t>(order));
    if (depthView == vk::ImageView{}) {
      depthView = layerDepth->imageView();
    }
    CHECK(depthView != vk::ImageView{}) << "Layer depth attachment view missing for 2D fast path.";
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
    ZVulkanAttachmentInfo depthAttachment{};
    depthAttachment.image = layerDepth->image();
    depthAttachment.view = depthView;
    depthAttachment.format = layerDepth->format();
    depthAttachment.initialLayout = layerDepth->layout();
    depthAttachment.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depthAttachment.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
    depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    depthAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    depthAttachment.dstStage =
      vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    depthAttachment.srcAccess = {};
    depthAttachment.dstAccess =
      vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depthAttachment.aspect = attachAspect;
    depthAttachment.trackingTexture = layerDepth;

    CHECK(resources.image2DDescriptor != nullptr) << "Raycaster 2D layered path missing descriptor";
    std::vector<vk::DescriptorSet> descriptorSets{resources.image2DDescriptor->descriptorSet()};
    struct Image2DPush
    {
      glm::mat4 projectionView;
    } pcLayer{projectionView};

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = layerRect;
    spec.viewports = {layerViewport};
    spec.scissors = {layerRect};
    spec.colorAttachments = {colorAttachment};
    spec.depthStencilAttachment = depthAttachment;
    spec.pipelineHandle = layerPipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = layerPipeline.pipeline->pipelineLayoutHandle();
    spec.descriptorSets = std::move(descriptorSets);
    spec.descriptorSetFirst = 0;
    spec.expectedDescriptorSetCount = 1;
    spec.vertexBuffers = {m_entryVertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.pushConstantsData = &pcLayer;
    spec.pushConstantsSize = sizeof(pcLayer);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    spec.requirePushConstants = true;
    spec.instanceCount = 1;
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      const uint32_t indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      spec.indexBuffer = m_entryIndexBuffer->buffer();
      spec.indexOffset = 0;
      spec.indexType = vk::IndexType::eUint32;
      spec.indexCount = indexCount;
      spec.firstIndex = 0;
      spec.vertexOffset = 0;
      spec.firstInstance = 0;
    } else {
      spec.vertexCount = vertexCount;
      spec.firstVertex = 0;
      spec.firstInstance = 0;
    }

    recorder.recordGraphicsPass(spec);
  }
  if (tLayers) {
    m_backend.endGpuScope(*tLayers);
  }

  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  const auto [readLayout, descAspect] = depthReadDescriptorLayoutAndAspect(*layerDepth);
  const auto barrierAspect = depthReadBarrierAspect(*layerDepth);
  layerDepth->transitionLayout(cmd, layerDepth->layout(), readLayout, barrierAspect);
  layerDepth->setDescriptorLayout(readLayout);

  bindMergeDescriptor(*layerColor, layerDepth);

  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = composite.maxProjectionMerge;
  mergeKey.resultOpaque = resultOpaque;
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;
  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);

  std::vector<vk::DescriptorSet> mergeSets{m_mergeDescriptor->descriptorSet()};

  ZVulkanGraphicsPassSpec mergeSpec{};
  mergeSpec.renderArea = scissor;
  mergeSpec.viewports = {viewport};
  mergeSpec.scissors = {scissor};
  mergeSpec.colorAttachments = finalColorAttachments;
  mergeSpec.depthStencilAttachment = finalDepthAttachment;
  mergeSpec.pipelineHandle = mergePipeline.pipeline->pipelineHandle();
  mergeSpec.pipelineLayoutHandle = mergePipeline.pipeline->pipelineLayoutHandle();
  mergeSpec.descriptorSets = std::move(mergeSets);
  mergeSpec.descriptorSetFirst = 0;
  mergeSpec.expectedDescriptorSetCount = 1;
  mergeSpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
  mergeSpec.vertexOffsets = {0};
  mergeSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  mergeSpec.instanceCount = 1;

  recorder.recordGraphicsPass(mergeSpec);
}

void ZVulkanImgRaycasterPipelineContext::renderFastSlice2D(Z3DRendererBase& renderer,
                                                           const RenderBatch& batch,
                                                           const ImgRaycasterPayload& payload,
                                                           const vk::Viewport& viewport,
                                                           const vk::Rect2D& scissor,
                                                           vk::raii::CommandBuffer& cmd,
                                                           const CompositingConfig& composite)
{
  const size_t channelCount = payload.visibleChannels.size();
  if (channelCount == 0) {
    return;
  }

  const ImgCompositingMode sanitizedMode = composite.mode;
  const bool resultOpaque = composite.resultOpaque;

  CHECK(payload.image) << "Vulkan img raycaster missing image context.";
  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster slice fast path: payload missing transferFunctions vector (fatal)";
  const auto& transferFunctions = *payload.transferFunctions;

  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster slice fast color attachment");
    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    info.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                              attachment.clearValue.color.g,
                                                                              attachment.clearValue.color.b,
                                                                              attachment.clearValue.color.a})};
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

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster slice fast depth attachment");
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(texture);
    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    info.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil =
      vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
    info.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    info.dstStage = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    info.srcAccess = {};
    info.dstAccess =
      vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    info.aspect = attachAspect;
    info.trackingTexture = &texture;
    return info;
  };

  std::vector<ZVulkanAttachmentInfo> finalColorAttachments;
  finalColorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      finalColorAttachments.push_back(*info);
    }
  }

  std::optional<ZVulkanAttachmentInfo> finalDepthAttachment;
  if (batch.pass.depthAttachment) {
    finalDepthAttachment = buildDepthAttachment(*batch.pass.depthAttachment);
  }

  glm::uvec2 outputSize = payload.outputSize;
  if (outputSize.x == 0u || outputSize.y == 0u) {
    const auto& viewportState = renderer.frameState().viewport;
    outputSize = glm::uvec2(std::max<uint32_t>(1u, viewportState.z), std::max<uint32_t>(1u, viewportState.w));
  }

  const auto& viewState = renderer.viewState();
  const size_t eyeIndex = std::min<size_t>(static_cast<size_t>(batch.eye), viewState.eyes.size() - 1);
  const auto& eyeState = viewState.eyes[eyeIndex];
  const glm::mat4 projectionView = eyeState.projectionMatrix * eyeState.viewMatrix;
  const glm::mat4 viewMatrix = eyeState.viewMatrix;

  vulkan::AttachmentFormats finalFormats = vulkan::extractAttachmentFormats(batch);

  const uint32_t vertexCount = static_cast<uint32_t>(payload.entryPositions.size());
  CHECK_GT(vertexCount, 0u) << "Raycaster slice fast path missing vertex data.";

  ZVulkanPipelineCommandRecorder recorder(cmd);

  if (channelCount == 1) {
    const size_t channelIndex = payload.visibleChannels.front();
    CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
      << "Missing transfer function for channel " << channelIndex;

    m_backend.validateFormatsOrCrash(finalFormats, "img raycaster slice fast path");

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
    updateChannelSliceDescriptors(resources, volumeTex, transferTex);

    FastPipelineKey pipelineKey;
    pipelineKey.variant = FastPipelineVariant::Slice2D;
    pipelineKey.mode = sanitizedMode;
    pipelineKey.resultOpaque = resultOpaque;
    pipelineKey.depthEnabled = false;
    pipelineKey.colorFormats = finalFormats.colorFormats;
    pipelineKey.depthFormat = finalFormats.depthFormat;
    PipelineInstance& pipeline = ensureFastPipeline(pipelineKey);

    CHECK(resources.sliceDescriptor != nullptr) << "Raycaster slice fast path missing descriptor";
    std::vector<vk::DescriptorSet> descriptorSets{resources.sliceDescriptor->descriptorSet()};
    struct SlicePush
    {
      glm::mat4 projectionView;
      glm::mat4 view;
    } pc{projectionView, viewMatrix};

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = scissor;
    spec.viewports = {viewport};
    spec.scissors = {scissor};
    spec.colorAttachments = finalColorAttachments;
    spec.depthStencilAttachment = finalDepthAttachment;
    spec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    spec.descriptorSets = std::move(descriptorSets);
    spec.descriptorSetFirst = 0;
    spec.expectedDescriptorSetCount = 1;
    spec.vertexBuffers = {m_entryVertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.pushConstantsData = &pc;
    spec.pushConstantsSize = sizeof(pc);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    spec.requirePushConstants = true;
    spec.instanceCount = 1;
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      const uint32_t indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      spec.indexBuffer = m_entryIndexBuffer->buffer();
      spec.indexOffset = 0;
      spec.indexType = vk::IndexType::eUint32;
      spec.indexCount = indexCount;
      spec.firstIndex = 0;
      spec.vertexOffset = 0;
      spec.firstInstance = 0;
    } else {
      spec.vertexCount = vertexCount;
      spec.firstVertex = 0;
      spec.firstInstance = 0;
    }

    recorder.recordGraphicsPass(spec);
    return;
  }

  auto* layerLease = payload.channelLayerLease ? payload.channelLayerLease.get() : nullptr;
  CHECK(layerLease && layerLease->hasVulkanImage())
    << "Slice fast path requires layer array lease for multi-channel output.";

  ZVulkanTexture* layerColor = layerLease->colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease->depthAttachmentTexture();
  CHECK(layerColor) << "Layer array color attachment unavailable for slice fast path.";
  CHECK(layerDepth) << "Layer array depth attachment unavailable for slice fast path.";

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  layerFormats.depthFormat = layerDepth->format();

  FastPipelineKey layerKey;
  layerKey.variant = FastPipelineVariant::Slice2D;
  layerKey.mode = sanitizedMode;
  layerKey.resultOpaque = resultOpaque;
  layerKey.depthEnabled = false;
  layerKey.colorFormats = layerFormats.colorFormats;
  layerKey.depthFormat = layerFormats.depthFormat;
  PipelineInstance& layerPipeline = ensureFastPipeline(layerKey);

  vk::Viewport layerViewport = viewport;
  vk::Rect2D layerRect = scissor;

  auto tLayers = m_backend.beginGpuScope("ray_slice_layers");
  for (size_t order = 0; order < channelCount; ++order) {
    const size_t channelIndex = payload.visibleChannels[order];
    CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
      << "Missing transfer function for channel " << channelIndex;

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
    ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
    ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunctions[channelIndex]);
    updateChannelSliceDescriptors(resources, volumeTex, transferTex);

    auto colorView = layerColor->layerImageView(static_cast<uint32_t>(order));
    if (colorView == vk::ImageView{}) {
      colorView = layerColor->imageView();
    }
    ZVulkanAttachmentInfo colorAttachment{};
    colorAttachment.image = layerColor->image();
    colorAttachment.view = colorView;
    colorAttachment.format = layerColor->format();
    colorAttachment.initialLayout = layerColor->layout();
    colorAttachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    colorAttachment.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    colorAttachment.srcAccess = {};
    colorAttachment.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    colorAttachment.aspect = vk::ImageAspectFlagBits::eColor;
    colorAttachment.trackingTexture = layerColor;

    auto depthView = layerDepth->layerImageView(static_cast<uint32_t>(order));
    if (depthView == vk::ImageView{}) {
      depthView = layerDepth->imageView();
    }
    CHECK(depthView != vk::ImageView{}) << "Layer depth attachment view missing for slice layered path.";
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
    ZVulkanAttachmentInfo depthAttachment{};
    depthAttachment.image = layerDepth->image();
    depthAttachment.view = depthView;
    depthAttachment.format = layerDepth->format();
    depthAttachment.initialLayout = layerDepth->layout();
    depthAttachment.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depthAttachment.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
    depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    depthAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    depthAttachment.dstStage =
      vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    depthAttachment.srcAccess = {};
    depthAttachment.dstAccess =
      vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depthAttachment.aspect = attachAspect;
    depthAttachment.trackingTexture = layerDepth;

    CHECK(resources.sliceDescriptor != nullptr) << "Raycaster slice layered path missing descriptor";
    std::vector<vk::DescriptorSet> descriptorSets{resources.sliceDescriptor->descriptorSet()};
    struct SlicePush
    {
      glm::mat4 projectionView;
      glm::mat4 view;
    } pcLayer{projectionView, viewMatrix};

    ZVulkanGraphicsPassSpec spec{};
    spec.renderArea = layerRect;
    spec.viewports = {layerViewport};
    spec.scissors = {layerRect};
    spec.colorAttachments = {colorAttachment};
    spec.depthStencilAttachment = depthAttachment;
    spec.pipelineHandle = layerPipeline.pipeline->pipelineHandle();
    spec.pipelineLayoutHandle = layerPipeline.pipeline->pipelineLayoutHandle();
    spec.descriptorSets = std::move(descriptorSets);
    spec.descriptorSetFirst = 0;
    spec.expectedDescriptorSetCount = 1;
    spec.vertexBuffers = {m_entryVertexBuffer->buffer()};
    spec.vertexOffsets = {0};
    spec.pushConstantsData = &pcLayer;
    spec.pushConstantsSize = sizeof(pcLayer);
    spec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex;
    spec.requirePushConstants = true;
    spec.instanceCount = 1;
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      const uint32_t indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      spec.indexBuffer = m_entryIndexBuffer->buffer();
      spec.indexOffset = 0;
      spec.indexType = vk::IndexType::eUint32;
      spec.indexCount = indexCount;
      spec.firstIndex = 0;
      spec.vertexOffset = 0;
      spec.firstInstance = 0;
    } else {
      spec.vertexCount = vertexCount;
      spec.firstVertex = 0;
      spec.firstInstance = 0;
    }

    recorder.recordGraphicsPass(spec);
  }
  if (tLayers) {
    m_backend.endGpuScope(*tLayers);
  }

  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  const auto [readLayout, descAspect] = depthReadDescriptorLayoutAndAspect(*layerDepth);
  const auto barrierAspect = depthReadBarrierAspect(*layerDepth);
  layerDepth->transitionLayout(cmd, layerDepth->layout(), readLayout, barrierAspect);
  layerDepth->setDescriptorLayout(readLayout);

  bindMergeDescriptor(*layerColor, layerDepth);

  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = composite.maxProjectionMerge;
  mergeKey.resultOpaque = resultOpaque;
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;
  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);

  std::vector<vk::DescriptorSet> mergeSets{m_mergeDescriptor->descriptorSet()};

  ZVulkanGraphicsPassSpec mergeSpec{};
  mergeSpec.renderArea = scissor;
  mergeSpec.viewports = {viewport};
  mergeSpec.scissors = {scissor};
  mergeSpec.colorAttachments = finalColorAttachments;
  mergeSpec.depthStencilAttachment = finalDepthAttachment;
  mergeSpec.pipelineHandle = mergePipeline.pipeline->pipelineHandle();
  mergeSpec.pipelineLayoutHandle = mergePipeline.pipeline->pipelineLayoutHandle();
  mergeSpec.descriptorSets = std::move(mergeSets);
  mergeSpec.descriptorSetFirst = 0;
  mergeSpec.expectedDescriptorSetCount = 1;
  mergeSpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
  mergeSpec.vertexOffsets = {0};
  mergeSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  mergeSpec.instanceCount = 1;

  recorder.recordGraphicsPass(mergeSpec);
}

void ZVulkanImgRaycasterPipelineContext::renderProgressivePath(Z3DRendererBase& renderer,
                                                               const RenderBatch& batch,
                                                               const ImgRaycasterPayload& payload,
                                                               const vk::Viewport& viewport,
                                                               const vk::Rect2D& scissor,
                                                               vk::raii::CommandBuffer& cmd,
                                                               const CompositingConfig& composite,
                                                               bool skipBlockIdPass,
                                                               std::optional<Finalization> finalizeAfterProgressive)
{
  auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  processEventsAndMaybeCancel(cancellationToken);
  const uint32_t channelCount = static_cast<uint32_t>(payload.visibleChannels.size());
  if (channelCount == 0u) {
    return;
  }

  // GL parity: fast preview indicated by channelIndexRaw < 0.
  if (static_cast<int32_t>(payload.channelIndexRaw) < 0) {
    ensureProgressiveLayerTargets(payload.outputSize, channelCount, payload.progressiveGeneration, cmd);
    renderFastVolume(renderer, batch, payload, viewport, scissor, cmd, composite);
    // Request renderer to flip channelIdx from -1 to 0 for subsequent rounds (GL parity)
    if (payload.streamKey != 0) {
      m_pendingFinalization = Finalization{.streamKey = payload.streamKey,
                                           .eye = batch.eye,
                                           .lastRound = false,
                                           .channelCount = static_cast<uint32_t>(payload.visibleChannels.size())};
    }
    return;
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);

  const ImgCompositingMode sanitizedMode = composite.mode;
  const bool resultOpaque = composite.resultOpaque;
  const bool localMip = composite.localMip;

  if (!skipBlockIdPass) {
    processEventsAndMaybeCancel(cancellationToken);
    CHECK(payload.blockIdLease && payload.blockIdLease->attachments != 0)
      << "Vulkan raycaster progressive path missing block-ID lease.";
  }

  CHECK(payload.lastAccumLease && payload.currentAccumLease)
    << "Vulkan raycaster progressive path missing accumulator leases.";

  CHECK(m_imageBlockUploader) << "Vulkan raycaster progressive path missing image block uploader.";
  m_imageBlockUploader->bindToImage(*payload.image);

  // Use raw GL booking; negative means fast preview, otherwise raw channel index (validated by renderer)
  const int32_t rawIdx = payload.channelIndexRaw;
  CHECK_GE(rawIdx, 0) << "Negative channelIndexRaw (fast preview) not expected in progressive path.";
  CHECK_LT(static_cast<uint32_t>(rawIdx), channelCount) << "channelIndexRaw out of range for visibleChannels.";
  const uint32_t activeChannelIndex = static_cast<uint32_t>(rawIdx);
  CHECK_LT(activeChannelIndex, payload.visibleChannels.size());
  const size_t channelIndex = payload.visibleChannels[activeChannelIndex];

  ChannelResources& resources = ensureChannelResources(channelIndex);

  const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
  const uint64_t volGen = payload.image->volumeGeneration(channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex, volGen);
  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster progressive path: payload missing transferFunctions vector (fatal)";
  const auto& transferList = *payload.transferFunctions;
  CHECK(channelIndex < transferList.size() && transferList[channelIndex] != nullptr)
    << "Vulkan raycaster missing transfer function for channel " << channelIndex;
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferList[channelIndex]);

  // Ensure paging caches (page directory + page table) are uploaded before the first Block-ID pass.
  // Without this, the Vulkan shader may read undefined textures and fail to collect missing blocks.
  {
    ZBenchTimer uploadTimer("vulkan_upload_page_caches");
    m_imageBlockUploader->uploadPageCaches(*payload.image, channelIndex, uploadTimer);
  }

  auto* entryTexture = payload.entryExitLease ? payload.entryExitLease->colorAttachment(0) : nullptr;
  auto* lastColor = payload.lastAccumLease->colorAttachment(0);
  auto* lastDepth = payload.lastAccumLease->colorAttachment(1);
  auto* currentColor = payload.currentAccumLease->colorAttachment(0);
  auto* currentDepth = payload.currentAccumLease->colorAttachment(1);

  CHECK(entryTexture && lastColor && lastDepth && currentColor && currentDepth)
    << "Vulkan raycaster progressive path missing required textures.";

  // GL parity: on round 0 for a channel, clear the last accumulators before Block-ID pass.
  if (payload.roundIndexRaw == 0) {
    // Clear lastColor (RGBA) to zeros
    {
      const auto clear = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
      const auto range = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, 1u};
      lastColor->transitionLayout(cmd, lastColor->layout(), vk::ImageLayout::eTransferDstOptimal);
      cmd.clearColorImage(lastColor->image(), vk::ImageLayout::eTransferDstOptimal, clear, range);
      lastColor->transitionLayout(cmd, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
      lastColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
    }
    // Clear lastDepth (accumulator stored as color) to zeros
    {
      const auto clear = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
      const auto range = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, 1u};
      lastDepth->transitionLayout(cmd, lastDepth->layout(), vk::ImageLayout::eTransferDstOptimal);
      cmd.clearColorImage(lastDepth->image(), vk::ImageLayout::eTransferDstOptimal, clear, range);
      lastDepth->transitionLayout(cmd, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
      lastDepth->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
    }
  }

  const glm::uvec2 outputSize = payload.outputSize;
  CHECK(outputSize.x > 0u && outputSize.y > 0u) << "Vulkan raycaster progressive path requires non-zero output size.";

  if (FLAGS_atlas_vk_debug_raycaster_dump) {
    VLOG(1) << fmt::format(
      "Raycaster draw ctx: viewport=({}, {}, {}x{}) scissor=({}, {}, {}x{}) out={}x{} channels={} channelIdxRaw={} channel={} ",
      static_cast<int>(viewport.x),
      static_cast<int>(viewport.y),
      static_cast<int>(viewport.width),
      static_cast<int>(viewport.height),
      scissor.offset.x,
      scissor.offset.y,
      scissor.extent.width,
      scissor.extent.height,
      static_cast<int>(outputSize.x),
      static_cast<int>(outputSize.y),
      payload.visibleChannels.size(),
      rawIdx,
      channelIndex);
  }

  ZVulkanTexture* layerColor = nullptr;
  ZVulkanTexture* layerDepth = nullptr;
  ensureProgressiveLayerTargets(outputSize, channelCount, payload.progressiveGeneration, cmd);
  layerColor = m_progressiveLayerColor.get();
  layerDepth = m_progressiveLayerDepth.get();
  CHECK(layerColor && layerDepth) << "Vulkan raycaster progressive path missing layer-array targets.";

  const auto& viewState = renderer.viewState();
  const auto& sceneState = renderer.sceneState();
  const auto& monoEyeState = viewState.eyes[static_cast<size_t>(Z3DEye::MonoEye)];
  float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
  float farClip = viewState.farClip;
  glm::vec2 pixelEyeSpaceSize =
    monoEyeState.frustumNearPlaneSize / glm::vec2(std::max(1u, outputSize.x), std::max(1u, outputSize.y));
  float zeToScreenPixelVoxelSize =
    -std::min(pixelEyeSpaceSize.x, pixelEyeSpaceSize.y) / nearClip * sceneState.devicePixelRatio;
  float zeToZW_a = farClip * nearClip / (farClip - nearClip);
  float zeToZW_b = 0.5f * (farClip + nearClip) / (farClip - nearClip) + 0.5f;

  // Ensure previous-round data is ready for sampling (all are color images).
  entryTexture->transitionLayout(cmd, entryTexture->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  lastColor->transitionLayout(cmd, lastColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  // lastDepth is a color-format accumulation buffer (not a depth image). Sample as shader-read color.
  lastDepth->transitionLayout(cmd, lastDepth->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  entryTexture->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  lastColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  lastDepth->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  if (FLAGS_atlas_vk_debug_raycaster_dump) {
    auto dumpTex = [](const char* tag, ZVulkanTexture* t) {
      if (!t) {
        return;
      }
      VLOG(1) << fmt::format("Bind {}: tex=0x{:x} fmt={} size={}x{}x{} layout={} descrLayout={} mips={} layers={}",
                             tag,
                             reinterpret_cast<uint64_t>(t),
                             enumOrUnderlying(t->format(), 16),
                             t->width(),
                             t->height(),
                             t->depth(),
                             enumOrUnderlying(t->layout(), 16),
                             enumOrUnderlying(t->descriptorLayout(), 16),
                             t->mipLevels(),
                             t->arrayLayers());
    };
    dumpTex("entryExit", entryTexture);
    dumpTex("lastDepth", lastDepth);
    dumpTex("lastColor", lastColor);
    dumpTex("layerColor", layerColor);
    dumpTex("layerDepth", layerDepth);
    dumpTex("pageDirectory", resources.boundPageDirectoryTex);
    dumpTex("pageTable", resources.boundPageTableTex);
    dumpTex("imageCache", resources.boundImageCacheTex);
    dumpTex("volume", resources.boundVolumeTex);
    dumpTex("transfer", resources.boundTransferTex);
  }

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

  if (!updatePageDescriptors(resources,
                             payload,
                             *entryTexture,
                             *lastDepth,
                             *lastColor,
                             volumeTex,
                             transferTex,
                             *payload.image,
                             channelIndex,
                             zeToScreenPixelVoxelSize,
                             /*freshOverrideDescriptors=*/false)) {
    return;
  }

  // GL parity: bind current textures immediately for the Block-ID pass.
  // Persistent static descriptors are allocated after frame completion; to
  // avoid stale bindings when switching channels within the same command buffer,
  // always prefer a per-draw override static descriptor here.
  const bool preferOverrideStaticGlobal = true;

  if (preferOverrideStaticGlobal) {
    if (!resources.staticDescriptor) {
      resources.staticDescriptor = m_backend.allocateOverrideDescriptorSet(**m_progressiveStaticSetLayout);
    }
    if (resources.staticDescriptor) {
      // Ensure override static set is fully updated from current textures.
      // Use nearest clamp for integer paging resources.
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

  // ---------------- Block-ID pass ----------------
  // When skipBlockIdPass is true we have a deferred progressive-only round and
  // can reuse the previous frame's block-ID results (no draw or compaction here).
  if (!skipBlockIdPass) {
    std::vector<uint32_t> rawBlockIds;
    rawBlockIds.reserve(static_cast<size_t>(payload.blockIdLease->descriptor.size.x) *
                        payload.blockIdLease->descriptor.size.y * 4ull);

    const uint32_t blockAttachmentCount = payload.blockIdLease->attachments;
    auto* firstBlockAttachment = payload.blockIdLease->colorAttachment(0);
    CHECK(firstBlockAttachment) << "Vulkan raycaster progressive path missing block-ID attachment.";
    const vk::Format blockFormat = firstBlockAttachment->format();
    BlockIdPipelineKey blockKey{resources.levelCount, blockAttachmentCount, blockFormat};
    auto& blockPipeline = ensureBlockIdPipeline(blockKey, blockFormat);

    // Let the dynamic rendering pre-transition handle attachment layouts.
    // Ensure sampled inputs for block-ID pass are in sampled layout.
    auto ensureSampled = [&](ZVulkanTexture* tex) {
      if (!tex) {
        return;
      }
      const vk::ImageLayout desired = vk::ImageLayout::eShaderReadOnlyOptimal;
      if (tex->layout() != desired) {
        tex->transitionLayout(cmd, tex->layout(), desired);
      }
      tex->setDescriptorLayout(desired);
    };
    ensureSampled(resources.boundPageDirectoryTex);
    ensureSampled(resources.boundPageTableTex);
    ensureSampled(resources.boundImageCacheTex);
    ensureSampled(resources.boundVolumeTex);
    ensureSampled(resources.boundTransferTex);

    std::vector<ZVulkanAttachmentInfo> blockAttachments;
    blockAttachments.reserve(blockAttachmentCount);
    for (uint32_t att = 0; att < blockAttachmentCount; ++att) {
      auto* texture = payload.blockIdLease->colorAttachment(att);
      if (!texture) {
        continue;
      }
      ZVulkanAttachmentInfo attachment{};
      attachment.image = texture->image();
      attachment.view = texture->imageView();
      attachment.format = texture->format();
      // Begin from UNDEFINED; recorder will transition to COLOR_ATTACHMENT_OPTIMAL
      attachment.initialLayout = vk::ImageLayout::eUndefined;
      // Make the block-ID image ready for sampled read (texelFetch) in the compute compaction pass
      attachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
      attachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
      attachment.loadOp = vk::AttachmentLoadOp::eClear;
      attachment.storeOp = vk::AttachmentStoreOp::eStore;
      attachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
      // After rendering, transition for compute read
      attachment.dstStage = vk::PipelineStageFlagBits2::eComputeShader;
      attachment.srcAccess = {};
      attachment.dstAccess = vk::AccessFlagBits2::eShaderRead;
      attachment.aspect = vk::ImageAspectFlagBits::eColor;
      attachment.trackingTexture = texture;
      blockAttachments.push_back(attachment);
    }

    CHECK(!blockAttachments.empty()) << "Vulkan raycaster progressive path failed to prepare block-ID attachments.";

    vk::Rect2D blockRect = scissor;
    vk::Viewport blockViewport = viewport;

    std::vector<vk::DescriptorSet> blockDescriptorSets =
      collectProgressiveDescriptorSets(resources, preferOverrideStaticGlobal);
    struct RayPC
    {
      float s, i, l, a, b;
    } pcB{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};

    if (FLAGS_atlas_vk_debug_raycaster_dump) {
      VLOG(1) << fmt::format("BlockID push: samplingRate={:.6f} iso={:.6f} localMipTh={:.6f} zeA={:.6f} zeB={:.6f}",
                             pcB.s,
                             pcB.i,
                             pcB.l,
                             pcB.a,
                             pcB.b);
    }

    ZVulkanGraphicsPassSpec blockSpec{};
    blockSpec.renderArea = blockRect;
    blockSpec.viewports = {blockViewport};
    blockSpec.scissors = {blockRect};
    blockSpec.colorAttachments = blockAttachments;
    blockSpec.pipelineHandle = blockPipeline.pipeline->pipelineHandle();
    blockSpec.pipelineLayoutHandle = blockPipeline.pipeline->pipelineLayoutHandle();
    blockSpec.descriptorSets = std::move(blockDescriptorSets);
    blockSpec.descriptorSetFirst = 0;
    blockSpec.expectedDescriptorSetCount = 3;
    blockSpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
    blockSpec.vertexOffsets = {0};
    blockSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
    blockSpec.instanceCount = 1;
    blockSpec.pushConstantsData = &pcB;
    blockSpec.pushConstantsSize = sizeof(pcB);
    blockSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    blockSpec.requirePushConstants = true;

    recorder.recordGraphicsPass(blockSpec);
    if (FLAGS_atlas_vk_debug_blockid_count) {
      auto leaseRef = payload.blockIdLease;
      // Read back via end-of-frame path to guarantee ordering w.r.t GPU writes.
      // This mirrors how GL readbacks are ordered inside a frame and avoids stale data.
      if (leaseRef && leaseRef->hasVulkanImage()) {
        const uint32_t attCount = std::max<uint32_t>(1u, leaseRef->attachments);
        for (uint32_t att = 0; att < attCount; ++att) {
          ZVulkanTexture* tex = leaseRef->colorAttachment(att);
          if (!tex) {
            continue;
          }
          m_backend.requestEndOfFrameColorReadback(
            *tex,
            batch.eye,
            [att](const void* mapped, size_t bytes, vk::Format fmt, glm::uvec2 size, std::function<void()> release) {
              const uint32_t w = size.x;
              const uint32_t h = size.y;
              const size_t elems = static_cast<size_t>(w) * h * 4ull;
              if (fmt != vk::Format::eR32G32B32A32Uint) {
                LOG(WARNING) << "BlockID debug readback unexpected format: " << enumOrUnderlying(fmt, 16);
              }
              if (bytes < elems * sizeof(uint32_t)) {
                LOG(ERROR) << "BlockID debug readback buffer too small: bytes=" << bytes
                           << " need=" << (elems * sizeof(uint32_t));
                if (release) {
                  release();
                }
                return;
              }
              const uint32_t* data = static_cast<const uint32_t*>(mapped);
              size_t nonZero = 0;
              size_t validIds = 0;
              uint32_t rawMin = std::numeric_limits<uint32_t>::max();
              uint32_t rawMax = 0u;
              uint32_t validMin = std::numeric_limits<uint32_t>::max();
              uint32_t validMax = 0u;
              for (size_t i = 0; i < elems; ++i) {
                const uint32_t v = data[i];
                if (v != 0u) {
                  ++nonZero;
                }
                if (v != 0u && v != 0xFFFFFFFFu) {
                  ++validIds;
                  if (v < validMin) {
                    validMin = v;
                  }
                  if (v > validMax) {
                    validMax = v;
                  }
                }
                if (v < rawMin) {
                  rawMin = v;
                }
                if (v > rawMax) {
                  rawMax = v;
                }
              }
              if (validIds == 0) {
                validMin = 0u;
                validMax = 0u;
              }
              VLOG(1) << fmt::format(
                "BlockID debug: att={} nonZero={} validIds={} of {} ({}x{} RGBA32UI) rawMin={} rawMax={} validMin={} validMax={}",
                att,
                nonZero,
                validIds,
                elems,
                w,
                h,
                rawMin,
                rawMax,
                validMin,
                validMax);
              if (release) {
                release();
              }
            });
        }
      }
    }
    // Compact block-ID image(s) via compute and schedule a small buffer readback
    processEventsAndMaybeCancel(cancellationToken);
    recordBlockIdCompaction(renderer, batch, payload, cmd);
  } // end block-ID stage

  // Streaming continues; compaction finalization is deferred to a post-fence callback that sets m_pendingFinalization.

  vulkan::AttachmentFormats progressiveFormats;
  progressiveFormats.colorFormats = {currentColor->format(), currentDepth->format()};

  ProgressivePipelineKey progressiveKey{currentColor->format(),
                                        currentDepth->format(),
                                        sanitizedMode,
                                        localMip,
                                        resultOpaque};
  progressiveKey.levelCount = resources.levelCount;
  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format(
      "ensureProgressivePipeline: color0Fmt={} color1Fmt={} levelCount={} mode={} localMip={} opaque={}",
      enumOrUnderlying(progressiveKey.colorFormat, 16),
      enumOrUnderlying(progressiveKey.accumulatorFormat, 16),
      progressiveKey.levelCount,
      static_cast<int>(progressiveKey.mode),
      progressiveKey.localMip ? 1 : 0,
      progressiveKey.resultOpaque ? 1 : 0);
  }
  auto& progressivePipeline = ensureProgressivePipeline(progressiveKey, progressiveFormats);

  ZVulkanAttachmentInfo colorAttachment{};
  colorAttachment.image = currentColor->image();
  colorAttachment.view = currentColor->imageView();
  colorAttachment.format = currentColor->format();
  // Use tracked layout; pre-transition recorded above keeps validation consistent
  colorAttachment.initialLayout = currentColor->layout();
  colorAttachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
  colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
  colorAttachment.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  colorAttachment.srcAccess = {};
  colorAttachment.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
  colorAttachment.aspect = vk::ImageAspectFlagBits::eColor;
  colorAttachment.trackingTexture = currentColor;

  ZVulkanAttachmentInfo accumAttachment{};
  accumAttachment.image = currentDepth->image();
  accumAttachment.view = currentDepth->imageView();
  accumAttachment.format = currentDepth->format();
  accumAttachment.initialLayout = currentDepth->layout();
  accumAttachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  accumAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
  accumAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  accumAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  accumAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
  accumAttachment.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  accumAttachment.srcAccess = {};
  accumAttachment.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
  accumAttachment.aspect = vk::ImageAspectFlagBits::eColor;
  accumAttachment.trackingTexture = currentDepth;

  std::vector<ZVulkanAttachmentInfo> progressiveAttachments;
  progressiveAttachments.push_back(colorAttachment);
  progressiveAttachments.push_back(accumAttachment);

  // Collect descriptor sets for progressive raycast. Expect exactly 3.
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "about to collect progressive descriptor sets";
  }
  if (VLOG_IS_ON(2)) {
    VLOG(2) << fmt::format("preferOverrideStaticGlobal={} hasOverrideStatic={} hasPersistentStatic={}",
                           preferOverrideStaticGlobal ? 1 : 0,
                           resources.staticDescriptor != nullptr ? 1 : 0,
                           resources.persistentStaticDescriptor != nullptr ? 1 : 0);
  }
  std::vector<vk::DescriptorSet> progressiveSets =
    collectProgressiveDescriptorSets(resources, preferOverrideStaticGlobal);
  CHECK_EQ(progressiveSets.size(), 3u) << "Progressive raycaster requires exactly 3 descriptor sets";
  struct RayPC2
  {
    float s, i, l, a, b;
  } pcP{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};

  ZVulkanGraphicsPassSpec progressiveSpec{};
  progressiveSpec.renderArea = scissor;
  progressiveSpec.viewports = {viewport};
  progressiveSpec.scissors = {scissor};
  progressiveSpec.colorAttachments = progressiveAttachments;
  progressiveSpec.pipelineHandle = progressivePipeline.pipeline->pipelineHandle();
  progressiveSpec.pipelineLayoutHandle = progressivePipeline.pipeline->pipelineLayoutHandle();
  progressiveSpec.descriptorSets = std::move(progressiveSets);
  progressiveSpec.descriptorSetFirst = 0;
  progressiveSpec.expectedDescriptorSetCount = 3;
  progressiveSpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
  progressiveSpec.vertexOffsets = {0};
  progressiveSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  progressiveSpec.instanceCount = 1;
  progressiveSpec.pushConstantsData = &pcP;
  progressiveSpec.pushConstantsSize = sizeof(pcP);
  progressiveSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  progressiveSpec.requirePushConstants = true;

  if (FLAGS_atlas_vk_debug_raycaster_dump) {
    // Not available here directly: use unit estimate; detailed logging already in page UBO
    VLOG(1) << fmt::format("Raycaster push: samplingRate={:.6f} iso={:.6f} localMipTh={:.6f} zeA={:.6f} zeB={:.6f}",
                           pcP.s,
                           pcP.i,
                           pcP.l,
                           pcP.a,
                           pcP.b);
  }

  if (VLOG_IS_ON(2)) {
    const auto& sets = progressiveSpec.descriptorSets;
    VLOG(2) << fmt::format(
      "record progressive pass: pipeline=0x{:x} layout=0x{:x} ds0=0x{:x} ds1=0x{:x} ds2=0x{:x}",
      reinterpret_cast<uintptr_t>(static_cast<VkPipeline>(progressivePipeline.pipeline->pipeline())),
      reinterpret_cast<uintptr_t>(static_cast<VkPipelineLayout>(progressivePipeline.pipeline->pipelineLayout())),
      reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(sets.size() > 0 ? sets[0] : vk::DescriptorSet{})),
      reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(sets.size() > 1 ? sets[1] : vk::DescriptorSet{})),
      reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(sets.size() > 2 ? sets[2] : vk::DescriptorSet{})));
  }
  recorder.recordGraphicsPass(progressiveSpec);
  processEventsAndMaybeCancel(cancellationToken);

  currentColor->transitionLayout(cmd, currentColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  // currentDepth is also a color-format accumulation buffer. Sample as shader-read color.
  currentDepth->transitionLayout(cmd, currentDepth->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  currentColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  currentDepth->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  // Copy the current accumulation into the persistent layer array slice
  // Clear only the active slice before recording the layer copy pass. Vulkan's
  // loadOp clear applies to the full subresource range referenced by the
  // attachment view; however, our layout tracking transitions operate on the
  // entire image. When multiple channels are accumulated in the same array this
  // can inadvertently wipe previously rendered slices (observed as the final
  // image disappearing after the third channel). Mirror the GL behaviour by
  // issuing explicit per-slice clears via the transfer path and then rebind the
  // slice for color/depth rendering.
  const vk::ImageSubresourceRange colorSlice{vk::ImageAspectFlagBits::eColor, 0u, 1u, activeChannelIndex, 1u};
  layerColor->transitionLayout(cmd,
                               layerColor->layout(),
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ImageAspectFlagBits::eColor);
  cmd.clearColorImage(layerColor->image(),
                      vk::ImageLayout::eTransferDstOptimal,
                      vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f}),
                      colorSlice);
  layerColor->transitionLayout(cmd,
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ImageLayout::eColorAttachmentOptimal,
                               vk::ImageAspectFlagBits::eColor);

  const vk::ImageSubresourceRange depthSlice{vk::ImageAspectFlagBits::eDepth, 0u, 1u, activeChannelIndex, 1u};
  layerDepth->transitionLayout(cmd,
                               layerDepth->layout(),
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ImageAspectFlagBits::eDepth);
  cmd.clearDepthStencilImage(layerDepth->image(),
                             vk::ImageLayout::eTransferDstOptimal,
                             vk::ClearDepthStencilValue{1.0f, 0u},
                             depthSlice);
  layerDepth->transitionLayout(cmd,
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ImageLayout::eDepthAttachmentOptimal,
                               vk::ImageAspectFlagBits::eDepth);

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  layerFormats.depthFormat = layerDepth->format();
  CopyPipelineKey layerCopyKey{layerFormats.colorFormats, layerFormats.depthFormat};
  auto& layerCopyPipeline = ensureCopyPipeline(layerCopyKey, layerFormats);

  if (!m_copyDescriptor) {
    m_copyDescriptor = m_backend.allocateOverrideDescriptorSet(**m_copySetLayout);
  }
  CHECK(m_copyDescriptor != nullptr) << "Raycaster layer copy: override descriptor allocation failed (fatal)";
  m_copyDescriptor->updateTexture(0, *currentColor, m_backend.defaultSampler());
  // Bind currentDepth as a regular color sampler (shader-read-only layout, color aspect).
  m_copyDescriptor->updateTexture(1,
                                  *currentDepth,
                                  m_backend.defaultSampler(),
                                  vk::ImageLayout::eShaderReadOnlyOptimal,
                                  vk::ImageAspectFlags{});

  ZVulkanAttachmentInfo layerColorAttachment{};
  CHECK_LT(activeChannelIndex, layerColor->arrayLayers());
  auto layerColorView = layerColor->layerImageView(activeChannelIndex);
  CHECK(layerColorView != vk::ImageView{}) << "Layer color view must be valid for active channel.";
  layerColorAttachment.image = layerColor->image();
  layerColorAttachment.view = layerColorView;
  layerColorAttachment.format = layerColor->format();
  layerColorAttachment.initialLayout = layerColor->layout();
  layerColorAttachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  layerColorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
  layerColorAttachment.loadOp = vk::AttachmentLoadOp::eLoad;
  layerColorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  layerColorAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
  layerColorAttachment.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  layerColorAttachment.srcAccess = {};
  layerColorAttachment.dstAccess =
    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
  layerColorAttachment.aspect = vk::ImageAspectFlagBits::eColor;
  layerColorAttachment.trackingTexture = layerColor;

  CHECK_LT(activeChannelIndex, layerDepth->arrayLayers());
  auto layerDepthView = layerDepth->layerImageView(activeChannelIndex);
  CHECK(layerDepthView != vk::ImageView{}) << "Layer depth attachment view missing for progressive copy.";
  const auto [attachLayout, _attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
  ZVulkanAttachmentInfo layerDepthAttachment{};
  layerDepthAttachment.image = layerDepth->image();
  layerDepthAttachment.view = layerDepthView;
  layerDepthAttachment.format = layerDepth->format();
  layerDepthAttachment.initialLayout = layerDepth->layout();
  layerDepthAttachment.finalLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
  layerDepthAttachment.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
  layerDepthAttachment.loadOp = vk::AttachmentLoadOp::eLoad;
  layerDepthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  layerDepthAttachment.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
  layerDepthAttachment.dstStage =
    vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
  layerDepthAttachment.srcAccess = {};
  layerDepthAttachment.dstAccess =
    vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
  layerDepthAttachment.aspect = _attachAspect;
  layerDepthAttachment.trackingTexture = layerDepth;

  std::vector<vk::DescriptorSet> layerSets{m_copyDescriptor->descriptorSet()};

  ZVulkanGraphicsPassSpec copySpec{};
  copySpec.renderArea = scissor;
  copySpec.viewports = {viewport};
  copySpec.scissors = {scissor};
  copySpec.colorAttachments = {layerColorAttachment};
  copySpec.depthStencilAttachment = layerDepthAttachment;
  copySpec.pipelineHandle = layerCopyPipeline.pipeline->pipelineHandle();
  copySpec.pipelineLayoutHandle = layerCopyPipeline.pipeline->pipelineLayoutHandle();
  copySpec.descriptorSets = std::move(layerSets);
  copySpec.descriptorSetFirst = 0;
  copySpec.expectedDescriptorSetCount = 1;
  copySpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
  copySpec.vertexOffsets = {0};
  copySpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  copySpec.instanceCount = 1;

  recorder.recordGraphicsPass(copySpec);

  // No explicit post-pass transitions here: recorder sets final layouts and
  // updates trackingTexture for both attachments (color: ShaderReadOnly,
  // depth: DepthReadOnly). Avoid duplicating with potentially stale oldLayout.

  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster blend pass color attachment");
    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    info.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                     attachment.clearValue.color.g,
                                                                     attachment.clearValue.color.b,
                                                                     attachment.clearValue.color.a});
    info.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    info.dstStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    info.srcAccess = {};
    info.dstAccess = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    info.aspect = vk::ImageAspectFlagBits::eColor;
    info.trackingTexture = &texture;
    return info;
  };

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<ZVulkanAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster blend pass depth attachment");
    const auto [blendAttachLayout, blendAttachAspect] = depthAttachmentLayoutAndAspect(texture);
    ZVulkanAttachmentInfo info{};
    info.image = texture.image();
    info.view = texture.imageView();
    info.format = texture.format();
    info.initialLayout = texture.layout();
    info.finalLayout = blendAttachLayout;
    // Clear-on-first-use (per frame) to avoid stale depth when composing layered result.
    const VkImage imgHandle = static_cast<VkImage>(texture.image());
    const bool firstUse = m_depthClearedThisFrame.insert(imgHandle).second;
    const bool forceClear = false;
    info.loadOp = (forceClear || firstUse) ? vk::AttachmentLoadOp::eClear : vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0u);
    info.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
    info.dstStage = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    info.srcAccess = {};
    info.dstAccess =
      vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    info.aspect = blendAttachAspect;
    info.trackingTexture = &texture;
    return info;
  };

  std::vector<ZVulkanAttachmentInfo> colorAttachments;
  colorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      colorAttachments.push_back(*info);
    }
  }

  std::optional<ZVulkanAttachmentInfo> depthAttachment;
  if (batch.pass.depthAttachment) {
    depthAttachment = buildDepthAttachment(*batch.pass.depthAttachment);
  }

  vulkan::AttachmentFormats finalFormats = vulkan::extractAttachmentFormats(batch);
  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = composite.maxProjectionMerge;
  mergeKey.resultOpaque = resultOpaque;
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;

  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);
  bindMergeDescriptor(*layerColor, layerDepth);

  std::vector<vk::DescriptorSet> finalMergeSets{m_mergeDescriptor->descriptorSet()};

  ZVulkanGraphicsPassSpec finalMergeSpec{};
  finalMergeSpec.renderArea = scissor;
  finalMergeSpec.viewports = {viewport};
  finalMergeSpec.scissors = {scissor};
  finalMergeSpec.colorAttachments = colorAttachments;
  finalMergeSpec.depthStencilAttachment = depthAttachment;
  finalMergeSpec.pipelineHandle = mergePipeline.pipeline->pipelineHandle();
  finalMergeSpec.pipelineLayoutHandle = mergePipeline.pipeline->pipelineLayoutHandle();
  finalMergeSpec.descriptorSets = std::move(finalMergeSets);
  finalMergeSpec.descriptorSetFirst = 0;
  finalMergeSpec.expectedDescriptorSetCount = 1;
  finalMergeSpec.vertexBuffers = {m_quadVertexBuffer->buffer()};
  finalMergeSpec.vertexOffsets = {0};
  finalMergeSpec.vertexCount = static_cast<uint32_t>(m_quadVertexCount);
  finalMergeSpec.instanceCount = 1;

  recorder.recordGraphicsPass(finalMergeSpec);
  processEventsAndMaybeCancel(cancellationToken);

  // Defer progressive finalization to the backend/renderer; just stash the result.
  if (payload.streamKey != 0) {
    // Default to "continue this channel" unless the compaction callback asked
    // us to finalize after this progressive-only round.
    Finalization fin{};
    fin.streamKey = payload.streamKey;
    fin.eye = batch.eye;
    fin.lastRound = finalizeAfterProgressive ? finalizeAfterProgressive->lastRound : false;
    fin.channelCount = finalizeAfterProgressive ? finalizeAfterProgressive->channelCount
                                                : static_cast<uint32_t>(payload.visibleChannels.size());
    m_pendingFinalization = fin;
  }
}

} // namespace nim
namespace {
// Read SPIR-V file utility (local copy to avoid changing shader subsystem)
std::vector<uint32_t> readSpirvFile(const std::string& path)
{
  VLOG(2) << "opening SPIR-V file: " << path;
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file) {
    throw nim::ZException(fmt::format("Failed to open SPIR-V file: {}", path));
  }
  const size_t fileSize = static_cast<size_t>(file.tellg());
  if (fileSize % 4 != 0) {
    throw nim::ZException(fmt::format("Invalid SPIR-V size (must be multiple of 4): {}", path));
  }
  std::vector<uint32_t> buffer(fileSize / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
  return buffer;
}
} // namespace
  // No extra per-context channel tracking needed; renderer controls payload.roundIndexRaw.
