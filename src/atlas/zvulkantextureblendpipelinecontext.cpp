#include "zvulkantextureblendpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dtextureblendrenderer.h"
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

int toComposeMode(TextureBlendMode mode)
{
  switch (mode) {
    case TextureBlendMode::DepthTest:
      return 0;
    case TextureBlendMode::FirstOnTop:
      return 1;
    case TextureBlendMode::SecondOnTop:
      return 2;
    case TextureBlendMode::MIPImageDepthTestBlending:
      return 3;
    case TextureBlendMode::DepthTestBlending:
      return 4;
    case TextureBlendMode::FirstOnTopBlending:
      return 5;
    case TextureBlendMode::SecondOnTopBlending:
      return 6;
  }
  return 0;
}

} // namespace

ZVulkanTextureBlendPipelineContext::ZVulkanTextureBlendPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureBlendPipelineContext::~ZVulkanTextureBlendPipelineContext() = default;

void ZVulkanTextureBlendPipelineContext::resetFrame()
{
  m_vertexCount = 0;
}

void ZVulkanTextureBlendPipelineContext::record(Z3DRendererBase& renderer,
                                                const RenderBatch& batch,
                                                const TextureBlendPayload& payload,
                                                const vk::Viewport& viewport,
                                                const vk::Rect2D& scissor,
                                                vk::raii::CommandBuffer& cmd)
{
  if (!payload.renderer) {
    return;
  }

  CHECK(payload.colorAttachmentHandle0.backend == AttachmentBackend::Vulkan)
    << "GL colorAttachmentHandle0 in Vulkan path";
  CHECK(payload.depthAttachmentHandle0.backend == AttachmentBackend::Vulkan)
    << "GL depthAttachmentHandle0 in Vulkan path";
  CHECK(payload.colorAttachmentHandle1.backend == AttachmentBackend::Vulkan)
    << "GL colorAttachmentHandle1 in Vulkan path";
  CHECK(payload.depthAttachmentHandle1.backend == AttachmentBackend::Vulkan)
    << "GL depthAttachmentHandle1 in Vulkan path";

  auto* color0 = reinterpret_cast<ZVulkanTexture*>(payload.colorAttachmentHandle0.id);
  auto* depth0 = reinterpret_cast<ZVulkanTexture*>(payload.depthAttachmentHandle0.id);
  auto* color1 = reinterpret_cast<ZVulkanTexture*>(payload.colorAttachmentHandle1.id);
  auto* depth1 = reinterpret_cast<ZVulkanTexture*>(payload.depthAttachmentHandle1.id);

  if (!color0 || !depth0 || !color1 || !depth1) {
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

  m_descriptorSet->updateTexture(0, *color0);
  m_descriptorSet->updateTexture(1, *depth0);
  m_descriptorSet->updateTexture(2, *color1);
  m_descriptorSet->updateTexture(3, *depth1);

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
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

  TextureBlendPushConstants constants;
  constants.screenDimRcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);

  cmd.pushConstants<TextureBlendPushConstants>(pipeline.pipeline->pipelineLayout(),
                                               vk::ShaderStageFlagBits::eFragment,
                                               0,
                                               constants);

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanTextureBlendPipelineContext::ensureDescriptorLayout()
{
  if (m_setTextures) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  std::array<vk::DescriptorSetLayoutBinding, 4> bindings{
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
                                   .stageFlags = vk::ShaderStageFlagBits::eFragment}
  };

  vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                               .pBindings = bindings.data()};
  m_setTextures.emplace(vkDevice, createInfo);
}

void ZVulkanTextureBlendPipelineContext::ensureDescriptorSet()
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

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureBlendPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(QuadVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 1> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0}
  };

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanTextureBlendPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(QuadVertex);
  if (requiredBytes <= m_vertexCapacity) {
    return;
  }

  size_t newCapacity = std::max(requiredBytes, m_vertexCapacity == 0 ? requiredBytes : m_vertexCapacity * 2);
  auto& device = m_backend.device();
  m_vertexBuffer =
    device.createBuffer(newCapacity,
                        vk::BufferUsageFlagBits::eVertexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_vertexCapacity = newCapacity;
}

void ZVulkanTextureBlendPipelineContext::uploadGeometry()
{
  const std::array<QuadVertex, 4> vertices{QuadVertex{glm::vec3(-1.f, 1.f, kQuadDepth)},
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

ZVulkanTextureBlendPipelineContext::PipelineInstance&
ZVulkanTextureBlendPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
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
                                                    shaderBase + "compositor.frag.spv",
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

  // Final compositor pass needs premultiplied alpha blending over the
  // already-rendered background, matching the GL path.
  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_TRUE;
  blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
  blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
  blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
  blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  vk::SpecializationMapEntry entry{.constantID = 60, .offset = 0, .size = sizeof(int)};
  int composeMode = toComposeMode(key.mode);
  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    {entry},
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&composeMode),
                         reinterpret_cast<const uint8_t*>(&composeMode) + sizeof(composeMode)));

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(TextureBlendPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
