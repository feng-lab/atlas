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
#include "zimg.h"
#include "zimgformat.h"

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
            "Save Vulkan raycaster layered color outputs (one TIF per layer) after rendering.");
DEFINE_bool(atlas_debug_save_raycaster_merge_out,
            false,
            "Save Vulkan raycaster merged output (first color attachment) after merge.");
DEFINE_bool(atlas_debug_save_raycaster_layer_depth,
            false,
            "Save Vulkan raycaster layered depth array (one TIF per layer).");

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
    channel.rayParamDescriptor = nullptr;
    channel.pagedDescriptor = nullptr;
    channel.pageDescriptor = nullptr;
    channel.blockIdDescriptor.reset();
  }
  m_emptyDescriptor.reset();
  m_entryTransformDescriptor = nullptr;
  m_copyDescriptor = nullptr;
  m_mergeDescriptor = nullptr;
  if (m_descriptorPool) {
    m_descriptorPool->reset();
  }
}

void ZVulkanImgRaycasterPipelineContext::record(Z3DRendererBase& renderer,
                                                const RenderBatch& batch,
                                                const ImgRaycasterPayload& payload,
                                                const vk::Viewport& viewport,
                                                const vk::Rect2D& scissor,
                                                vk::raii::CommandBuffer& cmd)
{
  VLOG(2) << fmt::format("Raycaster::record begin fastOnly={} channels={} out={}x{} leases: entryExit={} lastAccum={} currentAccum={} blockId={}",
                         payload.fastPathOnly,
                         payload.visibleChannels.size(),
                         static_cast<int>(payload.outputSize.x),
                         static_cast<int>(payload.outputSize.y),
                         static_cast<bool>(payload.entryExitLease && payload.entryExitLease->hasVulkanImage()),
                         static_cast<bool>(payload.lastAccumLease && payload.lastAccumLease->hasVulkanImage()),
                         static_cast<bool>(payload.currentAccumLease && payload.currentAccumLease->hasVulkanImage()),
                         static_cast<bool>(payload.blockIdLease && payload.blockIdLease->hasVulkanImage()));
  m_pendingFinalization.reset();
  if (!payload.entryExitLease || !payload.entryExitLease->hasVulkanImage()) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan img raycaster missing entry/exit lease.";
    return;
  }

  if (payload.visibleChannels.empty()) {
    return;
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureEmptyDescriptor();
  ensureQuadVertexBuffer();

  uploadEntryGeometry(payload);
  VLOG(2) << "Raycaster::record after uploadEntryGeometry";
  renderEntryExit(renderer, batch, payload, viewport, scissor, cmd);
  VLOG(2) << "Raycaster::record after renderEntryExit";

  if (payload.fastPathOnly) {
    VLOG(2) << "Raycaster::record dispatch fast path";
    renderFastPath(renderer, batch, payload, viewport, scissor, cmd);
  } else {
    VLOG(2) << "Raycaster::record dispatch progressive path";
    renderProgressivePath(renderer, batch, payload, viewport, scissor, cmd);
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

  if (!m_progressiveSetLayout) {
    std::array<vk::DescriptorSetLayoutBinding, 8> bindings{
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 5,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 6,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 7,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_progressiveSetLayout.emplace(device, info);
  }

  if (!m_pageSetLayout) {
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 2,
                                     .descriptorType = vk::DescriptorType::eUniformBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 3,
                                     .descriptorType = vk::DescriptorType::eUniformBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
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
    m_copySetLayout.emplace(device, info);
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
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_mergeSetLayout.emplace(device, info);
  }

  if (!m_emptySetLayout) {
    vk::DescriptorSetLayoutCreateInfo info{};
    m_emptySetLayout.emplace(device, info);
  }

  if (!m_rayParamSetLayout) {
    vk::DescriptorSetLayoutBinding binding{.binding = 3,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_rayParamSetLayout.emplace(device, info);
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
    m_entryVertexBuffer =
      device.createBuffer(m_entryVertexCapacity * sizeof(EntryVertex),
                          vk::BufferUsageFlagBits::eVertexBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    CHECK(m_entryVertexBuffer != nullptr) << "Failed to allocate entry vertex buffer, count=" << m_entryVertexCapacity;
  }

  if (indexCount > m_entryIndexCapacity) {
    m_entryIndexCapacity = indexCount;
    if (m_entryIndexCapacity == 0) {
      m_entryIndexBuffer.reset();
    } else {
      m_entryIndexBuffer =
        device.createBuffer(m_entryIndexCapacity * sizeof(uint32_t),
                            vk::BufferUsageFlagBits::eIndexBuffer,
                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
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
  std::array<glm::vec2, 4> quad = {
    glm::vec2{-1.f, -1.f},
    glm::vec2{-1.f, 1.f },
    glm::vec2{1.f,  -1.f},
    glm::vec2{1.f,  1.f }
  };
  m_quadVertexCount = quad.size();
  m_quadVertexBuffer =
    device.createBuffer(quad.size() * sizeof(glm::vec2),
                        vk::BufferUsageFlagBits::eVertexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  CHECK(m_quadVertexBuffer != nullptr) << "Failed to allocate raycaster quad vertex buffer";
  m_quadVertexBuffer->copyData(quad.data(), quad.size() * sizeof(glm::vec2));
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
  m_entryVertexBuffer->copyData(vertices.data(), vertices.size() * sizeof(EntryVertex));

  if (payload.entryHasIndices && !payload.entryIndices.empty() && m_entryIndexBuffer) {
    m_entryIndexBuffer->copyData(payload.entryIndices.data(), payload.entryIndices.size() * sizeof(uint32_t));
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
    std::vector<vk::DescriptorSetLayout> descriptorLayouts{**m_entrySetLayout,
                                                           **m_emptySetLayout,
                                                           **m_transformSetLayout};
    instance.pipeline->setDescriptorSetLayouts(std::move(descriptorLayouts));
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
                                            .stride = sizeof(glm::vec2),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32Sfloat,
                                           .offset = 0};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_progressiveSetLayout, **m_emptySetLayout, **m_pageSetLayout});
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
                                            .stride = sizeof(glm::vec2),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32Sfloat,
                                           .offset = 0};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_progressiveSetLayout, **m_emptySetLayout, **m_pageSetLayout});
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
                                            .stride = sizeof(glm::vec2),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32Sfloat,
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
                                            .stride = sizeof(glm::vec2),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32Sfloat,
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
  // Honor debug depth toggles; also disabling depth when showing layer0-only to ensure shader runs everywhere.
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthWriteEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);

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

  // Screen-space quad as triangle strip with vec2 positions at location 0
  vk::VertexInputBindingDescription binding{.binding = 0,
                                            .stride = sizeof(glm::vec2),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32Sfloat,
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
                                                                        size_t channelIndex)
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
  }

  if (byteSize > 0 && data) {
    resources.volumeTexture->uploadData(data, byteSize);
  }

  return *resources.volumeTexture;
}

ZVulkanTexture& ZVulkanImgRaycasterPipelineContext::ensureTransferTexture(ChannelResources& resources,
                                                                          const Z3DTransferFunction& transferFunction)
{
  auto& device = m_backend.device();
  const uint32_t width = static_cast<uint32_t>(transferFunction.dimensions().x);
  const uint64_t gen = transferFunction.generation();

  bool createdOrResized = false;
  if (!resources.transferTexture || resources.transferWidth != width) {
    vulkan::ensure1DLUTTexture(device, resources.transferTexture, width);
    resources.transferWidth = width;
    createdOrResized = true;
  }

  if (createdOrResized || resources.transferGeneration != gen) {
    std::vector<uint8_t> texels;
    transferFunction.buildLUTBGRA8(texels, width);
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
  if (!resources.fastDescriptor) {
    resources.fastDescriptor = m_backend.allocateOverrideDescriptorSet(**m_fastSetLayout);
  }
  CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path: override descriptor allocation failed (fatal)";
  resources.fastDescriptor->updateTexture(0, entryExitTexture, m_backend.defaultSampler());
  resources.fastDescriptor->updateTexture(1, volume, m_backend.defaultSampler());
  resources.fastDescriptor->updateTexture(2, transfer, m_backend.defaultSampler());

  if (!resources.rayParamBuffer) {
    resources.rayParamBuffer = m_backend.device().createBuffer(sizeof(RayParamsData),
                                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  RayParamsData params{};
  params.samplingRate = payload.samplingRate;
  params.isoValue = payload.isoValue;
  params.localMIPThreshold = payload.localMIPThreshold;
  params.zeToZWA = zeToZW_a;
  params.zeToZWB = zeToZW_b;
  params.volumeDimensions = volumeDimensions;
  resources.rayParamBuffer->copyData(&params, sizeof(RayParamsData));

  VLOG(2) << "Raycaster UBO params: ch=" << channelIndex << " sampling=" << params.samplingRate
          << " iso=" << params.isoValue << " localMIP=" << params.localMIPThreshold << " zeToZW=(" << params.zeToZWA
          << "," << params.zeToZWB << ")"
          << " volDim=(" << params.volumeDimensions.x << "," << params.volumeDimensions.y << ","
          << params.volumeDimensions.z << ")";

  if (!resources.rayParamDescriptor) {
    resources.rayParamDescriptor = m_backend.allocateOverrideDescriptorSet(**m_rayParamSetLayout);
  }
  CHECK(resources.rayParamDescriptor != nullptr) << "Raycaster params: override descriptor allocation failed (fatal)";
  resources.rayParamDescriptor->updateUniformBuffer(3, *resources.rayParamBuffer);
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
  if (!resources.pagedDescriptor) {
    resources.pagedDescriptor = m_backend.allocateOverrideDescriptorSet(**m_progressiveSetLayout);
  }

  if (!resources.pagedDescriptor) {
    LOG_FIRST_N(ERROR, 5) << "Failed to allocate Vulkan raycaster paging descriptor set.";
    return false;
  }

  auto* pageDirectory =
    m_imageBlockUploader ? m_imageBlockUploader->pageDirectoryTexture(*payload.image, channelIndex) : nullptr;
  auto* pageTable =
    m_imageBlockUploader ? m_imageBlockUploader->pageTableTexture(*payload.image, channelIndex) : nullptr;
  auto* imageCache =
    m_imageBlockUploader ? m_imageBlockUploader->imageCacheTexture(*payload.image, channelIndex) : nullptr;
  if (!pageDirectory || !pageTable || !imageCache) {
    LOG_FIRST_N(WARNING, 5) << "Paging textures unavailable for Vulkan raycaster channel " << channelIndex;
    return false;
  }

  resources.pagedDescriptor->updateTexture(0, *pageDirectory, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(1, *pageTable, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(2, *imageCache, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(3, volume, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(4, transfer, m_backend.defaultSampler());
  resources.pagedDescriptor->updateTexture(5, entryExit, m_backend.defaultSampler());
  const auto [lastDepthLayout, lastDepthAspect] = depthReadDescriptorLayoutAndAspect(lastDepth);
  resources.pagedDescriptor->updateTexture(6, lastDepth, m_backend.defaultSampler(), lastDepthLayout, lastDepthAspect);
  resources.pagedDescriptor->updateTexture(7, lastColor, m_backend.defaultSampler());

  const uint32_t levelCount = static_cast<uint32_t>(std::min<size_t>(image.numLevels(), kMaxPagingLevels));
  auto pageData = buildPageDataBuffer(image, channelIndex, zeToScreenPixelVoxelSize, levelCount);

  if (!resources.pageDataBuffer || resources.pageDataCapacity < pageData.size()) {
    resources.pageDataBuffer = m_backend.device().createBuffer(pageData.size(),
                                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  if (!resources.pageDataBuffer) {
    LOG_FIRST_N(ERROR, 5) << "Failed to allocate Vulkan raycaster paging uniform buffer.";
    return false;
  }

  resources.pageDataBuffer->copyData(pageData.data(), pageData.size());
  resources.pageDataCapacity = pageData.size();

  if (!resources.pageDescriptor) {
    resources.pageDescriptor = m_backend.allocateOverrideDescriptorSet(**m_pageSetLayout);
  }

  if (!resources.pageDescriptor) {
    LOG_FIRST_N(ERROR, 5) << "Failed to allocate Vulkan raycaster page descriptor set.";
    return false;
  }

  resources.pageDescriptor->updateUniformBuffer(2, *resources.pageDataBuffer);
  if (resources.rayParamBuffer) {
    resources.pageDescriptor->updateUniformBuffer(3, *resources.rayParamBuffer);
  }

  resources.levelCount = levelCount;
  return true;
}

void ZVulkanImgRaycasterPipelineContext::bindProgressiveDescriptors(ChannelResources& resources,
                                                                    vk::PipelineLayout layout,
                                                                    vk::raii::CommandBuffer& cmd)
{
  if (!resources.pagedDescriptor || !resources.pageDescriptor) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive descriptors not initialised.";
    return;
  }

  ensureEmptyDescriptor();

  std::array<vk::DescriptorSet, 3> sets{resources.pagedDescriptor->descriptorSet(),
                                        m_emptyDescriptor ? m_emptyDescriptor->descriptorSet() : vk::DescriptorSet{},
                                        resources.pageDescriptor->descriptorSet()};
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
  if (!texture) {
    LOG_FIRST_N(WARNING, 5) << "Entry/exit lease missing color attachment.";
    return;
  }
  VLOG(2) << fmt::format("Raycaster::renderEntryExit target tex=0x{:x}", reinterpret_cast<uint64_t>(texture));

  ensureEntryPipelines(texture->format());
  ensureEntryTransformResources(renderer, batch, payload);
  ensureEmptyDescriptor();

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
    std::array<vk::DescriptorSet, 3> descriptorSets{
      m_emptyDescriptor ? m_emptyDescriptor->descriptorSet() : vk::DescriptorSet{},
      m_emptyDescriptor ? m_emptyDescriptor->descriptorSet() : vk::DescriptorSet{},
      m_entryTransformDescriptor ? m_entryTransformDescriptor->descriptorSet() : vk::DescriptorSet{}};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           pipeline.pipeline->pipelineLayout(),
                           0,
                           descriptorSets,
                           {});
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
          LOG(WARNING) << "Entry/exit debug save: color attachment missing";
          return;
        }
        if (tex->format() != vk::Format::eR32G32B32A32Sfloat) {
          LOG(WARNING) << "Entry/exit debug save: unsupported format for save (expected RGBA32F)";
        }
        const uint32_t w = tex->width();
        const uint32_t h = tex->height();
        const size_t pixels = static_cast<size_t>(w) * h;
        const size_t bytes = pixels * 4u * sizeof(float);

        auto saveLayer = [&](uint32_t layer, const char* label) {
          std::vector<float> buf;
          buf.resize(pixels * 4u);
          try {
            tex->downloadArrayLayer(buf.data(), bytes, layer);
          }
          catch (const std::exception& e) {
            LOG(ERROR) << "Entry/exit debug save: download failed for layer " << layer << ": " << e.what();
            return;
          }
          QString base = QString("entry_exit_%1_%2x%3.tif").arg(label).arg(w).arg(h);
          QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!dir.isEmpty() && !dir.endsWith('/')) {
            dir += '/';
          }
          const QString filename = dir + base;
          // Save synchronously on the rendering thread; keep buffer alive until save completes.
          ZImg img;
          img.wrapData(buf.data(), w, h, 1, 4);
          ZImg tmp(img.info());
          ZImgFormat::CXYZtoXYZC(img, tmp);
          tmp.flip(Dimension::Y);
          try {
            tmp.save(filename);
            LOG(INFO) << "Entry/exit debug saved: " << filename.toStdString();
          }
          catch (const ZException& ze) {
            LOG(ERROR) << "Entry/exit debug save failed: " << ze.what();
          }
        };

        // Save front (layer 0) and back (layer 1)
        saveLayer(0u, "front");
        if (tex->arrayLayers() > 1u) {
          saveLayer(1u, "back");
        }
        // Optionally also save the depth attachment of the merged output
        if (FLAGS_atlas_debug_save_raycaster_merge_out) {
          // We assume the depth handle is aligned with the same active surface
          // Capture the first valid depth attachment from the original pass
        }
      });
    }
  }
}

void ZVulkanImgRaycasterPipelineContext::ensureFastPipeline(ImgCompositingMode mode,
                                                            bool resultOpaque,
                                                            const vulkan::AttachmentFormats& formats)
{
  FastPipelineKey key;
  key.mode = mode;
  key.resultOpaque = resultOpaque;
  key.depthEnabled = true;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  if (m_fastPipeline.pipeline && m_fastPipelineKey && *m_fastPipelineKey == key) {
    return;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  if (!m_fastPipeline.shader) {
    m_fastPipeline.shader = std::make_unique<ZVulkanShader>(device,
                                                            shaderBase + "pass.vert.spv",
                                                            shaderBase + "volume_raycaster_single_channel.frag.spv",
                                                            std::nullopt);
  }

  // Screen quad vertex input (vec2 positions)
  vk::VertexInputBindingDescription binding{.binding = 0,
                                            .stride = sizeof(glm::vec2),
                                            .inputRate = vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attr{.location = 0,
                                           .binding = 0,
                                           .format = vk::Format::eR32G32Sfloat,
                                           .offset = 0};
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  m_fastPipeline.pipeline.reset();
  m_fastPipeline.pipeline =
    device.createPipeline(*m_fastPipeline.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  m_fastPipeline.pipeline->setDescriptorSetLayouts({**m_fastSetLayout, **m_emptySetLayout, **m_rayParamSetLayout});
  m_fastPipeline.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  m_fastPipeline.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  m_fastPipeline.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  const bool depthEnabled = key.depthEnabled;
  m_fastPipeline.pipeline->setDepthTestEnable(depthEnabled);
  m_fastPipeline.pipeline->setDepthWriteEnable(depthEnabled);
  m_fastPipeline.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);

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
  m_fastPipeline.pipeline->setColorBlendAttachment(blend);

  std::array<vk::SpecializationMapEntry, 3> entries{
    vk::SpecializationMapEntry{.constantID = 80, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 81, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 51, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };
  std::array<uint32_t, 3> values{0u, 0u, resultOpaque ? 1u : 0u};
  if (mode == ImgCompositingMode::MaximumIntensityProjection || mode == ImgCompositingMode::LocalMIP ||
      mode == ImgCompositingMode::MIPOpaque || mode == ImgCompositingMode::LocalMIPOpaque) {
    values[0] = 1u; // MIP
  } else if (mode == ImgCompositingMode::IsoSurface) {
    values[0] = 2u;
  } else if (mode == ImgCompositingMode::XRay) {
    values[0] = 3u;
  }
  if (mode == ImgCompositingMode::LocalMIP || mode == ImgCompositingMode::LocalMIPOpaque) {
    values[1] = 1u;
  }
  std::vector<uint8_t> data(sizeof(values));
  std::memcpy(data.data(), values.data(), sizeof(values));
  m_fastPipeline.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                                    std::vector(entries.begin(), entries.end()),
                                                    data);

  m_fastPipeline.pipeline->create();
  m_fastPipeline.colorFormats = formats.colorFormats;
  m_fastPipeline.depthFormat = formats.depthFormat;
  m_fastPipelineKey = key;
}

void ZVulkanImgRaycasterPipelineContext::renderFastPath(Z3DRendererBase& renderer,
                                                        const RenderBatch& batch,
                                                        const ImgRaycasterPayload& payload,
                                                        const vk::Viewport& viewport,
                                                        const vk::Rect2D& scissor,
                                                        vk::raii::CommandBuffer& cmd)
{
  VLOG(2) << "VK raycaster fast-path: channels=" << payload.visibleChannels.size()
          << " output=" << static_cast<int>(payload.outputSize.x) << "x" << static_cast<int>(payload.outputSize.y)
          << " mode=" << static_cast<int>(payload.compositingMode) << " fastOnly=" << payload.fastPathOnly;
  const size_t channelCount = payload.visibleChannels.size();
  if (channelCount == 0) {
    return;
  }

  if (!payload.image) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan img raycaster missing image context.";
    return;
  }

  if (!payload.entryExitLease || !payload.entryExitLease->hasVulkanImage()) {
    LOG_FIRST_N(WARNING, 5) << "Raycaster fast path missing entry/exit lease.";
    return;
  }

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
  if (!entryTexture) {
    LOG_FIRST_N(WARNING, 5) << "Entry/exit texture unavailable.";
    return;
  }
  entryTexture->transitionLayout(cmd, entryTexture->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  entryTexture->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  ensureEmptyDescriptor();

  const ImgCompositingMode sanitizedMode = sanitizeMode(payload.compositingMode);
  const bool resultOpaque = resultsOpaque(payload.compositingMode);
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
      LOG_FIRST_N(WARNING, 5) << "Missing transfer function for channel " << channelIndex;
      return;
    }

    if (!m_backend.validateFormatsOrSkip(finalFormats, "img raycaster fast path")) {
      return;
    }
    ensureFastPipeline(sanitizedMode, resultOpaque, finalFormats);

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex);
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
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_fastPipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {offset});

    CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path missing fast descriptor";
    CHECK(resources.rayParamDescriptor != nullptr) << "Raycaster fast path missing parameter descriptor";
    std::array<vk::DescriptorSet, 3> fastSets{resources.fastDescriptor->descriptorSet(),
                                              m_emptyDescriptor ? m_emptyDescriptor->descriptorSet()
                                                                : vk::DescriptorSet{},
                                              resources.rayParamDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           m_fastPipeline.pipeline->pipelineLayout(),
                           0,
                           fastSets,
                           {});
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
    VLOG(2) << "VK raycaster fast-path draw: verts=" << m_quadVertexCount
            << " colorAttCount=" << finalColorAttachments.size() << " colorFmt0="
            << (finalFormats.colorFormats.empty() ? -1 : static_cast<int>(finalFormats.colorFormats.front()))
            << " depthFmt=" << (finalFormats.depthFormat ? static_cast<int>(*finalFormats.depthFormat) : -1)
            << " output=" << static_cast<int>(outputSize.x) << "x" << static_cast<int>(outputSize.y);
    cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
    cmd.endRendering();
    return;
  }

  auto* layerLease = payload.channelLayerLease ? payload.channelLayerLease.get() : nullptr;
  if (!layerLease || !layerLease->hasVulkanImage()) {
    LOG_FIRST_N(WARNING, 5) << "Multi-channel raycaster requires layer array lease.";
    return;
  }

  ZVulkanTexture* layerColor = layerLease->colorAttachment(0);
  ZVulkanTexture* layerDepth = layerLease->depthAttachmentTexture();
  if (!layerColor) {
    LOG_FIRST_N(WARNING, 5) << "Layer array color attachment unavailable.";
    return;
  }

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  if (layerDepth) {
    layerFormats.depthFormat = layerDepth->format();
  }
  ensureFastPipeline(sanitizedMode, resultOpaque, layerFormats);

  // Derive per-layer viewport/scissor from the actual layer target to avoid offset/extent mismatches.
  glm::uvec2 layerSize(layerColor->width(), layerColor->height());
  // Use the input viewport/scissor from the compositor; avoid overriding with texture extents.
  vk::Viewport layerViewport = viewport;
  vk::Rect2D layerRect = scissor;

  // Ensure layered depth array starts from a known state (1.0) before per-layer draws.
  // Even though each layer draw uses loadOp=Clear, pre-clearing avoids any driver-specific
  // behavior and guarantees clean background for layers that may be skipped.
  if (layerDepth) {
    const uint32_t layers = std::max(1u, layerDepth->arrayLayers());
    auto depthRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0u, 1u, 0u, layers};
    // Use full aspect mask for barriers so internal layout tracking stays correct for D24S8.
    const auto fullAspect = layerDepth->info().aspectMask;
    layerDepth->transitionLayout(cmd, layerDepth->layout(), vk::ImageLayout::eTransferDstOptimal, fullAspect);
    cmd.clearDepthStencilImage(layerDepth->image(),
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearDepthStencilValue{1.0f, 0u},
                               depthRange);
    const auto [attachLayout, _attachAspect] = depthAttachmentLayoutAndAspect(*layerDepth);
    layerDepth->transitionLayout(cmd, vk::ImageLayout::eTransferDstOptimal, attachLayout, fullAspect);
  }

  for (size_t order = 0; order < channelCount; ++order) {
    const size_t channelIndex = payload.visibleChannels[order];
    if (channelIndex >= transferFunctions.size() || transferFunctions[channelIndex] == nullptr) {
      LOG_FIRST_N(WARNING, 5) << "Missing transfer function for channel " << channelIndex;
      continue;
    }

    ChannelResources& resources = ensureChannelResources(channelIndex);
    const ZImg& channelImage = *payload.image->channelImageShared(channelIndex);
    ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex);
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
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_fastPipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {offset});

    CHECK(resources.fastDescriptor != nullptr) << "Raycaster fast path missing fast descriptor (layered)";
    CHECK(resources.rayParamDescriptor != nullptr) << "Raycaster fast path missing parameter descriptor (layered)";
    std::array<vk::DescriptorSet, 3> layeredSets{resources.fastDescriptor->descriptorSet(),
                                                 m_emptyDescriptor ? m_emptyDescriptor->descriptorSet()
                                                                   : vk::DescriptorSet{},
                                                 resources.rayParamDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           m_fastPipeline.pipeline->pipelineLayout(),
                           0,
                           layeredSets,
                           {});
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

    layerColor->transitionLayout(cmd,
                                 vk::ImageLayout::eColorAttachmentOptimal,
                                 vk::ImageLayout::eShaderReadOnlyOptimal);
    layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
    if (layerDepth) {
      const auto [depthReadLayout, depthDescAspect] = depthReadDescriptorLayoutAndAspect(*layerDepth);
      const auto barrierAspect = depthReadBarrierAspect(*layerDepth);
      layerDepth->transitionLayout(cmd, vk::ImageLayout::eDepthAttachmentOptimal, depthReadLayout, barrierAspect);
      layerDepth->setDescriptorLayout(depthReadLayout);
    }
  }

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
        if (!tex) {
          return;
        }
        const uint32_t w = tex->width();
        const uint32_t h = tex->height();
        const size_t pixels = static_cast<size_t>(w) * h * 4u;
        auto saveLayer = [&](uint32_t layer) {
          std::vector<uint8_t> bytes(pixels);
          try {
            tex->downloadArrayLayer(bytes.data(), bytes.size(), layer);
          }
          catch (...) {
            return;
          }
          QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!dir.isEmpty() && !dir.endsWith('/')) {
            dir += '/';
          }
          const QString filename = dir + QString("raycaster_layer_%1_%2x%3.tif").arg(layer).arg(w).arg(h);
          ZImg img;
          img.wrapData(bytes.data(), w, h, 1, 4);
          img.flip(Dimension::Y);
          try {
            img.save(filename);
          }
          catch (...) {
          }
        };
        const uint32_t totalLayers = std::min<uint32_t>(static_cast<uint32_t>(channelCount), tex->arrayLayers());
        for (uint32_t layer = 0; layer < totalLayers; ++layer) {
          saveLayer(layer);
        }
      });
    }
  }

  // Optional debug dump of layered raycaster depth array to TIFs (per-layer)
  if (FLAGS_atlas_debug_save_raycaster_layer_depth) {
    auto leaseRef = payload.channelLayerLease;
    auto* backend = dynamic_cast<Z3DRendererVulkanBackend*>(renderer.backend());
    if (backend && leaseRef && leaseRef->hasVulkanImage()) {
      backend->scheduleAfterCurrentFrameCompletion([leaseRef, channelCount]() {
        ZVulkanTexture* dtex = leaseRef->depthAttachmentTexture();
        if (!dtex) {
          return;
        }
        const uint32_t w = dtex->width(), h = dtex->height();
        const size_t pixels = static_cast<size_t>(w) * h;
        auto saveLayer = [&](uint32_t layer) {
          QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!dir.isEmpty() && !dir.endsWith('/')) {
            dir += '/';
          }
          const QString filename = dir + QString("raycaster_layer_depth_%1_%2x%3.tif").arg(layer).arg(w).arg(h);
          if (dtex->format() == vk::Format::eD32Sfloat) {
            std::vector<float> f(pixels);
            try {
              dtex->downloadArrayLayer(f.data(), f.size() * sizeof(float), layer, vk::ImageAspectFlagBits::eDepth);
            }
            catch (...) {
              return;
            }
            ZImg img;
            img.wrapData(f.data(), w, h, 1);
            img.flip(Dimension::Y);
            try {
              img.save(filename);
            }
            catch (...) {
            }
          } else {
            std::vector<uint32_t> raw(pixels);
            try {
              dtex->downloadArrayLayer(raw.data(),
                                       raw.size() * sizeof(uint32_t),
                                       layer,
                                       vk::ImageAspectFlagBits::eDepth);
            }
            catch (...) {
              return;
            }
            std::vector<uint32_t> u32(pixels);
            for (size_t i = 0; i < pixels; ++i) {
              uint32_t d24 = (raw[i] & 0x00FFFFFFu);
              u32[i] = static_cast<uint32_t>((static_cast<double>(d24) / 16777215.0) * 4294967295.0 + 0.5);
            }
            ZImg img;
            img.wrapData(u32.data(), w, h, 1);
            img.flip(Dimension::Y);
            try {
              img.save(filename);
            }
            catch (...) {
            }
          }
        };
        const uint32_t totalLayers = std::min<uint32_t>(static_cast<uint32_t>(channelCount), dtex->arrayLayers());
        for (uint32_t layer = 0; layer < totalLayers; ++layer) {
          saveLayer(layer);
        }
      });
    }
  }

  MergePipelineKey mergeKey{};
  mergeKey.numVolumes = static_cast<int>(channelCount);
  mergeKey.maxProjectionMerge = requiresMaxProjectionMerge(payload.compositingMode);
  mergeKey.resultOpaque = resultsOpaque(payload.compositingMode);
  mergeKey.colorFormats = finalFormats.colorFormats;
  mergeKey.depthFormat = finalFormats.depthFormat;

  auto& mergePipeline = ensureMergePipeline(mergeKey, finalFormats);
  bindMergeDescriptor(*layerColor, layerDepth);

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
          << " depth=" << (finalDepthAttachment ? 1 : 0) << " color0Fmt="
          << (finalFormats.colorFormats.empty() ? -1 : static_cast<int>(finalFormats.colorFormats.front()))
          << " depthFmt=" << (finalFormats.depthFormat ? static_cast<int>(*finalFormats.depthFormat) : -1);

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
        if (!handle.valid() || handle.backend != AttachmentBackend::Vulkan) {
          LOG(WARNING) << "Raycaster merge debug save: invalid color attachment handle";
          return;
        }
        auto& tex = vulkan::textureFromHandle(handle, m_backend.device(), "img raycaster merge debug");
        const uint32_t w = tex.width();
        const uint32_t h = tex.height();
        const size_t pixels = static_cast<size_t>(w) * h;

        auto saveRGBA = [&](const QString& filename, const std::vector<uint8_t>& rgba) {
          ZImg img;
          img.wrapData(const_cast<uint8_t*>(rgba.data()), w, h, 1, 4);
          ZImg tmp(img.info());
          ZImgFormat::CXYZtoXYZC(img, tmp);
          tmp.flip(Dimension::Y);
          try {
            tmp.save(filename);
            LOG(INFO) << "Raycaster merge debug saved: " << filename.toStdString();
          }
          catch (const ZException& ze) {
            LOG(ERROR) << "Raycaster merge debug save failed: " << ze.what();
          }
        };

        QString dir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
        if (!dir.isEmpty() && !dir.endsWith('/')) {
          dir += '/';
        }
        const QString filename = dir + QString("raycaster_merge_%1x%2.tif").arg(w).arg(h);

        switch (tex.format()) {
          case vk::Format::eR8G8B8A8Unorm: {
            std::vector<uint8_t> bytes(pixels * 4u);
            tex.downloadData(bytes.data(), bytes.size());
            saveRGBA(filename, bytes);
            break;
          }
          case vk::Format::eR16G16B16A16Unorm: {
            std::vector<uint16_t> u16(pixels * 4u);
            tex.downloadData(u16.data(), u16.size() * sizeof(uint16_t));
            std::vector<uint8_t> bytes(pixels * 4u);
            for (size_t i = 0; i < pixels; ++i) {
              auto to8 = [](uint16_t v) {
                return static_cast<uint8_t>((static_cast<uint32_t>(v) + 127u) / 257u);
              };
              bytes[4 * i + 0] = to8(u16[4 * i + 0]);
              bytes[4 * i + 1] = to8(u16[4 * i + 1]);
              bytes[4 * i + 2] = to8(u16[4 * i + 2]);
              bytes[4 * i + 3] = to8(u16[4 * i + 3]);
            }
            saveRGBA(filename, bytes);
            break;
          }
          default: {
            std::vector<uint8_t> bytes(pixels * 4u);
            tex.downloadData(bytes.data(), bytes.size());
            saveRGBA(filename, bytes);
            break;
          }
        }

        // Save depth if available
        if (depthHandle && depthHandle->valid() && depthHandle->backend == AttachmentBackend::Vulkan) {
          auto& dtex = vulkan::textureFromHandle(*depthHandle, m_backend.device(), "img raycaster merge depth debug");
          const uint32_t dw = dtex.width();
          const uint32_t dh = dtex.height();
          const size_t dpixels = static_cast<size_t>(dw) * dh;
          QString ddir = QString::fromStdString(FLAGS_atlas_debug_save_dir);
          if (!ddir.isEmpty() && !ddir.endsWith('/')) {
            ddir += '/';
          }
          const QString dname = ddir + QString("raycaster_merge_depth_%1x%2.tif").arg(dw).arg(dh);

          // Save the depth as float TIF (single channel), normalizing 0..1 if needed.
          std::vector<float> depthF(dpixels);
          if (dtex.format() == vk::Format::eD32Sfloat) {
            try {
              dtex.downloadData(depthF.data(), depthF.size() * sizeof(float));
            }
            catch (const std::exception& e) {
              LOG(ERROR) << "Depth download failed: " << e.what();
              return;
            }
          } else {
            std::vector<uint32_t> raw(dpixels);
            try {
              dtex.downloadData(raw.data(), raw.size() * sizeof(uint32_t));
            }
            catch (const std::exception& e) {
              LOG(ERROR) << "Depth download failed: " << e.what();
              return;
            }
            for (size_t i = 0; i < dpixels; ++i) {
              uint32_t d24 = (raw[i] & 0x00FFFFFFu);
              depthF[i] = static_cast<float>(d24) / 16777215.0f;
            }
          }
          ZImg dimg;
          dimg.wrapData(depthF.data(), dw, dh, 1);
          dimg.flip(Dimension::Y);
          try {
            dimg.save(dname);
            LOG(INFO) << "Raycaster merge depth saved: " << dname.toStdString();
          }
          catch (const ZException& ze) {
            LOG(ERROR) << "Depth save failed: " << ze.what();
          }
        }
      });
    }
  }
}

