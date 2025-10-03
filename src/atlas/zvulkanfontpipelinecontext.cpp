#include "zvulkanfontpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dfontrenderer.h"
#include "z3dsdfont.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkanbuffer.h"
#include "zvulkanrenderconversions.h"
#include "zlog.h"

#include <array>

namespace nim {

ZVulkanFontPipelineContext::ZVulkanFontPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanFontPipelineContext::~ZVulkanFontPipelineContext() = default;

void ZVulkanFontPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
}

void ZVulkanFontPipelineContext::record(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const FontPayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd)
{
  if (!payload.renderer || payload.positions.empty() || payload.texcoords.empty() || payload.indices.empty()) {
    return;
  }

  if (payload.pickingPass && payload.pickingColors.empty()) {
    return;
  }
  if (!payload.pickingPass && payload.colors.empty()) {
    return;
  }

  // Upload/prepare geometry
  uploadGeometry(payload);
  if (!m_vertexBuffer || !m_indexBuffer || m_vertexCount == 0 || m_indexCount == 0) {
    return;
  }

  // Ensure texture descriptor
  ensureDescriptorLayout();
  ensureDescriptorSet();
  ZVulkanTexture* atlas = payload.atlasTexture ? ensureTextureUpload(*payload.atlasTexture) : nullptr;
  if (!m_descriptorSet || !atlas) {
    return;
  }
  m_descriptorSet->updateTexture(0, *atlas);

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.picking = payload.pickingPass;
  key.showOutline = payload.showOutline;
  key.showShadow = payload.showShadow;
  key.outlineMode = payload.outlineMode;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  // Bind pipeline and resources
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());

  vk::DeviceSize offsets = 0;
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offsets});
  cmd.bindIndexBuffer(m_indexBuffer->buffer(), 0, vk::IndexType::eUint32);

  if (m_descriptorSet) {
    std::array<vk::DescriptorSet, 1> sets{m_descriptorSet->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  // Compose push constants
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  const auto& params = renderer.parameterState();
  glm::mat4 mvp = eyeState.projectionMatrix * eyeState.viewMatrix * params.coordTransform;

  FontPushConstants constants;
  constants.mvp = mvp;
  constants.outlineColor = payload.outlineColor;
  constants.shadowColor = payload.shadowColor;
  constants.softedgeScale = payload.softedgeScale;
  uint32_t flags = 0u;
  flags |= (payload.pickingPass ? 1u : 0u);
  flags |= (payload.showOutline ? (1u << 1) : 0u);
  flags |= (payload.showShadow ? (1u << 2) : 0u);
  flags |= (static_cast<uint32_t>(payload.outlineMode & 0xFF) << 8);
  constants.flags = flags;

  cmd.pushConstants<FontPushConstants>(pipeline.pipeline->pipelineLayout(),
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       constants);

  cmd.drawIndexed(static_cast<uint32_t>(m_indexCount), 1, 0, 0, 0);
}

void ZVulkanFontPipelineContext::ensureDescriptorLayout()
{
  if (m_setTexture) {
    return;
  }
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  std::array<vk::DescriptorSetLayoutBinding, 1> bindings{
    vk::DescriptorSetLayoutBinding{.binding = 0,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eFragment}};

  vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                               .pBindings = bindings.data()};
  m_setTexture.emplace(vkDevice, createInfo);
}

void ZVulkanFontPipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();

  auto& device = m_backend.device();
  if (!m_descriptorPool) {
    m_descriptorPool = device.createDescriptorPool();
  }

  if (!m_descriptorSet) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_setTexture);
    m_descriptorSet = std::make_unique<ZVulkanDescriptorSet>(device, std::move(descriptorSet));
  }
}

vk::PipelineVertexInputStateCreateInfo ZVulkanFontPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(FontVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 3> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(FontVertex, position))},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(FontVertex, texcoord))},
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(FontVertex, color))}};

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanFontPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(FontVertex);
  if (requiredBytes <= m_vertexCapacity) {
    return;
  }
  size_t newCapacity = std::max(requiredBytes, m_vertexCapacity == 0 ? requiredBytes : m_vertexCapacity * 2);
  auto& device = m_backend.device();
  m_vertexBuffer = device.createBuffer(newCapacity,
                                       vk::BufferUsageFlagBits::eVertexBuffer,
                                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_vertexCapacity = newCapacity;
}

