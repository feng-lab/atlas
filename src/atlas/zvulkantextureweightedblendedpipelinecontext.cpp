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
#include "zvulkanpipelinecontext_raii.h"
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
  m_dsLighting.reset();
}

void ZVulkanTextureWeightedBlendedPipelineContext::record(Z3DRendererBase& renderer,
                                                          const RenderBatch& batch,
                                                          const TextureWeightedBlendedPayload& payload,
                                                          const vk::Viewport& viewport,
                                                          const vk::Rect2D& scissor,
                                                          vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  VLOG(2) << fmt::format("WB::record begin accum=0x{:x} trans=0x{:x}",
                         payload.accumulationAttachment.id,
                         payload.transmittanceAttachment.id);
  CHECK(payload.accumulationAttachment.backend == RenderBackend::Vulkan)
    << "GL accumulationAttachment in Vulkan path";
  CHECK(payload.transmittanceAttachment.backend == RenderBackend::Vulkan)
    << "GL transmittanceAttachment in Vulkan path";
  if (!payload.accumulationAttachment.valid() || !payload.transmittanceAttachment.valid()) {
    return;
  }

  // Shared fullscreen quad
  m_vertexCount = 4;

  ensureDescriptorLayout();
  ensureDescriptorSet();
  if (!m_descriptorSet) {
    return;
  }

  

  auto& accumulationTexture = vulkan::textureFromHandle(payload.accumulationAttachment,
                                                        m_backend.device(),
                                                        "texture-weighted-blended accumulation attachment");
  auto& transmittanceTexture = vulkan::textureFromHandle(payload.transmittanceAttachment,
                                                         m_backend.device(),
                                                         "texture-weighted-blended transmittance attachment");

  // Allocate a fresh per-draw override descriptor set to avoid update-after-bind hazards
  ZVulkanDescriptorSet* ds = nullptr;
  if (m_setLayout) {
    ds = m_backend.allocateOverrideDescriptorSet(**m_setLayout);
  }
  CHECK(ds != nullptr) << "WB resolve: override descriptor allocation failed (fatal)";
  // Like the weighted-average resolve, override descriptor sets arrive with undefined
  // bindings, so prime both textures before the draw.
  ds->updateTexture(vkbind::kBindingWBAccum, accumulationTexture, m_backend.defaultSampler());
  ds->updateTexture(vkbind::kBindingWBTransmittance, transmittanceTexture, m_backend.defaultSampler());

  const auto formats = vulkan::extractAttachmentFormats(batch);

  // Composite resolve invariant: single color attachment; depth optional
  CHECK_EQ(formats.colorFormats.size(), size_t{1}) << "WB resolve requires exactly one color attachment.";
  m_backend.validateFormatsOrCrash(formats, "WB_resolve");

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& instance = ensurePipeline(key, formats);
  VLOG(2) << fmt::format("WB: ensured pipeline colors={} depth={}",
                         formats.colorFormats.size(),
                         formats.depthFormat.has_value());

  // Draw-only: backend manages attachments and area

  // No OIT UBO required for WB resolve
  // Pipeline uses lighting UBO (set=1) for depth scale/zw transform
  m_dynLightingOffset = m_backend.frameSharedLightingOffset();
  CHECK(m_dsLighting) << "WB resolve: lighting descriptor set not initialised";

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = instance.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = instance.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetInputs;
  const std::array<vk::DescriptorSet, 2> descriptorSets{ds->descriptorSet(), m_dsLighting->descriptorSet()};
  const std::array<uint32_t, 1> dynamicOffsets{static_cast<uint32_t>(m_dynLightingOffset)};
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.dynamicOffsets = dynamicOffsets; // (set1,b0)
  drawSpec.expectedDescriptorSetCount = 2;
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  const std::array<vk::Buffer, 1> vertexBuffers{quad.buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{vk::DeviceSize(0)};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = static_cast<uint32_t>(m_vertexCount);
  drawSpec.instanceCount = 1;
  drawSpec.pushConstantsData = nullptr;
  drawSpec.pushConstantsSize = 0;
  drawSpec.pushConstantsStages = {};
  drawSpec.requirePushConstants = false;

  VLOG(2) << fmt::format("WB(draw-only): draw {} verts", m_vertexCount);
  ZVulkanPipelineCommandRecorder recorder(cmd);
  recorder.recordGraphicsDraw(drawSpec);
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
    m_setPlaceholder = m_backend.emptyDescriptorSetLayout();
  }

  if (!m_setLighting) {
    m_setLighting = m_backend.lightingDescriptorSetLayout();
  }
  
}

void ZVulkanTextureWeightedBlendedPipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();

  if (!m_descriptorSet) {
    m_descriptorSet = m_backend.allocateFrameDescriptorSet(**m_setLayout);
  }
}

void ZVulkanTextureWeightedBlendedPipelineContext::ensureOITResources()
{
  // Pre-prime persistent lighting descriptor set (set=1) before recording begins.
  ensureDescriptorLayout();
  if (!m_dsLighting && m_setLighting) {
    m_dsLighting = m_backend.allocateFrameDescriptorSet(m_setLighting);
  }
  if (m_dsLighting && !m_backend.isRecording()) {
    // Safe to write descriptors only when not recording
    m_dsLighting->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(LightingUBOStd140));
  }
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
  // Sets: 0 = inputs (samplers), 1 = lighting UBO
  instance.pipeline->setDescriptorSetLayouts({**m_setLayout, m_setLighting});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  const bool hasDepth = formats.depthFormat.has_value();
  instance.pipeline->setDepthTestEnable(hasDepth);
  if (hasDepth) {
    // Keep Vulkan resolve depth semantics aligned with the OpenGL path: only
    // commit the resolved depth when it is not farther than the stored value.
    // This preserves the compositor's follow-up depth-tested blend step.
    instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);
    instance.pipeline->setDepthWriteEnable(true);
  } else {
    instance.pipeline->setDepthWriteEnable(false);
  }

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

  // No push constants
  instance.pipeline->setPushConstantRanges({});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
