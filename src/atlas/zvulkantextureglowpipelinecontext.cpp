#include "zvulkantextureglowpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkanbuffer.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbindings.h"
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
  resetDescriptors();
}

void ZVulkanTextureGlowPipelineContext::resetDescriptors() {}

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

  ensureDescriptorLayouts();

  const auto formats = vulkan::extractAttachmentFormats(batch);
  if (formats.colorFormats.empty()) {
    return;
  }
  CHECK(formats.depthFormat.has_value()) << "Vulkan glow passes require a depth attachment";

  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  ZVulkanPipelineCommandRecorder recorder(cmd);

  auto recordBlur = [&](bool horizontal) {
    CHECK(m_blurSetLayout.has_value()) << "Glow blur requires blur descriptor set layout";
    ZVulkanDescriptorSet* blurDS = m_backend.allocateOverrideDescriptorSet(**m_blurSetLayout);
    CHECK(blurDS != nullptr) << "Glow blur: override descriptor allocation failed (fatal)";
    blurDS->updateTexture(vkbind::kBlurBindingColorIn, inputColor, m_backend.defaultSampler());
    blurDS->updateTexture(vkbind::kBlurBindingDepthIn, inputDepth, m_backend.defaultSampler());

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

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = blurPipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = blurPipeline.pipeline->pipelineLayoutHandle();
    const std::array<vk::DescriptorSet, 1> descriptorSets{blurDS->descriptorSet()};
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
    CHECK(m_glowSetLayout.has_value()) << "Glow composite requires glow descriptor set layout";
    CHECK(payload.blurColorAttachmentHandle.valid() && payload.blurDepthAttachmentHandle.valid())
      << "Glow composite stage missing blur inputs";
    auto& blurColor = vulkan::textureFromHandle(payload.blurColorAttachmentHandle,
                                                m_backend.device(),
                                                "texture-glow blur color attachment");
    auto& blurDepth = vulkan::textureFromHandle(payload.blurDepthAttachmentHandle,
                                                m_backend.device(),
                                                "texture-glow blur depth attachment");

    ZVulkanDescriptorSet* glowDS = m_backend.allocateOverrideDescriptorSet(**m_glowSetLayout);
    CHECK(glowDS != nullptr) << "Glow composite: override descriptor allocation failed (fatal)";
    glowDS->updateTexture(vkbind::kGlowBindingColorIn, inputColor, m_backend.defaultSampler());
    glowDS->updateTexture(vkbind::kGlowBindingDepthIn, inputDepth, m_backend.defaultSampler());
    glowDS->updateTexture(vkbind::kGlowBindingBlurIn0, blurColor, m_backend.defaultSampler());
    glowDS->updateTexture(vkbind::kGlowBindingBlurIn1, blurDepth, m_backend.defaultSampler());

    GlowPipelineKey glowKey{};
    glowKey.mode = payload.mode;
    glowKey.colorFormats = formats.colorFormats;
    glowKey.depthFormat = formats.depthFormat;
    PipelineInstance& glowPipeline = ensureGlowPipeline(glowKey, formats);

    CHECK(viewport.width > 0.0f && viewport.height > 0.0f) << "Glow composite requires a valid viewport extent";
    GlowPushConstants glowConstants{};
    glowConstants.screenDimRcp = glm::vec2(1.0f / viewport.width, 1.0f / viewport.height);

    ZVulkanGraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = glowPipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = glowPipeline.pipeline->pipelineLayoutHandle();
    const std::array<vk::DescriptorSet, 1> descriptorSets{glowDS->descriptorSet()};
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

void ZVulkanTextureGlowPipelineContext::ensureDescriptorLayouts()
{
  if (!m_blurSetLayout) {
    auto& device = m_backend.device();
    auto& vkDevice = device.context().device();

    std::array<vk::DescriptorSetLayoutBinding, 2> blurBindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    // Immutable default samplers for blur inputs
    vk::Sampler immutable = m_backend.defaultSampler();
    blurBindings[0].pImmutableSamplers = &immutable;
    blurBindings[1].pImmutableSamplers = &immutable;

    vk::DescriptorSetLayoutCreateInfo blurInfo{.bindingCount = static_cast<uint32_t>(blurBindings.size()),
                                               .pBindings = blurBindings.data()};
    m_blurSetLayout.emplace(vkDevice, blurInfo);
  }

  if (!m_glowSetLayout) {
    auto& device = m_backend.device();
    auto& vkDevice = device.context().device();

    std::array<vk::DescriptorSetLayoutBinding, 4> glowBindings{
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 3,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };

    // Immutable default samplers for glow inputs
    vk::Sampler immutableGlow = m_backend.defaultSampler();
    glowBindings[0].pImmutableSamplers = &immutableGlow;
    glowBindings[1].pImmutableSamplers = &immutableGlow;
    glowBindings[2].pImmutableSamplers = &immutableGlow;
    glowBindings[3].pImmutableSamplers = &immutableGlow;

    vk::DescriptorSetLayoutCreateInfo glowInfo{.bindingCount = static_cast<uint32_t>(glowBindings.size()),
                                               .pBindings = glowBindings.data()};
    m_glowSetLayout.emplace(vkDevice, glowInfo);
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
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + "pass.vert.spv", shaderBase + "blur.frag.spv", std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_blurSetLayout});
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
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + "pass.vert.spv", shaderBase + "glow.frag.spv", std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_glowSetLayout});
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
