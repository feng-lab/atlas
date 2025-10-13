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
#include "zsysteminfo.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanpagedimageblockuploader.h"
#include "zcancellation.h"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>
#include <utility>

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

template<typename T>
void appendScalar(std::vector<uint8_t>& data, T value)
{
  static_assert(std::is_trivially_copyable_v<T>);
  const size_t offset = data.size();
  data.resize(offset + sizeof(T));
  std::memcpy(data.data() + offset, &value, sizeof(T));
}

void appendUvec3(std::vector<uint8_t>& data, const glm::uvec3& value)
{
  appendScalar(data, value.x);
  appendScalar(data, value.y);
  appendScalar(data, value.z);
}

void appendVec3(std::vector<uint8_t>& data, const glm::vec3& value)
{
  appendScalar(data, value.x);
  appendScalar(data, value.y);
  appendScalar(data, value.z);
}

std::vector<uint8_t>
buildPageDataBuffer(const Z3DImg& image, size_t channel, float zeToScreenPixelVoxelSize, uint32_t levelCount)
{
  levelCount = std::min<uint32_t>(levelCount, kMaxPagingLevels);

  std::vector<uint8_t> data;
  data.reserve(256);

  const auto& pageDirectoryBases = image.pageDirectoryBases();
  const auto& posToBlockIDs = image.posToBlockIDsLevels();
  const auto& imageDimensions = image.imageDimensionsLevels();
  const auto& voxelWorldSizes = image.voxelWorldSizesLevels();

  CHECK_GE(pageDirectoryBases.size(), levelCount);
  CHECK_GE(posToBlockIDs.size(), levelCount);
  CHECK_GE(imageDimensions.size(), levelCount);
  CHECK_GE(voxelWorldSizes.size(), levelCount);

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
  appendVec3(data, image.imageAddressToNormalizedTextureCoord(channel));
  appendScalar(data, zeToScreenPixelVoxelSize);

  for (uint32_t level = 0; level < levelCount; ++level) {
    appendUvec3(data, posToBlockIDs[level]);
  }

  return data;
}

