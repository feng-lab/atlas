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
  (void)payload;
  VLOG(2) << fmt::format("TextureCopy::record begin hook={} color=0x{:x} depth=0x{:x}",
                         static_cast<int>(renderer.shaderHookType()),
                         payload.colorAttachmentHandle.id,
                         payload.depthAttachmentHandle.id);

  CHECK(payload.colorAttachmentHandle.valid() && payload.depthAttachmentHandle.valid())
    << "Skipping Vulkan texture copy pass due to missing attachments";

  auto& colorTexture =
    vulkan::textureFromHandle(payload.colorAttachmentHandle, m_backend.device(), "texture-copy color attachment");
  auto& depthTexture =
    vulkan::textureFromHandle(payload.depthAttachmentHandle, m_backend.device(), "texture-copy depth attachment");

  VLOG(1) << fmt::format(
    "TextureCopy inputs: color=0x{:x} layout={} descr={} fmt={} | depth=0x{:x} layout={} descr={} fmt={}",
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
  // Optionally ensure persistent pool + descriptor set (across frames)
  if (m_enablePersistentScheduling) {
    if (!m_pool) {
      m_pool = m_backend.device().createDescriptorPool();
    }
    if (!m_persistentTexturesDS && m_setTextures && m_pool) {
      m_persistentTexturesDS = m_backend.device().createDescriptorSet(*m_pool, **m_setTextures, /*override*/ false);
    }
  }

  // Decide which descriptor set to bind (persistent vs. per-frame override)
  const bool ddpPeel = (renderer.shaderHookType() == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  uint64_t desiredColor = payload.colorAttachmentHandle.id;
  uint64_t desiredDepth = payload.depthAttachmentHandle.id;
  uint64_t desiredDdpDepth = 0;
  uint64_t desiredDdpFront = 0;
  if (ddpPeel) {
    const auto& hookPara = renderer.shaderHookPara();
    desiredDdpDepth = hookPara.dualDepthPeelingDepthBlenderHandle.id;
    desiredDdpFront = hookPara.dualDepthPeelingFrontBlenderHandle.id;
  }

  auto usePersistentNow = [&]() -> bool {
    if (!m_persistentTexturesDS) {
      return false;
    }
    if (!m_cachedTextures.valid) {
      return false;
    }
    if (m_cachedTextures.color != desiredColor || m_cachedTextures.depth != desiredDepth) {
      return false;
    }
    if (ddpPeel && (m_cachedTextures.ddpDepth != desiredDdpDepth || m_cachedTextures.ddpFront != desiredDdpFront)) {
      return false;
    }
    return true;
  }();

  ZVulkanDescriptorSet* ds = nullptr;
  const auto sampler = m_backend.nearestClampSampler();
  if (!m_backend.isRecording()) {
    // Safe to rewrite persistent set before recording begins
    if (m_persistentTexturesDS) {
      VLOG(2) << "TextureCopy: using persistent textures DS (pre-record rewrite)";
      m_persistentTexturesDS->updateTexture(0, colorTexture, sampler);
      m_persistentTexturesDS->updateTexture(1, depthTexture, sampler);
      if (ddpPeel) {
        const auto& hookPara = renderer.shaderHookPara();
        if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
          auto& tex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                m_backend.device(),
                                                "DDP depth blender for image peel");
          m_persistentTexturesDS->updateTexture(3, tex, sampler);
        }
        if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
          auto& tex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                m_backend.device(),
                                                "DDP front blender for image peel");
          m_persistentTexturesDS->updateTexture(4, tex, sampler);
        }
      }
      m_cachedTextures = {desiredColor, desiredDepth, desiredDdpDepth, desiredDdpFront, true};
      ds = m_persistentTexturesDS.get();
    }
  } else if (m_enablePersistentScheduling && usePersistentNow) {
    VLOG(2) << "TextureCopy: using persistent textures DS (cached)";
    ds = m_persistentTexturesDS.get();
  } else {
    // Allocate a fresh per-draw override descriptor set during recording to avoid
    // update-after-bind hazards when using a single command buffer for all passes.
    if (m_setTextures) {
      ds = m_backend.allocateOverrideDescriptorSet(**m_setTextures);
    }
    CHECK(ds != nullptr) << "Texture copy: override descriptor allocation failed (fatal)";
    const bool attachmentsChanged =
      (!m_cachedTextures.valid || m_cachedTextures.color != desiredColor || m_cachedTextures.depth != desiredDepth ||
       m_cachedTextures.ddpDepth != desiredDdpDepth || m_cachedTextures.ddpFront != desiredDdpFront);
    // Override descriptor sets are newly allocated per draw; every binding must be rewritten even
    // when the attachments repeat so we do not replay empty descriptors that would drop colour/depth.
    VLOG(2) << "TextureCopy: writing per-draw override descriptors";
    ds->updateTexture(0, colorTexture, sampler);
    ds->updateTexture(1, depthTexture, sampler);
    if (ddpPeel) {
      const auto& hookPara = renderer.shaderHookPara();
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& tex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                              m_backend.device(),
                                              "DDP depth blender for image peel");
        VLOG(2) << fmt::format("TextureCopy: writing DDP depth blender binding3 tex=0x{:x}",
                               hookPara.dualDepthPeelingDepthBlenderHandle.id);
        ds->updateTexture(3, tex, sampler);
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& tex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                              m_backend.device(),
                                              "DDP front blender for image peel");
        VLOG(2) << fmt::format("TextureCopy: writing DDP front blender binding4 tex=0x{:x}",
                               hookPara.dualDepthPeelingFrontBlenderHandle.id);
        ds->updateTexture(4, tex, sampler);
      }
    }
    m_cachedTextures = {desiredColor, desiredDepth, desiredDdpDepth, desiredDdpFront, true};
    // Optionally schedule a persistent rewrite for the next frame when it is safe
    if (m_enablePersistentScheduling && m_persistentTexturesDS && attachmentsChanged) {
      const uint64_t colorId = desiredColor;
      const uint64_t depthId = desiredDepth;
      const uint64_t ddpDepthId = desiredDdpDepth;
      const uint64_t ddpFrontId = desiredDdpFront;
      m_backend.scheduleAfterCurrentFrameCompletion([this, colorId, depthId, ddpDepthId, ddpFrontId]() {
        if (!m_persistentTexturesDS || !m_setTextures) {
          return;
        }
        try {
          auto& colorTex = vulkan::textureFromHandle(AttachmentHandle{colorId, 0u, AttachmentBackend::Vulkan},
                                                     m_backend.device(),
                                                     "texture-copy color (scheduled)");
          auto& depthTex = vulkan::textureFromHandle(AttachmentHandle{depthId, 0u, AttachmentBackend::Vulkan},
                                                     m_backend.device(),
                                                     "texture-copy depth (scheduled)");
          auto samplerLocal = m_backend.nearestClampSampler();
          m_persistentTexturesDS->updateTexture(0, colorTex, samplerLocal);
          m_persistentTexturesDS->updateTexture(1, depthTex, samplerLocal);
          if (ddpDepthId || ddpFrontId) {
            if (ddpDepthId) {
              auto& dep = vulkan::textureFromHandle(AttachmentHandle{ddpDepthId, 0u, AttachmentBackend::Vulkan},
                                                    m_backend.device(),
                                                    "DDP depth blender (scheduled)");
              m_persistentTexturesDS->updateTexture(3, dep, samplerLocal);
            }
            if (ddpFrontId) {
              auto& fr = vulkan::textureFromHandle(AttachmentHandle{ddpFrontId, 0u, AttachmentBackend::Vulkan},
                                                   m_backend.device(),
                                                   "DDP front blender (scheduled)");
              m_persistentTexturesDS->updateTexture(4, fr, samplerLocal);
            }
          }
          m_cachedTextures = {colorId, depthId, ddpDepthId, ddpFrontId, true};
          VLOG(2) << "TextureCopy: scheduled persistent DS rewrite completed";
        }
        catch (...) {
          VLOG(1) << "TextureCopy: scheduled persistent DS rewrite skipped (handles not resolvable)";
        }
      });
      VLOG(2) << "TextureCopy: scheduled persistent textures DS rewrite after frame";
    }
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.discardTransparent = payload.discardTransparent;
  key.mode = payload.mode;
  key.flipY = payload.flipY;
  key.waInit = (renderer.shaderHookType() == Z3DRendererBase::ShaderHookType::WeightedAverageInit);
  key.wbInit = (renderer.shaderHookType() == Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
  key.ddpInit = (renderer.shaderHookType() == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
  key.ddpPeel = (renderer.shaderHookType() == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);
  VLOG(2) << fmt::format("TextureCopy: ensured pipeline waInit={} wbInit={} ddpInit={} ddpPeel={} colors={} depth={}",
                         key.waInit,
                         key.wbInit,
                         key.ddpInit,
                         key.ddpPeel,
                         formats.colorFormats.size(),
                         formats.depthFormat.has_value());

  // Ensure and bind local quad VBO with positions + UVs
  ensureVertexCapacity(m_vertexCount);
  uploadGeometry();

  // Bind OIT UBO when needed for image OIT paths (for screen_dim_RCP and depth transforms)
  if (key.waInit || key.wbInit || key.ddpPeel) {
    // Ensure OIT descriptor/UBO are primed before recording; avoid descriptor writes while recording
    if (m_backend.isRecording()) {
      CHECK(m_descriptorSetOIT && m_uboOIT) << "Texture copy OIT resources not primed before recording";
    } else {
      ensureDescriptorLayout();
      ensureOITResources();
    }
    if (m_descriptorSetOIT && m_uboOIT) {
      // Update UBO contents only (host write is allowed during recording)
      OITParamsUBOStd140 oit{};
      glm::vec2 extent = batch.pass.viewport.extent;
      if (!(extent.x > 0.0f && extent.y > 0.0f)) {
        const auto& vp = renderer.frameState().viewport;
        extent = glm::vec2(static_cast<float>(vp.z), static_cast<float>(vp.w));
      }
      if (extent.x > 0.0f && extent.y > 0.0f) {
        oit.screen_dim_RCP = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
      }
      const float n = renderer.viewState().nearClip;
      const float f = renderer.viewState().farClip;
      const float denom = std::max(f - n, 1e-6f);
      oit.ze_to_zw_a = (f * n) / denom;
      oit.ze_to_zw_b = 0.5f * (f + n) / denom + 0.5f;
      oit.weighted_blended_depth_scale = renderer.sceneState().weightedBlendedDepthScale;
      m_uboOIT->copyData(&oit, sizeof(oit));
    } else {
      CHECK(false) << "Texture copy OIT resources not primed before recording";
    }
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
  if (key.waInit || key.wbInit || key.ddpPeel) {
    drawSpec.expectedDescriptorSetCount = 4;
    CHECK(m_descriptorSetOIT) << "Texture copy OIT descriptor set missing";
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = 3;
    oitBind.sets = {m_descriptorSetOIT->descriptorSet()};
    drawSpec.extraDescriptorBinds.push_back(std::move(oitBind));
  } else {
    drawSpec.expectedDescriptorSetCount = 1;
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);
  recorder.recordGraphicsDraw(drawSpec);
  VLOG(2) << fmt::format("TextureCopy: draw {} verts", m_vertexCount);
}

void ZVulkanTextureCopyPipelineContext::ensureDescriptorLayout()
{
  if (m_setTextures) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  // Immutable nearest-clamp sampler for exact texel copies without filtering
  vk::Sampler imm = m_backend.nearestClampSampler();
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
  // Empty placeholder layout (for sets 1 and 2 when OIT is used at set=3)
  if (!m_setPlaceholder) {
    vk::DescriptorSetLayoutCreateInfo emptyInfo{.bindingCount = 0, .pBindings = nullptr};
    m_setPlaceholder.emplace(vkDevice, emptyInfo);
  }
  // OIT params UBO layout (set=3 to match include/oit_params.glslinc)
  vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                         .descriptorType = vk::DescriptorType::eUniformBuffer,
                                         .descriptorCount = 1,
                                         .stageFlags = vk::ShaderStageFlagBits::eFragment};
  vk::DescriptorSetLayoutCreateInfo info{.bindingCount = 1, .pBindings = &binding};
  m_setOIT.emplace(vkDevice, info);
}

void ZVulkanTextureCopyPipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();
  // Textures set is persistent or override per-frame; avoid per-frame alloc here
  if (!m_descriptorSetOIT && m_setOIT) {
    m_descriptorSetOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }
}

void ZVulkanTextureCopyPipelineContext::ensureOITResources()
{
  ensureDescriptorLayout();
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_descriptorSetOIT && m_setOIT) {
    m_descriptorSetOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
  }
  if (m_descriptorSetOIT && m_uboOIT) {
    // One-time descriptor write; must happen before recording begins
    m_descriptorSetOIT->writeUniformBufferOnce(0, *m_uboOIT);
    VLOG(2) << "TextureCopy: primed OIT UBO descriptor (set=3, binding=0)";
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
  if (key.ddpInit) {
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
  if (key.waInit || key.wbInit || key.ddpPeel) {
    instance.pipeline->setDescriptorSetLayouts({**m_setTextures, **m_setPlaceholder, **m_setPlaceholder, **m_setOIT});
  } else {
    instance.pipeline->setDescriptorSetLayouts({**m_setTextures});
  }
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  const bool hasDepth = formats.depthFormat.has_value();
  instance.pipeline->setDepthTestEnable(hasDepth);
  instance.pipeline->setDepthWriteEnable(hasDepth);
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
