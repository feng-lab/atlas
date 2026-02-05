#include "zvulkantexturedualpeelpipelinecontext.h"

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

ZVulkanTextureDualPeelPipelineContext::ZVulkanTextureDualPeelPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureDualPeelPipelineContext::~ZVulkanTextureDualPeelPipelineContext() = default;

void ZVulkanTextureDualPeelPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  resetDescriptors();
}

void ZVulkanTextureDualPeelPipelineContext::resetDescriptors()
{
  m_blendDescriptor.reset();
  m_finalDescriptor.reset();
  m_descriptorOIT.reset();
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

  ensureDescriptorLayouts();
  ZVulkanDescriptorSet* descriptor = nullptr;
  // Allocate a fresh per-draw override set for each stage to avoid update-after-bind hazards
  if (stage == Stage::Carry && m_carrySetLayout) {
    descriptor = m_backend.allocateOverrideDescriptorSet(**m_carrySetLayout);
  } else if (stage == Stage::Blend && m_blendSetLayout) {
    descriptor = m_backend.allocateOverrideDescriptorSet(**m_blendSetLayout);
  } else if (stage == Stage::Final && m_finalSetLayout) {
    descriptor = m_backend.allocateOverrideDescriptorSet(**m_finalSetLayout);
  }
  CHECK(descriptor != nullptr) << "DDP texture stage: override descriptor allocation failed (fatal)";

  if (stage == Stage::Carry) {
    auto& prevDepthTexture =
      vulkan::textureFromHandle(payload.depthAttachment, m_backend.device(), "dual-peel carry prev depth blender");
    auto& prevFrontTexture =
      vulkan::textureFromHandle(payload.frontAttachment, m_backend.device(), "dual-peel carry prev front blender");
    descriptor->updateTexture(vkbind::kBindingDDPDepthBlender, prevDepthTexture, m_backend.defaultSampler());
    descriptor->updateTexture(vkbind::kBindingDDPFrontBlender, prevFrontTexture, m_backend.defaultSampler());
  } else if (stage == Stage::Blend) {
    CHECK(payload.tempAttachment.valid()) << "Skipping Vulkan dual-peel blend stage because temp attachment is missing";
    auto& tempTexture =
      vulkan::textureFromHandle(payload.tempAttachment, m_backend.device(), "dual-peel blend attachment");
    VLOG(2) << fmt::format("DDP blend: updating binding0 temp=0x{:x}", payload.tempAttachment.id);
    descriptor->updateTexture(0, tempTexture, m_backend.defaultSampler());
  } else {
    auto& depthTexture =
      vulkan::textureFromHandle(payload.depthAttachment, m_backend.device(), "dual-peel depth attachment");
    auto& frontTexture =
      vulkan::textureFromHandle(payload.frontAttachment, m_backend.device(), "dual-peel front attachment");
    auto& backTexture =
      vulkan::textureFromHandle(payload.backAttachment, m_backend.device(), "dual-peel back attachment");
    VLOG(2) << fmt::format("DDP final: updating depth/front/back bindings depth=0x{:x} front=0x{:x} back=0x{:x}",
                           payload.depthAttachment.id,
                           payload.frontAttachment.id,
                           payload.backAttachment.id);
    descriptor->updateTexture(vkbind::kBindingDDPFinalDepth, depthTexture, m_backend.defaultSampler());
    descriptor->updateTexture(vkbind::kBindingDDPFinalFront, frontTexture, m_backend.defaultSampler());
    descriptor->updateTexture(vkbind::kBindingDDPFinalBack, backTexture, m_backend.defaultSampler());
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

  if (descriptor) {
    std::array<vk::DescriptorSet, 1> sets{descriptor->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           instance.pipeline->pipelineLayout(),
                           vkbind::kSetInputs,
                           sets,
                           {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  // No push constants needed

  // Ensure and bind set 3 (DDP flag); descriptor is set at allocation time.
  VLOG(2) << "DDP: ensureOITResources + update UBO";
  ensureOITResources();
  
  if (m_descriptorOIT) {
    std::array<vk::DescriptorSet, 1> sets3{m_descriptorOIT->descriptorSet()};
    VLOG(2) << "DDP: binding set 3 (DDP flag)";
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                           instance.pipeline->pipelineLayout(),
                           vkbind::kSetOITParams,
                           sets3,
                           {});
  }

  VLOG(2) << fmt::format("DDP: draw {} verts", m_vertexCount);
  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanTextureDualPeelPipelineContext::ensureDescriptorLayouts()
{
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_carrySetLayout) {
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingDDPDepthBlender,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingDDPFrontBlender,
                                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::Sampler immutable = m_backend.defaultSampler();
    bindings[0].pImmutableSamplers = &immutable;
    bindings[1].pImmutableSamplers = &immutable;
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_carrySetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_blendSetLayout) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    // Immutable sampler for blend input
    vk::Sampler immutable = m_backend.defaultSampler();
    binding.pImmutableSamplers = &immutable;
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_blendSetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_finalSetLayout) {
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    // Immutable samplers for final inputs
    vk::Sampler immutable = m_backend.defaultSampler();
    bindings[0].pImmutableSamplers = &immutable;
    bindings[1].pImmutableSamplers = &immutable;
    bindings[2].pImmutableSamplers = &immutable;
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_finalSetLayout.emplace(vkDevice, createInfo);
  }

  if (!m_setPlaceholder) {
    vk::DescriptorSetLayoutCreateInfo emptyInfo{.bindingCount = 0, .pBindings = nullptr};
    m_setPlaceholder.emplace(vkDevice, emptyInfo);
  }

  if (!m_setOIT) {
    m_setOIT = m_backend.oitDescriptorSetLayout();
  }
}

ZVulkanDescriptorSet* ZVulkanTextureDualPeelPipelineContext::ensureDescriptor(Stage stage)
{
  ensureDescriptorLayouts();

  if (stage == Stage::Blend) {
    if (!m_blendDescriptor) {
      m_blendDescriptor = m_backend.allocateFrameDescriptorSet(**m_blendSetLayout);
    }
    return m_blendDescriptor.get();
  }

  if (!m_finalDescriptor) {
    m_finalDescriptor = m_backend.allocateFrameDescriptorSet(**m_finalSetLayout);
  }
  return m_finalDescriptor.get();
}

void ZVulkanTextureDualPeelPipelineContext::ensureOITResources()
{
  ensureDescriptorLayouts();
  if (!m_descriptorOIT && m_setOIT) {
    m_descriptorOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }
  CHECK(m_descriptorOIT != nullptr) << "DDP: failed to allocate descriptor set";

  if (m_descriptorOIT && !m_backend.isRecording()) {
    if (auto* buf = m_backend.ddpChangedFlagBufferObj()) {
      m_descriptorOIT->writeStorageBufferOnce(vkbind::kBindingOITDDPFlag, *buf);
    }
  }
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

void ZVulkanTextureDualPeelPipelineContext::ensureVertexCapacity(size_t vertexCount)
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

void ZVulkanTextureDualPeelPipelineContext::uploadGeometry()
{
  const std::array<glm::vec3, 4> vertices{glm::vec3(-1.f, 1.f, kQuadDepth),
                                          glm::vec3(-1.f, -1.f, kQuadDepth),
                                          glm::vec3(1.f, 1.f, kQuadDepth),
                                          glm::vec3(1.f, -1.f, kQuadDepth)};

  ensureVertexCapacity(vertices.size());
  if (!m_vertexBuffer) {
    return;
  }

  m_vertexBuffer->copyData(vertices.data(), vertices.size() * sizeof(glm::vec3));
  m_vertexCount = vertices.size();
}

ZVulkanTextureDualPeelPipelineContext::PipelineInstance&
ZVulkanTextureDualPeelPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayouts();

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.stage = key.stage;

  const char* fragmentShader = "dual_peeling_blend.frag.spv";
  if (key.stage == Stage::Final) {
    fragmentShader = "dual_peeling_final.frag.spv";
  } else if (key.stage == Stage::Carry) {
    fragmentShader = "dual_peeling_carry.frag.spv";
  }

  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + "pass.vert.spv", shaderBase + fragmentShader, std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);

  const vk::DescriptorSetLayout oitLayout = m_setOIT ? m_setOIT : **m_setPlaceholder;
  if (key.stage == Stage::Final) {
    std::vector<vk::DescriptorSetLayout> layouts{**m_finalSetLayout, **m_setPlaceholder, **m_setPlaceholder, oitLayout};
    instance.pipeline->setDescriptorSetLayouts(layouts);
  } else if (key.stage == Stage::Carry) {
    std::vector<vk::DescriptorSetLayout> layouts{**m_carrySetLayout, **m_setPlaceholder, **m_setPlaceholder, oitLayout};
    instance.pipeline->setDescriptorSetLayouts(layouts);
  } else {
    std::vector<vk::DescriptorSetLayout> layouts{**m_blendSetLayout, **m_setPlaceholder, **m_setPlaceholder, oitLayout};
    instance.pipeline->setDescriptorSetLayouts(layouts);
  }
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

  // No push constants
  instance.pipeline->setPushConstantRanges({});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
