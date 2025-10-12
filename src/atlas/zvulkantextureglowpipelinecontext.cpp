#include "zvulkantextureglowpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dtextureglowrenderer.h"
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
#include "zlog.h"

#include <algorithm>
#include <array>
#include <vector>

namespace nim {

namespace {

constexpr float kQuadDepth = 0.0f;

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
  m_vertexCount = 0;
  resetDescriptors();
}

void ZVulkanTextureGlowPipelineContext::resetDescriptors()
{
  m_blurDescriptor.reset();
  m_glowDescriptor.reset();
}

void ZVulkanTextureGlowPipelineContext::record(Z3DRendererBase& renderer,
                                               const RenderBatch& batch,
                                               const TextureGlowPayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  (void)payload;

  CHECK(payload.colorAttachmentHandle.valid() && payload.depthAttachmentHandle.valid())
    << "Skipping Vulkan glow pass due to missing attachments";

  auto& colorTexture =
    vulkan::textureFromHandle(payload.colorAttachmentHandle, m_backend.device(), "texture-glow color attachment");
  auto& depthTexture =
    vulkan::textureFromHandle(payload.depthAttachmentHandle, m_backend.device(), "texture-glow depth attachment");

  glm::uvec2 size(colorTexture.width(), colorTexture.height());
  if (size.x == 0u || size.y == 0u) {
    return;
  }

  // Shared fullscreen quad
  m_vertexCount = 4;

  ensureDescriptorLayouts();

  const vk::Format colorFormat = colorTexture.format();
  ensureIntermediateTextures(size, colorFormat);
  if (!m_blurIntermediate0.color.texture || !m_blurIntermediate0.depth.texture || !m_blurIntermediate1.color.texture ||
      !m_blurIntermediate1.depth.texture) {
    return;
  }

  auto rebuildRenderingAttachments = [&](std::vector<vk::RenderingAttachmentInfo>& colors,
                                         std::optional<vk::RenderingAttachmentInfo>& depth) {
    colors.clear();
    depth.reset();

    auto makeColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
      if (!attachment.handle.valid()) {
        return std::nullopt;
      }
      auto& texture =
        vulkan::textureFromHandle(attachment.handle, m_backend.device(), "texture-glow intermediate color attachment");
      const auto desiredLayout = vk::ImageLayout::eColorAttachmentOptimal;
      texture.transitionLayout(cmd, texture.layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture.imageView();
      info.imageLayout = desiredLayout;
      info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
      info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
      vk::ClearValue clear{};
      clear.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                             attachment.clearValue.color.g,
                                                             attachment.clearValue.color.b,
                                                             attachment.clearValue.color.a});
      info.clearValue = clear;
      return info;
    };

