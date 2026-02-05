#include "zvulkantexturecopypipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dtexturecopyrenderer.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorset.h"
#include "zvulkandescriptorpool.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbindings.h"
#include "zvulkanbuffer.h"
#include "zvulkanuniforms.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zlog.h"

#include <algorithm>
#include <array>
#include <vector>

namespace nim {

ZVulkanTextureCopyPipelineContext::ZVulkanTextureCopyPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureCopyPipelineContext::~ZVulkanTextureCopyPipelineContext() = default;

void ZVulkanTextureCopyPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  resetDescriptors();
}

void ZVulkanTextureCopyPipelineContext::resetDescriptors()
{
  m_descriptorSetOIT.reset();
}

void ZVulkanTextureCopyPipelineContext::record(Z3DRendererBase& renderer,
                                               const RenderBatch& batch,
                                               const TextureCopyPayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  (void)payload;
  CHECK(batch.shaderHook.captured) << "Texture copy batch missing shader hook snapshot";
  VLOG(2) << fmt::format("record begin hook={} color=0x{:x} depth=0x{:x}",
                         static_cast<int>(batch.shaderHook.type),
                         payload.colorAttachmentHandle.id,
                         payload.depthAttachmentHandle.id);

  CHECK(payload.colorAttachmentHandle.valid() && payload.depthAttachmentHandle.valid())
    << "Skipping Vulkan texture copy pass due to missing attachments";

  auto& colorTexture =
    vulkan::textureFromHandle(payload.colorAttachmentHandle, m_backend.device(), "texture-copy color attachment");
  auto& depthTexture =
    vulkan::textureFromHandle(payload.depthAttachmentHandle, m_backend.device(), "texture-copy depth attachment");

  VLOG(1) << fmt::format("inputs: color=0x{:x} layout={} descr={} fmt={} | depth=0x{:x} layout={} descr={} fmt={}",
                         payload.colorAttachmentHandle.id,
                         enumOrUnderlying(colorTexture.layout(), 16),
                         enumOrUnderlying(colorTexture.descriptorLayout(), 16),
                         enumOrUnderlying(colorTexture.format(), 16),
                         payload.depthAttachmentHandle.id,
                         enumOrUnderlying(depthTexture.layout(), 16),
                         enumOrUnderlying(depthTexture.descriptorLayout(), 16),
                         enumOrUnderlying(depthTexture.format(), 16));

  // Fullscreen quad with UVs
  m_vertexCount = 4;

  ensureDescriptorLayout();
  ensureDescriptorSet();
  // Always use a per-draw override descriptor set to avoid update-after-bind hazards
  const bool ddpPeel = (batch.shaderHook.type == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  ZVulkanDescriptorSet* ds = nullptr;
  // Use linear filtering to downsample supersampled inputs (color and depth)
  const auto sampler = m_backend.defaultSampler();
  // Allocate a fresh per-draw override descriptor set and write all required bindings
  CHECK(m_setTextures) << "Texture copy: descriptor set layout not initialized";
  ds = m_backend.allocateOverrideDescriptorSet(**m_setTextures);
  CHECK(ds != nullptr) << "Texture copy: override descriptor allocation failed (fatal)";
  VLOG(2) << "writing per-draw override descriptors";
  ds->updateTexture(0, colorTexture, sampler);
  ds->updateTexture(1, depthTexture, sampler);
  if (ddpPeel) {
    const auto& hookPara = batch.shaderHook.para;
    if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
      auto& tex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                            m_backend.device(),
                                            "DDP depth blender for image peel");
      ds->updateTexture(3, tex, sampler);
    }
    if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
      auto& tex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                            m_backend.device(),
                                            "DDP front blender for image peel");
      ds->updateTexture(4, tex, sampler);
    }
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.discardTransparent = payload.discardTransparent;
  key.mode = payload.mode;
  key.flipY = payload.flipY;
  key.waInit = (batch.shaderHook.type == Z3DRendererBase::ShaderHookType::WeightedAverageInit);
  key.wbInit = (batch.shaderHook.type == Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
  key.ddpInit = (batch.shaderHook.type == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
  key.ddpPeel = (batch.shaderHook.type == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  key.ppllCount = (batch.shaderHook.type == Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount);
  key.ppllStore = (batch.shaderHook.type == Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore);
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);
  VLOG(2) << fmt::format("ensured pipeline waInit={} wbInit={} ddpInit={} ddpPeel={} colors={} depth={}",
                         key.waInit,
                         key.wbInit,
                         key.ddpInit,
                         key.ddpPeel,
                         formats.colorFormats.size(),
                         formats.depthFormat.has_value());

  // Ensure and bind local quad VBO with positions + UVs
  ensureVertexCapacity(m_vertexCount);
  uploadGeometry();

  const bool usesOITSet = key.ddpPeel || key.ppllCount || key.ppllStore;
  // Bind set=3 for OIT passes (DDP peel / PPLL count/store). Descriptors must be primed
  // before recording; do not write descriptors during dynamic rendering.
  if (usesOITSet && !m_backend.isRecording()) {
    ensureOITResources();
  }

  // Draw-only spec under backend-managed segment
  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = {viewport};
  drawSpec.scissors = {scissor};
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetInputs;
  drawSpec.descriptorSets = {ds->descriptorSet()};
  drawSpec.vertexBuffers = {m_vertexBuffer->buffer()};
  drawSpec.vertexOffsets = {vk::DeviceSize(0)};
  drawSpec.vertexCount = static_cast<uint32_t>(m_vertexCount);
  drawSpec.instanceCount = 1;
  drawSpec.expectedDescriptorSetCount = usesOITSet ? 4 : 1;
  if (usesOITSet && m_descriptorSetOIT) {
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = vkbind::kSetOITParams;
    oitBind.sets = {m_descriptorSetOIT->descriptorSet()};
    drawSpec.extraDescriptorBinds.push_back(std::move(oitBind));
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);
  recorder.recordGraphicsDraw(drawSpec);
  VLOG(2) << fmt::format("draw {} verts", m_vertexCount);
}

void ZVulkanTextureCopyPipelineContext::ensureDescriptorLayout()
{
  if (m_setTextures) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  // Immutable linear clamp sampler to match GL resolve filtering for color and depth
  vk::Sampler imm = m_backend.defaultSampler();
  std::array<vk::Sampler, 4> imms{imm, imm, imm, imm};
  std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
  const uint32_t map[4] = {0, 1, 3, 4};
  for (uint32_t i = 0; i < 4; ++i) {
    bindings[i].binding = map[i];
    bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
    bindings[i].pImmutableSamplers = &imms[i];
  }

  vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                               .pBindings = bindings.data()};
  m_setTextures.emplace(vkDevice, createInfo);
  // Set 3 is reserved for OIT SSBO bindings (DDP flag / PPLL buffers).
  if (!m_setOIT) {
    m_setOIT = m_backend.oitDescriptorSetLayout();
  }
}

void ZVulkanTextureCopyPipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();
  // Textures set is persistent or override per-frame; avoid per-frame alloc here
  if (!m_descriptorSetOIT && m_setOIT) {
    m_descriptorSetOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }
}

void ZVulkanTextureCopyPipelineContext::ensureOITResources()
{
  ensureDescriptorLayout();
  if (!m_descriptorSetOIT && m_setOIT) {
    m_descriptorSetOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }
  if (m_descriptorSetOIT && !m_backend.isRecording()) {
    m_backend.primeOITDescriptorSet(*m_descriptorSetOIT);
  }
}

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureCopyPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(QuadVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 2> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(QuadVertex, position))},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(QuadVertex, uv))      }
  };

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanTextureCopyPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t required = vertexCount * sizeof(QuadVertex);
  if (!m_vertexBuffer || m_vertexCapacity < required) {
    // Host-visible VB so we can update without staging inside dynamic rendering
    m_vertexBuffer = m_backend.device().createBuffer(required,
                                                     vk::BufferUsageFlagBits::eVertexBuffer,
                                                     vk::MemoryPropertyFlagBits::eHostVisible |
                                                       vk::MemoryPropertyFlagBits::eHostCoherent);
    m_vertexCapacity = required;
  }
}

void ZVulkanTextureCopyPipelineContext::uploadGeometry()
{
  if (!m_vertexBuffer) {
    return;
  }
  QuadVertex verts[4] = {
    {glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec2(0.0f, 0.0f)},
    {glm::vec3(1.0f,  -1.0f, 0.0f), glm::vec2(1.0f, 0.0f)},
    {glm::vec3(-1.0f, 1.0f,  0.0f), glm::vec2(0.0f, 1.0f)},
    {glm::vec3(1.0f,  1.0f,  0.0f), glm::vec2(1.0f, 1.0f)},
  };
  m_vertexBuffer->copyData(verts, sizeof(verts));
}

