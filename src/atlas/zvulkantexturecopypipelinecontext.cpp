#include "zvulkantexturecopypipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dtexturecopyrenderer.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
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
}

void ZVulkanTextureCopyPipelineContext::record(Z3DRendererBase& renderer,
                                               const RenderBatch& batch,
                                               const TextureCopyPayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  CHECK(batch.shaderHook.captured) << "Texture copy batch missing shader hook snapshot";
  CHECK(payload.copyDepth || batch.shaderHook.type == Z3DRendererBase::ShaderHookType::Normal)
    << "Depth-disabled texture copy is only valid for normal color copy passes";
  VLOG(2) << fmt::format("record begin hook={} color=0x{:x} depth=0x{:x}",
                         static_cast<int>(batch.shaderHook.type),
                         payload.colorAttachmentHandle.id,
                         payload.depthAttachmentHandle.id);

  CHECK(payload.colorAttachmentHandle.valid()) << "Skipping Vulkan texture copy pass due to missing color attachment";
  CHECK(!payload.copyDepth || payload.depthAttachmentHandle.valid())
    << "Skipping Vulkan texture copy pass due to missing depth attachment";

  auto& colorTexture =
    vulkan::textureFromHandle(payload.colorAttachmentHandle, m_backend.device(), "texture-copy color attachment");
  ZVulkanTexture* depthTexture = nullptr;
  if (payload.copyDepth) {
    depthTexture =
      &vulkan::textureFromHandle(payload.depthAttachmentHandle, m_backend.device(), "texture-copy depth attachment");
  }

  if (payload.copyDepth) {
    VLOG(1) << fmt::format("inputs: color=0x{:x} layout={} descr={} fmt={} | depth=0x{:x} layout={} descr={} fmt={}",
                           payload.colorAttachmentHandle.id,
                           enumOrUnderlying(colorTexture.layout(), 16),
                           enumOrUnderlying(colorTexture.descriptorLayout(), 16),
                           enumOrUnderlying(colorTexture.format(), 16),
                           payload.depthAttachmentHandle.id,
                           enumOrUnderlying(depthTexture->layout(), 16),
                           enumOrUnderlying(depthTexture->descriptorLayout(), 16),
                           enumOrUnderlying(depthTexture->format(), 16));
  } else {
    VLOG(1) << fmt::format("inputs: color=0x{:x} layout={} descr={} fmt={} | depth copy disabled",
                           payload.colorAttachmentHandle.id,
                           enumOrUnderlying(colorTexture.layout(), 16),
                           enumOrUnderlying(colorTexture.descriptorLayout(), 16),
                           enumOrUnderlying(colorTexture.format(), 16));
  }

  // Fullscreen quad with UVs
  m_vertexCount = 4;
  const bool ddpPeel = (batch.shaderHook.type == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  CopyImagePushConstants copyPC{};
  copyPC.colorTex = m_backend.bindlessLookupSampledImageAutoOrCrash(colorTexture, "texture_copy color");
  if (payload.copyDepth) {
    copyPC.depthTex = m_backend.bindlessLookupSampledImageAutoOrCrash(*depthTexture, "texture_copy depth");
  }
  if (ddpPeel) {
    const auto& hookPara = batch.shaderHook.para;
    if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
      auto& tex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                            m_backend.device(),
                                            "DDP depth blender for image peel");
      copyPC.ddpDepthBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(tex, "ddp_depth_blender");
    }
    if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
      auto& tex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                            m_backend.device(),
                                            "DDP front blender for image peel");
      copyPC.ddpFrontBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(tex, "ddp_front_blender");
    }
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.discardTransparent = payload.discardTransparent;
  key.mode = payload.mode;
  key.flipY = payload.flipY;
  key.copyDepth = payload.copyDepth;
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
  const bool usesLightingSet = key.wbInit;
  // Descriptor sets are primed by the backend in beginRender(); avoid record-time rewrites.
  const vk::DescriptorSet dsLighting = usesLightingSet ? m_backend.sharedLightingDescriptorSet() : vk::DescriptorSet{};
  CHECK(!usesLightingSet || dsLighting) << "WB init: shared lighting descriptor set missing";

  // Draw-only spec under backend-managed segment
  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = 0;

  std::array<vk::DescriptorSet, 2> descriptorSets{};
  uint32_t descriptorSetCount = 0;
  std::array<uint32_t, 1> dynamicOffsets{};
  uint32_t dynamicOffsetCount = 0;
  if (usesLightingSet) {
    descriptorSets[0] = m_backend.bindlessSampledImageDescriptorSet();
    descriptorSets[1] = dsLighting;
    descriptorSetCount = 2;
    dynamicOffsets[0] = static_cast<uint32_t>(m_backend.frameSharedLightingOffset());
    dynamicOffsetCount = 1;
  } else {
    descriptorSets[0] = m_backend.bindlessSampledImageDescriptorSet();
    descriptorSetCount = 1;
  }
  drawSpec.descriptorSets = std::span<const vk::DescriptorSet>(descriptorSets.data(), descriptorSetCount);
  drawSpec.dynamicOffsets = std::span<const uint32_t>(dynamicOffsets.data(), dynamicOffsetCount);
  const std::array<vk::Buffer, 1> vertexBuffers{m_vertexBuffer->buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{vk::DeviceSize(0)};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = static_cast<uint32_t>(m_vertexCount);
  drawSpec.instanceCount = 1;
  drawSpec.pushConstantsData = &copyPC;
  drawSpec.pushConstantsSize = sizeof(copyPC);
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;
  drawSpec.expectedDescriptorSetCount = usesOITSet ? 4u : (usesLightingSet ? 2u : 1u);
  std::array<vk::DescriptorSet, 1> oitDescriptorSets{};
  std::array<ZVulkanDescriptorBindInfo, 1> extraBinds{};
  uint32_t extraBindCount = 0;
  if (usesOITSet) {
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = vkbind::kSetOIT;
    oitDescriptorSets[0] = m_backend.sharedOITDescriptorSet();
    oitBind.sets = oitDescriptorSets;
    extraBinds[0] = oitBind;
    extraBindCount = 1;
  }
  drawSpec.extraDescriptorBinds = std::span<const ZVulkanDescriptorBindInfo>(extraBinds.data(), extraBindCount);

  ZVulkanPipelineCommandRecorder recorder(cmd);
  recorder.recordGraphicsDraw(drawSpec);
  VLOG(2) << fmt::format("draw {} verts", m_vertexCount);
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

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  // Select fragment shader for image compositing modes
  const bool usesOitShader = key.ddpPeel || key.ddpInit || key.ppllCount || key.ppllStore || key.waInit || key.wbInit;
  CHECK(key.copyDepth || !usesOitShader) << "Depth-disabled texture copy is only valid for normal color copies";
  CHECK(key.copyDepth || !formats.depthFormat.has_value())
    << "Depth-disabled texture copy must not bind a depth attachment";

  std::string frag = shaderBase + std::string(key.copyDepth ? "copyimage.frag.spv" : "copyimage_color.frag.spv");
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
  const bool usesLightingSet = key.wbInit;
  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  if (usesOITSet) {
    std::vector<vk::DescriptorSetLayout> layouts;
    layouts.reserve(4);
    layouts.push_back(bindlessLayout); // set 0 (bindless sampled images)
    layouts.push_back(m_backend.emptyDescriptorSetLayout());            // set 1 placeholder
    layouts.push_back(m_backend.emptyDescriptorSetLayout());            // set 2 placeholder
    layouts.push_back(m_backend.oitDescriptorSetLayout()); // set 3 (OIT SSBOs)
    instance.pipeline->setDescriptorSetLayouts(layouts);
  } else if (usesLightingSet) {
    const vk::DescriptorSetLayout lightingLayout = m_backend.lightingDescriptorSetLayout();
    CHECK(lightingLayout) << "WB init requires lighting descriptor set layout";
    // Sets: 0 = bindless sampled images, 1 = lighting UBO (depth transform)
    instance.pipeline->setDescriptorSetLayouts({bindlessLayout, lightingLayout});
  } else {
    instance.pipeline->setDescriptorSetLayouts({bindlessLayout});
  }
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  const bool hasDepth = formats.depthFormat.has_value();
  instance.pipeline->setDepthTestEnable(hasDepth);
  if (key.ppllCount || key.ppllStore || key.ddpInit || key.ddpPeel || key.waInit || key.wbInit) {
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

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(CopyImagePushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
