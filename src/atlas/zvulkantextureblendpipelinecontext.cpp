#include "zvulkantextureblendpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dtextureblendrenderer.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbuffer.h"
#include "zlog.h"

#include <algorithm>
#include <array>
#include <vector>

namespace nim {

namespace {

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
  // No per-frame descriptor state retained; bindless set is owned by the backend.
}

void ZVulkanTextureBlendPipelineContext::record(Z3DRendererBase& renderer,
                                                const RenderBatch& batch,
                                                const TextureBlendPayload& payload,
                                                const vk::Viewport& viewport,
                                                const vk::Rect2D& scissor,
                                                vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  (void)payload;

  CHECK(payload.colorAttachmentHandle0.valid() && payload.depthAttachmentHandle0.valid() &&
        payload.colorAttachmentHandle1.valid() && payload.depthAttachmentHandle1.valid())
    << "Skipping Vulkan texture blend pass due to missing attachments";

  auto& color0 =
    vulkan::textureFromHandle(payload.colorAttachmentHandle0, m_backend.device(), "texture-blend color attachment 0");
  auto& depth0 =
    vulkan::textureFromHandle(payload.depthAttachmentHandle0, m_backend.device(), "texture-blend depth attachment 0");
  auto& color1 =
    vulkan::textureFromHandle(payload.colorAttachmentHandle1, m_backend.device(), "texture-blend color attachment 1");
  auto& depth1 =
    vulkan::textureFromHandle(payload.depthAttachmentHandle1, m_backend.device(), "texture-blend depth attachment 1");

  // Shared fullscreen quad
  m_vertexCount = 4;

  const uint32_t color0Idx = m_backend.bindlessLookupSampledImageAutoOrCrash(color0, "texture_blend color0");
  const uint32_t depth0Idx = m_backend.bindlessLookupSampledImageAutoOrCrash(depth0, "texture_blend depth0");
  const uint32_t color1Idx = m_backend.bindlessLookupSampledImageAutoOrCrash(color1, "texture_blend color1");
  const uint32_t depth1Idx = m_backend.bindlessLookupSampledImageAutoOrCrash(depth1, "texture_blend depth1");

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.mode = payload.mode;
  key.enableBlend = payload.enableFixedBlend;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  cmd.bindVertexBuffers(0, {quad.buffer()}, {vk::DeviceSize(0)});

  {
    std::array<vk::DescriptorSet, 1> sets{m_backend.bindlessSampledImageDescriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  const glm::vec2 extent(viewport.width, viewport.height);
  CHECK(extent.x > 0.0f && extent.y > 0.0f) << "Vulkan texture blend pass requires a valid viewport extent";

  TextureBlendPushConstants constants;
  constants.screenDimRcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  constants.colorTexture0 = color0Idx;
  constants.depthTexture0 = depth0Idx;
  constants.colorTexture1 = color1Idx;
  constants.depthTexture1 = depth1Idx;

  cmd.pushConstants<TextureBlendPushConstants>(pipeline.pipeline->pipelineLayout(),
                                               vk::ShaderStageFlagBits::eFragment,
                                               0,
                                               constants);

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
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

void ZVulkanTextureBlendPipelineContext::ensureVertexCapacity(size_t) {}
void ZVulkanTextureBlendPipelineContext::uploadGeometry() {}

ZVulkanTextureBlendPipelineContext::PipelineInstance&
ZVulkanTextureBlendPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
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
                                    ZVulkanShader::spirvResourcePath(QStringLiteral("compositor.frag.spv")),
                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({m_backend.bindlessSampledImageDescriptorSetLayout()});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  // Depth: compositor writes gl_FragDepth; use Always and write enabled.
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
  instance.pipeline->setDepthWriteEnable(true);

  // Color blending: select at pipeline creation time based on per-draw key.
  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  if (key.enableBlend) {
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;             // premultiplied alpha
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  } else {
    blendAttachment.blendEnable = VK_FALSE;
  }
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
