#include "zvulkantextureweightedaveragepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanbindings.h"
#include "zvulkanbuffer.h"
#include "zvulkanuniforms.h"
#include "zlog.h"

#include <array>
#include <vector>

namespace nim {

namespace {

constexpr float kQuadDepth = 0.0f;

} // namespace

ZVulkanTextureWeightedAveragePipelineContext::ZVulkanTextureWeightedAveragePipelineContext(
  Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureWeightedAveragePipelineContext::~ZVulkanTextureWeightedAveragePipelineContext() = default;

void ZVulkanTextureWeightedAveragePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  resetDescriptors();
}

void ZVulkanTextureWeightedAveragePipelineContext::resetDescriptors()
{
  m_descriptorSet.reset();
  
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

  // Shared fullscreen quad
  m_vertexCount = 4;

  ensureDescriptorLayout();
  ensureDescriptorSet();
  if (!m_descriptorSet) {
    return;
  }

  auto& accumulationTexture = vulkan::textureFromHandle(payload.accumulationAttachment,
                                                        m_backend.device(),
                                                        "texture-weighted-average accumulation attachment");
  auto& momentsTexture = vulkan::textureFromHandle(payload.momentsAttachment,
                                                   m_backend.device(),
                                                   "texture-weighted-average moments attachment");

  // Allocate a fresh per-draw override descriptor set to avoid update-after-bind hazards
  ZVulkanDescriptorSet* ds = nullptr;
  if (m_setLayout) {
    ds = m_backend.allocateOverrideDescriptorSet(**m_setLayout);
  }
  CHECK(ds != nullptr) << "override descriptor allocation failed (fatal)";
  // Override sets are transient; every allocation hands us a fresh VkDescriptorSet with
  // undefined bindings, so prime both bindings before issuing the draw.
  ds->updateTexture(vkbind::kBindingWAAccum, accumulationTexture, m_backend.defaultSampler());
  ds->updateTexture(vkbind::kBindingWAMoments, momentsTexture, m_backend.defaultSampler());

  const auto formats = vulkan::extractAttachmentFormats(batch);

  // Composite resolve invariant: single color attachment; depth optional
  CHECK_EQ(formats.colorFormats.size(), size_t{1}) << "WA resolve requires exactly one color attachment.";
  m_backend.validateFormatsOrCrash(formats, "WA_resolve");

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& instance = ensurePipeline(key, formats);
  VLOG(2) << fmt::format("ensured pipeline colors={} depth={}",
                         formats.colorFormats.size(),
                         formats.depthFormat.has_value());

  // Draw-only; backend manages attachments and render area

  // No OIT UBO needed
  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = instance.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = instance.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetInputs;

  const std::array<vk::DescriptorSet, 1> descriptorSets{ds->descriptorSet()};
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.expectedDescriptorSetCount = 1;
  auto& quad = m_backend.fullscreenQuadVertexBuffer();

  const std::array<vk::Buffer, 1> vertexBuffers{quad.buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{vk::DeviceSize(0)};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = static_cast<uint32_t>(m_vertexCount);
  drawSpec.instanceCount = 1;
  drawSpec.pushConstantsData = nullptr;
  drawSpec.pushConstantsSize = 0;
  drawSpec.pushConstantsStages = {};
  drawSpec.requirePushConstants = false;

  VLOG(2) << fmt::format("draw-only: {} verts", m_vertexCount);
  ZVulkanPipelineCommandRecorder recorder(cmd);
  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureDescriptorLayout()
{
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setLayout) {
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    // Immutable samplers for resolve inputs to avoid MSL sampler class issues
    vk::Sampler immutable = m_backend.defaultSampler();
    bindings[0].pImmutableSamplers = &immutable;
    bindings[1].pImmutableSamplers = &immutable;

    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setLayout.emplace(vkDevice, createInfo);
  }

  if (!m_setPlaceholder) {
    m_setPlaceholder = m_backend.emptyDescriptorSetLayout();
  }

  
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();

  if (!m_descriptorSet) {
    m_descriptorSet = m_backend.allocateFrameDescriptorSet(**m_setLayout);
  }
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

void ZVulkanTextureWeightedAveragePipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(glm::vec3);
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

void ZVulkanTextureWeightedAveragePipelineContext::uploadGeometry()
{
  const std::array<glm::vec3, 4> vertices{glm::vec3(-1.f, 1.f, kQuadDepth),
                                          glm::vec3(-1.f, -1.f, kQuadDepth),
                                          glm::vec3(1.f, 1.f, kQuadDepth),
                                          glm::vec3(1.f, -1.f, kQuadDepth)};

  m_vertexCount = vertices.size();
}

ZVulkanTextureWeightedAveragePipelineContext::PipelineInstance&
ZVulkanTextureWeightedAveragePipelineContext::ensurePipeline(const PipelineKey& key,
                                                             const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayout();

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "wavg_final.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  // Sets: 0 = images
  instance.pipeline->setDescriptorSetLayouts({**m_setLayout});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  // Resolve pass writes a representative depth (mean depth from moments) via
  // gl_FragDepth in the fragment shader. To guarantee the fragment shader runs
  // across the full-screen quad even when existing depth contains nearer values
  // (e.g., opaque slices drawn earlier), use Always for the depth compare and
  // allow the shader to determine the final per-pixel depth. This mirrors the
  // GL path and avoids early-depth culling of the quad when the VBO places it
  // at the far plane.
  // Keep depth test enabled and force Always compare so late depth testing
  // does not cull fragments before the shader writes gl_FragDepth. Depth
  // writes remain enabled so the resolved representative depth lands in the
  // target.
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

  // No push constants needed for resolve shaders
  instance.pipeline->setPushConstantRanges({});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