void ZVulkanImgRaycasterPipelineContext::renderProgressivePath(Z3DRendererBase& renderer,
                                                               const RenderBatch& batch,
                                                               const ImgRaycasterPayload& payload,
                                                               const vk::Viewport& viewport,
                                                               const vk::Rect2D& scissor,
                                                               vk::raii::CommandBuffer& cmd)
{
  const uint32_t channelCount = static_cast<uint32_t>(payload.visibleChannels.size());
  if (channelCount == 0u) {
    return;
  }

  if (!payload.blockIdLease || payload.blockIdLease->attachments == 0) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive path missing block-ID lease.";
    return;
  }

  if (!payload.lastAccumLease || !payload.currentAccumLease) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive path missing accumulator leases.";
    return;
  }

  if (!m_imageBlockUploader) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive path missing image block uploader.";
    return;
  }
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
  ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex);
  CHECK(payload.transferFunctions != nullptr)
    << "Raycaster progressive path: payload missing transferFunctions vector (fatal)";
  const auto& transferList = *payload.transferFunctions;
  if (channelIndex >= transferList.size() || transferList[channelIndex] == nullptr) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster missing transfer function for channel " << channelIndex;
    return;
  }
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferList[channelIndex]);

  auto* entryTexture = payload.entryExitLease ? payload.entryExitLease->colorAttachment(0) : nullptr;
  auto* lastColor = payload.lastAccumLease->colorAttachment(0);
  auto* lastDepth = payload.lastAccumLease->colorAttachment(1);
  auto* currentColor = payload.currentAccumLease->colorAttachment(0);
  auto* currentDepth = payload.currentAccumLease->colorAttachment(1);

  if (!entryTexture || !lastColor || !lastDepth || !currentColor || !currentDepth) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive path missing required textures.";
    return;
  }

  const glm::uvec2 outputSize = payload.outputSize;
  if (outputSize.x == 0u || outputSize.y == 0u) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive path requires non-zero output size.";
    return;
  }

  ensureProgressiveLayerTargets(outputSize, channelCount, payload.progressiveGeneration, cmd);
  ZVulkanTexture* layerColor = m_progressiveLayerColor.get();
  ZVulkanTexture* layerDepth = m_progressiveLayerDepth.get();
  if (!layerColor || !layerDepth) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive path missing layer-array targets.";
    return;
  }

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
  if (!firstBlockAttachment) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive path missing block-ID attachment.";
    return;
  }
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

  if (blockAttachments.empty()) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan raycaster progressive path failed to prepare block-ID attachments.";
    return;
  }

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
                                        payload.compositingMode,
                                        usesLocalMip(payload.compositingMode),
                                        resultsOpaque(payload.compositingMode)};
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
  mergeKey.maxProjectionMerge = requiresMaxProjectionMerge(payload.compositingMode);
  mergeKey.resultOpaque = resultsOpaque(payload.compositingMode);
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
