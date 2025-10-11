#include "zvulkantextureweightedblendedpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbindings.h"
#include "zvulkanbuffer.h"
#include "zvulkanuniforms.h"
#include "zlog.h"

#include <array>
#include <vector>

namespace nim {

ZVulkanTextureWeightedBlendedPipelineContext::ZVulkanTextureWeightedBlendedPipelineContext(
  Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureWeightedBlendedPipelineContext::~ZVulkanTextureWeightedBlendedPipelineContext() = default;

void ZVulkanTextureWeightedBlendedPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  resetDescriptors();
}

void ZVulkanTextureWeightedBlendedPipelineContext::resetDescriptors()
{
  m_descriptorSet.reset();
  m_descriptorSetOIT.reset();
}

void ZVulkanTextureWeightedBlendedPipelineContext::record(Z3DRendererBase& renderer,
                                                          const RenderBatch& batch,
                                                          const TextureWeightedBlendedPayload& payload,
                                                          const vk::Viewport& viewport,
                                                          const vk::Rect2D& scissor,
                                                          vk::raii::CommandBuffer& cmd)
{
  VLOG(2) << fmt::format("WB::record begin accum=0x{:x} trans=0x{:x}",
                         payload.accumulationAttachment.id,
                         payload.transmittanceAttachment.id);
  CHECK(payload.accumulationAttachment.backend == AttachmentBackend::Vulkan)
    << "GL accumulationAttachment in Vulkan path";
  CHECK(payload.transmittanceAttachment.backend == AttachmentBackend::Vulkan)
    << "GL transmittanceAttachment in Vulkan path";
  if (!payload.accumulationAttachment.valid() || !payload.transmittanceAttachment.valid()) {
    return;
  }

  // Shared fullscreen quad
  m_vertexCount = 4;

  ensureDescriptorLayout();
  ensureDescriptorSet();
  if (m_backend.isRecording()) {
    CHECK(m_descriptorSetOIT && m_uboOIT) << "WB OIT resources not primed before recording";
  } else {
    ensureOITResources();
  }
  if (!m_descriptorSet) {
    return;
  }

  auto& accumulationTexture = vulkan::textureFromHandle(payload.accumulationAttachment,
                                                        m_backend.device(),
                                                        "texture-weighted-blended accumulation attachment");
  auto& transmittanceTexture = vulkan::textureFromHandle(payload.transmittanceAttachment,
                                                         m_backend.device(),
                                                         "texture-weighted-blended transmittance attachment");

  // Allocate per-draw override descriptor set for inputs
  ZVulkanDescriptorSet* ds = nullptr;
  if (m_setLayout) {
    ds = m_backend.allocateOverrideDescriptorSet(**m_setLayout);
  }
  CHECK(ds != nullptr) << "WB resolve: override descriptor allocation failed (fatal)";
  VLOG(2) << "WB: updating override set bindings accum/trans";
  ds->updateTexture(vkbind::kBindingWBAccum, accumulationTexture, m_backend.defaultSampler());
  ds->updateTexture(vkbind::kBindingWBTransmittance, transmittanceTexture, m_backend.defaultSampler());

  const auto formats = vulkan::extractAttachmentFormats(batch);

  // Composite resolve invariant: single color attachment, no depth
  CHECK(formats.colorFormats.size() == 1 && !formats.depthFormat.has_value())
    << "WB resolve invariant violated: expected 1 color, no depth";
  CHECK(m_backend.validateFormatsOrSkip(formats, "WB_resolve")) << "WB resolve formats mismatched with current segment";

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& instance = ensurePipeline(key, formats);
  VLOG(2) << fmt::format("WB: ensured pipeline colors={} depth={}", formats.colorFormats.size(), formats.depthFormat.has_value());

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, instance.pipeline->pipeline());
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  const vk::DeviceSize offsets[] = {0};
  cmd.bindVertexBuffers(0, {quad.buffer()}, offsets);

  {
    std::array<vk::DescriptorSet, 1> sets{ds->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           instance.pipeline->pipelineLayout(),
                           vkbind::kSetInputs,
                           sets,
                           {});
  }