std::vector<uint32_t> collectUniqueBlockIDs(const std::vector<uint32_t>& blockIDs)
{
  std::unordered_set<uint32_t> unique;
  unique.reserve(blockIDs.size());
  for (uint32_t id : blockIDs) {
    if (id != 0u && id != 0xFFFFFFFFu) {
      unique.insert(id);
    }
  }
  return std::vector<uint32_t>(unique.begin(), unique.end());
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
  VLOG(2) << fmt::format(
    "Raycaster::record begin fastOnly={} channels={} out={}x{} leases: entryExit={} lastAccum={} currentAccum={} blockId={}",
    payload.fastPathOnly,
    payload.visibleChannels.size(),
    static_cast<int>(payload.outputSize.x),
    static_cast<int>(payload.outputSize.y),
    static_cast<bool>(payload.entryExitLease && payload.entryExitLease->hasVulkanImage()),
    static_cast<bool>(payload.lastAccumLease && payload.lastAccumLease->hasVulkanImage()),
    static_cast<bool>(payload.currentAccumLease && payload.currentAccumLease->hasVulkanImage()),
    static_cast<bool>(payload.blockIdLease && payload.blockIdLease->hasVulkanImage()));
  m_pendingFinalization.reset();
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
  VLOG(2) << "Raycaster::record after uploadEntryGeometry";

  CHECK(payload.image) << "Raycaster payload missing image pointer.";
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
  if (needsEntryExit) {
    if (auto t = m_backend.beginGpuScope("ray_entry_exit")) {
      renderEntryExit(renderer, batch, payload, viewport, scissor, cmd);
      m_backend.endGpuScope(*t);
    } else {
      renderEntryExit(renderer, batch, payload, viewport, scissor, cmd);
    }
    VLOG(2) << "Raycaster::record after renderEntryExit";
  }

  if (payload.fastPathOnly) {
    VLOG(2) << "Raycaster::record dispatch fast path";
    if (auto t = m_backend.beginGpuScope("ray_fast")) {
      renderFastPath(renderer, batch, payload, viewport, scissor, cmd, fastVariant, composite);
      m_backend.endGpuScope(*t);
    } else {
      renderFastPath(renderer, batch, payload, viewport, scissor, cmd, fastVariant, composite);
    }
  } else {
    CHECK(fastVariant == FastPipelineVariant::Volume)
      << "Progressive raycaster path only supports volumetric rendering.";
    VLOG(2) << "Raycaster::record dispatch progressive path";
    if (auto t = m_backend.beginGpuScope("ray_progressive")) {
      renderProgressivePath(renderer, batch, payload, viewport, scissor, cmd, composite);
      m_backend.endGpuScope(*t);
    } else {
      renderProgressivePath(renderer, batch, payload, viewport, scissor, cmd, composite);
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
    // Use immutable samplers to bake the default sampler into the layout.
    vk::Sampler imm = m_backend.defaultSampler();
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
    // Also bake immutable samplers for dynamic set; updates only change image views/layouts.
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

  instance.pipeline->create();

  // Push constants for ray params used by block-ID shader (5 floats)
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = sizeof(float) * 5};
  instance.pipeline->setPushConstantRanges({pcRange});

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
                                                               float zeToScreenPixelVoxelSize)
{
  // Always allocate/update a transient per-draw descriptor for dynamic textures (set=1).
  if (!resources.pagedDescriptor) {
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

  resources.pagedDescriptor->updateTexture(0, *pageDirectory, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(1, *pageTable, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(2, *imageCache, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(3, volume, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(4, transfer, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(0, entryExit, m_backend.defaultSampler());
  const auto [lastDepthLayout, lastDepthAspect] = depthReadDescriptorLayoutAndAspect(lastDepth);
  resources.pagedDescriptor->updateTexture(1, lastDepth, m_backend.defaultSampler(), lastDepthLayout, lastDepthAspect);
  resources.pagedDescriptor->updateTexture(2, lastColor, m_backend.defaultSampler());

  const uint32_t levelCount = static_cast<uint32_t>(std::min<size_t>(image.numLevels(), kMaxPagingLevels));
  auto pageData = buildPageDataBuffer(image, channelIndex, zeToScreenPixelVoxelSize, levelCount);

  if (!resources.pageDataBuffer || resources.pageDataCapacity < pageData.size()) {
    resources.pageDataBuffer = m_backend.device().createBuffer(pageData.size(),
                                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  CHECK(resources.pageDataBuffer) << "Failed to allocate Vulkan raycaster paging uniform buffer.";

  resources.pageDataBuffer->copyData(pageData.data(), pageData.size());
  resources.pageDataCapacity = pageData.size();

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
    resources.staticDescriptor->updateTexture(0, *pageDirectory, m_backend.defaultSampler());
    resources.staticDescriptor->updateTexture(1, *pageTable, m_backend.defaultSampler());
    resources.staticDescriptor->updateTexture(2, *imageCache, m_backend.defaultSampler());
    resources.staticDescriptor->updateTexture(3, volume, m_backend.defaultSampler());
    resources.staticDescriptor->updateTexture(4, transfer, m_backend.defaultSampler());

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
        res.persistentStaticDescriptor->writeTextureOnce(0, *pd, backendPtr->defaultSampler());
        res.persistentStaticDescriptor->writeTextureOnce(1, *pt, backendPtr->defaultSampler());
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
          res.persistentStaticDescriptor->updateTexture(0, *pd, m_backend.defaultSampler());
          res.persistentStaticDescriptor->updateTexture(1, *pt, m_backend.defaultSampler());
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
  }

  // Also maintain a per-draw override pageDescriptor for the current frame so
  // the very first frame has a valid set before the persistent one exists.
  if (!resources.persistentPageDescriptor) {
    if (!resources.pageDescriptor) {
      resources.pageDescriptor = m_backend.allocateOverrideDescriptorSet(**m_pageSetLayout);
    }
    CHECK(resources.pageDescriptor) << "Failed to allocate Vulkan raycaster page descriptor set.";
    resources.pageDescriptor->updateUniformBuffer(2, *resources.pageDataBuffer);
  }

  resources.levelCount = levelCount;
  return true;
}

void ZVulkanImgRaycasterPipelineContext::bindProgressiveDescriptors(ChannelResources& resources,
                                                                    vk::PipelineLayout layout,
                                                                    vk::raii::CommandBuffer& cmd)
{
  CHECK(resources.pagedDescriptor) << "Vulkan raycaster progressive descriptors not initialised.";

  const vk::DescriptorSet staticSet =
    resources.persistentStaticDescriptor
      ? resources.persistentStaticDescriptor->descriptorSet()
      : (resources.staticDescriptor ? resources.staticDescriptor->descriptorSet() : vk::DescriptorSet{});
  CHECK(staticSet) << "Vulkan raycaster progressive static descriptor missing.";

  const vk::DescriptorSet pageSet =
    resources.persistentPageDescriptor
      ? resources.persistentPageDescriptor->descriptorSet()
      : (resources.pageDescriptor ? resources.pageDescriptor->descriptorSet() : vk::DescriptorSet{});
  CHECK(pageSet) << "Vulkan raycaster progressive page descriptor missing.";

  std::array<vk::DescriptorSet, 3> sets{staticSet, resources.pagedDescriptor->descriptorSet(), pageSet};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, sets, {});
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

  texture->transitionLayout(cmd, texture->layout(), vk::ImageLayout::eColorAttachmentOptimal);

  for (uint32_t layer = 0; layer < 2; ++layer) {
    auto& pipeline = (layer == 0) ? (flipped ? m_entryBackPipeline : m_entryFrontPipeline)
                                  : (flipped ? m_entryFrontPipeline : m_entryBackPipeline);

    auto layerView = texture->layerImageView(layer);
    if (layerView == vk::ImageView{}) {
      layerView = texture->imageView();
    }
    if (layerView == vk::ImageView{}) {
      continue;
    }
    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.imageView = layerView;
    colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f})};

    VLOG(2) << "VK raycaster entry/exit: layer=" << layer << " flipped=" << flipped
            << " texFmt=" << enumOrUnderlying(texture->format(), 16)
            << " size=" << static_cast<int>(payload.outputSize.x) << "x" << static_cast<int>(payload.outputSize.y);
    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = scissor;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    cmd.beginRendering(renderingInfo);
    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
    // No descriptor sets bound for entry; only push constants
    cmd.bindVertexBuffers(0, {m_entryVertexBuffer->buffer()}, {offset});
    cmd.pushConstants<EntryPushConstant>(pipeline.pipeline->pipelineLayout(),
                                         vk::ShaderStageFlagBits::eVertex,
                                         0,
                                         pushConstant);
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      VLOG(2) << "VK raycaster entry/exit drawIndexed: count=" << payload.entryIndices.size();
      cmd.bindIndexBuffer(m_entryIndexBuffer->buffer(), 0, vk::IndexType::eUint32);
      cmd.drawIndexed(static_cast<uint32_t>(payload.entryIndices.size()), 1, 0, 0, 0);
    } else {
      VLOG(2) << "VK raycaster entry/exit draw: verts=" << payload.entryPositions.size();
      cmd.draw(static_cast<uint32_t>(payload.entryPositions.size()), 1, 0, 0);
    }
    VLOG(2) << "Raycaster::renderEntryExit end";
    cmd.endRendering();
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

  CHECK(payload.image) << "Vulkan img raycaster missing image context.";

  CHECK(payload.entryExitLease && payload.entryExitLease->hasVulkanImage())
    << "Raycaster fast path missing entry/exit lease.";

  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster fast pass color attachment");
    texture.transitionLayout(cmd, texture.layout(), vk::ImageLayout::eColorAttachmentOptimal);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                              attachment.clearValue.color.g,
                                                                              attachment.clearValue.color.b,
                                                                              attachment.clearValue.color.a})};
    VLOG(2) << fmt::format("ray fast color: loadOp={} storeOp={} fmt={}",
                           enumOrUnderlying(info.loadOp),
                           enumOrUnderlying(info.storeOp),
                           enumOrUnderlying(texture.format()));
    return info;
  };

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster fast pass depth attachment");
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(texture);
    texture.transitionLayout(cmd, texture.layout(), attachLayout, attachAspect);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = attachLayout;
    // Clear-on-first-use: ensure background depth is deterministic when no prior writes.
    // Track by VkImage per frame.
    const VkImage imgHandle = static_cast<VkImage>(texture.image());
    const bool firstUse = m_depthClearedThisFrame.insert(imgHandle).second;
    info.loadOp = firstUse ? vk::AttachmentLoadOp::eClear : vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil = vk::ClearDepthStencilValue(firstUse ? 1.0f : attachment.clearValue.depth,
                                                              firstUse ? 0u : attachment.clearValue.stencil);
    if (firstUse) {
      VLOG(2) << "Raycaster merge first-use depth clear on image=" << reinterpret_cast<uint64_t>(imgHandle);
    }
    VLOG(2) << fmt::format("ray fast depth: loadOp={} storeOp={} fmt={}",
                           enumOrUnderlying(info.loadOp),
                           enumOrUnderlying(info.storeOp),
                           enumOrUnderlying(texture.format()));
    return info;
  };

  std::vector<vk::RenderingAttachmentInfo> finalColorAttachments;
  finalColorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      finalColorAttachments.push_back(*info);
    }
  }

  std::optional<vk::RenderingAttachmentInfo> finalDepthAttachment;
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

    if (!m_backend.validateFormatsOrSkip(finalFormats, "img raycaster fast path")) {
      return;
    }
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

    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = scissor;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(finalColorAttachments.size());
    renderingInfo.pColorAttachments = finalColorAttachments.empty() ? nullptr : finalColorAttachments.data();
    vk::RenderingAttachmentInfo depthAttachmentInfo{};
    if (finalDepthAttachment) {
      depthAttachmentInfo = *finalDepthAttachment;
      renderingInfo.pDepthAttachment = &depthAttachmentInfo;
    }

    cmd.beginRendering(renderingInfo);
    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, fastPipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {offset});

    CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path missing fast descriptor";
    std::array<vk::DescriptorSet, 1> fastSets{resources.fastDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, fastPipeline.pipeline->pipelineLayout(), 0, fastSets, {});
    struct RayPC
    {
      float s;
      float i;
      float l;
      float a;
      float b;
    } pc{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};
    cmd.pushConstants<RayPC>(fastPipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pc);
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
    VLOG(2) << "VK raycaster fast-path draw: verts=" << m_quadVertexCount
            << " colorAttCount=" << finalColorAttachments.size()
            << " colorFmt0=" << enumOrUnderlying(finalFormats.colorFormats.front())
            << " depthFmt=" << enumOrUnderlying(finalFormats.depthFormat.value()) << " output=" << outputSize.x << "x"
            << outputSize.y;
    cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
    cmd.endRendering();
    return;
  }

  auto* layerLease = payload.channelLayerLease ? payload.channelLayerLease.get() : nullptr;
  CHECK(layerLease && layerLease->hasVulkanImage()) << "Multi-channel raycaster requires layer array lease.";

  ZVulkanTexture* layerColor = layerLease->colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease->depthAttachmentTexture();
  CHECK(layerColor) << "Layer array color attachment unavailable.";

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  if (layerDepth) {
    layerFormats.depthFormat = layerDepth->format();
  }
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

    layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eColorAttachmentOptimal);
    if (layerDepth) {
      const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
      layerDepth->transitionLayout(cmd, layerDepth->layout(), attachLayout, attachAspect);
    }

    auto colorView = layerColor->layerImageView(static_cast<uint32_t>(order));
    if (colorView == vk::ImageView{}) {
      colorView = layerColor->imageView();
    }
    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.imageView = colorView;
    colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});

    std::optional<vk::RenderingAttachmentInfo> depthAttachment{};
    vk::RenderingAttachmentInfo depthAttachmentInfo{};
    if (layerDepth) {
      auto depthView = layerDepth->layerImageView(static_cast<uint32_t>(order));
      if (depthView == vk::ImageView{}) {
        depthView = layerDepth->imageView();
      }
      if (depthView != vk::ImageView{}) {
        const auto [attachLayout, _attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
        depthAttachmentInfo.imageView = depthView;
        depthAttachmentInfo.imageLayout = attachLayout;
        depthAttachmentInfo.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttachmentInfo.storeOp = vk::AttachmentStoreOp::eStore;
        depthAttachmentInfo.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
        depthAttachment = depthAttachmentInfo;
      }
    }

    vk::RenderingInfo layerInfo{};
    layerInfo.renderArea = layerRect;
    layerInfo.layerCount = 1;
    layerInfo.colorAttachmentCount = 1;
    layerInfo.pColorAttachments = &colorAttachment;
    vk::RenderingAttachmentInfo depthAttachmentStorage{};
    if (depthAttachment) {
      depthAttachmentStorage = *depthAttachment;
      layerInfo.pDepthAttachment = &depthAttachmentStorage;
    }

    cmd.beginRendering(layerInfo);
    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, layerPipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {offset});

    CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path missing fast descriptor (layered)";
    std::array<vk::DescriptorSet, 1> layeredSets{resources.fastDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           layerPipeline.pipeline->pipelineLayout(),
                           0,
                           layeredSets,
                           {});
    struct RayPC
    {
      float s;
      float i;
      float l;
      float a;
      float b;
    } pcL{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};
    cmd.pushConstants<RayPC>(layerPipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pcL);
    cmd.setViewport(0, layerViewport);
    cmd.setScissor(0, layerRect);
    {
      auto depthFmt = layerDepth ? enumOrUnderlying(layerDepth->format(), 16) : enumOrUnderlying(vk::Format{}, 16);
      VLOG(2) << "VK raycaster layered draw: order=" << order << " channelIndex=" << channelIndex
              << " verts=" << m_quadVertexCount << " colorFmt=" << enumOrUnderlying(layerColor->format(), 16)
              << " depthFmt=" << depthFmt;
    }
    cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
    cmd.endRendering();
  }

  // Transition the entire color array once for merge sampling.
  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  // Ensure the entire depth array is in the descriptor sampling layout before merge.
  if (layerDepth) {
    const auto [readLayout, _descAspect] = depthReadDescriptorLayoutAndAspect(*layerDepth);
    const auto fullBarrierAspect = depthReadBarrierAspect(*layerDepth);
    layerDepth->transitionLayout(cmd, layerDepth->layout(), readLayout, fullBarrierAspect);
    layerDepth->setDescriptorLayout(readLayout);
  }

  // Optional debug dump of layered raycaster color array to TIFs (per-layer)
  if (FLAGS_atlas_debug_save_raycaster_layers) {
    auto leaseRef = payload.channelLayerLease;
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend && leaseRef && leaseRef->hasVulkanImage()) {
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
            if (!tex->saveToImage(filename, opts)) {
              LOG(ERROR) << "Raycaster color layer debug save failed for layer " << layer;
            }
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
            if (!dtex->saveToImage(filename, opts)) {
              LOG(ERROR) << "Raycaster depth layer debug save failed for layer " << layer;
            }
          }
        }
      });
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
  vk::RenderingInfo mergeInfo{};
  // Use the input scissor/renderArea provided by the compositor.
  mergeInfo.renderArea = scissor;
  mergeInfo.layerCount = 1;
  mergeInfo.colorAttachmentCount = static_cast<uint32_t>(finalColorAttachments.size());
  mergeInfo.pColorAttachments = finalColorAttachments.empty() ? nullptr : finalColorAttachments.data();
  vk::RenderingAttachmentInfo mergeDepthAttachment{};
  if (finalDepthAttachment) {
    mergeDepthAttachment = *finalDepthAttachment;
    mergeInfo.pDepthAttachment = &mergeDepthAttachment;
  }

  VLOG(2) << "VK raycaster merge: colors=" << mergeInfo.colorAttachmentCount
          << " depth=" << (finalDepthAttachment ? 1 : 0)
          << " color0Fmt=" << enumOrUnderlying(finalFormats.colorFormats.front())
          << " depthFmt=" << enumOrUnderlying(finalFormats.depthFormat.value());

  cmd.beginRendering(mergeInfo);
  vk::DeviceSize mergeOffset = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {mergeOffset});
  std::array<vk::DescriptorSet, 1> mergeSets{m_mergeDescriptor->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipelineLayout(), 0, mergeSets, {});
  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
  cmd.endRendering();
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

  const ImgCompositingMode sanitizedMode = composite.mode;
  const bool resultOpaque = composite.resultOpaque;

  CHECK(payload.image) << "Vulkan img raycaster missing image context.";
  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster 2D fast path: payload missing transferFunctions vector (fatal)";
  const auto& transferFunctions = *payload.transferFunctions;

  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster 2D fast color attachment");
    texture.transitionLayout(cmd, texture.layout(), vk::ImageLayout::eColorAttachmentOptimal);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                              attachment.clearValue.color.g,
                                                                              attachment.clearValue.color.b,
                                                                              attachment.clearValue.color.a})};
    return info;
  };

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster 2D fast depth attachment");
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(texture);
    texture.transitionLayout(cmd, texture.layout(), attachLayout, attachAspect);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = attachLayout;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil =
      vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
    return info;
  };

  std::vector<vk::RenderingAttachmentInfo> finalColorAttachments;
  finalColorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      finalColorAttachments.push_back(*info);
    }
  }

  std::optional<vk::RenderingAttachmentInfo> finalDepthAttachment;
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

    if (!m_backend.validateFormatsOrSkip(finalFormats, "img raycaster 2d fast path")) {
      return;
    }

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

    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = scissor;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(finalColorAttachments.size());
    renderingInfo.pColorAttachments = finalColorAttachments.empty() ? nullptr : finalColorAttachments.data();
    vk::RenderingAttachmentInfo depthAttachmentInfo{};
    if (finalDepthAttachment) {
      depthAttachmentInfo = *finalDepthAttachment;
      renderingInfo.pDepthAttachment = &depthAttachmentInfo;
    }

    cmd.beginRendering(renderingInfo);
    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_entryVertexBuffer->buffer()}, {offset});
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      cmd.bindIndexBuffer(m_entryIndexBuffer->buffer(), 0, vk::IndexType::eUint32);
    }

    CHECK(resources.image2DDescriptor != nullptr) << "Raycaster 2D fast path missing descriptor";
    std::array<vk::DescriptorSet, 1> sets{resources.image2DDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});

    struct Image2DPush
    {
      glm::mat4 projectionView;
    } pc{projectionView};
    cmd.pushConstants<Image2DPush>(pipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eVertex, 0, pc);
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      const uint32_t indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      cmd.drawIndexed(indexCount, 1, 0, 0, 0);
    } else {
      cmd.draw(vertexCount, 1, 0, 0);
    }
    cmd.endRendering();
    return;
  }

  auto* layerLease = payload.channelLayerLease ? payload.channelLayerLease.get() : nullptr;
  CHECK(layerLease && layerLease->hasVulkanImage())
    << "2D fast path requires layer array lease for multi-channel output.";

  ZVulkanTexture* layerColor = layerLease->colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease->depthAttachmentTexture();
  CHECK(layerColor) << "Layer array color attachment unavailable for 2D fast path.";

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  if (layerDepth) {
    layerFormats.depthFormat = layerDepth->format();
  }

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

    layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eColorAttachmentOptimal);
    if (layerDepth) {
      const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
      layerDepth->transitionLayout(cmd, layerDepth->layout(), attachLayout, attachAspect);
    }

    auto colorView = layerColor->layerImageView(static_cast<uint32_t>(order));
    if (colorView == vk::ImageView{}) {
      colorView = layerColor->imageView();
    }
    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.imageView = colorView;
    colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});

    std::optional<vk::RenderingAttachmentInfo> depthAttachment{};
    vk::RenderingAttachmentInfo depthAttachmentInfo{};
    if (layerDepth) {
      auto depthView = layerDepth->layerImageView(static_cast<uint32_t>(order));
      if (depthView == vk::ImageView{}) {
        depthView = layerDepth->imageView();
      }
      if (depthView != vk::ImageView{}) {
        const auto [attachLayout, _] = depthAttachmentLayoutAndAspect(*layerDepth);
        depthAttachmentInfo.imageView = depthView;
        depthAttachmentInfo.imageLayout = attachLayout;
        depthAttachmentInfo.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttachmentInfo.storeOp = vk::AttachmentStoreOp::eStore;
        depthAttachmentInfo.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
        depthAttachment = depthAttachmentInfo;
      }
    }

    vk::RenderingInfo layerInfo{};
    layerInfo.renderArea = layerRect;
    layerInfo.layerCount = 1;
    layerInfo.colorAttachmentCount = 1;
    layerInfo.pColorAttachments = &colorAttachment;
    vk::RenderingAttachmentInfo depthStorage{};
    if (depthAttachment) {
      depthStorage = *depthAttachment;
      layerInfo.pDepthAttachment = &depthStorage;
    }

    cmd.beginRendering(layerInfo);
    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, layerPipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_entryVertexBuffer->buffer()}, {offset});
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      cmd.bindIndexBuffer(m_entryIndexBuffer->buffer(), 0, vk::IndexType::eUint32);
    }

    CHECK(resources.image2DDescriptor != nullptr) << "Raycaster 2D layered path missing descriptor";
    std::array<vk::DescriptorSet, 1> sets{resources.image2DDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layerPipeline.pipeline->pipelineLayout(), 0, sets, {});
    struct Image2DPush
    {
      glm::mat4 projectionView;
    } pcLayer{projectionView};
    cmd.pushConstants<Image2DPush>(layerPipeline.pipeline->pipelineLayout(),
                                   vk::ShaderStageFlagBits::eVertex,
                                   0,
                                   pcLayer);
    cmd.setViewport(0, layerViewport);
    cmd.setScissor(0, layerRect);
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      const uint32_t indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      cmd.drawIndexed(indexCount, 1, 0, 0, 0);
    } else {
      cmd.draw(vertexCount, 1, 0, 0);
    }
    cmd.endRendering();
  }
  if (tLayers) {
    m_backend.endGpuScope(*tLayers);
  }

  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  if (layerDepth) {
    const auto [readLayout, descAspect] = depthReadDescriptorLayoutAndAspect(*layerDepth);
    const auto barrierAspect = depthReadBarrierAspect(*layerDepth);
    layerDepth->transitionLayout(cmd, layerDepth->layout(), readLayout, barrierAspect);
    layerDepth->setDescriptorLayout(readLayout);
  }

  bindMergeDescriptor(*layerColor, layerDepth);

  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = composite.maxProjectionMerge;
  mergeKey.resultOpaque = resultOpaque;
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;
  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);

  vk::RenderingInfo mergeInfo{};
  mergeInfo.renderArea = scissor;
  mergeInfo.layerCount = 1;
  mergeInfo.colorAttachmentCount = static_cast<uint32_t>(finalColorAttachments.size());
  mergeInfo.pColorAttachments = finalColorAttachments.empty() ? nullptr : finalColorAttachments.data();
  vk::RenderingAttachmentInfo mergeDepth{};
  if (finalDepthAttachment) {
    mergeDepth = *finalDepthAttachment;
    mergeInfo.pDepthAttachment = &mergeDepth;
  }

  cmd.beginRendering(mergeInfo);
  vk::DeviceSize mergeOffset = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {mergeOffset});
  std::array<vk::DescriptorSet, 1> mergeSets{m_mergeDescriptor->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipelineLayout(), 0, mergeSets, {});
  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
  cmd.endRendering();
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

  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster slice fast color attachment");
    texture.transitionLayout(cmd, texture.layout(), vk::ImageLayout::eColorAttachmentOptimal);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                              attachment.clearValue.color.g,
                                                                              attachment.clearValue.color.b,
                                                                              attachment.clearValue.color.a})};
    return info;
  };

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster slice fast depth attachment");
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(texture);
    texture.transitionLayout(cmd, texture.layout(), attachLayout, attachAspect);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = attachLayout;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil =
      vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
    return info;
  };

  std::vector<vk::RenderingAttachmentInfo> finalColorAttachments;
  finalColorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      finalColorAttachments.push_back(*info);
    }
  }

  std::optional<vk::RenderingAttachmentInfo> finalDepthAttachment;
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

  if (channelCount == 1) {
    const size_t channelIndex = payload.visibleChannels.front();
    CHECK(channelIndex < transferFunctions.size() && transferFunctions[channelIndex] != nullptr)
      << "Missing transfer function for channel " << channelIndex;

    if (!m_backend.validateFormatsOrSkip(finalFormats, "img raycaster slice fast path")) {
      return;
    }

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

    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = scissor;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(finalColorAttachments.size());
    renderingInfo.pColorAttachments = finalColorAttachments.empty() ? nullptr : finalColorAttachments.data();
    vk::RenderingAttachmentInfo depthAttachmentInfo{};
    if (finalDepthAttachment) {
      depthAttachmentInfo = *finalDepthAttachment;
      renderingInfo.pDepthAttachment = &depthAttachmentInfo;
    }

    cmd.beginRendering(renderingInfo);
    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_entryVertexBuffer->buffer()}, {offset});
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      cmd.bindIndexBuffer(m_entryIndexBuffer->buffer(), 0, vk::IndexType::eUint32);
    }

    CHECK(resources.sliceDescriptor != nullptr) << "Raycaster slice fast path missing descriptor";
    std::array<vk::DescriptorSet, 1> sets{resources.sliceDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
    struct SlicePush
    {
      glm::mat4 projectionView;
      glm::mat4 view;
    } pc{projectionView, viewMatrix};
    cmd.pushConstants<SlicePush>(pipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eVertex, 0, pc);
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      const uint32_t indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      cmd.drawIndexed(indexCount, 1, 0, 0, 0);
    } else {
      cmd.draw(vertexCount, 1, 0, 0);
    }
    cmd.endRendering();
    return;
  }

  auto* layerLease = payload.channelLayerLease ? payload.channelLayerLease.get() : nullptr;
  CHECK(layerLease && layerLease->hasVulkanImage())
    << "Slice fast path requires layer array lease for multi-channel output.";

  ZVulkanTexture* layerColor = layerLease->colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease->depthAttachmentTexture();
  CHECK(layerColor) << "Layer array color attachment unavailable for slice fast path.";

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  if (layerDepth) {
    layerFormats.depthFormat = layerDepth->format();
  }

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

    layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eColorAttachmentOptimal);
    if (layerDepth) {
      const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
      layerDepth->transitionLayout(cmd, layerDepth->layout(), attachLayout, attachAspect);
    }

    auto colorView = layerColor->layerImageView(static_cast<uint32_t>(order));
    if (colorView == vk::ImageView{}) {
      colorView = layerColor->imageView();
    }
    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.imageView = colorView;
    colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});

    std::optional<vk::RenderingAttachmentInfo> depthAttachment{};
    vk::RenderingAttachmentInfo depthAttachmentInfo{};
    if (layerDepth) {
      auto depthView = layerDepth->layerImageView(static_cast<uint32_t>(order));
      if (depthView == vk::ImageView{}) {
        depthView = layerDepth->imageView();
      }
      if (depthView != vk::ImageView{}) {
        const auto [attachLayout, _] = depthAttachmentLayoutAndAspect(*layerDepth);
        depthAttachmentInfo.imageView = depthView;
        depthAttachmentInfo.imageLayout = attachLayout;
        depthAttachmentInfo.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttachmentInfo.storeOp = vk::AttachmentStoreOp::eStore;
        depthAttachmentInfo.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
        depthAttachment = depthAttachmentInfo;
      }
    }

    vk::RenderingInfo layerInfo{};
    layerInfo.renderArea = layerRect;
    layerInfo.layerCount = 1;
    layerInfo.colorAttachmentCount = 1;
    layerInfo.pColorAttachments = &colorAttachment;
    vk::RenderingAttachmentInfo depthStorage{};
    if (depthAttachment) {
      depthStorage = *depthAttachment;
      layerInfo.pDepthAttachment = &depthStorage;
    }

    cmd.beginRendering(layerInfo);
    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, layerPipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_entryVertexBuffer->buffer()}, {offset});
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      cmd.bindIndexBuffer(m_entryIndexBuffer->buffer(), 0, vk::IndexType::eUint32);
    }

    CHECK(resources.sliceDescriptor != nullptr) << "Raycaster slice layered path missing descriptor";
    std::array<vk::DescriptorSet, 1> sets{resources.sliceDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layerPipeline.pipeline->pipelineLayout(), 0, sets, {});
    struct SlicePush
    {
      glm::mat4 projectionView;
      glm::mat4 view;
    } pcLayer{projectionView, viewMatrix};
    cmd.pushConstants<SlicePush>(layerPipeline.pipeline->pipelineLayout(),
                                 vk::ShaderStageFlagBits::eVertex,
                                 0,
                                 pcLayer);
    cmd.setViewport(0, layerViewport);
    cmd.setScissor(0, layerRect);
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      const uint32_t indexCount = static_cast<uint32_t>(payload.entryIndices.size());
      cmd.drawIndexed(indexCount, 1, 0, 0, 0);
    } else {
      cmd.draw(vertexCount, 1, 0, 0);
    }
    cmd.endRendering();
  }
  if (tLayers) {
    m_backend.endGpuScope(*tLayers);
  }

  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  if (layerDepth) {
    const auto [readLayout, descAspect] = depthReadDescriptorLayoutAndAspect(*layerDepth);
    const auto barrierAspect = depthReadBarrierAspect(*layerDepth);
    layerDepth->transitionLayout(cmd, layerDepth->layout(), readLayout, barrierAspect);
    layerDepth->setDescriptorLayout(readLayout);
  }

  bindMergeDescriptor(*layerColor, layerDepth);

  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = composite.maxProjectionMerge;
  mergeKey.resultOpaque = resultOpaque;
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;
  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);

  vk::RenderingInfo mergeInfo{};
  mergeInfo.renderArea = scissor;
  mergeInfo.layerCount = 1;
  mergeInfo.colorAttachmentCount = static_cast<uint32_t>(finalColorAttachments.size());
  mergeInfo.pColorAttachments = finalColorAttachments.empty() ? nullptr : finalColorAttachments.data();
  vk::RenderingAttachmentInfo mergeDepth{};
  if (finalDepthAttachment) {
    mergeDepth = *finalDepthAttachment;
    mergeInfo.pDepthAttachment = &mergeDepth;
  }

  cmd.beginRendering(mergeInfo);
  vk::DeviceSize mergeOffset = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {mergeOffset});
  std::array<vk::DescriptorSet, 1> mergeSets{m_mergeDescriptor->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipelineLayout(), 0, mergeSets, {});
  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
  cmd.endRendering();
}

