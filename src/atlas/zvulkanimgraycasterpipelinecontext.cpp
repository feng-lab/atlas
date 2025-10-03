#include "zvulkanimgraycasterpipelinecontext.h"

#include "z3dimg.h"
#include "z3dimgraycasterrenderer.h"
#include "z3drendererbase.h"
#include "z3drendererstates.h"
#include "z3dtransferfunction.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkanbuffer.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkanrenderconversions.h"
#include "zsysteminfo.h"

#include <fmt/format.h>
#include <algorithm>

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
  return vk::Rect2D{vk::Offset2D{0, 0}, vk::Extent2D{size.x, size.y}};
}

vk::PrimitiveTopology toVkTopology(uint32_t primitive)
{
  switch (static_cast<ZMesh::Type>(primitive)) {
    case ZMesh::Type::Triangles:
      return vk::PrimitiveTopology::eTriangleList;
    case ZMesh::Type::TriangleStrip:
      return vk::PrimitiveTopology::eTriangleStrip;
    case ZMesh::Type::Lines:
      return vk::PrimitiveTopology::eLineList;
    case ZMesh::Type::LineStrip:
      return vk::PrimitiveTopology::eLineStrip;
    default:
      return vk::PrimitiveTopology::eTriangleList;
  }
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

} // namespace

ZVulkanImgRaycasterPipelineContext::ZVulkanImgRaycasterPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanImgRaycasterPipelineContext::~ZVulkanImgRaycasterPipelineContext() = default;

void ZVulkanImgRaycasterPipelineContext::resetFrame()
{
  // Channel resources persist per renderer instance; nothing to clear per frame yet.
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

  if (payload.visibleChannels.size() != 1) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan img raycaster currently supports single-channel fast path only.";
    return;
  }

  if (!payload.fastPathOnly) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan img raycaster full-resolution paging not yet implemented.";
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  ensureQuadVertexBuffer();

  uploadEntryGeometry(payload);
  renderEntryExit(renderer, batch, payload, cmd);
  renderFastPath(renderer, batch, payload, viewport, scissor, cmd);
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                           .pBindings = bindings.data()};
    m_fastSetLayout.emplace(device, info);
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

