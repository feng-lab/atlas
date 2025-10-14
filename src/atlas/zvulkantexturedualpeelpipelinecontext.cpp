#include "zvulkantexturedualpeelpipelinecontext.h"

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
  resetDescriptors();
}

void ZVulkanTextureDualPeelPipelineContext::resetDescriptors()
{
  m_blendDescriptor.reset();
  m_finalDescriptor.reset();
  m_descriptorOIT.reset();
}

void ZVulkanTextureDualPeelPipelineContext::record(Z3DRendererBase& renderer,
                                                   const RenderBatch& batch,
                                                   const TextureDualPeelPayload& payload,
                                                   const vk::Viewport& viewport,
                                                   const vk::Rect2D& scissor,
                                                   vk::raii::CommandBuffer& cmd)
{
  // Shared fullscreen quad
  m_vertexCount = 4;

  VLOG(2) << fmt::format("DDP::record begin stage={} temp=0x{:x} depth=0x{:x} front=0x{:x} back=0x{:x}",
                         payload.stage == TextureDualPeelPayload::Stage::Final ? "final" : "blend",
                         payload.tempAttachment.id,
                         payload.depthAttachment.id,
                         payload.frontAttachment.id,
                         payload.backAttachment.id);

  Stage stage = (payload.stage == TextureDualPeelPayload::Stage::Final) ? Stage::Final : Stage::Blend;
  const bool useOcclusionQuery =
    (stage == Stage::Blend) && payload.hasOcclusionQuery() && m_backend.supportsOcclusionQueries();

  if (stage == Stage::Blend) {
    CHECK(payload.tempAttachment.backend == AttachmentBackend::Vulkan)
      << "GL tempAttachment in Vulkan dual-peel blend path";
    if (!payload.tempAttachment.valid()) {
      return;
    }
  } else {
    CHECK(payload.frontAttachment.valid() && payload.backAttachment.valid() && payload.depthAttachment.valid())
      << "Skipping Vulkan dual-peel final stage due to missing attachments";
  }

  ensureDescriptorLayouts();
  ZVulkanDescriptorSet* descriptor = nullptr;
  // Allocate a fresh per-draw override set for each stage to avoid update-after-bind hazards
  if (stage == Stage::Blend && m_blendSetLayout) {
    descriptor = m_backend.allocateOverrideDescriptorSet(**m_blendSetLayout);
  } else if (stage == Stage::Final && m_finalSetLayout) {
    descriptor = m_backend.allocateOverrideDescriptorSet(**m_finalSetLayout);
  }
  CHECK(descriptor != nullptr) << "DDP texture stage: override descriptor allocation failed (fatal)";

  if (stage == Stage::Blend) {
    CHECK(payload.tempAttachment.valid()) << "Skipping Vulkan dual-peel blend stage because temp attachment is missing";
    auto& tempTexture =
      vulkan::textureFromHandle(payload.tempAttachment, m_backend.device(), "dual-peel blend attachment");
    VLOG(2) << fmt::format("DDP blend: updating binding0 temp=0x{:x}", payload.tempAttachment.id);
    descriptor->updateTexture(0, tempTexture, m_backend.defaultSampler());
  } else {
    auto& depthTexture =
      vulkan::textureFromHandle(payload.depthAttachment, m_backend.device(), "dual-peel depth attachment");
    auto& frontTexture =
      vulkan::textureFromHandle(payload.frontAttachment, m_backend.device(), "dual-peel front attachment");
    auto& backTexture =
      vulkan::textureFromHandle(payload.backAttachment, m_backend.device(), "dual-peel back attachment");
    VLOG(2) << fmt::format("DDP final: updating depth/front/back bindings depth=0x{:x} front=0x{:x} back=0x{:x}",
                           payload.depthAttachment.id,
                           payload.frontAttachment.id,
                           payload.backAttachment.id);
    descriptor->updateTexture(vkbind::kBindingDDPFinalDepth, depthTexture, m_backend.defaultSampler());
    descriptor->updateTexture(vkbind::kBindingDDPFinalFront, frontTexture, m_backend.defaultSampler());
    descriptor->updateTexture(vkbind::kBindingDDPFinalBack, backTexture, m_backend.defaultSampler());
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  // Composite resolve invariant: enforce single color attachment; depth optional
  if (stage == Stage::Final) {
    CHECK(formats.colorFormats.size() == 1) << "Skipping DDP final: expected exactly 1 color attachment.";
  }
  m_backend.validateFormatsOrCrash(formats, stage == Stage::Final ? "DDP_final" : "DDP_blend");

  PipelineKey key;
  key.stage = stage;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& instance = ensurePipeline(key, formats);
  VLOG(2) << fmt::format("DDP {}: ensured pipeline colors={} depth={}",
                         (stage == Stage::Final ? "final" : "blend"),
                         formats.colorFormats.size(),
                         formats.depthFormat.has_value());

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, instance.pipeline->pipeline());
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  cmd.bindVertexBuffers(0, {quad.buffer()}, {vk::DeviceSize(0)});

  if (descriptor) {
    std::array<vk::DescriptorSet, 1> sets{descriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           instance.pipeline->pipelineLayout(),
                           vkbind::kSetInputs,
                           sets,
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

  DualPeelPushConstants constants;
  constants.screenDimRcp = payload.screenDimRcp;
  if (constants.screenDimRcp.x <= 0.0f || constants.screenDimRcp.y <= 0.0f) {
    constants.screenDimRcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  }

  cmd.pushConstants<DualPeelPushConstants>(instance.pipeline->pipelineLayout(),
                                           vk::ShaderStageFlagBits::eFragment,
                                           0,
                                           constants);

  // Ensure and bind OIT params (set = 3); descriptor is set at allocation time.
  VLOG(2) << "DDP: ensureOITResources + update UBO";
  ensureOITResources();
  updateOITParamsUBO(renderer, batch, constants.screenDimRcp);
  if (m_descriptorOIT && m_uboOIT) {
    std::array<vk::DescriptorSet, 1> sets3{m_descriptorOIT->descriptorSet()};
    VLOG(2) << "DDP: binding OIT params set at index 3";
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           instance.pipeline->pipelineLayout(),
                           vkbind::kSetOITParams,
                           sets3,
                           {});
  }

  if (useOcclusionQuery) {
    VLOG(2) << fmt::format("DDP: begin occlusion query idx={}", payload.occlusionQueryIndex);
    m_backend.beginOcclusionQuery(cmd, payload.occlusionQueryIndex);
  }

  VLOG(2) << fmt::format("DDP: draw {} verts", m_vertexCount);
  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);

  if (useOcclusionQuery) {
    VLOG(2) << fmt::format("DDP: end occlusion query idx={}", payload.occlusionQueryIndex);
    m_backend.endOcclusionQuery(cmd, payload.occlusionQueryIndex);
  }
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
    // Immutable sampler for blend input
    vk::Sampler immutable = m_backend.defaultSampler();
    binding.pImmutableSamplers = &immutable;
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    // Immutable samplers for final inputs
    vk::Sampler immutable = m_backend.defaultSampler();
    bindings[0].pImmutableSamplers = &immutable;
    bindings[1].pImmutableSamplers = &immutable;
    bindings[2].pImmutableSamplers = &immutable;
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_finalSetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_setPlaceholder) {
    vk::DescriptorSetLayoutCreateInfo emptyInfo{.bindingCount = 0, .pBindings = nullptr};
    m_setPlaceholder.emplace(vkDevice, emptyInfo);
  }

  if (!m_setOIT) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_setOIT.emplace(vkDevice, createInfo);
  }
}