    auto makeDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
      if (!attachment.handle.valid()) {
        return std::nullopt;
      }
      auto& texture =
        vulkan::textureFromHandle(attachment.handle, m_backend.device(), "texture-glow intermediate depth attachment");
      const auto desiredLayout = vk::ImageLayout::eDepthAttachmentOptimal;
      texture.transitionLayout(cmd, texture.layout(), desiredLayout, vk::ImageAspectFlagBits::eDepth);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture.imageView();
      info.imageLayout = desiredLayout;
      info.loadOp = vulkan::toVkLoadOp(attachment.loadOp);
      info.storeOp = vulkan::toVkStoreOp(attachment.storeOp);
      vk::ClearValue clear{};
      clear.depthStencil = vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
      info.clearValue = clear;
      return info;
    };

    for (const auto& attachment : batch.pass.colorAttachments) {
      if (auto vkAttachment = makeColorAttachment(attachment)) {
        colors.push_back(*vkAttachment);
      }
    }
    if (batch.pass.depthAttachment) {
      depth = makeDepthAttachment(*batch.pass.depthAttachment);
    }
  };

  std::vector<vk::RenderingAttachmentInfo> colorAttachments;
  std::optional<vk::RenderingAttachmentInfo> depthAttachment;
  rebuildRenderingAttachments(colorAttachments, depthAttachment);

  runBlurPass(renderer,
              cmd,
              colorTexture,
              depthTexture,
              m_blurIntermediate0,
              true,
              size,
              payload.blurRadius,
              payload.blurScale,
              payload.blurStrength);

  runBlurPass(renderer,
              cmd,
              *m_blurIntermediate0.color.texture,
              *m_blurIntermediate0.depth.texture,
              m_blurIntermediate1,
              false,
              size,
              payload.blurRadius,
              payload.blurScale,
              payload.blurStrength);

  // Final pass rendering begins here
  const vk::Rect2D renderArea{
    vk::Offset2D{static_cast<int32_t>(batch.pass.viewport.origin.x),
                 static_cast<int32_t>(batch.pass.viewport.origin.y) },
    vk::Extent2D{static_cast<uint32_t>(batch.pass.viewport.extent.x),
                 static_cast<uint32_t>(batch.pass.viewport.extent.y)}
  };

  vk::RenderingInfo renderingInfo;
  renderingInfo.renderArea = renderArea;
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
  renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
  vk::RenderingAttachmentInfo depthAttachmentInfo;
  if (depthAttachment) {
    depthAttachmentInfo = *depthAttachment;
    renderingInfo.pDepthAttachment = &depthAttachmentInfo;
  } else {
    renderingInfo.pDepthAttachment = nullptr;
  }

  cmd.beginRendering(renderingInfo);

  ZVulkanDescriptorSet* glowDS = nullptr;
  if (m_glowSetLayout) {
    glowDS = m_backend.allocateOverrideDescriptorSet(**m_glowSetLayout);
  }
  CHECK(glowDS != nullptr) << "Glow final: override descriptor allocation failed (fatal)";
  glowDS->updateTexture(vkbind::kGlowBindingColorIn, colorTexture, m_backend.defaultSampler());
  glowDS->updateTexture(vkbind::kGlowBindingDepthIn, depthTexture, m_backend.defaultSampler());
  glowDS->updateTexture(vkbind::kGlowBindingBlurIn0, *m_blurIntermediate1.color.texture, m_backend.defaultSampler());
  glowDS->updateTexture(vkbind::kGlowBindingBlurIn1, *m_blurIntermediate1.depth.texture, m_backend.defaultSampler());

  const auto formats = vulkan::extractAttachmentFormats(batch);
  GlowPipelineKey glowKey;
  glowKey.mode = payload.mode;
  glowKey.colorFormats = formats.colorFormats;
  glowKey.depthFormat = formats.depthFormat;

  PipelineInstance& glowPipeline = ensureGlowPipeline(glowKey, formats);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, glowPipeline.pipeline->pipeline());
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  const vk::DeviceSize offsets[] = {0};
  cmd.bindVertexBuffers(0, {quad.buffer()}, offsets);
  {
    std::array<vk::DescriptorSet, 1> sets{glowDS->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           glowPipeline.pipeline->pipelineLayout(),
                           vkbind::kSetInputs,
                           sets,
                           {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  glm::vec2 finalExtent = batch.pass.viewport.extent;
  if (finalExtent.x <= 0.0f || finalExtent.y <= 0.0f) {
    const auto& viewportState = renderer.frameState().viewport;
    finalExtent = glm::vec2(static_cast<float>(viewportState.z), static_cast<float>(viewportState.w));
  }
  if (finalExtent.x <= 0.0f) {
    finalExtent.x = 1.0f;
  }
  if (finalExtent.y <= 0.0f) {
    finalExtent.y = 1.0f;
  }

  GlowPushConstants glowConstants;
  glowConstants.screenDimRcp = glm::vec2(1.0f / finalExtent.x, 1.0f / finalExtent.y);
  cmd.pushConstants<GlowPushConstants>(glowPipeline.pipeline->pipelineLayout(),
                                       vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       glowConstants);

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
  // Self-managed context: ensure we close the dynamic rendering segment
  cmd.endRendering();
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

void ZVulkanTextureGlowPipelineContext::ensureDescriptorPool() {}

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

void ZVulkanTextureGlowPipelineContext::ensureIntermediateTextures(const glm::uvec2& size, vk::Format colorFormat)
{
  auto ensureTexture =
    [&](TextureUpload& upload, vk::Format format, vk::ImageUsageFlags usage, vk::ImageLayout descriptorLayout) {
      const glm::uvec3 extent(size.x, size.y, 1u);
      if (!upload.texture || upload.extent != extent || upload.format != format) {
        auto& device = m_backend.device();
        auto info = ZVulkanTexture::CreateInfo::make2D(static_cast<uint32_t>(size.x),
                                                       static_cast<uint32_t>(size.y),
                                                       format,
                                                       usage,
                                                       vk::MemoryPropertyFlagBits::eDeviceLocal,
                                                       1,
                                                       true,
                                                       descriptorLayout);
        upload.texture = device.createTexture(info);
        CHECK(upload.texture != nullptr) << "Glow: failed to create intermediate texture";
        upload.extent = extent;
        upload.format = format;
      }
    };

  const auto colorUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
  const auto depthUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
  ensureTexture(m_blurIntermediate0.color, colorFormat, colorUsage, vk::ImageLayout::eShaderReadOnlyOptimal);
  ensureTexture(m_blurIntermediate0.depth, vk::Format::eD32Sfloat, depthUsage, vk::ImageLayout::eDepthReadOnlyOptimal);
  ensureTexture(m_blurIntermediate1.color, colorFormat, colorUsage, vk::ImageLayout::eShaderReadOnlyOptimal);
  ensureTexture(m_blurIntermediate1.depth, vk::Format::eD32Sfloat, depthUsage, vk::ImageLayout::eDepthReadOnlyOptimal);
}

void ZVulkanTextureGlowPipelineContext::ensureVertexCapacity(size_t) {}

void ZVulkanTextureGlowPipelineContext::uploadGeometry()
{
  const std::array<QuadVertex, 4> vertices{QuadVertex{glm::vec3(-1.f, 1.f, kQuadDepth)},
                                           QuadVertex{glm::vec3(-1.f, -1.f, kQuadDepth)},
                                           QuadVertex{glm::vec3(1.f, 1.f, kQuadDepth)},
                                           QuadVertex{glm::vec3(1.f, -1.f, kQuadDepth)}};

  m_vertexCount = vertices.size();
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

void ZVulkanTextureGlowPipelineContext::runBlurPass(Z3DRendererBase& renderer,
                                                    vk::raii::CommandBuffer& cmd,
                                                    ZVulkanTexture& inputColor,
                                                    ZVulkanTexture& inputDepth,
                                                    BlurIntermediate& output,
                                                    bool horizontal,
                                                    const glm::uvec2& size,
                                                    int blurRadius,
                                                    float blurScale,
                                                    float blurStrength)
{
  (void)renderer;
  if (!output.color.texture || !output.depth.texture) {
    return;
  }

  output.color.texture->transitionLayout(cmd, output.color.texture->layout(), vk::ImageLayout::eColorAttachmentOptimal);
  {
    // Always use depth-only layout/aspect (stencil unused)
    output.depth.texture->transitionLayout(cmd,
                                           output.depth.texture->layout(),
                                           vk::ImageLayout::eDepthAttachmentOptimal,
                                           vk::ImageAspectFlagBits::eDepth);
  }

  vk::RenderingAttachmentInfo colorAttachment;
  colorAttachment.imageView = output.color.texture->imageView();
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});

  vk::RenderingAttachmentInfo depthAttachment;
  depthAttachment.imageView = output.depth.texture->imageView();
  depthAttachment.imageLayout = output.depth.texture->layout();
  depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  depthAttachment.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

  vk::RenderingInfo renderingInfo;
  renderingInfo.renderArea = vk::Rect2D{
    vk::Offset2D{0,      0     },
    vk::Extent2D{size.x, size.y}
  };
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = &depthAttachment;

  cmd.beginRendering(renderingInfo);

  ZVulkanDescriptorSet* blurDS = nullptr;
  if (m_blurSetLayout) {
    blurDS = m_backend.allocateOverrideDescriptorSet(**m_blurSetLayout);
  }
  CHECK(blurDS != nullptr) << "Glow blur: override descriptor allocation failed (fatal)";

  blurDS->updateTexture(vkbind::kBlurBindingColorIn, inputColor, m_backend.defaultSampler());
  blurDS->updateTexture(vkbind::kBlurBindingDepthIn, inputDepth, m_backend.defaultSampler());

  vulkan::AttachmentFormats formats;
  formats.colorFormats.push_back(output.color.texture->format());
  formats.depthFormat = output.depth.texture->format();

  BlurPipelineKey key;
  key.horizontal = horizontal;
  PipelineInstance& pipeline = ensureBlurPipeline(key, formats);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  const vk::DeviceSize offsets[] = {0};
  cmd.bindVertexBuffers(0, {quad.buffer()}, offsets);
  {
    std::array<vk::DescriptorSet, 1> sets{blurDS->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           pipeline.pipeline->pipelineLayout(),
                           vkbind::kSetInputs,
                           sets,
                           {});
  }

  vk::Viewport localViewport;
  localViewport.x = 0.0f;
  localViewport.y = 0.0f;
  localViewport.width = static_cast<float>(size.x);
  localViewport.height = static_cast<float>(size.y);
  localViewport.minDepth = 0.0f;
  localViewport.maxDepth = 1.0f;
  cmd.setViewport(0, localViewport);

  vk::Rect2D localScissor{
    vk::Offset2D{0,      0     },
    vk::Extent2D{size.x, size.y}
  };
  cmd.setScissor(0, localScissor);

  BlurPushConstants constants;
  constants.screenDimRcp = glm::vec2(1.0f / size.x, 1.0f / size.y);
  constants.blurRadius = blurRadius;
  constants.blurScale = blurScale;
  constants.blurStrength = blurStrength;
  cmd.pushConstants<BlurPushConstants>(pipeline.pipeline->pipelineLayout(),
                                       vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       constants);

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
  cmd.endRendering();

  output.color.texture->transitionLayout(cmd,
                                         vk::ImageLayout::eColorAttachmentOptimal,
                                         vk::ImageLayout::eShaderReadOnlyOptimal);
  output.depth.texture->transitionLayout(cmd,
                                         vk::ImageLayout::eDepthAttachmentOptimal,
                                         vk::ImageLayout::eDepthReadOnlyOptimal,
                                         vk::ImageAspectFlagBits::eDepth);
}

} // namespace nim
