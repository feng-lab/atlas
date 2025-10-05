#include "zvulkanimgraycasterpipelinecontext.h"

#include "z3dimg.h"
#include "z3dimgraycasterrenderer.h"
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
#include "zsysteminfo.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanpagedimageblockuploader.h"
#include "zcancellation.h"

#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>

namespace nim {

namespace {

vk::Viewport makeViewport(const glm::uvec2& size)
{
  return vk::Viewport{0.0f,
                      0.0f,
                      static_cast<float>(std::max<uint32_t>(1u, size.x)),
                      static_cast<float>(std::max<uint32_t>(1u, size.y)),
                      0.0f,
                      1.0f};
}

vk::Rect2D makeRect(const glm::uvec2& size)
{
  return vk::Rect2D{
    vk::Offset2D{0,      0     },
    vk::Extent2D{size.x, size.y}
  };
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

} // namespace

ZVulkanImgRaycasterPipelineContext::ZVulkanImgRaycasterPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
  , m_imageBlockUploader(std::make_unique<ZVulkanPagedImageBlockUploader>(backend.device()))
{}

ZVulkanImgRaycasterPipelineContext::~ZVulkanImgRaycasterPipelineContext() = default;

void ZVulkanImgRaycasterPipelineContext::resetFrame()
{
  resetDescriptors();
}

void ZVulkanImgRaycasterPipelineContext::resetDescriptors()
{
  for (auto& channel : m_channelResources) {
    channel.fastDescriptor.reset();
    channel.rayParamDescriptor.reset();
    channel.pagedDescriptor.reset();
    channel.pageDescriptor.reset();
    channel.blockIdDescriptor.reset();
  }
  m_emptyDescriptor.reset();
  m_copyDescriptor.reset();
  m_mergeDescriptor.reset();
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
  renderEntryExit(renderer, batch, payload, cmd);

  if (payload.fastPathOnly) {
    renderFastPath(renderer, batch, payload, viewport, scissor, cmd);
  } else {
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
  auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_emptySetLayout);
  m_emptyDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
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

void ZVulkanImgRaycasterPipelineContext::ensureEntryPipelines()
{
  if (m_entryFrontPipeline.pipeline && m_entryBackPipeline.pipeline) {
    return;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  auto buildPipeline = [&](PipelineInstance& instance, vk::CullModeFlagBits cullMode) {
    instance.shader =
      std::make_unique<ZVulkanShader>(device,
                                      shaderBase + "transform_with_3dtexture_and_eye_coordinate.vert.spv",
                                      shaderBase + "render_3dtexture_coordinate_and_eye_coordinate.frag.spv",
                                      std::nullopt);

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

    instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
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
  };

  if (!m_entryFrontPipeline.pipeline) {
    buildPipeline(m_entryFrontPipeline, vk::CullModeFlagBits::eFront);
  }
  if (!m_entryBackPipeline.pipeline) {
    buildPipeline(m_entryBackPipeline, vk::CullModeFlagBits::eBack);
  }
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
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);

  vk::PipelineColorBlendAttachmentState colorBlend{};
  colorBlend.blendEnable = VK_TRUE;
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
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
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
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthWriteEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE;
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  std::vector<vk::PipelineColorBlendAttachmentState> blends(formats.colorFormats.size(), blend);
  instance.pipeline->setColorBlendAttachments(std::move(blends));

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

  auto [inserted, _] = m_mergePipelines.emplace(key, std::move(instance));
  return inserted->second;
}

void ZVulkanImgRaycasterPipelineContext::bindMergeDescriptor(ZVulkanTexture& colorArray, ZVulkanTexture* depthArray)
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
  std::vector<uint8_t> texels;
  transferFunction.buildLUTBGRA8(texels, width);
  vulkan::ensure1DLUTTexture(device, resources.transferTexture, width);
  vulkan::uploadLUT(*resources.transferTexture, texels.data(), texels.size());
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
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_fastSetLayout);
    resources.fastDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }

  resources.fastDescriptor->updateTexture(0, entryExitTexture);
  resources.fastDescriptor->updateTexture(1, volume);
  resources.fastDescriptor->updateTexture(2, transfer);

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

  if (!resources.rayParamDescriptor) {
    auto ds = m_descriptorPool->allocateDescriptorSet(**m_rayParamSetLayout);
    resources.rayParamDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(ds));
  }

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
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_progressiveSetLayout);
    resources.pagedDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
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

  resources.pagedDescriptor->updateTexture(0, *pageDirectory);
  resources.pagedDescriptor->updateTexture(1, *pageTable);
  resources.pagedDescriptor->updateTexture(2, *imageCache);
  resources.pagedDescriptor->updateTexture(3, volume);
  resources.pagedDescriptor->updateTexture(4, transfer);
  resources.pagedDescriptor->updateTexture(5, entryExit);
  resources.pagedDescriptor->updateTexture(6, lastDepth);
  resources.pagedDescriptor->updateTexture(7, lastColor);

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
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_pageSetLayout);
    resources.pageDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
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
                                                         vk::raii::CommandBuffer& cmd)
{
  ensureEntryPipelines();

  auto* texture = payload.entryExitLease->colorAttachment(0);
  if (!texture) {
    LOG_FIRST_N(WARNING, 5) << "Entry/exit lease missing color attachment.";
    return;
  }

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

    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = makeRect(payload.entryExitSize);
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    cmd.beginRendering(renderingInfo);
    vk::DeviceSize offset = 0;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
    cmd.bindVertexBuffers(0, {m_entryVertexBuffer->buffer()}, {offset});
    cmd.pushConstants<EntryPushConstant>(pipeline.pipeline->pipelineLayout(),
                                         vk::ShaderStageFlagBits::eVertex,
                                         0,
                                         pushConstant);
    cmd.setViewport(0, makeViewport(payload.entryExitSize));
    cmd.setScissor(0, makeRect(payload.entryExitSize));
    if (payload.entryHasIndices && m_entryIndexBuffer) {
      cmd.bindIndexBuffer(m_entryIndexBuffer->buffer(), 0, vk::IndexType::eUint32);
      cmd.drawIndexed(static_cast<uint32_t>(payload.entryIndices.size()), 1, 0, 0, 0);
    } else {
      cmd.draw(static_cast<uint32_t>(payload.entryPositions.size()), 1, 0, 0);
    }
    cmd.endRendering();
  }

  // Transition for sampling in raycaster path
  texture->transitionLayout(cmd, texture->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  texture->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
}