void ZVulkanImgRaycasterPipelineContext::ensureEntryVertexCapacity(size_t vertexCount, size_t indexCount)
{
  auto& device = m_backend.device();
  if (vertexCount > m_entryVertexCapacity) {
    m_entryVertexCapacity = vertexCount;
    m_entryVertexBuffer = device.createBuffer(m_entryVertexCapacity * sizeof(EntryVertex),
                                              vk::BufferUsageFlagBits::eVertexBuffer,
                                              vk::MemoryPropertyFlagBits::eHostVisible |
                                                vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  if (indexCount > m_entryIndexCapacity) {
    m_entryIndexCapacity = indexCount;
    if (m_entryIndexCapacity == 0) {
      m_entryIndexBuffer.reset();
    } else {
      m_entryIndexBuffer = device.createBuffer(m_entryIndexCapacity * sizeof(uint32_t),
                                               vk::BufferUsageFlagBits::eIndexBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
    }
  }
}

void ZVulkanImgRaycasterPipelineContext::ensureQuadVertexBuffer()
{
  if (m_quadVertexBuffer && m_quadVertexCount == 4) {
    return;
  }

  auto& device = m_backend.device();
  std::array<glm::vec2, 4> quad = {glm::vec2{-1.f, -1.f}, glm::vec2{-1.f, 1.f}, glm::vec2{1.f, -1.f}, glm::vec2{1.f, 1.f}};
  m_quadVertexCount = quad.size();
  m_quadVertexBuffer = device.createBuffer(quad.size() * sizeof(glm::vec2),
                                           vk::BufferUsageFlagBits::eVertexBuffer,
                                           vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent);
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
  static const std::string shaderBase =
    ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  auto buildPipeline = [&](PipelineInstance& instance, vk::CullModeFlagBits cullMode) {
    instance.shader = std::make_unique<ZVulkanShader>(device,
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
                                          .offset = offsetof(EntryVertex, texCoord)}};
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
    instance.pipeline->setColorBlendAttachment(vk::PipelineColorBlendAttachmentState{.blendEnable = VK_FALSE,
                                                                                      .colorWriteMask = vk::ColorComponentFlagBits::eR |
                                                                                                       vk::ColorComponentFlagBits::eG |
                                                                                                       vk::ColorComponentFlagBits::eB |
                                                                                                       vk::ColorComponentFlagBits::eA});
    instance.pipeline->create();
  };

  if (!m_entryFrontPipeline.pipeline) {
    buildPipeline(m_entryFrontPipeline, vk::CullModeFlagBits::eFront);
  }
  if (!m_entryBackPipeline.pipeline) {
    buildPipeline(m_entryBackPipeline, vk::CullModeFlagBits::eBack);
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
                                                                        size_t channelIndex)
{
  const uint32_t width = static_cast<uint32_t>(image.width());
  const uint32_t height = static_cast<uint32_t>(image.height());
  const uint32_t depth = static_cast<uint32_t>(image.depth());
  const size_t byteSize = image.byteNumber();

  CHECK_EQ(image.info().bytesPerVoxel, 1u)
    << "Vulkan raycaster currently expects 8-bit single-channel volumes.";
  const uint8_t* data = image.channelData<uint8_t>(0);

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
  const uint32_t width = static_cast<uint32_t>(transferFunction.textureDimensions().x);
  if (!resources.transferTexture || resources.transferTexture->extent().width != width) {
    auto info = ZVulkanTexture::CreateInfo::make1D(width,
                                                   vk::Format::eR8G8B8A8Unorm,
                                                   vk::ImageUsageFlagBits::eSampled |
                                                     vk::ImageUsageFlagBits::eTransferDst,
                                                   vk::MemoryPropertyFlagBits::eDeviceLocal,
                                                   1u,
                                                   true,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
    resources.transferTexture = device.createTexture(info);
  }

  std::vector<glm::col4> texels(width);
  for (uint32_t x = 0; x < width; ++x) {
    texels[x] = transferFunction.mappedColorBGRA(static_cast<double>(x) / std::max<uint32_t>(1u, width - 1u));
  }
  resources.transferTexture->uploadData(texels.data(), texels.size() * sizeof(glm::col4));
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
  if (!resources.fastDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_fastSetLayout);
    resources.fastDescriptor =
      std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
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
    resources.rayParamDescriptor =
      std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(ds));
  }

  resources.rayParamDescriptor->updateUniformBuffer(3, *resources.rayParamBuffer);
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
  static const std::string shaderBase =
    ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

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

  m_fastPipeline.pipeline = device.createPipeline(*m_fastPipeline.shader,
                                                  vertexInput,
                                                  vk::PrimitiveTopology::eTriangleStrip);
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
    vk::SpecializationMapEntry{.constantID = 51, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}};
  std::array<uint32_t, 3> values{0u, 0u, resultOpaque ? 1u : 0u};
  if (mode == ImgCompositingMode::MaximumIntensityProjection ||
      mode == ImgCompositingMode::LocalMIP ||
      mode == ImgCompositingMode::MIPOpaque ||
      mode == ImgCompositingMode::LocalMIPOpaque) {
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
  if (payload.visibleChannels.empty()) {
    return;
  }

  if (payload.renderer == nullptr) {
    LOG_FIRST_N(WARNING, 5) << "Img raycaster payload missing renderer pointer.";
    return;
  }

  if (payload.image == nullptr) {
    LOG_FIRST_N(WARNING, 5) << "Img raycaster payload missing image pointer.";
    return;
  }

  if (!payload.entryExitLease || !payload.entryExitLease->hasVulkanImage()) {
    LOG_FIRST_N(WARNING, 5) << "Raycaster fast path missing entry/exit lease.";
    return;
  }

  ensureFastPipeline(sanitizeMode(payload.compositingMode),
                     payload.compositingMode == ImgCompositingMode::MIPOpaque ||
                       payload.compositingMode == ImgCompositingMode::LocalMIPOpaque);

  auto* entryTexture = payload.entryExitLease->colorAttachment(0);
  if (!entryTexture) {
    LOG_FIRST_N(WARNING, 5) << "Entry/exit texture unavailable.";
    return;
  }

  const size_t channelIndex = payload.visibleChannels.front();
  const auto& tfList = payload.renderer->transferFunctions();
  if (channelIndex >= tfList.size()) {
    LOG_FIRST_N(WARNING, 5) << "Transfer function missing for channel " << channelIndex;
    return;
  }
  ChannelResources& resources = ensureChannelResources(channelIndex);
  const ZImg& channelImage = payload.image->channelVolumeImage(channelIndex);
  ZVulkanTexture& volumeTex = ensureVolumeTexture(resources, channelImage, channelIndex);
  const auto* transferFunction = tfList[channelIndex];
  if (!transferFunction) {
    LOG_FIRST_N(WARNING, 5) << "Missing transfer function for channel " << channelIndex;
    return;
  }
  ZVulkanTexture& transferTex = ensureTransferTexture(resources, *transferFunction);

  const auto& viewState = renderer.viewState();
  const float n = viewState.nearClip;
  const float f = viewState.farClip;
  const float zeToZWA = f * n / (f - n);
  const float zeToZWB = 0.5f * (f + n) / (f - n) + 0.5f;

  updateChannelFastDescriptors(resources,
                               payload,
                               channelIndex,
                               *entryTexture,
                               volumeTex,
                               transferTex,
                               zeToZWA,
                               zeToZWB,
                               glm::vec3(static_cast<float>(channelImage.width()),
                                         static_cast<float>(channelImage.height()),
                                         static_cast<float>(channelImage.depth())));

  auto descriptorSet = resources.fastDescriptor->descriptorSet();
  vk::DescriptorSet rayParamSet = resources.rayParamDescriptor
                                    ? resources.rayParamDescriptor->descriptorSet()
                                    : vk::DescriptorSet{};

  std::vector<vk::RenderingAttachmentInfo> colorAttachments;
  colorAttachments.reserve(batch.pass.colorAttachments.size());
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (attachment.handle.backend != AttachmentBackend::Vulkan || attachment.handle.id == 0) {
      continue;
    }
    auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
    if (!texture) {
      continue;
    }
    texture->transitionLayout(cmd,
                              texture->layout(),
                              vk::ImageLayout::eColorAttachmentOptimal);
    vk::RenderingAttachmentInfo info{};
    info.imageView = texture->imageView();
    info.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
    info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
    info.clearValue = vk::ClearValue{vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                                              attachment.clearValue.color.g,
                                                                              attachment.clearValue.color.b,
                                                                              attachment.clearValue.color.a})};
    colorAttachments.push_back(info);
  }

  std::optional<vk::RenderingAttachmentInfo> depthAttachment{};
  if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.backend == AttachmentBackend::Vulkan &&
      batch.pass.depthAttachment->handle.id != 0) {
    auto* depth = reinterpret_cast<ZVulkanTexture*>(batch.pass.depthAttachment->handle.id);
    if (depth) {
      depth->transitionLayout(cmd,
                              depth->layout(),
                              vk::ImageLayout::eDepthStencilAttachmentOptimal);
      vk::RenderingAttachmentInfo info{};
      info.imageView = depth->imageView();
      info.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
      info.loadOp = vulkan::toVkLoadOp(batch.pass.depthAttachment->loadOp);
      info.storeOp = vulkan::toVkStoreOp(batch.pass.depthAttachment->storeOp);
      info.clearValue.depthStencil = vk::ClearDepthStencilValue(batch.pass.depthAttachment->clearValue.depth,
                                                               batch.pass.depthAttachment->clearValue.stencil);
      depthAttachment = info;
    }
  }

  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = scissor;
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
  renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
  vk::RenderingAttachmentInfo depthAttachmentInfo{};
  if (depthAttachment) {
    depthAttachmentInfo = *depthAttachment;
    renderingInfo.pDepthAttachment = &depthAttachmentInfo;
  }

  cmd.beginRendering(renderingInfo);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_fastPipeline.pipeline->pipeline());
  vk::DeviceSize offset = 0;
  cmd.bindVertexBuffers(0, {m_quadVertexBuffer->buffer()}, {offset});
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
}

} // namespace nim