void ZVulkanTextureDualPeelPipelineContext::ensureDescriptorPool()
{
  // No-op: descriptor sets are allocated from the backend per-frame arena.
}

ZVulkanDescriptorSet* ZVulkanTextureDualPeelPipelineContext::ensureDescriptor(Stage stage)
{
  ensureDescriptorLayouts();

  if (stage == Stage::Blend) {
    if (!m_blendDescriptor) {
      m_blendDescriptor = m_backend.allocateFrameDescriptorSet(**m_blendSetLayout);
    }
    return m_blendDescriptor.get();
  }

  if (!m_finalDescriptor) {
    m_finalDescriptor = m_backend.allocateFrameDescriptorSet(**m_finalSetLayout);
  }
  return m_finalDescriptor.get();
}

void ZVulkanTextureDualPeelPipelineContext::ensureOITResources()
{
  ensureDescriptorLayouts();
  if (!m_descriptorOIT && m_setOIT) {
    m_descriptorOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  CHECK(m_descriptorOIT != nullptr) << "DDP: failed to allocate OIT descriptor set";
  CHECK(m_uboOIT != nullptr) << "DDP: failed to allocate OIT UBO";
  if (m_descriptorOIT && m_uboOIT) {
    m_descriptorOIT->writeUniformBufferOnce(vkbind::kBindingOITParamsUBO, *m_uboOIT);
  }
}

void ZVulkanTextureDualPeelPipelineContext::updateOITParamsUBO(Z3DRendererBase& renderer,
                                                               const RenderBatch&,
                                                               const glm::vec2& fallbackScreenDimRcp)
{
  if (!m_uboOIT) {
    return;
  }
  OITParamsUBOStd140 oit{};
  glm::vec2 rcp = fallbackScreenDimRcp;
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

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureDualPeelPipelineContext::makeVertexInputState() const
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

void ZVulkanTextureDualPeelPipelineContext::ensureVertexCapacity(size_t vertexCount)
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

void ZVulkanTextureDualPeelPipelineContext::uploadGeometry()
{
  const std::array<glm::vec3, 4> vertices{glm::vec3(-1.f, 1.f, kQuadDepth),
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
ZVulkanTextureDualPeelPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayouts();

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.stage = key.stage;

  const char* fragmentShader =
    (key.stage == Stage::Final) ? "dual_peeling_final.frag.spv" : "dual_peeling_blend.frag.spv";

  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + "pass.vert.spv", shaderBase + fragmentShader, std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);

  if (key.stage == Stage::Final) {
    std::vector<vk::DescriptorSetLayout> layouts{**m_finalSetLayout,
                                                 **m_setPlaceholder,
                                                 **m_setPlaceholder,
                                                 **m_setOIT};
    instance.pipeline->setDescriptorSetLayouts(layouts);
  } else {
    std::vector<vk::DescriptorSetLayout> layouts{**m_blendSetLayout,
                                                 **m_setPlaceholder,
                                                 **m_setPlaceholder,
                                                 **m_setOIT};
    instance.pipeline->setDescriptorSetLayouts(layouts);
  }
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  if (key.stage == Stage::Final) {
    // Final composite writes resolved depth (from depth texture). Use ALWAYS to ensure writes.
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
    // Accumulate pass: add the temporary back color into the back-blend buffer
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  } else {
    // Final composite must blend the resolved transparent layer over the
    // already-loaded background (GL behavior: ONE, ONE_MINUS_SRC_ALPHA).
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
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
