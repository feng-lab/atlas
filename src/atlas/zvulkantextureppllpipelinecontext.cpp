#include "zvulkantextureppllpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zlog.h"
#include "zvulkanbuffer.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"

#include <array>
#include <cstdint>
#include <vector>

namespace nim {

namespace {

constexpr uint32_t kOpaqueDepthPushDescriptorSet = 1u;
constexpr uint32_t kOpaqueDepthPushDescriptorBinding = 0u;

} // namespace

ZVulkanTexturePPLLPipelineContext::ZVulkanTexturePPLLPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTexturePPLLPipelineContext::~ZVulkanTexturePPLLPipelineContext() = default;

void ZVulkanTexturePPLLPipelineContext::resetFrame() {}

void ZVulkanTexturePPLLPipelineContext::record(Z3DRendererBase& renderer,
                                               const RenderBatch& batch,
                                               const TexturePPLLResolvePayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  (void)renderer;

  const vk::DescriptorSet oitSet = m_backend.sharedOITDescriptorSet();
  CHECK(oitSet) << "PPLL resolve missing shared OIT descriptor set";

  // Opaque depth (or a constant-1.0 placeholder) so the resolve shader can drop
  // any stored fragments that lie behind already-rendered opaque geometry.
  ZVulkanTexture& opaqueDepthTexture =
    payload.opaqueDepthAttachment.valid() && payload.opaqueDepthAttachment.backend == RenderBackend::Vulkan
      ? vulkan::textureFromHandle(payload.opaqueDepthAttachment, m_backend.device(), "PPLL opaque depth attachment")
      : m_backend.defaultPlaceholderTexture2D();
  CHECK_EQ(&opaqueDepthTexture.ownerDevice(), &m_backend.device());
  CHECK(opaqueDepthTexture.resident()) << "PPLL resolve opaque depth texture is not resident";
  CHECK(static_cast<bool>(opaqueDepthTexture.info().usage & vk::ImageUsageFlagBits::eSampled))
    << "PPLL resolve opaque depth texture is not sampleable";

  const bool usePushDescriptors = m_backend.device().context().supportsPushDescriptors();
  uint32_t opaqueDepthIdx = 0u;
  if (!usePushDescriptors) {
    opaqueDepthIdx = m_backend.bindlessLookupSampledImageAutoOrCrash(opaqueDepthTexture, "ppll_resolve opaque_depth");
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);
  CHECK_EQ(formats.colorFormats.size(), size_t{1}) << "PPLL resolve requires exactly one color attachment.";
  m_backend.validateFormatsOrCrash(formats, "PPLL_resolve");

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;
  PipelineInstance& pipeline = ensurePipeline(key, formats);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());

  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  cmd.bindVertexBuffers(0, {quad.buffer()}, {vk::DeviceSize(0)});

  if (!usePushDescriptors) {
    const std::array<vk::DescriptorSet, 1> sets0{m_backend.bindlessSampledImageDescriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets0, {});
  }

  // Bind set 3 (OIT SSBOs). Sets 1/2 are unused placeholders.
  {
    std::array<vk::DescriptorSet, 1> sets3{oitSet};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 3, sets3, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  if (usePushDescriptors) {
    vk::DescriptorImageInfo imageInfo = opaqueDepthTexture.descriptorInfo();
    CHECK(imageInfo.imageView) << "PPLL resolve push descriptor has no image view";
    CHECK(imageInfo.imageLayout == opaqueDepthTexture.layout())
      << fmt::format("PPLL resolve push descriptor layout {} does not match tracked texture layout {}",
                     enumOrUnderlying(imageInfo.imageLayout, 16),
                     enumOrUnderlying(opaqueDepthTexture.layout(), 16));
    imageInfo.sampler = vk::Sampler{};

    vk::WriteDescriptorSet write{};
    write.dstBinding = kOpaqueDepthPushDescriptorBinding;
    write.dstArrayElement = 0u;
    write.descriptorCount = 1u;
    write.descriptorType = vk::DescriptorType::eSampledImage;
    write.pImageInfo = &imageInfo;
    const std::array<vk::WriteDescriptorSet, 1> writes{write};
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,
                             pipeline.pipeline->pipelineLayout(),
                             kOpaqueDepthPushDescriptorSet,
                             writes);
  } else {
    PPLLResolvePushConstants constants;
    constants.opaqueDepthTexture = opaqueDepthIdx;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&constants);
    const vk::ArrayProxy<const std::uint8_t> payloadBytes(sizeof(constants), bytes);
    cmd.pushConstants(pipeline.pipeline->pipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, payloadBytes);
  }

  cmd.draw(4, 1, 0, 0);
}

vk::PipelineVertexInputStateCreateInfo ZVulkanTexturePPLLPipelineContext::makeVertexInputState() const
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

ZVulkanTexturePPLLPipelineContext::PipelineInstance&
ZVulkanTexturePPLLPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  const bool usePushDescriptors = device.context().supportsPushDescriptors();

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(
    device,
    ZVulkanShader::spirvResourcePath(QStringLiteral("pass.vert.spv")),
    ZVulkanShader::spirvResourcePath(usePushDescriptors ? QStringLiteral("ppll_resolve_push.frag.spv")
                                                        : QStringLiteral("ppll_resolve.frag.spv")),
    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);

  const vk::DescriptorSetLayout empty = m_backend.emptyDescriptorSetLayout();
  CHECK(empty) << "PPLL resolve missing empty descriptor set layout";
  const vk::DescriptorSetLayout oitLayout = m_backend.oitDescriptorSetLayout();
  CHECK(oitLayout) << "PPLL resolve missing OIT descriptor set layout";
  if (usePushDescriptors) {
    if (!m_opaqueDepthPushDescriptorSetLayout.has_value()) {
      const vk::DescriptorSetLayoutBinding binding{.binding = kOpaqueDepthPushDescriptorBinding,
                                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                                   .descriptorCount = 1u,
                                                   .stageFlags = vk::ShaderStageFlagBits::eFragment};
      const vk::DescriptorSetLayoutCreateInfo layoutInfo{
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1u,
        .pBindings = &binding,
      };
      m_opaqueDepthPushDescriptorSetLayout.emplace(device.context().device(), layoutInfo);
    }
    CHECK(m_opaqueDepthPushDescriptorSetLayout.has_value());
    const std::vector<vk::DescriptorSetLayout> layouts{empty, **m_opaqueDepthPushDescriptorSetLayout, empty, oitLayout};
    instance.pipeline->setDescriptorSetLayouts(layouts);
  } else {
    const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
    CHECK(bindlessLayout) << "PPLL resolve missing bindless descriptor set layout";
    const std::vector<vk::DescriptorSetLayout> layouts{bindlessLayout, empty, empty, oitLayout};
    instance.pipeline->setDescriptorSetLayouts(layouts);
  }
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  const bool hasDepth = formats.depthFormat.has_value();
  instance.pipeline->setDepthTestEnable(hasDepth);
  if (hasDepth) {
    // Resolve pass writes resolved depth. Only commit it when in front of (or equal to)
    // the loaded opaque depth buffer. This prevents pixels with no visible transparent
    // fragments from clobbering opaque depth with 1.0.
    instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);
    instance.pipeline->setDepthWriteEnable(true);
  } else {
    instance.pipeline->setDepthWriteEnable(false);
  }

  // Blend premultiplied transparent composite over the already-loaded background.
  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  blendAttachment.blendEnable = VK_TRUE;
  blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
  blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
  blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
  blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  if (!usePushDescriptors) {
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                      .offset = 0,
                                      .size = static_cast<uint32_t>(sizeof(PPLLResolvePushConstants))};
    instance.pipeline->setPushConstantRanges({range});
  }
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
