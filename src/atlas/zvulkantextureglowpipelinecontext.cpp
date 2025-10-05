#include "zvulkantextureglowpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dtextureglowrenderer.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
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
}

void ZVulkanTextureGlowPipelineContext::record(Z3DRendererBase& renderer,
                                               const RenderBatch& batch,
                                               const TextureGlowPayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  if (!payload.renderer) {
    return;
  }

  CHECK(payload.colorAttachmentHandle.backend == AttachmentBackend::Vulkan)
    << "GL colorAttachmentHandle in Vulkan path";
  CHECK(payload.depthAttachmentHandle.backend == AttachmentBackend::Vulkan)
    << "GL depthAttachmentHandle in Vulkan path";

  auto* colorTexture = reinterpret_cast<ZVulkanTexture*>(payload.colorAttachmentHandle.id);
  auto* depthTexture = reinterpret_cast<ZVulkanTexture*>(payload.depthAttachmentHandle.id);
  if (!colorTexture || !depthTexture) {
    return;
  }

  glm::uvec2 size(colorTexture->width(), colorTexture->height());
  if (size.x == 0u || size.y == 0u) {
    return;
  }

  ensureVertexCapacity(4);
  uploadGeometry();
  if (!m_vertexBuffer || m_vertexCount == 0) {
    return;
  }

  ensureDescriptorLayouts();
  ensureDescriptorPool();

  const vk::Format colorFormat = colorTexture->format();
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
      if (attachment.handle.id == 0) {
        return std::nullopt;
      }
      CHECK(attachment.handle.backend == AttachmentBackend::Vulkan)
        << "GL color attachment encountered in Vulkan glow pipeline";
      auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
      if (!texture) {
        return std::nullopt;
      }
      const auto desiredLayout = vk::ImageLayout::eColorAttachmentOptimal;
      texture->transitionLayout(cmd, texture->layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture->imageView();
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
      if (attachment.handle.id == 0) {
        return std::nullopt;
      }
      CHECK(attachment.handle.backend == AttachmentBackend::Vulkan)
        << "GL depth attachment encountered in Vulkan glow pipeline";
      auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
      if (!texture) {
        return std::nullopt;
      }
      const auto desiredLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
      texture->transitionLayout(cmd, texture->layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture->imageView();
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

  cmd.endRendering();

  runBlurPass(renderer,
              cmd,
              *colorTexture,
              *depthTexture,
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

  if (!m_glowDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_glowSetLayout);
    m_glowDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }

  m_glowDescriptor->updateTexture(0, *colorTexture);
  m_glowDescriptor->updateTexture(1, *depthTexture);
  m_glowDescriptor->updateTexture(2, *m_blurIntermediate1.color.texture);
  m_glowDescriptor->updateTexture(3, *m_blurIntermediate1.depth.texture);

  const auto formats = vulkan::extractAttachmentFormats(batch);
  GlowPipelineKey glowKey;
  glowKey.mode = payload.mode;
  glowKey.colorFormats = formats.colorFormats;
  glowKey.depthFormat = formats.depthFormat;

  PipelineInstance& glowPipeline = ensureGlowPipeline(glowKey, formats);

  vk::DeviceSize offsets = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, glowPipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offsets});
  if (m_glowDescriptor) {
    std::array<vk::DescriptorSet, 1> sets{m_glowDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, glowPipeline.pipeline->pipelineLayout(), 0, sets, {});
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

    vk::DescriptorSetLayoutCreateInfo glowInfo{.bindingCount = static_cast<uint32_t>(glowBindings.size()),
                                               .pBindings = glowBindings.data()};
    m_glowSetLayout.emplace(vkDevice, glowInfo);
  }
}

void ZVulkanTextureGlowPipelineContext::ensureDescriptorPool()
{
  if (!m_descriptorPool) {
    m_descriptorPool = m_backend.device().createDescriptorPool();
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
        upload.extent = extent;
        upload.format = format;
      }
    };

  const auto colorUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
  const auto depthUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
  ensureTexture(m_blurIntermediate0.color, colorFormat, colorUsage, vk::ImageLayout::eShaderReadOnlyOptimal);
  ensureTexture(m_blurIntermediate0.depth,
                vk::Format::eD32Sfloat,
                depthUsage,
                vk::ImageLayout::eDepthStencilReadOnlyOptimal);
  ensureTexture(m_blurIntermediate1.color, colorFormat, colorUsage, vk::ImageLayout::eShaderReadOnlyOptimal);
  ensureTexture(m_blurIntermediate1.depth,
                vk::Format::eD32Sfloat,
                depthUsage,
                vk::ImageLayout::eDepthStencilReadOnlyOptimal);
}

void ZVulkanTextureGlowPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(QuadVertex);
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

void ZVulkanTextureGlowPipelineContext::uploadGeometry()
{
  const std::array<QuadVertex, 4> vertices{QuadVertex{glm::vec3(-1.f, 1.f, kQuadDepth)},
                                           QuadVertex{glm::vec3(-1.f, -1.f, kQuadDepth)},
                                           QuadVertex{glm::vec3(1.f, 1.f, kQuadDepth)},
                                           QuadVertex{glm::vec3(1.f, -1.f, kQuadDepth)}};

  if (!m_vertexBuffer) {
    return;
  }

  m_vertexBuffer->copyData(vertices.data(), vertices.size() * sizeof(QuadVertex));
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
  output.depth.texture->transitionLayout(cmd,
                                         output.depth.texture->layout(),
                                         vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                         vk::ImageAspectFlagBits::eDepth);

  vk::RenderingAttachmentInfo colorAttachment;
  colorAttachment.imageView = output.color.texture->imageView();
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});

  vk::RenderingAttachmentInfo depthAttachment;
  depthAttachment.imageView = output.depth.texture->imageView();
  depthAttachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
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

  if (!m_blurDescriptor) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_blurSetLayout);
    m_blurDescriptor = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }

  m_blurDescriptor->updateTexture(0, inputColor);
  m_blurDescriptor->updateTexture(1, inputDepth);

  vulkan::AttachmentFormats formats;
  formats.colorFormats.push_back(output.color.texture->format());
  formats.depthFormat = output.depth.texture->format();

  BlurPipelineKey key;
  key.horizontal = horizontal;
  PipelineInstance& pipeline = ensureBlurPipeline(key, formats);

  vk::DeviceSize offsets = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offsets});
  if (m_blurDescriptor) {
    std::array<vk::DescriptorSet, 1> sets{m_blurDescriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
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
                                         vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                         vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                                         vk::ImageAspectFlagBits::eDepth);
}

} // namespace nim
