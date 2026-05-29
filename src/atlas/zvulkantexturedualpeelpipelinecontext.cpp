#include "zvulkantexturedualpeelpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanbuffer.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zlog.h"

#include <QString>
#include <array>
#include <vector>

namespace nim {

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
  (void)renderer;
  // Shared fullscreen quad
  m_vertexCount = 4;

  const char* stageLabel = "blend";
  switch (payload.stage) {
    case TextureDualPeelPayload::Stage::Carry:
      stageLabel = "carry";
      break;
    case TextureDualPeelPayload::Stage::Blend:
      stageLabel = "blend";
      break;
    case TextureDualPeelPayload::Stage::Final:
      stageLabel = "final";
      break;
  }
  VLOG(2) << fmt::format("DDP::record begin stage={} temp=0x{:x} depth=0x{:x} front=0x{:x} back=0x{:x}",
                         stageLabel,
                         payload.tempAttachment.id,
                         payload.depthAttachment.id,
                         payload.frontAttachment.id,
                         payload.backAttachment.id);

  Stage stage = Stage::Blend;
  if (payload.stage == TextureDualPeelPayload::Stage::Final) {
    stage = Stage::Final;
  } else if (payload.stage == TextureDualPeelPayload::Stage::Carry) {
    stage = Stage::Carry;
  }

  if (stage == Stage::Blend) {
    CHECK(payload.tempAttachment.backend == RenderBackend::Vulkan)
      << "GL tempAttachment in Vulkan dual-peel blend path";
    if (!payload.tempAttachment.valid()) {
      return;
    }
  } else if (stage == Stage::Carry) {
    CHECK(payload.depthAttachment.valid() && payload.frontAttachment.valid())
      << "Skipping Vulkan dual-peel carry stage due to missing attachments";
  } else {
    CHECK(payload.frontAttachment.valid() && payload.backAttachment.valid() && payload.depthAttachment.valid())
      << "Skipping Vulkan dual-peel final stage due to missing attachments";
  }

  DualPeelPushConstants ddpPC{};
  if (stage == Stage::Carry) {
    auto& prevDepthTexture =
      vulkan::textureFromHandle(payload.depthAttachment, m_backend.device(), "dual-peel carry prev depth blender");
    auto& prevFrontTexture =
      vulkan::textureFromHandle(payload.frontAttachment, m_backend.device(), "dual-peel carry prev front blender");
    ddpPC.tex0 = m_backend.bindlessLookupSampledImageAutoOrCrash(prevDepthTexture, "ddp_carry depth_prev");
    ddpPC.tex1 = m_backend.bindlessLookupSampledImageAutoOrCrash(prevFrontTexture, "ddp_carry front_prev");
  } else if (stage == Stage::Blend) {
    CHECK(payload.tempAttachment.valid()) << "Skipping Vulkan dual-peel blend stage because temp attachment is missing";
    auto& tempTexture =
      vulkan::textureFromHandle(payload.tempAttachment, m_backend.device(), "dual-peel blend attachment");
    ddpPC.tex0 = m_backend.bindlessLookupSampledImageAutoOrCrash(tempTexture, "ddp_blend temp");
  } else {
    auto& depthTexture =
      vulkan::textureFromHandle(payload.depthAttachment, m_backend.device(), "dual-peel depth attachment");
    auto& frontTexture =
      vulkan::textureFromHandle(payload.frontAttachment, m_backend.device(), "dual-peel front attachment");
    auto& backTexture =
      vulkan::textureFromHandle(payload.backAttachment, m_backend.device(), "dual-peel back attachment");
    ddpPC.tex0 = m_backend.bindlessLookupSampledImageAutoOrCrash(depthTexture, "ddp_final depth");
    ddpPC.tex1 = m_backend.bindlessLookupSampledImageAutoOrCrash(frontTexture, "ddp_final front");
    ddpPC.tex2 = m_backend.bindlessLookupSampledImageAutoOrCrash(backTexture, "ddp_final back");
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  // Composite resolve invariant: enforce single color attachment; depth optional
  if (stage == Stage::Final) {
    CHECK(formats.colorFormats.size() == 1) << "Skipping DDP final: expected exactly 1 color attachment.";
  } else if (stage == Stage::Carry) {
    // Carry is executed into the peel surface; it must have 3 attachments (depth/front/backTemp).
    CHECK(formats.colorFormats.size() == 3) << "Skipping DDP carry: expected exactly 3 color attachments.";
  }
  const char* formatTag = "DDP_blend";
  if (stage == Stage::Final) {
    formatTag = "DDP_final";
  } else if (stage == Stage::Carry) {
    formatTag = "DDP_carry";
  }
  m_backend.validateFormatsOrCrash(formats, formatTag);

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

  {
    const std::array<vk::DescriptorSet, 1> sets{m_backend.bindlessSampledImageDescriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, instance.pipeline->pipelineLayout(), 0, sets, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  cmd.pushConstants<DualPeelPushConstants>(instance.pipeline->pipelineLayout(),
                                           vk::ShaderStageFlagBits::eFragment,
                                           0,
                                           ddpPC);

  VLOG(2) << fmt::format("DDP: draw {} verts", m_vertexCount);
  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
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

ZVulkanTextureDualPeelPipelineContext::PipelineInstance&
ZVulkanTextureDualPeelPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();

  PipelineInstance instance;
  instance.stage = key.stage;

  QString fragmentShader = QStringLiteral("dual_peeling_blend.frag.spv");
  if (key.stage == Stage::Final) {
    fragmentShader = QStringLiteral("dual_peeling_final.frag.spv");
  } else if (key.stage == Stage::Carry) {
    fragmentShader = QStringLiteral("dual_peeling_carry.frag.spv");
  }

  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    ZVulkanShader::spirvResourcePath(QStringLiteral("pass.vert.spv")),
                                                    ZVulkanShader::spirvResourcePath(fragmentShader),
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({m_backend.bindlessSampledImageDescriptorSetLayout()});
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
    // Accumulate pass: composite the temporary back color into the back-blend
    // buffer using premultiplied alpha (matches GL: ONE, ONE_MINUS_SRC_ALPHA).
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  } else if (key.stage == Stage::Final) {
    // Final composite must blend the resolved transparent layer over the
    // already-loaded background (GL behavior: ONE, ONE_MINUS_SRC_ALPHA).
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  } else {
    // Carry pass: overwrite current ping targets (no blending).
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