void ZVulkanImgRaycasterPipelineContext::renderProgressivePath(Z3DRendererBase& renderer,
                                                               const RenderBatch& batch,
                                                               const ImgRaycasterPayload& payload,
                                                               const vk::Viewport& viewport,
                                                               const vk::Rect2D& scissor,
                                                               vk::raii::CommandBuffer& cmd,
                                                               const CompositingConfig& composite)
{
  const uint32_t channelCount = static_cast<uint32_t>(payload.visibleChannels.size());
  if (channelCount == 0u) {
    return;
  }

  const ImgCompositingMode sanitizedMode = composite.mode;
  const bool resultOpaque = composite.resultOpaque;
  const bool localMip = composite.localMip;

  CHECK(payload.blockIdLease && payload.blockIdLease->attachments != 0)
    << "Vulkan raycaster progressive path missing block-ID lease.";

  CHECK(payload.lastAccumLease && payload.currentAccumLease)
    << "Vulkan raycaster progressive path missing accumulator leases.";

  CHECK(m_imageBlockUploader) << "Vulkan raycaster progressive path missing image block uploader.";
  m_imageBlockUploader->bindToImage(*payload.image);

  uint32_t activeChannelIndex = payload.activeChannelIndex;
  if (activeChannelIndex >= channelCount) {
    activeChannelIndex = 0u;
  }
  size_t channelIndex = payload.activeChannel;
  if (channelIndex == std::numeric_limits<size_t>::max() || activeChannelIndex >= payload.visibleChannels.size()) {
    channelIndex = payload.visibleChannels[activeChannelIndex];
  }

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

  auto* entryTexture = payload.entryExitLease ? payload.entryExitLease->colorAttachment(0) : nullptr;
  auto* lastColor = payload.lastAccumLease->colorAttachment(0);
  auto* lastDepth = payload.lastAccumLease->colorAttachment(1);
  auto* currentColor = payload.currentAccumLease->colorAttachment(0);
  auto* currentDepth = payload.currentAccumLease->colorAttachment(1);

  CHECK(entryTexture && lastColor && lastDepth && currentColor && currentDepth)
    << "Vulkan raycaster progressive path missing required textures.";

  const glm::uvec2 outputSize = payload.outputSize;
  CHECK(outputSize.x > 0u && outputSize.y > 0u) << "Vulkan raycaster progressive path requires non-zero output size.";

  ensureProgressiveLayerTargets(outputSize, channelCount, payload.progressiveGeneration, cmd);
  ZVulkanTexture* layerColor = m_progressiveLayerColor.get();
  ZVulkanTexture* layerDepth = m_progressiveLayerDepth.get();
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

  // Ensure previous-round data is ready for sampling.
  entryTexture->transitionLayout(cmd, entryTexture->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  lastColor->transitionLayout(cmd, lastColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  const auto [lastDepthReadLayout, lastDepthDescAspect] = depthReadDescriptorLayoutAndAspect(*lastDepth);
  const auto lastDepthBarrierAspect = depthReadBarrierAspect(*lastDepth);
  lastDepth->transitionLayout(cmd, lastDepth->layout(), lastDepthReadLayout, lastDepthBarrierAspect);
  entryTexture->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  lastColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  lastDepth->setDescriptorLayout(lastDepthReadLayout);

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
                             zeToScreenPixelVoxelSize)) {
    return;
  }

  // ---------------- Block-ID pass ----------------
  std::vector<uint32_t> rawBlockIds;
  rawBlockIds.reserve(static_cast<size_t>(payload.blockIdLease->descriptor.size.x) *
                      payload.blockIdLease->descriptor.size.y * 4ull);

  const uint32_t blockAttachmentCount = payload.blockIdLease->attachments;
  auto* firstBlockAttachment = payload.blockIdLease->colorAttachment(0);
  CHECK(firstBlockAttachment) << "Vulkan raycaster progressive path missing block-ID attachment.";
  const vk::Format blockFormat = firstBlockAttachment->format();
  BlockIdPipelineKey blockKey{resources.levelCount, blockAttachmentCount, blockFormat};
  auto& blockPipeline = ensureBlockIdPipeline(blockKey, blockFormat);

  std::vector<vk::RenderingAttachmentInfo> blockAttachments;
  blockAttachments.reserve(blockAttachmentCount);
  for (uint32_t att = 0; att < blockAttachmentCount; ++att) {
    auto* texture = payload.blockIdLease->colorAttachment(att);
    if (!texture) {
      continue;
    }
    texture->transitionLayout(cmd, texture->layout(), vk::ImageLayout::eColorAttachmentOptimal);
    vk::RenderingAttachmentInfo attachment{};
    attachment.imageView = texture->imageView();
    attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp = vk::AttachmentLoadOp::eClear;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;
    attachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
    blockAttachments.push_back(attachment);
  }

  CHECK(!blockAttachments.empty()) << "Vulkan raycaster progressive path failed to prepare block-ID attachments.";

  vk::Rect2D blockRect = scissor;
  vk::Viewport blockViewport = viewport;

  vk::RenderingInfo blockInfo{};
  blockInfo.renderArea = blockRect;
  blockInfo.layerCount = 1;
  blockInfo.colorAttachmentCount = static_cast<uint32_t>(blockAttachments.size());
  blockInfo.pColorAttachments = blockAttachments.data();

  cmd.beginRendering(blockInfo);
  vk::DeviceSize offset = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, blockPipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {offset});
  bindProgressiveDescriptors(resources, blockPipeline.pipeline->pipelineLayout(), cmd);
  struct RayPC
  {
    float s, i, l, a, b;
  } pcB{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};
  cmd.pushConstants<RayPC>(blockPipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pcB);
  cmd.setViewport(0, blockViewport);
  cmd.setScissor(0, blockRect);
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
  cmd.endRendering();

  std::vector<uint32_t> blockIds;
  for (uint32_t att = 0; att < blockAttachmentCount; ++att) {
    auto* texture = payload.blockIdLease->colorAttachment(att);
    if (!texture) {
      continue;
    }
    const vk::Extent3D extent = texture->extent();
    const size_t pixelCount = static_cast<size_t>(extent.width) * extent.height;
    resources.blockIdScratch.resize(pixelCount * 4u);
    texture->downloadData(resources.blockIdScratch.data(), resources.blockIdScratch.size() * sizeof(uint32_t));
    blockIds.insert(blockIds.end(), resources.blockIdScratch.begin(), resources.blockIdScratch.end());
  }

  auto missingBlocks = collectUniqueBlockIDs(blockIds);
  bool lastRound = missingBlocks.empty();

  if (!missingBlocks.empty()) {
    auto cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
    ZBenchTimer uploadTimer(fmt::format("vulkan_raycaster_channel_{}", channelIndex));
    bool fullyCached =
      payload.image->updateAndUploadPageDirectoryCaches(missingBlocks, channelIndex, cancellationToken, uploadTimer);
    lastRound = lastRound && fullyCached;
  }

  // ---------------- Progressive raycast pass ----------------
  currentColor->transitionLayout(cmd, currentColor->layout(), vk::ImageLayout::eColorAttachmentOptimal);
  currentDepth->transitionLayout(cmd,
                                 currentDepth->layout(),
                                 vk::ImageLayout::eColorAttachmentOptimal,
                                 vk::ImageAspectFlagBits::eColor);

  vulkan::AttachmentFormats progressiveFormats;
  progressiveFormats.colorFormats = {currentColor->format(), currentDepth->format()};

  ProgressivePipelineKey progressiveKey{currentColor->format(),
                                        currentDepth->format(),
                                        sanitizedMode,
                                        localMip,
                                        resultOpaque};
  progressiveKey.levelCount = resources.levelCount;
  auto& progressivePipeline = ensureProgressivePipeline(progressiveKey, progressiveFormats);

  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = currentColor->imageView();
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});

  vk::RenderingAttachmentInfo accumAttachment{};
  accumAttachment.imageView = currentDepth->imageView();
  accumAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  accumAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  accumAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  accumAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});

  std::array<vk::RenderingAttachmentInfo, 2> progressiveAttachments{colorAttachment, accumAttachment};
  vk::RenderingInfo progressiveInfo{};
  progressiveInfo.renderArea = scissor;
  progressiveInfo.layerCount = 1;
  progressiveInfo.colorAttachmentCount = static_cast<uint32_t>(progressiveAttachments.size());
  progressiveInfo.pColorAttachments = progressiveAttachments.data();

  cmd.beginRendering(progressiveInfo);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, progressivePipeline.pipeline->pipeline());
  vk::DeviceSize progOffset = 0;
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {progOffset});
  bindProgressiveDescriptors(resources, progressivePipeline.pipeline->pipelineLayout(), cmd);
  struct RayPC2
  {
    float s, i, l, a, b;
  } pcP{payload.samplingRate, payload.isoValue, payload.localMIPThreshold, zeToZW_a, zeToZW_b};
  cmd.pushConstants<RayPC2>(progressivePipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pcP);
  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
  cmd.endRendering();

  currentColor->transitionLayout(cmd, currentColor->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  const auto [currentDepthReadLayout, currentDepthDescAspect] = depthReadDescriptorLayoutAndAspect(*currentDepth);
  const auto currentDepthBarrierAspect = depthReadBarrierAspect(*currentDepth);
  currentDepth->transitionLayout(cmd, currentDepth->layout(), currentDepthReadLayout, currentDepthBarrierAspect);
  currentColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  currentDepth->setDescriptorLayout(currentDepthReadLayout);

  // Copy the current accumulation into the persistent layer array slice
  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eColorAttachmentOptimal);
  layerDepth->transitionLayout(cmd,
                               layerDepth->layout(),
                               vk::ImageLayout::eDepthAttachmentOptimal,
                               vk::ImageAspectFlagBits::eDepth);

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  layerFormats.depthFormat = layerDepth ? std::optional<vk::Format>(layerDepth->format()) : std::nullopt;
  CopyPipelineKey layerCopyKey{layerFormats.colorFormats, layerFormats.depthFormat};
  auto& layerCopyPipeline = ensureCopyPipeline(layerCopyKey, layerFormats);

  if (!m_copyDescriptor) {
    m_copyDescriptor = m_backend.allocateOverrideDescriptorSet(**m_copySetLayout);
  }
  CHECK(m_copyDescriptor != nullptr) << "Raycaster layer copy: override descriptor allocation failed (fatal)";
  m_copyDescriptor->updateTexture(0, *currentColor, m_backend.defaultSampler());
  const auto [copyDepthLayout, copyDepthAspect] = depthReadDescriptorLayoutAndAspect(*currentDepth);
  m_copyDescriptor->updateTexture(1, *currentDepth, m_backend.defaultSampler(), copyDepthLayout, copyDepthAspect);

  vk::RenderingAttachmentInfo layerColorAttachment{};
  auto layerColorView = layerColor->layerImageView(activeChannelIndex);
  if (layerColorView == vk::ImageView{}) {
    layerColorView = layerColor->imageView();
  }
  layerColorAttachment.imageView = layerColorView;
  layerColorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  layerColorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  layerColorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  layerColorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});

  std::optional<vk::RenderingAttachmentInfo> layerDepthAttachment{};
  vk::RenderingAttachmentInfo layerDepthInfo{};
  if (layerDepth) {
    auto layerDepthView = layerDepth->layerImageView(activeChannelIndex);
    if (layerDepthView == vk::ImageView{}) {
      layerDepthView = layerDepth->imageView();
    }
    const auto [attachLayout, _attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
    layerDepthInfo.imageView = layerDepthView;
    layerDepthInfo.imageLayout = attachLayout;
    layerDepthInfo.loadOp = vk::AttachmentLoadOp::eClear;
    layerDepthInfo.storeOp = vk::AttachmentStoreOp::eStore;
    layerDepthInfo.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
    layerDepthAttachment = layerDepthInfo;
  }

  vk::RenderingInfo layerInfo{};
  layerInfo.renderArea = scissor;
  layerInfo.layerCount = 1;
  layerInfo.colorAttachmentCount = 1;
  layerInfo.pColorAttachments = &layerColorAttachment;
  vk::RenderingAttachmentInfo depthAttachmentStorage{};
  if (layerDepthAttachment) {
    depthAttachmentStorage = *layerDepthAttachment;
    layerInfo.pDepthAttachment = &depthAttachmentStorage;
  }

  cmd.beginRendering(layerInfo);
  vk::DeviceSize layerOffset = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, layerCopyPipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {layerOffset});
  std::array<vk::DescriptorSet, 1> layerSets{m_copyDescriptor->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                         layerCopyPipeline.pipeline->pipelineLayout(),
                         0,
                         layerSets,
                         {});
  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
  cmd.endRendering();

  layerColor->transitionLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
  layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  const auto [layerDepthReadLayout, layerDepthDescAspect] = depthReadDescriptorLayoutAndAspect(*layerDepth);
  const auto [layerAttachLayout, _layerAttachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
  const auto layerBarrierAspect = depthReadBarrierAspect(*layerDepth);
  layerDepth->transitionLayout(cmd, layerAttachLayout, layerDepthReadLayout, layerBarrierAspect);
  layerDepth->setDescriptorLayout(layerDepthReadLayout);

  auto buildColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster blend pass color attachment");
    texture.transitionLayout(cmd, texture.layout(), vk::ImageLayout::eColorAttachmentOptimal);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                     attachment.clearValue.color.g,
                                                                     attachment.clearValue.color.b,
                                                                     attachment.clearValue.color.a});
    return info;
  };

  auto buildDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
    if (!attachment.handle.valid()) {
      return std::nullopt;
    }
    auto& texture =
      vulkan::textureFromHandle(attachment.handle, m_backend.device(), "img raycaster blend pass depth attachment");
    const auto [attachLayout, attachAspect] = depthAttachmentLayoutAndAspect(texture);
    texture.transitionLayout(cmd, texture.layout(), attachLayout, attachAspect);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = attachLayout;
    // Clear-on-first-use (per frame) to avoid stale depth when composing layered result.
    const VkImage imgHandle = static_cast<VkImage>(texture.image());
    const bool firstUse = m_depthClearedThisFrame.insert(imgHandle).second;
    const bool forceClear = false;
    info.loadOp = (forceClear || firstUse) ? vk::AttachmentLoadOp::eClear : vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0u);
    return info;
  };

  std::vector<vk::RenderingAttachmentInfo> colorAttachments;
  colorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (auto info = buildColorAttachment(attachment)) {
      colorAttachments.push_back(*info);
    }
  }

  std::optional<vk::RenderingAttachmentInfo> depthAttachment;
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

  vk::RenderingInfo mergeInfo{};
  // Use compositor-provided render area
  mergeInfo.renderArea = scissor;
  mergeInfo.layerCount = 1;
  mergeInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
  mergeInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
  vk::RenderingAttachmentInfo mergeDepthAttachment{};
  if (depthAttachment) {
    mergeDepthAttachment = *depthAttachment;
    mergeInfo.pDepthAttachment = &mergeDepthAttachment;
  }

  cmd.beginRendering(mergeInfo);
  vk::DeviceSize mergeOffset = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {mergeOffset});
  std::array<vk::DescriptorSet, 1> mergeSets{m_mergeDescriptor->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, mergePipeline.pipeline->pipelineLayout(), 0, mergeSets, {});
  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
  cmd.endRendering();

  // Defer progressive finalization to the backend/renderer; just stash the result.
  if (payload.streamKey != 0) {
    m_pendingFinalization = Finalization{.streamKey = payload.streamKey,
                                         .eye = batch.eye,
                                         .lastRound = lastRound,
                                         .channelCount = static_cast<uint32_t>(payload.visibleChannels.size())};
  }
}

} // namespace nim