  // Ensure and bind OIT params (set = 3). Descriptor is set at allocation time.
  if (m_descriptorSetOIT && m_uboOIT) {
    std::array<vk::DescriptorSet, 1> sets3{m_descriptorSetOIT->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           instance.pipeline->pipelineLayout(),
                           vkbind::kSetOITParams,
                           sets3,
                           {});
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

  WeightedBlendedPushConstants constants;
  constants.screenDimRcp = payload.screenDimRcp;
  if (constants.screenDimRcp.x <= 0.0f || constants.screenDimRcp.y <= 0.0f) {
    constants.screenDimRcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  }

  // Update OIT UBO values
  updateOITParamsUBO(renderer, batch, constants.screenDimRcp);

  cmd.pushConstants<WeightedBlendedPushConstants>(instance.pipeline->pipelineLayout(),
                                                  vk::ShaderStageFlagBits::eFragment,
                                                  0,
                                                  constants);

  VLOG(2) << fmt::format("WB: draw {} verts", m_vertexCount);
  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanTextureWeightedBlendedPipelineContext::ensureDescriptorLayout()
{
  if (!m_setLayout) {
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    // Immutable samplers for resolve inputs
    vk::Sampler immutable = m_backend.defaultSampler();
    bindings[0].pImmutableSamplers = &immutable;
    bindings[1].pImmutableSamplers = &immutable;

    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setLayout.emplace(vkDevice, createInfo);
  }

  // Ensure placeholder to align set indices (for set 1 and 2)
  if (!m_setPlaceholder) {
    auto& device = m_backend.device();
    auto& vkDevice = device.context().device();
    vk::DescriptorSetLayoutCreateInfo emptyInfo{.bindingCount = 0, .pBindings = nullptr};
    m_setPlaceholder.emplace(vkDevice, emptyInfo);
  }

  if (!m_setOIT) {
    auto& device = m_backend.device();
    auto& vkDevice = device.context().device();
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_setOIT.emplace(vkDevice, info);
  }
}

void ZVulkanTextureWeightedBlendedPipelineContext::ensureDescriptorPool() {}

void ZVulkanTextureWeightedBlendedPipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();

  if (!m_descriptorSet) {
    m_descriptorSet = m_backend.allocateFrameDescriptorSet(**m_setLayout);
  }
  if (!m_descriptorSetOIT && m_setOIT) {
    m_descriptorSetOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }
}

void ZVulkanTextureWeightedBlendedPipelineContext::ensureOITResources()
{
  // Ensure OIT UBO and descriptor for set = 3
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  CHECK(m_uboOIT != nullptr) << "WB: failed to allocate OIT UBO";
  if (!m_descriptorSetOIT && m_setOIT) {
    m_descriptorSetOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }
  CHECK(m_descriptorSetOIT != nullptr) << "WB: failed to allocate OIT descriptor set";
  if (m_descriptorSetOIT && m_uboOIT) {
    m_descriptorSetOIT->writeUniformBufferOnce(vkbind::kBindingOITParamsUBO, *m_uboOIT);
  }
}

void ZVulkanTextureWeightedBlendedPipelineContext::updateOITParamsUBO(Z3DRendererBase& renderer,
                                                                      const RenderBatch& batch,
                                                                      const glm::vec2& screenDimRcp)
{
  (void)batch;
  if (!m_uboOIT) {
    return;
  }

  OITParamsUBOStd140 oit{};

  glm::vec2 rcp = screenDimRcp;
  if (!(rcp.x > 0.0f && rcp.y > 0.0f)) {
    const auto& viewportState = renderer.frameState().viewport;
    const float w = static_cast<float>(viewportState.z);
    const float h = static_cast<float>(viewportState.w);
    if (w > 0.0f && h > 0.0f) {
      rcp = glm::vec2(1.0f / w, 1.0f / h);
    }
  }
  oit.screen_dim_RCP = rcp;

  const float n = renderer.viewState().nearClip;
  const float f = renderer.viewState().farClip;
  const float denom = std::max(f - n, 1e-6f);
  oit.ze_to_zw_a = (f * n) / denom;
  oit.ze_to_zw_b = 0.5f * (f + n) / denom + 0.5f;
  oit.weighted_blended_depth_scale = renderer.sceneState().weightedBlendedDepthScale;

  m_uboOIT->copyData(&oit, sizeof(oit));
}

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureWeightedBlendedPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
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

void ZVulkanTextureWeightedBlendedPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(glm::vec3);
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

void ZVulkanTextureWeightedBlendedPipelineContext::uploadGeometry() {}

ZVulkanTextureWeightedBlendedPipelineContext::PipelineInstance&
ZVulkanTextureWeightedBlendedPipelineContext::ensurePipeline(const PipelineKey& key,
                                                             const vulkan::AttachmentFormats& formats)
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
                                                    shaderBase + "wblended_final.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  if (m_setOIT) {
    // Preserve set indices: set 0 = inputs, set 1/2 placeholders, set 3 = OIT UBO
    // Ensure we have placeholder empty layout; reuse WA’s approach by creating a local empty layout if needed
    if (!m_setPlaceholder) {
      auto& vkDevice = device.context().device();
      vk::DescriptorSetLayoutCreateInfo emptyInfo{.bindingCount = 0, .pBindings = nullptr};
      m_setPlaceholder.emplace(vkDevice, emptyInfo);
    }
    instance.pipeline->setDescriptorSetLayouts({**m_setLayout, **m_setPlaceholder, **m_setPlaceholder, **m_setOIT});
  } else {
    instance.pipeline->setDescriptorSetLayouts({**m_setLayout});
  }
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);

  // Blend weighted-blended result over background using premultiplied alpha.
  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = true;
  blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
  blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
  blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
  blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(WeightedBlendedPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
