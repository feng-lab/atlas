#include "zvulkantextureweightedblendedpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanbuffer.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkanuniforms.h"
#include "zlog.h"

#include <array>
#include <cstdint>
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
  m_dynLightingOffset = 0;
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

  const vk::DescriptorSet dsLighting = m_backend.sharedLightingDescriptorSet();
  CHECK(dsLighting) << "WB resolve: shared lighting descriptor set missing";

  auto& accumulationTexture = vulkan::textureFromHandle(payload.accumulationAttachment,
                                                        m_backend.device(),
                                                        "texture-weighted-blended accumulation attachment");
  auto& transmittanceTexture = vulkan::textureFromHandle(payload.transmittanceAttachment,
                                                         m_backend.device(),
                                                         "texture-weighted-blended transmittance attachment");

  const uint32_t accumIdx =
    m_backend.bindlessLookupSampledImageAutoOrCrash(accumulationTexture, "texture_weighted_blended accum");
  const uint32_t transIdx =
    m_backend.bindlessLookupSampledImageAutoOrCrash(transmittanceTexture, "texture_weighted_blended transmittance");

  const auto formats = vulkan::extractAttachmentFormats(batch);
  CHECK_EQ(formats.colorFormats.size(), size_t{1}) << "WB resolve requires exactly one color attachment.";
  m_backend.validateFormatsOrCrash(formats, "WB_resolve");

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());

  // Shared fullscreen quad
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  cmd.bindVertexBuffers(0, {quad.buffer()}, {vk::DeviceSize(0)});

  m_dynLightingOffset = m_backend.frameSharedLightingOffset();
  {
    std::array<vk::DescriptorSet, 2> sets{m_backend.bindlessSampledImageDescriptorSet(), dsLighting};
    std::array<uint32_t, 1> dynOffsets{static_cast<uint32_t>(m_dynLightingOffset)};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, dynOffsets);
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  WBlendedFinalPushConstants constants;
  constants.accumTex = accumIdx;
  constants.transmittanceTex = transIdx;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&constants);
  vk::ArrayProxy<const std::uint8_t> payloadBytes(sizeof(constants), bytes);
  cmd.pushConstants(pipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, payloadBytes);

  cmd.draw(4, 1, 0, 0);
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

ZVulkanTextureWeightedBlendedPipelineContext::PipelineInstance&
ZVulkanTextureWeightedBlendedPipelineContext::ensurePipeline(const PipelineKey& key,
                                                             const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();

  PipelineInstance instance;
  instance.shader =
    std::make_unique<ZVulkanShader>(device,
                                    ZVulkanShader::spirvResourcePath(QStringLiteral("pass.vert.spv")),
                                    ZVulkanShader::spirvResourcePath(QStringLiteral("wblended_final.frag.spv")),
                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);

  const vk::DescriptorSetLayout lightingLayout = m_backend.lightingDescriptorSetLayout();
  CHECK(lightingLayout) << "WB resolve requires lighting descriptor set layout";

  // Sets: 0 = bindless sampled images, 1 = lighting UBO
  instance.pipeline->setDescriptorSetLayouts({m_backend.bindlessSampledImageDescriptorSetLayout(), lightingLayout});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  const bool hasDepth = formats.depthFormat.has_value();
  instance.pipeline->setDepthTestEnable(hasDepth);
  if (hasDepth) {
    // Keep resolve depth semantics aligned with the OpenGL path: only commit the
    // resolved depth when it is not farther than the stored value. This preserves
    // the compositor's follow-up depth-tested blend step.
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

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(WBlendedFinalPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
