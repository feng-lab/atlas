#include "zvulkantextureglowpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkanbuffer.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zlog.h"

#include <algorithm>
#include <array>
#include <vector>

namespace nim {

namespace {

int toGlowModeConstant(GlowMode mode)
{
  switch (mode) {
    case GlowMode::Additive:
      return 0;
    case GlowMode::Screen:
      return 1;
    case GlowMode::Softlight:
      return 2;
    case GlowMode::Glowmap:
      return 3;
  }
  return 1;
}

} // namespace

ZVulkanTextureGlowPipelineContext::ZVulkanTextureGlowPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureGlowPipelineContext::~ZVulkanTextureGlowPipelineContext() = default;

void ZVulkanTextureGlowPipelineContext::resetFrame()
{
  // No per-frame descriptor state retained; bindless set is owned by the backend.
}

void ZVulkanTextureGlowPipelineContext::record(Z3DRendererBase& renderer,
                                               const RenderBatch& batch,
                                               const TextureGlowPayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  CHECK(payload.stage != TextureGlowPayload::Stage::Unspecified) << "Vulkan glow pass missing stage";
  CHECK(payload.colorAttachmentHandle.valid() && payload.depthAttachmentHandle.valid())
    << "Vulkan glow pass missing base input handles";

  auto& inputColor =
    vulkan::textureFromHandle(payload.colorAttachmentHandle, m_backend.device(), "texture-glow input color attachment");
  auto& inputDepth =
    vulkan::textureFromHandle(payload.depthAttachmentHandle, m_backend.device(), "texture-glow input depth attachment");

  const glm::uvec2 inputSize(inputColor.width(), inputColor.height());
  if (inputSize.x == 0u || inputSize.y == 0u) {
    return;
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);
  if (formats.colorFormats.empty()) {
    return;
  }
  CHECK(formats.depthFormat.has_value()) << "Vulkan glow passes require a depth attachment";

  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  ZVulkanPipelineCommandRecorder recorder(cmd);
  const vk::DescriptorSet bindlessSet = m_backend.bindlessSampledImageDescriptorSet();

  auto recordBlur = [&](bool horizontal) {
    const uint32_t inputColorIdx = m_backend.bindlessLookupSampledImageAutoOrCrash(inputColor, "glow_blur input color");
    const uint32_t inputDepthIdx = m_backend.bindlessLookupSampledImageAutoOrCrash(inputDepth, "glow_blur input depth");

    BlurPipelineKey blurKey{};
    blurKey.horizontal = horizontal;
    blurKey.colorFormats = formats.colorFormats;
    blurKey.depthFormat = formats.depthFormat;
    PipelineInstance& blurPipeline = ensureBlurPipeline(blurKey, formats);

    CHECK(viewport.width > 0.0f && viewport.height > 0.0f) << "Glow blur requires a valid viewport extent";
    BlurPushConstants blurConstants{};
    blurConstants.screenDimRcp = glm::vec2(1.0f / viewport.width, 1.0f / viewport.height);
    blurConstants.blurRadius = payload.blurRadius;
    blurConstants.blurScale = payload.blurScale;
    blurConstants.blurStrength = payload.blurStrength;
    blurConstants.colorTexture = inputColorIdx;
    blurConstants.depthTexture = inputDepthIdx;

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = blurPipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = blurPipeline.pipeline->pipelineLayoutHandle();
    const std::array<vk::DescriptorSet, 1> descriptorSets{bindlessSet};
    drawSpec.descriptorSets = descriptorSets;
    drawSpec.descriptorSetFirst = 0;
    drawSpec.expectedDescriptorSetCount = 1;
    const std::array<vk::Buffer, 1> vertexBuffers{quad.buffer()};
    const std::array<vk::DeviceSize, 1> vertexOffsets{vk::DeviceSize(0)};
    drawSpec.vertexBuffers = vertexBuffers;
    drawSpec.vertexOffsets = vertexOffsets;
    drawSpec.vertexCount = 4;
    drawSpec.instanceCount = 1;
    drawSpec.pushConstantsData = &blurConstants;
    drawSpec.pushConstantsSize = sizeof(blurConstants);
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    drawSpec.requirePushConstants = true;
    recorder.recordGraphicsDraw(drawSpec);
  };

  auto recordComposite = [&]() {
    CHECK(payload.blurColorAttachmentHandle.valid() && payload.blurDepthAttachmentHandle.valid())
      << "Glow composite stage missing blur inputs";
    auto& blurColor = vulkan::textureFromHandle(payload.blurColorAttachmentHandle,
                                                m_backend.device(),
                                                "texture-glow blur color attachment");
    auto& blurDepth = vulkan::textureFromHandle(payload.blurDepthAttachmentHandle,
                                                m_backend.device(),
                                                "texture-glow blur depth attachment");

    const uint32_t inputColorIdx =
      m_backend.bindlessLookupSampledImageAutoOrCrash(inputColor, "glow_composite input color");
    const uint32_t inputDepthIdx =
      m_backend.bindlessLookupSampledImageAutoOrCrash(inputDepth, "glow_composite input depth");
    const uint32_t blurColorIdx =
      m_backend.bindlessLookupSampledImageAutoOrCrash(blurColor, "glow_composite blur color");
    const uint32_t blurDepthIdx =
      m_backend.bindlessLookupSampledImageAutoOrCrash(blurDepth, "glow_composite blur depth");

    GlowPipelineKey glowKey{};
    glowKey.mode = payload.mode;
    glowKey.colorFormats = formats.colorFormats;
    glowKey.depthFormat = formats.depthFormat;
    PipelineInstance& glowPipeline = ensureGlowPipeline(glowKey, formats);

    CHECK(viewport.width > 0.0f && viewport.height > 0.0f) << "Glow composite requires a valid viewport extent";
    GlowPushConstants glowConstants{};
    glowConstants.screenDimRcp = glm::vec2(1.0f / viewport.width, 1.0f / viewport.height);
    glowConstants.colorTexture = inputColorIdx;
    glowConstants.depthTexture = inputDepthIdx;
    glowConstants.glowmapColorTexture = blurColorIdx;
    glowConstants.glowmapDepthTexture = blurDepthIdx;

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = glowPipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = glowPipeline.pipeline->pipelineLayoutHandle();
    const std::array<vk::DescriptorSet, 1> descriptorSets{bindlessSet};
    drawSpec.descriptorSets = descriptorSets;
    drawSpec.descriptorSetFirst = 0;
    drawSpec.expectedDescriptorSetCount = 1;
    const std::array<vk::Buffer, 1> vertexBuffers{quad.buffer()};
    const std::array<vk::DeviceSize, 1> vertexOffsets{vk::DeviceSize(0)};
    drawSpec.vertexBuffers = vertexBuffers;
    drawSpec.vertexOffsets = vertexOffsets;
    drawSpec.vertexCount = 4;
    drawSpec.instanceCount = 1;
    drawSpec.pushConstantsData = &glowConstants;
    drawSpec.pushConstantsSize = sizeof(glowConstants);
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    drawSpec.requirePushConstants = true;
    recorder.recordGraphicsDraw(drawSpec);
  };

  switch (payload.stage) {
    case TextureGlowPayload::Stage::Unspecified:
      CHECK(false) << "Glow payload stage is Unspecified";
      break;
    case TextureGlowPayload::Stage::BlurX:
      recordBlur(true);
      break;
    case TextureGlowPayload::Stage::BlurY:
      recordBlur(false);
      break;
    case TextureGlowPayload::Stage::Composite:
      recordComposite();
      break;
  }
}

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureGlowPipelineContext::makeVertexInputState() const
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

ZVulkanTextureGlowPipelineContext::PipelineInstance&
ZVulkanTextureGlowPipelineContext::ensureBlurPipeline(const BlurPipelineKey& key,
                                                      const vulkan::AttachmentFormats& formats)
{
  auto it = m_blurPipelines.find(key);
  if (it != m_blurPipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    ZVulkanShader::spirvResourcePath(QStringLiteral("pass.vert.spv")),
                                                    ZVulkanShader::spirvResourcePath(QStringLiteral("blur.frag.spv")),
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({m_backend.bindlessSampledImageDescriptorSetLayout()});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
  instance.pipeline->setDepthWriteEnable(true);

  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_FALSE;
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  vk::SpecializationMapEntry entry{.constantID = 100, .offset = 0, .size = sizeof(int)};
  const int orientation = key.horizontal ? 0 : 1;
  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    {entry},
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&orientation),
                         reinterpret_cast<const uint8_t*>(&orientation) + sizeof(orientation)));

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(BlurPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_blurPipelines.insert({key, std::move(instance)});
  return inserted->second;
}

ZVulkanTextureGlowPipelineContext::PipelineInstance&
ZVulkanTextureGlowPipelineContext::ensureGlowPipeline(const GlowPipelineKey& key,
                                                      const vulkan::AttachmentFormats& formats)
{
  auto it = m_glowPipelines.find(key);
  if (it != m_glowPipelines.end()) {
    return it->second;
  }

  auto& device = m_backend.device();

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    ZVulkanShader::spirvResourcePath(QStringLiteral("pass.vert.spv")),
                                                    ZVulkanShader::spirvResourcePath(QStringLiteral("glow.frag.spv")),
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({m_backend.bindlessSampledImageDescriptorSetLayout()});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
  instance.pipeline->setDepthWriteEnable(true);

  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_FALSE;
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  vk::SpecializationMapEntry entry{.constantID = 110, .offset = 0, .size = sizeof(int)};
  const int glowMode = toGlowModeConstant(key.mode);
  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    {entry},
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&glowMode),
                         reinterpret_cast<const uint8_t*>(&glowMode) + sizeof(glowMode)));

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(GlowPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_glowPipelines.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