ZVulkanTextureCopyPipelineContext::PipelineInstance&
ZVulkanTextureCopyPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayout();

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  // Select fragment shader for image compositing modes
  std::string frag = shaderBase + std::string("copyimage.frag.spv");
  if (key.ppllCount) {
    frag = shaderBase + std::string("ppll_count_image.frag.spv");
  } else if (key.ppllStore) {
    frag = shaderBase + std::string("ppll_store_image.frag.spv");
  } else if (key.ddpInit) {
    frag = shaderBase + std::string("dual_peeling_init_image.frag.spv");
  } else if (key.ddpPeel) {
    frag = shaderBase + std::string("dual_peeling_peel_image.frag.spv");
  } else if (key.wbInit) {
    frag = shaderBase + std::string("wblended_init_image.frag.spv");
  } else if (key.waInit) {
    frag = shaderBase + std::string("wavg_init_image.frag.spv");
  }
  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + "pass_with_2dtexture.vert.spv", frag, std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  const bool usesOITSet = key.ddpPeel || key.ppllCount || key.ppllStore;
  if (usesOITSet) {
    std::vector<vk::DescriptorSetLayout> layouts;
    layouts.reserve(4);
    layouts.push_back(**m_setTextures);                                  // set 0
    layouts.push_back(m_backend.emptyDescriptorSetLayout());            // set 1 placeholder
    layouts.push_back(m_backend.emptyDescriptorSetLayout());            // set 2 placeholder
    layouts.push_back(m_backend.oitDescriptorSetLayout()); // set 3 (OIT SSBOs)
    instance.pipeline->setDescriptorSetLayouts(layouts);
  } else {
    instance.pipeline->setDescriptorSetLayouts({**m_setTextures});
  }
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  const bool hasDepth = formats.depthFormat.has_value();
  instance.pipeline->setDepthTestEnable(hasDepth);
  if (key.ppllCount || key.ppllStore || key.ddpInit || key.ddpPeel) {
    // OIT passes must not clobber the loaded opaque depth buffer.
    instance.pipeline->setDepthWriteEnable(false);
  } else {
    instance.pipeline->setDepthWriteEnable(hasDepth);
  }
  if (hasDepth) {
    instance.pipeline->setDepthCompareOp(vk::CompareOp::eLessOrEqual);
  }

  if (key.waInit) {
    // Accumulate into both attachments using additive blending (ONE, ONE)
    vk::PipelineColorBlendAttachmentState add{};
    add.blendEnable = true;
    add.srcColorBlendFactor = vk::BlendFactor::eOne;
    add.dstColorBlendFactor = vk::BlendFactor::eOne;
    add.colorBlendOp = vk::BlendOp::eAdd;
    add.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    add.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    add.alphaBlendOp = vk::BlendOp::eAdd;
    add.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    // Replicate to match number of color attachments
    std::vector<vk::PipelineColorBlendAttachmentState> atts;
    atts.assign(std::max<size_t>(1, formats.colorFormats.size()), add);
    instance.pipeline->setColorBlendAttachments(std::move(atts));
  } else if (key.wbInit) {
    // Attachment 0: additive accumulation (ONE, ONE)
    vk::PipelineColorBlendAttachmentState att0{};
    att0.blendEnable = true;
    att0.srcColorBlendFactor = vk::BlendFactor::eOne;
    att0.dstColorBlendFactor = vk::BlendFactor::eOne;
    att0.colorBlendOp = vk::BlendOp::eAdd;
    att0.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    att0.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    att0.alphaBlendOp = vk::BlendOp::eAdd;
    att0.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    // Attachment 1: multiply transmittance
    vk::PipelineColorBlendAttachmentState att1{};
    att1.blendEnable = true;
    att1.srcColorBlendFactor = vk::BlendFactor::eZero;
    att1.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
    att1.colorBlendOp = vk::BlendOp::eAdd;
    att1.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    att1.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    att1.alphaBlendOp = vk::BlendOp::eAdd;
    att1.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    std::vector<vk::PipelineColorBlendAttachmentState> atts;
    atts.push_back(att0);
    atts.push_back(att1);
    while (atts.size() < formats.colorFormats.size()) {
      atts.push_back(atts.back());
    }
    instance.pipeline->setColorBlendAttachments(std::move(atts));
  } else if (key.ddpInit || key.ddpPeel) {
    // Dual depth peeling relies on MAX blending for its depth blenders and
    // per-pass front/back accumulators (matches GL's glBlendEquation(GL_MAX)).
    vk::PipelineColorBlendAttachmentState maxBlend{};
    maxBlend.blendEnable = true;
    maxBlend.srcColorBlendFactor = vk::BlendFactor::eOne;
    maxBlend.dstColorBlendFactor = vk::BlendFactor::eOne;
    maxBlend.colorBlendOp = vk::BlendOp::eMax;
    maxBlend.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    maxBlend.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    maxBlend.alphaBlendOp = vk::BlendOp::eMax;
    maxBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    std::vector<vk::PipelineColorBlendAttachmentState> atts;
    atts.assign(std::max<size_t>(1, formats.colorFormats.size()), maxBlend);
    instance.pipeline->setColorBlendAttachments(std::move(atts));
  } else {
    vk::PipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = false;
    blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                     vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    instance.pipeline->setColorBlendAttachment(blendAttachment);
  }

  std::array<vk::SpecializationMapEntry, 4> entries{
    vk::SpecializationMapEntry{.constantID = 60, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 61, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 62, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 63, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };

  const bool discard = key.discardTransparent;
  const bool multiply = key.mode == TextureCopyPayload::OutputMode::MultiplyAlpha;
  const bool divide = key.mode == TextureCopyPayload::OutputMode::DivideByAlpha;

  std::array<uint32_t, 4> specData{static_cast<uint32_t>(discard),
                                   static_cast<uint32_t>(multiply),
                                   static_cast<uint32_t>(divide),
                                   static_cast<uint32_t>(key.flipY)};

  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                         reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

  // No push constants used in copy pipeline; UVs drive sampling
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