void ZVulkanFontPipelineContext::ensureIndexCapacity(size_t indexCount)
{
  const size_t requiredBytes = indexCount * sizeof(uint32_t);
  if (requiredBytes <= m_indexCapacity) {
    return;
  }
  size_t newCapacity = std::max(requiredBytes, m_indexCapacity == 0 ? requiredBytes : m_indexCapacity * 2);
  auto& device = m_backend.device();
  m_indexBuffer = device.createBuffer(newCapacity,
                                      vk::BufferUsageFlagBits::eIndexBuffer,
                                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_indexCapacity = newCapacity;
}

void ZVulkanFontPipelineContext::uploadGeometry(const FontPayload& payload)
{
  m_vertexCount = 0;
  m_indexCount = 0;
  const size_t vtxCount = payload.positions.size();
  const size_t idxCount = payload.indices.size();
  if (vtxCount == 0 || idxCount == 0) {
    return;
  }

  ensureVertexCapacity(vtxCount);
  ensureIndexCapacity(idxCount);
  if (!m_vertexBuffer || !m_indexBuffer) {
    return;
  }

  std::vector<FontVertex> vertices(vtxCount);
  for (size_t i = 0; i < vtxCount; ++i) {
    FontVertex v;
    v.position = payload.positions[i];
    v.texcoord = payload.texcoords[i];
    if (payload.pickingPass) {
      v.color = payload.pickingColors.empty() ? glm::vec4(0.0f) : payload.pickingColors[i];
    } else {
      v.color = payload.colors.empty() ? glm::vec4(0.0f) : payload.colors[i];
    }
    vertices[i] = v;
  }

  m_vertexBuffer->copyData(vertices.data(), vertices.size() * sizeof(FontVertex));
  m_indexBuffer->copyData(payload.indices.data(), payload.indices.size() * sizeof(uint32_t));
  m_vertexCount = vtxCount;
  m_indexCount = idxCount;
}

ZVulkanTexture* ZVulkanFontPipelineContext::ensureTextureUpload(const Z3DTexture& source)
{
  const auto it = m_textureCache.find(&source);
  if (it != m_textureCache.end()) {
    return it->second.get();
  }

  std::optional<vk::Format> format;
  if (source.dataFormat() == GL_BGRA && source.dataType() == GL_UNSIGNED_INT_8_8_8_8_REV) {
    // SDF atlas built as BGRA8
    format = vk::Format::eB8G8R8A8Unorm;
  } else if (source.dataFormat() == GL_RGBA && source.dataType() == GL_UNSIGNED_BYTE) {
    format = vk::Format::eR8G8B8A8Unorm;
  }

  if (!format) {
    LOG_FIRST_N(WARNING, 5) << "Skipping unsupported font atlas texture format for Vulkan backend.";
    return nullptr;
  }

  const size_t byteSize = source.textureSizeOnGPU();
  if (byteSize == 0) {
    return nullptr;
  }

  std::vector<uint8_t> pixels(byteSize);
  source.downloadTextureToBuffer(source.dataFormat(), source.dataType(), pixels.data());

  auto& device = m_backend.device();
  std::unique_ptr<ZVulkanTexture> vkTexture;
  auto info = ZVulkanTexture::CreateInfo::make2D(static_cast<uint32_t>(source.width()),
                                                 static_cast<uint32_t>(source.height()),
                                                 *format,
                                                 vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                                 vk::MemoryPropertyFlagBits::eDeviceLocal);
  vkTexture = device.createTexture(info);
  if (!vkTexture) {
    return nullptr;
  }
  vkTexture->uploadData(pixels.data(), pixels.size());

  auto [inserted, _] = m_textureCache.emplace(&source, std::move(vkTexture));
  return inserted->second.get();
}

ZVulkanFontPipelineContext::PipelineInstance&
ZVulkanFontPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayout();

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "almag.vert.spv",
                                                    shaderBase + "almag.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  instance.pipeline->setDescriptorSetLayouts({**m_setTexture});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthWriteEnable(false);

  // Blending
  vk::PipelineColorBlendAttachmentState blendAttachment{};
  if (key.picking) {
    blendAttachment.blendEnable = VK_FALSE;
  } else {
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  }
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  // Specialization constants are not strictly required here; flags are passed via push constants.
  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(FontPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _2] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
