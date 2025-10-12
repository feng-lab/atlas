#include "zvulkantextureweightedaveragepipelinecontext.h"

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

ZVulkanTextureWeightedAveragePipelineContext::ZVulkanTextureWeightedAveragePipelineContext(
  Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureWeightedAveragePipelineContext::~ZVulkanTextureWeightedAveragePipelineContext() = default;

void ZVulkanTextureWeightedAveragePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  resetDescriptors();
}

void ZVulkanTextureWeightedAveragePipelineContext::resetDescriptors()
{
  m_descriptorSet.reset();
  m_descriptorSetOIT.reset();
  if (m_descriptorPool) {
    m_descriptorPool->reset();
  }
  m_lastInputs = {};
}

void ZVulkanTextureWeightedAveragePipelineContext::record(Z3DRendererBase& renderer,
                                                          const RenderBatch& batch,
                                                          const TextureWeightedAveragePayload& payload,
                                                          const vk::Viewport& viewport,
                                                          const vk::Rect2D& scissor,
                                                          vk::raii::CommandBuffer& cmd)
{
  VLOG(2) << fmt::format("WA::record begin accum=0x{:x} moments=0x{:x}",
                         payload.accumulationAttachment.id,
                         payload.momentsAttachment.id);
  CHECK(payload.accumulationAttachment.backend == AttachmentBackend::Vulkan)
    << "GL accumulationAttachment in Vulkan path";
  CHECK(payload.momentsAttachment.backend == AttachmentBackend::Vulkan) << "GL momentsAttachment in Vulkan path";
  if (!payload.accumulationAttachment.valid() || !payload.momentsAttachment.valid()) {
    return;
  }

  // Shared fullscreen quad
  m_vertexCount = 4;

  ensureDescriptorLayout();
  ensureDescriptorPool();
  ensureDescriptorSet();
  if (!m_descriptorSet) {
    return;
  }

  auto& accumulationTexture = vulkan::textureFromHandle(payload.accumulationAttachment,
                                                        m_backend.device(),
                                                        "texture-weighted-average accumulation attachment");
  auto& momentsTexture = vulkan::textureFromHandle(payload.momentsAttachment,
                                                   m_backend.device(),
                                                   "texture-weighted-average moments attachment");

  // Allocate a fresh per-draw override descriptor set to avoid update-after-bind hazards
  ZVulkanDescriptorSet* ds = nullptr;
  if (m_setLayout) {
    ds = m_backend.allocateOverrideDescriptorSet(**m_setLayout);
  }
  CHECK(ds != nullptr) << "WA resolve: override descriptor allocation failed (fatal)";
  const uint64_t accumId = payload.accumulationAttachment.id;
  const uint64_t momentsId = payload.momentsAttachment.id;
  if (!m_lastInputs.valid || m_lastInputs.accum != accumId) {
    ds->updateTexture(vkbind::kBindingWAAccum, accumulationTexture, m_backend.defaultSampler());
  }
  if (!m_lastInputs.valid || m_lastInputs.moments != momentsId) {
    ds->updateTexture(vkbind::kBindingWAMoments, momentsTexture, m_backend.defaultSampler());
  }
  m_lastInputs = {accumId, momentsId, true};

  const auto formats = vulkan::extractAttachmentFormats(batch);

  // Composite resolve invariant: single color attachment, no depth
  CHECK(formats.colorFormats.size() == 1 && !formats.depthFormat.has_value())
    << "WA resolve invariant violated: expected 1 color, no depth";
  CHECK(m_backend.validateFormatsOrSkip(formats, "WA_resolve")) << "WA resolve formats mismatched with current segment";

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& instance = ensurePipeline(key, formats);
  VLOG(2) << fmt::format("WA: ensured pipeline colors={} depth={}", formats.colorFormats.size(), formats.depthFormat.has_value());

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

  WeightedAveragePushConstants constants;
  constants.screenDimRcp = payload.screenDimRcp;
  if (constants.screenDimRcp.x <= 0.0f || constants.screenDimRcp.y <= 0.0f) {
    constants.screenDimRcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  }

  // Ensure OIT params UBO (set = 3); must be primed before recording
  if (m_backend.isRecording()) {
    CHECK(m_descriptorSetOIT && m_uboOIT) << "WA OIT resources not primed before recording";
  } else {
    ensureDescriptorLayout();
    ensureOITResources();
  }
  updateOITParamsUBO(renderer, batch, constants.screenDimRcp);
  if (m_descriptorSetOIT && m_uboOIT) {
    std::array<vk::DescriptorSet, 1> sets3{m_descriptorSetOIT->descriptorSet()};
    VLOG(2) << "WA: binding OIT params set at index 3";
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           instance.pipeline->pipelineLayout(),
                           vkbind::kSetOITParams,
                           sets3,
                           {});
  }

  cmd.pushConstants<WeightedAveragePushConstants>(instance.pipeline->pipelineLayout(),
                                                  vk::ShaderStageFlagBits::eFragment,
                                                  0,
                                                  constants);

  VLOG(2) << fmt::format("WA: draw {} verts", m_vertexCount);
  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureDescriptorLayout()
{
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setLayout) {
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
    // Immutable samplers for resolve inputs to avoid MSL sampler class issues
    vk::Sampler immutable = m_backend.defaultSampler();
    bindings[0].pImmutableSamplers = &immutable;
    bindings[1].pImmutableSamplers = &immutable;

    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setLayout.emplace(vkDevice, createInfo);
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
    vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
    m_setOIT.emplace(vkDevice, info);
  }
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureDescriptorPool() {}

void ZVulkanTextureWeightedAveragePipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();

  if (!m_descriptorSet) {
    m_descriptorSet = m_backend.allocateFrameDescriptorSet(**m_setLayout);
  }

  if (!m_descriptorSetOIT && m_setOIT) {
    m_descriptorSetOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureOITResources()
{
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  CHECK(m_uboOIT != nullptr) << "WA: failed to allocate OIT UBO";
  if (!m_descriptorSetOIT && m_setOIT) {
    m_descriptorSetOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }
  CHECK(m_descriptorSetOIT != nullptr) << "WA: failed to allocate OIT descriptor set";
  if (m_descriptorSetOIT && m_uboOIT) {
    // Ensure binding updated at least once per frame
    m_descriptorSetOIT->writeUniformBufferOnce(vkbind::kBindingOITParamsUBO, *m_uboOIT);
  }
}

void ZVulkanTextureWeightedAveragePipelineContext::updateOITParamsUBO(Z3DRendererBase& renderer,
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

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureWeightedAveragePipelineContext::makeVertexInputState() const
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

void ZVulkanTextureWeightedAveragePipelineContext::ensureVertexCapacity(size_t vertexCount)
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

void ZVulkanTextureWeightedAveragePipelineContext::uploadGeometry()
{
  const std::array<glm::vec3, 4> vertices{glm::vec3(-1.f, 1.f, kQuadDepth),
                                          glm::vec3(-1.f, -1.f, kQuadDepth),
                                          glm::vec3(1.f, 1.f, kQuadDepth),
                                          glm::vec3(1.f, -1.f, kQuadDepth)};

  m_vertexCount = vertices.size();
}

ZVulkanTextureWeightedAveragePipelineContext::PipelineInstance&
ZVulkanTextureWeightedAveragePipelineContext::ensurePipeline(const PipelineKey& key,
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
                                                    shaderBase + "wavg_final.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  // Sets: 0 = images, 1/2 = placeholders, 3 = OIT params
  std::vector<vk::DescriptorSetLayout> layouts{**m_setLayout, **m_setPlaceholder, **m_setPlaceholder, **m_setOIT};
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  // Resolve pass composites over background; avoid depth testing/writes interfering with blending.
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);

  // Blend weighted-average result over the existing background using
  // premultiplied alpha semantics (GL: ONE, ONE_MINUS_SRC_ALPHA).
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
                              .size = static_cast<uint32_t>(sizeof(WeightedAveragePushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
