#include "zvulkantextureweightedaveragepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanbuffer.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zlog.h"

#include <array>
#include <cstdint>
#include <vector>

namespace nim {

ZVulkanTextureWeightedAveragePipelineContext::ZVulkanTextureWeightedAveragePipelineContext(
  Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureWeightedAveragePipelineContext::~ZVulkanTextureWeightedAveragePipelineContext() = default;

void ZVulkanTextureWeightedAveragePipelineContext::resetFrame()
{
  m_vertexCount = 0;
}

void ZVulkanTextureWeightedAveragePipelineContext::record(Z3DRendererBase& renderer,
                                                          const RenderBatch& batch,
                                                          const TextureWeightedAveragePayload& payload,
                                                          const vk::Viewport& viewport,
                                                          const vk::Rect2D& scissor,
                                                          vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  VLOG(2) << fmt::format("record begin accum=0x{:x} moments=0x{:x}",
                         payload.accumulationAttachment.id,
                         payload.momentsAttachment.id);

  CHECK(payload.accumulationAttachment.backend == RenderBackend::Vulkan)
    << "GL accumulationAttachment in Vulkan path";
  CHECK(payload.momentsAttachment.backend == RenderBackend::Vulkan) << "GL momentsAttachment in Vulkan path";
  if (!payload.accumulationAttachment.valid() || !payload.momentsAttachment.valid()) {
    return;
  }

  auto& accumulationTexture = vulkan::textureFromHandle(payload.accumulationAttachment,
                                                        m_backend.device(),
                                                        "texture-weighted-average accumulation attachment");
  auto& momentsTexture = vulkan::textureFromHandle(payload.momentsAttachment,
                                                   m_backend.device(),
                                                   "texture-weighted-average moments attachment");

  const uint32_t accumIdx =
    m_backend.bindlessLookupSampledImageAutoOrCrash(accumulationTexture, "texture_weighted_average accum");
  const uint32_t momentsIdx =
    m_backend.bindlessLookupSampledImageAutoOrCrash(momentsTexture, "texture_weighted_average moments");

  const auto formats = vulkan::extractAttachmentFormats(batch);
  CHECK_EQ(formats.colorFormats.size(), size_t{1}) << "WA resolve requires exactly one color attachment.";
  m_backend.validateFormatsOrCrash(formats, "WA_resolve");

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());

  // Shared fullscreen quad
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  cmd.bindVertexBuffers(0, {quad.buffer()}, {vk::DeviceSize(0)});

  {
    std::array<vk::DescriptorSet, 1> sets{m_backend.bindlessSampledImageDescriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  WAvgFinalPushConstants constants;
  constants.accumTex = accumIdx;
  constants.momentsTex = momentsIdx;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&constants);
  vk::ArrayProxy<const std::uint8_t> payloadBytes(sizeof(constants), bytes);
  cmd.pushConstants(pipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, payloadBytes);

  cmd.draw(4, 1, 0, 0);
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

ZVulkanTextureWeightedAveragePipelineContext::PipelineInstance&
ZVulkanTextureWeightedAveragePipelineContext::ensurePipeline(const PipelineKey& key,
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
                                    ZVulkanShader::spirvResourcePath(QStringLiteral("wavg_final.frag.spv")),
                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({m_backend.bindlessSampledImageDescriptorSetLayout()});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  // Resolve pass writes a representative depth (mean depth from moments) via
  // gl_FragDepth in the fragment shader. To guarantee the fragment shader runs
  // across the full-screen quad even when existing depth contains nearer values,
  // use Always for the depth compare and allow the shader to determine the final
  // per-pixel depth. This mirrors the GL path and avoids early-depth culling.
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
  instance.pipeline->setDepthWriteEnable(true);

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
                              .size = static_cast<uint32_t>(sizeof(WAvgFinalPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
