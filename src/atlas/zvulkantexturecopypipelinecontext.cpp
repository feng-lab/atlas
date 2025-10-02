#include "zvulkantexturecopypipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dtexturecopyrenderer.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbuffer.h"
#include "zlog.h"

#include <algorithm>
#include <array>
#include <vector>

namespace nim {

namespace {

constexpr float kQuadDepth = 0.0f;

} // namespace

ZVulkanTextureCopyPipelineContext::ZVulkanTextureCopyPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureCopyPipelineContext::~ZVulkanTextureCopyPipelineContext() = default;

void ZVulkanTextureCopyPipelineContext::resetFrame()
{
  m_vertexCount = 0;
}

void ZVulkanTextureCopyPipelineContext::record(Z3DRendererBase& renderer,
                                               const RenderBatch& batch,
                                               const TextureCopyPayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  if (!payload.renderer) {
    return;
  }

  if (payload.colorAttachmentHandle.backend != AttachmentBackend::Vulkan ||
      payload.depthAttachmentHandle.backend != AttachmentBackend::Vulkan) {
    LOG_FIRST_N(WARNING, 5) << "Texture copy payload missing Vulkan attachment handles.";
    return;
  }

  auto* colorTexture = reinterpret_cast<ZVulkanTexture*>(payload.colorAttachmentHandle.id);
  auto* depthTexture = reinterpret_cast<ZVulkanTexture*>(payload.depthAttachmentHandle.id);
  if (!colorTexture || !depthTexture) {
    return;
  }

  uploadGeometry();
  if (!m_vertexBuffer || m_vertexCount == 0) {
    return;
  }

  ensureDescriptorLayout();
  ensureDescriptorSet();
  if (!m_descriptorSet) {
    return;
  }

  m_descriptorSet->updateTexture(0, *colorTexture);
  m_descriptorSet->updateTexture(1, *depthTexture);

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.discardTransparent = payload.discardTransparent;
  key.mode = payload.mode;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  vk::DeviceSize offsets = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offsets});

  if (m_descriptorSet) {
    std::array<vk::DescriptorSet, 1> sets{m_descriptorSet->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  glm::vec2 extent = batch.pass.viewport.extent;
  if (extent.x <= 0.0f || extent.y <= 0.0f) {
    const auto& viewportState = renderer.frameState().viewport;
    extent = glm::vec2(static_cast<float>(viewportState.z), static_cast<float>(viewportState.w));
  }
  if (extent.x <= 0.0f) {
    extent.x = 1.0f;
  }
  if (extent.y <= 0.0f) {
    extent.y = 1.0f;
  }

  TextureCopyPushConstants constants;
  constants.screenDimRcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);

  cmd.pushConstants<TextureCopyPushConstants>(pipeline.pipeline->pipelineLayout(),
                                              vk::ShaderStageFlagBits::eFragment,
                                              0,
                                              constants);

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanTextureCopyPipelineContext::ensureDescriptorLayout()
{
  if (m_setTextures) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
    vk::DescriptorSetLayoutBinding{.binding = 0,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eFragment},
    vk::DescriptorSetLayoutBinding{.binding = 1,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eFragment}};

  vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                               .pBindings = bindings.data()};
  m_setTextures.emplace(vkDevice, createInfo);
}

void ZVulkanTextureCopyPipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();

  auto& device = m_backend.device();
  if (!m_descriptorPool) {
    m_descriptorPool = device.createDescriptorPool();
  }

  if (!m_descriptorSet) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_setTextures);
    m_descriptorSet = std::make_unique<ZVulkanDescriptorSet>(device, std::move(descriptorSet));
  }
}

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureCopyPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(QuadVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 1> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0}};

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanTextureCopyPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(QuadVertex);
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

void ZVulkanTextureCopyPipelineContext::uploadGeometry()
{
  const std::array<QuadVertex, 4> vertices{
    QuadVertex{glm::vec3(-1.f, 1.f, kQuadDepth)},
    QuadVertex{glm::vec3(-1.f, -1.f, kQuadDepth)},
    QuadVertex{glm::vec3(1.f, 1.f, kQuadDepth)},
    QuadVertex{glm::vec3(1.f, -1.f, kQuadDepth)}};

  ensureVertexCapacity(vertices.size());
  if (!m_vertexBuffer) {
    return;
  }

  m_vertexBuffer->copyData(vertices.data(), vertices.size() * sizeof(QuadVertex));
  m_vertexCount = vertices.size();
}

ZVulkanTextureCopyPipelineContext::PipelineInstance&
ZVulkanTextureCopyPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
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
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "copyimage.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_setTextures});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
  instance.pipeline->setDepthWriteEnable(true);

  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_FALSE;
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  std::array<vk::SpecializationMapEntry, 3> entries{
    vk::SpecializationMapEntry{.constantID = 60, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 61, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 62, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}};

  const bool discard = key.discardTransparent;
  const bool multiply = key.mode == TextureCopyPayload::OutputMode::MultiplyAlpha;
  const bool divide = key.mode == TextureCopyPayload::OutputMode::DivideByAlpha;

  std::array<uint32_t, 3> specData{static_cast<uint32_t>(discard),
                                   static_cast<uint32_t>(multiply),
                                   static_cast<uint32_t>(divide)};

  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                              std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end()),
                                              std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                                                                   reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(TextureCopyPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