void ZVulkanImgRaycasterPipelineContext::ensureFastPipeline(ImgCompositingMode mode, bool resultOpaque)
{
  if (m_fastPipeline.pipeline) {
    return;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  m_fastPipeline.shader = std::make_unique<ZVulkanShader>(device,
                                                          shaderBase + "pass.vert.spv",
                                                          shaderBase + "volume_raycaster_single_channel.frag.spv",
                                                          std::nullopt);

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

  m_fastPipeline.pipeline =
    device.createPipeline(*m_fastPipeline.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  m_fastPipeline.pipeline->setDescriptorSetLayouts({**m_fastSetLayout, **m_rayParamSetLayout});
  m_fastPipeline.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  m_fastPipeline.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  m_fastPipeline.pipeline->setDepthTestEnable(true);
  m_fastPipeline.pipeline->setDepthWriteEnable(true);
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
}

void ZVulkanImgRaycasterPipelineContext::renderFastPath(Z3DRendererBase& renderer,
                                                        const RenderBatch& batch,
                                                        const ImgRaycasterPayload& payload,
                                                        const vk::Viewport& viewport,
                                                        const vk::Rect2D& scissor,
                                                        vk::raii::CommandBuffer& cmd)
{
  const size_t channelCount = payload.visibleChannels.size();
  if (channelCount == 0) {
    return;
  }

  if (!payload.renderer || !payload.image) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan img raycaster missing renderer or image context.";
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
    texture.transitionLayout(cmd, texture.layout(), vk::ImageLayout::eDepthStencilAttachmentOptimal);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
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

  auto* entryTexture = payload.entryExitLease->colorAttachment(0);
  if (!entryTexture) {
    LOG_FIRST_N(WARNING, 5) << "Entry/exit texture unavailable.";
    return;
  }
  entryTexture->transitionLayout(cmd, entryTexture->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  entryTexture->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  const auto& viewState = renderer.viewState();
  const float nearClip = std::abs(viewState.nearClip) < 1e-6f ? 1e-6f : viewState.nearClip;
  const float farClip = viewState.farClip;
  const float zeToZW_a = farClip * nearClip / (farClip - nearClip);
  const float zeToZW_b = 0.5f * (farClip + nearClip) / (farClip - nearClip) + 0.5f;

  const auto& transferFunctions = payload.renderer->transferFunctions();

  ensureFastPipeline(sanitizeMode(payload.compositingMode), resultsOpaque(payload.compositingMode));

  if (channelCount == 1) {
    const size_t channelIndex = payload.visibleChannels.front();
    if (channelIndex >= transferFunctions.size() || transferFunctions[channelIndex] == nullptr) {
      LOG_FIRST_N(WARNING, 5) << "Missing transfer function for channel " << channelIndex;
      return;
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

    auto descriptorSet = resources.fastDescriptor->descriptorSet();
    vk::DescriptorSet rayParamSet =
      resources.rayParamDescriptor ? resources.rayParamDescriptor->descriptorSet() : vk::DescriptorSet{};
    if (rayParamSet) {
      std::array<vk::DescriptorSet, 2> sets{descriptorSet, rayParamSet};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_fastPipeline.pipeline->pipelineLayout(), 0, sets, {});
    } else {
      std::array<vk::DescriptorSet, 1> sets{descriptorSet};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_fastPipeline.pipeline->pipelineLayout(), 0, sets, {});
    }
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
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

  vk::Viewport layerViewport = makeViewport(outputSize);
  vk::Rect2D layerRect = makeRect(outputSize);

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
      layerDepth->transitionLayout(cmd, layerDepth->layout(), vk::ImageLayout::eDepthStencilAttachmentOptimal);
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
        depthAttachmentInfo.imageView = depthView;
        depthAttachmentInfo.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
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

    auto descriptorSet = resources.fastDescriptor->descriptorSet();
    vk::DescriptorSet rayParamSet =
      resources.rayParamDescriptor ? resources.rayParamDescriptor->descriptorSet() : vk::DescriptorSet{};
    if (rayParamSet) {
      std::array<vk::DescriptorSet, 2> sets{descriptorSet, rayParamSet};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_fastPipeline.pipeline->pipelineLayout(), 0, sets, {});
    } else {
      std::array<vk::DescriptorSet, 1> sets{descriptorSet};
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_fastPipeline.pipeline->pipelineLayout(), 0, sets, {});
    }
    cmd.setViewport(0, layerViewport);
    cmd.setScissor(0, layerRect);
    cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
    cmd.endRendering();

    layerColor->transitionLayout(cmd,
                                 vk::ImageLayout::eColorAttachmentOptimal,
                                 vk::ImageLayout::eShaderReadOnlyOptimal);
    layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
    if (layerDepth) {
      layerDepth->transitionLayout(cmd,
                                   vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                   vk::ImageLayout::eDepthReadOnlyOptimal,
                                   vk::ImageAspectFlagBits::eDepth);
      layerDepth->setDescriptorLayout(vk::ImageLayout::eDepthReadOnlyOptimal);
    }
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
  mergeInfo.renderArea = scissor;
  mergeInfo.layerCount = 1;
  mergeInfo.colorAttachmentCount = static_cast<uint32_t>(finalColorAttachments.size());
  mergeInfo.pColorAttachments = finalColorAttachments.empty() ? nullptr : finalColorAttachments.data();
  vk::RenderingAttachmentInfo mergeDepthAttachment{};
  if (finalDepthAttachment) {
    mergeDepthAttachment = *finalDepthAttachment;
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
  const auto& transferList = payload.renderer->transferFunctions();
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
  lastDepth->transitionLayout(cmd,
                              lastDepth->layout(),
                              vk::ImageLayout::eDepthReadOnlyOptimal,
                              vk::ImageAspectFlagBits::eDepth);
  entryTexture->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  lastColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  lastDepth->setDescriptorLayout(vk::ImageLayout::eDepthReadOnlyOptimal);

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

  vk::Rect2D blockRect = makeRect(payload.blockIdLease->descriptor.size);
  vk::Viewport blockViewport = makeViewport(payload.blockIdLease->descriptor.size);

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
  currentDepth->transitionLayout(cmd, currentDepth->layout(), vk::ImageLayout::eColorAttachmentOptimal);

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
  currentDepth->transitionLayout(cmd, currentDepth->layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
  currentColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  currentDepth->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  // Copy the current accumulation into the persistent layer array slice
  layerColor->transitionLayout(cmd, layerColor->layout(), vk::ImageLayout::eColorAttachmentOptimal);
  layerDepth->transitionLayout(cmd,
                               layerDepth->layout(),
                               vk::ImageLayout::eDepthStencilAttachmentOptimal,
                               vk::ImageAspectFlagBits::eDepth);

  vulkan::AttachmentFormats layerFormats;
  layerFormats.colorFormats.push_back(layerColor->format());
  layerFormats.depthFormat = layerDepth ? std::optional<vk::Format>(layerDepth->format()) : std::nullopt;
  CopyPipelineKey layerCopyKey{layerFormats.colorFormats, layerFormats.depthFormat};
  auto& layerCopyPipeline = ensureCopyPipeline(layerCopyKey, layerFormats);

  if (!m_copyDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_copySetLayout);
    m_copyDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }
  m_copyDescriptor->updateTexture(0, *currentColor);
  m_copyDescriptor->updateTexture(1, *currentDepth);

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
    layerDepthInfo.imageView = layerDepthView;
    layerDepthInfo.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    layerDepthInfo.loadOp = vk::AttachmentLoadOp::eClear;
    layerDepthInfo.storeOp = vk::AttachmentStoreOp::eStore;
    layerDepthInfo.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);
    layerDepthAttachment = layerDepthInfo;
  }

  vk::RenderingInfo layerInfo{};
  layerInfo.renderArea = makeRect(outputSize);
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
  cmd.setViewport(0, makeViewport(outputSize));
  cmd.setScissor(0, makeRect(outputSize));
  cmd.draw(static_cast<uint32_t>(m_quadVertexCount), 1, 0, 0);
  cmd.endRendering();

  layerColor->transitionLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
  layerColor->setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
  layerDepth->transitionLayout(cmd,
                               vk::ImageLayout::eDepthStencilAttachmentOptimal,
                               vk::ImageLayout::eDepthReadOnlyOptimal,
                               vk::ImageAspectFlagBits::eDepth);
  layerDepth->setDescriptorLayout(vk::ImageLayout::eDepthReadOnlyOptimal);

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
    texture.transitionLayout(cmd, texture.layout(), vk::ImageLayout::eDepthStencilAttachmentOptimal);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture.imageView();
    info.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue.depthStencil =
      vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
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

  payload.renderer->finalizeProgressiveRound(batch.eye, lastRound, payload.visibleChannels.size());
}

} // namespace nim
