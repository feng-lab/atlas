#include "zvulkantexturedualpeelpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
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

#include <array>
#include <vector>

namespace nim {

namespace {

constexpr float kQuadDepth = 0.0f;

} // namespace

ZVulkanTextureDualPeelPipelineContext::ZVulkanTextureDualPeelPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureDualPeelPipelineContext::~ZVulkanTextureDualPeelPipelineContext() = default;

void ZVulkanTextureDualPeelPipelineContext::resetFrame()
{
  m_vertexCount = 0;
}

void ZVulkanTextureDualPeelPipelineContext::record(Z3DRendererBase& renderer,
                                                   const RenderBatch& batch,
                                                   const TextureDualPeelPayload& payload,
                                                   const vk::Viewport& viewport,
                                                   const vk::Rect2D& scissor,
                                                   vk::raii::CommandBuffer& cmd)
{
  uploadGeometry();
  if (!m_vertexBuffer || m_vertexCount == 0) {
    return;
  }

  Stage stage = (payload.stage == TextureDualPeelPayload::Stage::Final) ? Stage::Final : Stage::Blend;

  if (stage == Stage::Blend) {
    if (payload.tempAttachment.backend != AttachmentBackend::Vulkan || !payload.tempAttachment.valid()) {
      LOG_FIRST_N(WARNING, 5) << "Dual peel blend payload missing Vulkan attachment handle.";
      return;
    }
  } else {
    if (payload.frontAttachment.backend != AttachmentBackend::Vulkan || !payload.frontAttachment.valid() ||
        payload.backAttachment.backend != AttachmentBackend::Vulkan || !payload.backAttachment.valid() ||
        payload.depthAttachment.backend != AttachmentBackend::Vulkan || !payload.depthAttachment.valid()) {
      LOG_FIRST_N(WARNING, 5) << "Dual peel final payload missing Vulkan attachment handles.";
      return;
    }
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();
  auto* descriptor = ensureDescriptor(stage);
  if (!descriptor) {
    return;
  }

  if (stage == Stage::Blend) {
    auto* tempTexture = reinterpret_cast<ZVulkanTexture*>(payload.tempAttachment.id);
    if (!tempTexture) {
      return;
    }
    descriptor->updateTexture(0, *tempTexture);
  } else {
    auto* frontTexture = reinterpret_cast<ZVulkanTexture*>(payload.frontAttachment.id);
    auto* backTexture = reinterpret_cast<ZVulkanTexture*>(payload.backAttachment.id);
    auto* depthTexture = reinterpret_cast<ZVulkanTexture*>(payload.depthAttachment.id);
    if (!frontTexture || !backTexture || !depthTexture) {
      return;
    }
    descriptor->updateTexture(0, *depthTexture);
    descriptor->updateTexture(1, *frontTexture);
    descriptor->updateTexture(2, *backTexture);
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.stage = stage;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& instance = ensurePipeline(key, formats);

  vk::DeviceSize offsets = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, instance.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offsets});

  if (descriptor) {
    std::array<vk::DescriptorSet, 1> sets{descriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, instance.pipeline->pipelineLayout(), 0, sets, {});
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

  DualPeelPushConstants constants;
  constants.screenDimRcp = payload.screenDimRcp;
  if (constants.screenDimRcp.x <= 0.0f || constants.screenDimRcp.y <= 0.0f) {
    constants.screenDimRcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  }

  cmd.pushConstants<DualPeelPushConstants>(instance.pipeline->pipelineLayout(),
                                           vk::ShaderStageFlagBits::eFragment,
                                           0,
                                           constants);

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanTextureDualPeelPipelineContext::ensureDescriptorLayouts()
{
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_blendSetLayout) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_blendSetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_finalSetLayout) {
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
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_finalSetLayout.emplace(vkDevice, createInfo);
  }
}

void ZVulkanTextureDualPeelPipelineContext::ensureDescriptorPool()
{
  if (!m_descriptorPool) {
    m_descriptorPool = m_backend.device().createDescriptorPool();
  }
}

ZVulkanDescriptorSet*
ZVulkanTextureDualPeelPipelineContext::ensureDescriptor(Stage stage)
{
  ensureDescriptorLayouts();
  ensureDescriptorPool();

  if (stage == Stage::Blend) {
    if (!m_blendDescriptor) {
      auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_blendSetLayout);
      m_blendDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
    }
    return m_blendDescriptor.get();
  }

  if (!m_finalDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_finalSetLayout);
    m_finalDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }
  return m_finalDescriptor.get();
}

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureDualPeelPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
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

void ZVulkanTextureDualPeelPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(glm::vec3);
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

void ZVulkanTextureDualPeelPipelineContext::uploadGeometry()
{
  const std::array<glm::vec3, 4> vertices{
    glm::vec3(-1.f, 1.f, kQuadDepth),
    glm::vec3(-1.f, -1.f, kQuadDepth),
    glm::vec3(1.f, 1.f, kQuadDepth),
    glm::vec3(1.f, -1.f, kQuadDepth)};

  ensureVertexCapacity(vertices.size());
  if (!m_vertexBuffer) {
    return;
  }

  m_vertexBuffer->copyData(vertices.data(), vertices.size() * sizeof(glm::vec3));
  m_vertexCount = vertices.size();
}

ZVulkanTextureDualPeelPipelineContext::PipelineInstance&
ZVulkanTextureDualPeelPipelineContext::ensurePipeline(const PipelineKey& key,
                                                      const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayouts();

  auto& device = m_backend.device();
  static const std::string shaderBase =
    ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.stage = key.stage;

  const char* fragmentShader = (key.stage == Stage::Final) ? "dual_peeling_final.frag.spv"
                                                           : "dual_peeling_blend.frag.spv";

  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + fragmentShader,
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);

  if (key.stage == Stage::Final) {
    instance.pipeline->setDescriptorSetLayouts({**m_finalSetLayout});
  } else {
    instance.pipeline->setDescriptorSetLayouts({**m_blendSetLayout});
  }
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  if (key.stage == Stage::Final) {
    instance.pipeline->setDepthTestEnable(true);
    instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
    instance.pipeline->setDepthWriteEnable(true);
  } else {
    instance.pipeline->setDepthTestEnable(false);
    instance.pipeline->setDepthWriteEnable(false);
  }

  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  if (key.stage == Stage::Blend) {
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  } else {
    blendAttachment.blendEnable = VK_FALSE;
  }
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(DualPeelPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim

