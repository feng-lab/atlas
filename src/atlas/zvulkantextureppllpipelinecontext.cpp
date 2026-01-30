#include "zvulkantextureppllpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zlog.h"
#include "zsysteminfo.h"
#include "zvulkanbindings.h"
#include "zvulkanbuffer.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkandescriptorset.h"
#include "zvulkanpipeline.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanshader.h"

#include <array>

namespace nim {

ZVulkanTexturePPLLPipelineContext::ZVulkanTexturePPLLPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTexturePPLLPipelineContext::~ZVulkanTexturePPLLPipelineContext() = default;

void ZVulkanTexturePPLLPipelineContext::resetFrame()
{
  resetDescriptors();
}

void ZVulkanTexturePPLLPipelineContext::resetDescriptors()
{
  m_descriptorOIT.reset();
}

void ZVulkanTexturePPLLPipelineContext::record(Z3DRendererBase& renderer,
                                               const RenderBatch& batch,
                                               const TexturePPLLResolvePayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  (void)payload;

  ensureOITResources();
  if (!m_descriptorOIT) {
    return;
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  // PPLL resolve composites the accumulated transparent layer onto the active
  // surface; it must have exactly one color attachment.
  CHECK_EQ(formats.colorFormats.size(), size_t{1}) << "PPLL resolve requires exactly one color attachment.";
  m_backend.validateFormatsOrCrash(formats, "PPLL_resolve");

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;
  PipelineInstance& instance = ensurePipeline(key, formats);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, instance.pipeline->pipeline());
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  cmd.bindVertexBuffers(0, {quad.buffer()}, {vk::DeviceSize(0)});

  // Bind set 3 (OIT SSBOs). Sets 0..2 are unused by the resolve shader.
  std::array<vk::DescriptorSet, 1> sets3{m_descriptorOIT->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                         instance.pipeline->pipelineLayout(),
                         vkbind::kSetOITParams,
                         sets3,
                         {});

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);
  cmd.draw(4, 1, 0, 0);
}

void ZVulkanTexturePPLLPipelineContext::ensureDescriptorLayouts()
{
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setPlaceholder) {
    vk::DescriptorSetLayoutCreateInfo emptyInfo{.bindingCount = 0, .pBindings = nullptr};
    m_setPlaceholder.emplace(vkDevice, emptyInfo);
  }

  if (!m_setOIT) {
    m_setOIT = m_backend.oitDescriptorSetLayout();
  }
}

void ZVulkanTexturePPLLPipelineContext::ensureOITResources()
{
  ensureDescriptorLayouts();
  if (!m_descriptorOIT && m_setOIT) {
    m_descriptorOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }

  if (m_descriptorOIT && !m_backend.isRecording()) {
    m_backend.primeOITDescriptorSet(*m_descriptorOIT);
  }
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

  ensureDescriptorLayouts();

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "ppll_resolve.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);

  const vk::DescriptorSetLayout oitLayout = m_setOIT ? m_setOIT : **m_setPlaceholder;
  std::vector<vk::DescriptorSetLayout> layouts{**m_setPlaceholder, **m_setPlaceholder, **m_setPlaceholder, oitLayout};
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  if (formats.depthFormat.has_value()) {
    // Resolve pass writes resolved depth. Use ALWAYS to ensure depth writes.
    instance.pipeline->setDepthTestEnable(true);
    instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
    instance.pipeline->setDepthWriteEnable(true);
  } else {
    instance.pipeline->setDepthTestEnable(false);
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

  instance.pipeline->setPushConstantRanges({});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
