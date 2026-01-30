#include "zvulkanellipsoidpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3drendererstates.h"
#include "z3dellipsoidrenderer.h"
#include "zvulkanuniforms.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkandescriptorset.h"
#include "zvulkanbuffer.h"
#include "zsysteminfo.h"
#include "zlog.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanbindings.h"
#include "zexception.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

namespace nim {
namespace {

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

struct EllipsoidPushConstants
{
  float weighted_a = 0.0f;
  float weighted_b = 0.0f;
  float weighted_depth_scale = 0.0f;
  float _pad = 0.0f;
};

} // namespace

ZVulkanEllipsoidPipelineContext::ZVulkanEllipsoidPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanEllipsoidPipelineContext::~ZVulkanEllipsoidPipelineContext() = default;

void ZVulkanEllipsoidPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_axis1Buffer = VK_NULL_HANDLE;
  m_axis2Buffer = VK_NULL_HANDLE;
  m_axis3Buffer = VK_NULL_HANDLE;
  m_centerBuffer = VK_NULL_HANDLE;
  m_colorBuffer = VK_NULL_HANDLE;
  m_flagsBuffer = VK_NULL_HANDLE;
  m_specularBuffer = VK_NULL_HANDLE;
  m_axis1Offset = 0;
  m_axis2Offset = 0;
  m_axis3Offset = 0;
  m_centerOffset = 0;
  m_colorOffset = 0;
  m_flagsOffset = 0;
  m_specularOffset = 0;
  m_indexUploadBuffer = VK_NULL_HANDLE;
  m_indexUploadOffset = 0;
  resetDescriptors();
  m_ddpArgsPrepared = false;
  m_ddpArgsOffset = 0;
  m_ddpTransformsFrozen = false;
  m_ddpMaterialFrozen = false;
}

void ZVulkanEllipsoidPipelineContext::resetDescriptors()
{
  m_dsPlaceholder.reset();
  m_dsLighting.reset();
  m_dsTransforms.reset();
  m_dsOIT.reset();
}

void ZVulkanEllipsoidPipelineContext::record(Z3DRendererBase& renderer,
                                             const RenderBatch& batch,
                                             const EllipsoidPayload& payload,
                                             const vk::Viewport& viewport,
                                             const vk::Rect2D& scissor,
                                             vk::raii::CommandBuffer& cmd)
{
  if (payload.centers.empty()) {
    return;
  }

  // GL parity: in picking pass, skip if per-ellipsoid picking colors are missing/mismatched.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.centers.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  if (m_vertexCount == 0) {
    return;
  }

  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = renderer.shaderHookType();

  m_dynLightingOffset = m_backend.frameSharedLightingOffset();
  updateTransformUBO(renderer, batch, payload, pickingPass);
  // Descriptor sets are primed in beginRender(); avoid record-time rewrites.
  CHECK(m_dsLighting && m_dsTransforms) << "Ellipsoid pipeline descriptor sets missing (lighting/transforms)";

  ZVulkanDescriptorSet* dsPlaceholderOverride = nullptr;
  ensurePlaceholderTexture();
  const auto& hookPara = renderer.shaderHookPara();
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel && m_setPlaceholder) {
    dsPlaceholderOverride = m_backend.allocateOverrideDescriptorSet(m_setPlaceholder);
    CHECK(dsPlaceholderOverride != nullptr) << "Ellipsoid DDP peel: override descriptor allocation failed (fatal)";
    if (dsPlaceholderOverride) {
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                   m_backend.device(),
                                                   "ellipsoid dual-depth-peeling depth blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, depthTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, tex, m_backend.defaultSampler());
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                   m_backend.device(),
                                                   "ellipsoid dual-depth-peeling front blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, frontTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, tex, m_backend.defaultSampler());
      }
    }
  }

  const vulkan::AttachmentFormats formats = vulkan::extractAttachmentFormats(batch);

  m_backend.validateFormatsOrCrash(formats, "ellipsoid");

  PipelineKey key;
  key.dynamicMaterial = payload.useDynamicMaterial && !pickingPass;
  key.fogMode = renderer.sceneState().fog.mode;
  key.shaderHookType = shaderHook;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  // Draw-only path: backend manages render area/attachments

  EllipsoidPushConstants constants{};
  if (shaderHook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit) {
    const float n = renderer.viewState().nearClip;
    const float f = renderer.viewState().farClip;
    const float denom = std::max(f - n, 1e-6f);
    constants.weighted_a = (f * n) / denom;
    constants.weighted_b = 0.5f * (f + n) / denom + 0.5f;
    constants.weighted_depth_scale = renderer.sceneState().weightedBlendedDepthScale;
  }

  // Build draw-only spec
  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = {viewport};
  drawSpec.scissors = {scissor};
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetInputs;

  CHECK((dsPlaceholderOverride != nullptr) || (m_dsPlaceholder != nullptr))
    << "Ellipsoid pipeline placeholder descriptor set not initialised";
  std::vector<vk::DescriptorSet> sets;
  const vk::DescriptorSet ds0 =
    dsPlaceholderOverride ? dsPlaceholderOverride->descriptorSet() : m_dsPlaceholder->descriptorSet();
  sets.push_back(ds0);
  sets.push_back(m_dsLighting->descriptorSet());
  sets.push_back(m_dsTransforms->descriptorSet());
  drawSpec.descriptorSets = sets;
  drawSpec.dynamicOffsets = {static_cast<uint32_t>(m_dynLightingOffset),
                             static_cast<uint32_t>(m_dynTransformsOffset),
                             static_cast<uint32_t>(m_dynMaterialOffset)};

  uint32_t expectedSets = static_cast<uint32_t>(sets.size());
  if (m_dsOIT) {
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = vkbind::kSetOITParams;
    oitBind.sets = {m_dsOIT->descriptorSet()};
    drawSpec.extraDescriptorBinds.push_back(std::move(oitBind));
    expectedSets = std::max(expectedSets, vkbind::kSetOITParams + 1);
  }
  drawSpec.expectedDescriptorSetCount = expectedSets;

  drawSpec.vertexBuffers =
    {m_axis1Buffer, m_axis2Buffer, m_axis3Buffer, m_centerBuffer, m_colorBuffer, m_flagsBuffer, m_specularBuffer};
  drawSpec.vertexOffsets =
    {m_axis1Offset, m_axis2Offset, m_axis3Offset, m_centerOffset, m_colorOffset, m_flagsOffset, m_specularOffset};
  if (m_indexCount > 0 && m_indexUploadBuffer) {
    drawSpec.indexBuffer = m_indexUploadBuffer;
    drawSpec.indexOffset = m_indexUploadOffset;
    drawSpec.indexType = vk::IndexType::eUint32;
    drawSpec.indexCount = static_cast<uint32_t>(m_indexCount);
  } else {
    drawSpec.vertexCount = static_cast<uint32_t>(m_vertexCount);
  }
  drawSpec.instanceCount = 1;

  drawSpec.pushConstantsData = &constants;
  drawSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(EllipsoidPushConstants));
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;

  ZVulkanPipelineCommandRecorder recorder(cmd);
  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    const bool indexed = (drawSpec.indexCount > 0);
    if (static_cast<VkBuffer>(m_backend.ddpDeviceArgsBuffer()) != VK_NULL_HANDLE) {
      m_ddpArgsOffset = m_backend.ddpAllocDeviceArgsSlot(indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                                                 : sizeof(VkDrawIndirectCommand));
      if (indexed) {
        struct Cmd
        {
          uint32_t indexCount, instanceCount, firstIndex;
          int32_t vertexOffset;
          uint32_t firstInstance;
        } cmdPayload{drawSpec.indexCount,
                     drawSpec.instanceCount,
                     drawSpec.firstIndex,
                     drawSpec.vertexOffset,
                     drawSpec.firstInstance};
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), m_ddpArgsOffset, slice);
      } else {
        struct Cmd
        {
          uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
        } cmdPayload{drawSpec.vertexCount, drawSpec.instanceCount, drawSpec.firstVertex, drawSpec.firstInstance};
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), m_ddpArgsOffset, slice);
      }
      m_ddpArgsPrepared = true;
    }
    recorder.recordGraphicsDraw(drawSpec);
  } else if (m_backend.ddpIndirectCountEnabled() &&
             shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    const bool indexed = (drawSpec.indexCount > 0);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      CHECK(m_ddpArgsPrepared) << "Ellipsoid DDP peel: args not prepared in init";
      if (indexed) {
        c.drawIndexedIndirectCount(argsBuf, m_ddpArgsOffset, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
      } else {
        c.drawIndirectCount(argsBuf, m_ddpArgsOffset, cntBuf, 0, 1, sizeof(VkDrawIndirectCommand));
      }
    });
  } else {
    recorder.recordGraphicsDraw(drawSpec);
  }
}

void ZVulkanEllipsoidPipelineContext::ensureDescriptorLayouts()
{
  if (!m_setPlaceholder) {
    m_setPlaceholder = m_backend.dualTexturePlaceholderDescriptorSetLayout();
  }
  if (!m_setLighting) {
    m_setLighting = m_backend.lightingDescriptorSetLayout();
  }
  if (!m_setTransforms) {
    m_setTransforms = m_backend.transformDescriptorSetLayout();
  }
  if (!m_setOIT) {
    m_setOIT = m_backend.oitDescriptorSetLayout();
  }
}

void ZVulkanEllipsoidPipelineContext::ensureDescriptorSets()
{
  ensureDescriptorLayouts();

  if (!m_dsPlaceholder) {
    m_dsPlaceholder = m_backend.allocateFrameDescriptorSet(m_setPlaceholder);
  }
  if (!m_dsLighting) {
    m_dsLighting = m_backend.allocateFrameDescriptorSet(m_setLighting);
  }
  if (!m_dsTransforms) {
    m_dsTransforms = m_backend.allocateFrameDescriptorSet(m_setTransforms);
  }
  if (!m_dsOIT && m_setOIT) {
    m_dsOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }

  ensurePlaceholderTexture();
  if (m_dsPlaceholder) {
    auto& tex = m_backend.defaultPlaceholderTexture2D();
    m_dsPlaceholder->writeTextureOnce(0, tex, m_backend.defaultSampler());
    m_dsPlaceholder->writeTextureOnce(1, tex, m_backend.defaultSampler());
  }

  // Bind dynamic UBOs to the per-frame uniform arena once
  if (m_dsLighting) {
    m_dsLighting->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(LightingUBOStd140));
  }
  if (m_dsTransforms) {
    m_dsTransforms->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(TransformsUBOStd140));
    m_dsTransforms->writeUniformBufferDynamicOnce(1, m_backend.uniformArenaBuffer(), sizeof(MaterialUBOStd140));
  }
  if (m_dsOIT && !m_backend.isRecording()) {
    m_backend.primeOITDescriptorSet(*m_dsOIT);
  }
}

void ZVulkanEllipsoidPipelineContext::ensureOITResources()
{
  ensureDescriptorLayouts();
  if (!m_dsOIT && m_setOIT) {
    m_dsOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }
}

 

void ZVulkanEllipsoidPipelineContext::ensurePlaceholderTexture() {}

// Lighting UBO is shared per frame; no per-batch update required.

void ZVulkanEllipsoidPipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                         const RenderBatch& batch,
                                                         const EllipsoidPayload& payload,
                                                         bool pickingPass)
{
  const auto hook = renderer.shaderHookType();
  const bool ddp = (hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                    hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  CHECK(payload.params != nullptr) << "Ellipsoid payload missing params";

  TransformsUBOStd140 transforms{};
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  transforms.view_matrix = eyeState.viewMatrix;
  transforms.pos_transform = payload.params->coordTransform;

  const glm::mat4 combined = eyeState.viewMatrix * transforms.pos_transform;
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(combined)));
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(normalMatrix);
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  transforms.parameters = glm::vec4(payload.params->sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);

  if (!(ddp && m_ddpTransformsFrozen)) {
    auto slice = m_backend.suballocateUniform(sizeof(TransformsUBOStd140));
    std::memcpy(slice.mapped, &transforms, sizeof(transforms));
    m_dynTransformsOffset = slice.offset;
    if (ddp) {
      m_ddpTransformsFrozen = true;
    }
  }

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params->materialAmbient;
  material.material_specular = payload.params->materialSpecular;
  material.material_shininess = payload.params->materialShininess;
  material.alpha = pickingPass ? 1.0f : payload.params->opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);

  if (pickingPass) {
    material.material_specular = glm::vec4(0.0f);
    material.material_shininess = 0.0f;
  }

  if (!(ddp && m_ddpMaterialFrozen)) {
    auto slice = m_backend.suballocateUniform(sizeof(MaterialUBOStd140));
    std::memcpy(slice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = slice.offset;
    if (ddp) {
      m_ddpMaterialFrozen = true;
    }
  }

  VLOG(2) << fmt::format("VK ellipsoid params: sizeScale={:.3f} alpha={:.3f} picking={} ortho={}",
                         payload.params->sizeScale,
                         material.alpha,
                         pickingPass,
                         (eyeState.isPerspective ? 0 : 1));
}

ZVulkanEllipsoidPipelineContext::PipelineInstance&
ZVulkanEllipsoidPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  ensureDescriptorLayouts();

  PipelineInstance instance;

  auto selectFragmentShader = [](Z3DRendererBase::ShaderHookType hook) -> std::string {
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return "dual_peeling_init_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return "dual_peeling_peel_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        return "ppll_count_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        return "ppll_store_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return "wavg_init_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return "wblended_init_ellipsoid.frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return "ellipsoid.frag.spv";
    }
  };

  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "ellipsoid.vert.spv",
                                                    shaderBase + selectFragmentShader(key.shaderHookType),
                                                    std::nullopt);

  const uint32_t useDynamic = key.dynamicMaterial ? 1u : 0u;
  std::array<vk::SpecializationMapEntry, 1> vertEntries{
    vk::SpecializationMapEntry{.constantID = 60, .offset = 0, .size = sizeof(uint32_t)}
  };
  std::array<uint32_t, 1> vertData{useDynamic};
  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eVertex,
    std::vector<vk::SpecializationMapEntry>(vertEntries.begin(), vertEntries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(vertData.data()),
                         reinterpret_cast<const uint8_t*>(vertData.data()) + sizeof(vertData)));

  const uint32_t useLinearFog = key.fogMode == FogMode::Linear ? 1u : 0u;
  const uint32_t useExpFog = key.fogMode == FogMode::Exponential ? 1u : 0u;
  const uint32_t useExp2Fog = key.fogMode == FogMode::ExponentialSquared ? 1u : 0u;

  std::array<vk::SpecializationMapEntry, 4> fragEntries{
    vk::SpecializationMapEntry{.constantID = 60, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 20, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 21, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 22, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };
  std::array<uint32_t, 4> fragData{useDynamic, useLinearFog, useExpFog, useExp2Fog};
  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    std::vector<vk::SpecializationMapEntry>(fragEntries.begin(), fragEntries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(fragData.data()),
                         reinterpret_cast<const uint8_t*>(fragData.data()) + sizeof(fragData)));

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  std::vector<vk::DescriptorSetLayout> layouts{m_setPlaceholder, m_setLighting, m_setTransforms, m_setOIT};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  vk::PipelineColorBlendAttachmentState baseBlend{};
  baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  baseBlend.blendEnable = VK_FALSE;

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
      // Exact OIT PPLL: depth test against opaque depth, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(formats.colorFormats.size(), baseBlend);
      for (auto& attachment : attachments) {
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
        attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
      instance.pipeline->setDepthTestEnable(false);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::WeightedBlendedInit: {
      if (!formats.colorFormats.empty()) {
        std::vector<vk::PipelineColorBlendAttachmentState> attachments;
        attachments.reserve(formats.colorFormats.size());
        for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
          auto state = baseBlend;
          state.blendEnable = VK_TRUE;
          if (i == 0) {
            state.srcColorBlendFactor = vk::BlendFactor::eOne;
            state.dstColorBlendFactor = vk::BlendFactor::eOne;
            state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          } else {
            state.srcColorBlendFactor = vk::BlendFactor::eZero;
            state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
            state.srcAlphaBlendFactor = vk::BlendFactor::eZero;
            state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
          }
          state.colorBlendOp = vk::BlendOp::eAdd;
          state.alphaBlendOp = vk::BlendOp::eAdd;
          attachments.push_back(state);
        }
        instance.pipeline->setColorBlendAttachments(std::move(attachments));
      }
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
    case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel: {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments;
      attachments.reserve(formats.colorFormats.size());
      for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
        auto state = baseBlend;
        if (i == 0 || i == 3) {
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 1 || i == 4) {
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 2 || i == 5) {
          state.blendEnable = VK_TRUE;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eAdd;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eAdd;
        } else {
          state.blendEnable = VK_FALSE;
        }
        attachments.push_back(state);
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
      instance.pipeline->setDepthTestEnable(false);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    default:
      break;
  }

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(EllipsoidPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

void ZVulkanEllipsoidPipelineContext::uploadGeometry(const EllipsoidPayload& payload)
{
  m_vertexCount = payload.centers.size();
  m_indexCount = payload.indices.size();

  if (m_vertexCount == 0) {
    return;
  }

  CHECK(payload.axis1.size() == m_vertexCount && payload.axis2.size() == m_vertexCount &&
        payload.axis3.size() == m_vertexCount && payload.centers.size() == m_vertexCount)
    << "Vulkan ellipsoid backend skipping batch: axis buffers are incomplete.";

  // Allocate SoA slices
  const size_t axisBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t centerBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t colorBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t flagsBytes = m_vertexCount * sizeof(float);
  const size_t specBytes = m_vertexCount * sizeof(glm::vec4);
  m_backend.reserveUploadSlices({
    {axisBytes,                       alignof(glm::vec4)},
    {axisBytes,                       alignof(glm::vec4)},
    {axisBytes,                       alignof(glm::vec4)},
    {centerBytes,                     alignof(glm::vec4)},
    {colorBytes,                      alignof(glm::vec4)},
    {flagsBytes,                      alignof(float)    },
    {specBytes,                       alignof(glm::vec4)},
    {m_indexCount * sizeof(uint32_t), alignof(uint32_t) }
  });

  auto axis1Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
  auto axis2Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
  auto axis3Slice = m_backend.suballocateUpload(axisBytes, alignof(glm::vec4));
  auto centerSlice = m_backend.suballocateUpload(centerBytes, alignof(glm::vec4));
  auto colorSlice = m_backend.suballocateUpload(colorBytes, alignof(glm::vec4));
  auto flagsSlice = m_backend.suballocateUpload(flagsBytes, alignof(float));
  auto specSlice = m_backend.suballocateUpload(specBytes, alignof(glm::vec4));

  if (!axis1Slice.buffer || !axis1Slice.mapped || !axis2Slice.buffer || !axis2Slice.mapped || !axis3Slice.buffer ||
      !axis3Slice.mapped || !centerSlice.buffer || !centerSlice.mapped || !colorSlice.buffer || !colorSlice.mapped ||
      !flagsSlice.buffer || !flagsSlice.mapped || !specSlice.buffer || !specSlice.mapped) {
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }

  if (payload.useDynamicMaterial) {
    CHECK(payload.specularAndShininess.size() >= m_vertexCount)
      << "Vulkan ellipsoid backend: dynamic material buffer is incomplete; missing values default to zero.";
  }
  auto* axis1Out = static_cast<glm::vec4*>(axis1Slice.mapped);
  auto* axis2Out = static_cast<glm::vec4*>(axis2Slice.mapped);
  auto* axis3Out = static_cast<glm::vec4*>(axis3Slice.mapped);
  auto* centerOut = static_cast<glm::vec4*>(centerSlice.mapped);
  auto* colorOut = static_cast<glm::vec4*>(colorSlice.mapped);
  auto* flagsOut = static_cast<float*>(flagsSlice.mapped);
  auto* specOut = static_cast<glm::vec4*>(specSlice.mapped);

  const bool pickingPass = payload.pickingPass;

  std::memcpy(axis1Out, payload.axis1.data(), axisBytes);
  std::memcpy(axis2Out, payload.axis2.data(), axisBytes);
  std::memcpy(axis3Out, payload.axis3.data(), axisBytes);
  std::memcpy(centerOut, payload.centers.data(), centerBytes);
  if (pickingPass) {
    // Use picking colors when available; else default to zero
    for (size_t i = 0; i < m_vertexCount; ++i) {
      colorOut[i] = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
    }
  } else {
    // Default to black if missing
    for (size_t i = 0; i < m_vertexCount; ++i) {
      colorOut[i] = (i < payload.colors.size()) ? payload.colors[i] : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
  }
  for (size_t i = 0; i < m_vertexCount; ++i) {
    flagsOut[i] = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
  }
  if (!pickingPass && payload.useDynamicMaterial) {
    for (size_t i = 0; i < m_vertexCount; ++i) {
      specOut[i] = (i < payload.specularAndShininess.size()) ? payload.specularAndShininess[i] : glm::vec4(0.0f);
    }
  } else {
    std::memset(specOut, 0, specBytes);
  }

  if (m_indexCount > 0) {
    auto iSlice = m_backend.suballocateUpload(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
    if (iSlice.buffer && iSlice.mapped) {
      std::memcpy(iSlice.mapped, payload.indices.data(), m_indexCount * sizeof(uint32_t));
      m_indexUploadBuffer = iSlice.buffer;
      m_indexUploadOffset = iSlice.offset;
    } else {
      m_indexCount = 0;
    }
  } else {
    m_indexUploadBuffer = VK_NULL_HANDLE;
    m_indexUploadOffset = 0;
  }
  // Save SoA buffer + offsets
  m_axis1Buffer = axis1Slice.buffer;
  m_axis2Buffer = axis2Slice.buffer;
  m_axis3Buffer = axis3Slice.buffer;
  m_centerBuffer = centerSlice.buffer;
  m_colorBuffer = colorSlice.buffer;
  m_flagsBuffer = flagsSlice.buffer;
  m_specularBuffer = specSlice.buffer;
  m_axis1Offset = axis1Slice.offset;
  m_axis2Offset = axis2Slice.offset;
  m_axis3Offset = axis3Slice.offset;
  m_centerOffset = centerSlice.offset;
  m_colorOffset = colorSlice.offset;
  m_flagsOffset = flagsSlice.offset;
  m_specularOffset = specSlice.offset;

  // Attempt static promotion
  {
    CHECK(payload.streamKey != 0) << "Ellipsoid payload missing streamKey";
    CacheKey key{payload.streamKey, payload.pickingPass, payload.useDynamicMaterial};
    auto it = m_staticCache.find(key);
    const int kPromotionThreshold = 2;
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.centersGen = payload.centersGen;
      entry.axesGen = payload.axesGen;
      entry.colorsGen = payload.colorsGen;
      entry.pickingColorsGen = payload.pickingColorsGen;
      entry.specularGen = payload.specularGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
      m_staticCache.emplace(key, entry);
    } else {
      CacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
      const bool dataSame = entry.centersGen == payload.centersGen && entry.axesGen == payload.axesGen &&
                            entry.flagsGen == payload.flagsGen && entry.indexGen == payload.indexGen &&
                            entry.specularGen == payload.specularGen;
      if (sizeSame && dataSame) {
        entry.unchangedFrames++;
      } else {
        entry.unchangedFrames = 0;
      }

      if (entry.promoted && sizeSame) {
        bool anyChanged = (entry.axesGen != payload.axesGen) || (entry.centersGen != payload.centersGen) ||
                          (entry.flagsGen != payload.flagsGen) || (entry.indexGen != payload.indexGen);
        if (payload.pickingPass) {
          anyChanged = anyChanged || (entry.pickingColorsGen != payload.pickingColorsGen);
        } else {
          anyChanged =
            anyChanged || (entry.colorsGen != payload.colorsGen) || (entry.specularGen != payload.specularGen);
        }
        if (entry.axesGen != payload.axesGen) {
          m_backend.scheduleStaticCopy(entry.vbAxis1, entry.axis1Offset, axis1Slice, false);
          m_backend.scheduleStaticCopy(entry.vbAxis2, entry.axis2Offset, axis2Slice, false);
          m_backend.scheduleStaticCopy(entry.vbAxis3, entry.axis3Offset, axis3Slice, false);
        }
        if (entry.centersGen != payload.centersGen) {
          m_backend.scheduleStaticCopy(entry.vbCenter, entry.centerOffset, centerSlice, false);
        }
        if (payload.pickingPass) {
          if (entry.pickingColorsGen != payload.pickingColorsGen) {
            m_backend.scheduleStaticCopy(entry.vbColor, entry.colorOffset, colorSlice, false);
          }
        } else {
          if (entry.colorsGen != payload.colorsGen) {
            m_backend.scheduleStaticCopy(entry.vbColor, entry.colorOffset, colorSlice, false);
          }
          if (entry.specularGen != payload.specularGen) {
            m_backend.scheduleStaticCopy(entry.vbSpecular, entry.specularOffset, specSlice, false);
          }
        }
        if (entry.flagsGen != payload.flagsGen) {
          m_backend.scheduleStaticCopy(entry.vbFlags, entry.flagsOffset, flagsSlice, false);
        }
        if (entry.indexGen != payload.indexGen && m_indexUploadBuffer && m_indexCount > 0) {
          Z3DRendererVulkanBackend::UploadSlice iUpload{m_indexUploadBuffer,
                                                        m_indexUploadOffset,
                                                        nullptr,
                                                        m_indexCount * sizeof(uint32_t)};
          m_backend.scheduleStaticCopy(entry.ib, entry.ibOffset, iUpload, true);
        }
        if (!anyChanged) {
          // Bind static
          m_axis1Buffer = entry.vbAxis1;
          m_axis2Buffer = entry.vbAxis2;
          m_axis3Buffer = entry.vbAxis3;
          m_centerBuffer = entry.vbCenter;
          m_colorBuffer = entry.vbColor;
          m_flagsBuffer = entry.vbFlags;
          m_specularBuffer = entry.vbSpecular;
          m_axis1Offset = entry.axis1Offset;
          m_axis2Offset = entry.axis2Offset;
          m_axis3Offset = entry.axis3Offset;
          m_centerOffset = entry.centerOffset;
          m_colorOffset = entry.colorOffset;
          m_flagsOffset = entry.flagsOffset;
          m_specularOffset = entry.specularOffset;
          if (entry.indexCount > 0 && entry.ib) {
            m_indexUploadBuffer = entry.ib;
            m_indexUploadOffset = entry.ibOffset;
          }
          return;
        }
        return; // use upload slices for this frame
      }

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        // Allocate per-stream static slices
        auto axis1Dst = m_backend.allocateStaticVB(axisBytes, alignof(glm::vec4));
        auto axis2Dst = m_backend.allocateStaticVB(axisBytes, alignof(glm::vec4));
        auto axis3Dst = m_backend.allocateStaticVB(axisBytes, alignof(glm::vec4));
        auto centerDst = m_backend.allocateStaticVB(centerBytes, alignof(glm::vec4));
        auto colorDst = m_backend.allocateStaticVB(colorBytes, alignof(glm::vec4));
        auto flagsDst = m_backend.allocateStaticVB(flagsBytes, alignof(float));
        auto specDst = m_backend.allocateStaticVB(specBytes, alignof(glm::vec4));
        Z3DRendererVulkanBackend::StaticSlice ibDst{};
        if (m_indexCount > 0) {
          ibDst = m_backend.allocateStaticIB(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
        }
        if (axis1Dst.buffer && axis2Dst.buffer && axis3Dst.buffer && centerDst.buffer && colorDst.buffer &&
            flagsDst.buffer && specDst.buffer && (m_indexCount == 0 || ibDst.buffer)) {
          // Record per-stream copies
          m_backend.scheduleStaticCopy(axis1Dst.buffer, axis1Dst.offset, axis1Slice, false);
          m_backend.scheduleStaticCopy(axis2Dst.buffer, axis2Dst.offset, axis2Slice, false);
          m_backend.scheduleStaticCopy(axis3Dst.buffer, axis3Dst.offset, axis3Slice, false);
          m_backend.scheduleStaticCopy(centerDst.buffer, centerDst.offset, centerSlice, false);
          m_backend.scheduleStaticCopy(colorDst.buffer, colorDst.offset, colorSlice, false);
          m_backend.scheduleStaticCopy(flagsDst.buffer, flagsDst.offset, flagsSlice, false);
          m_backend.scheduleStaticCopy(specDst.buffer, specDst.offset, specSlice, false);
          if (m_indexCount > 0) {
            Z3DRendererVulkanBackend::UploadSlice iUpload{m_indexUploadBuffer,
                                                          m_indexUploadOffset,
                                                          nullptr,
                                                          m_indexCount * sizeof(uint32_t)};
            m_backend.scheduleStaticCopy(ibDst.buffer, ibDst.offset, iUpload, /*isIndexBuffer=*/true);
          }
          VLOG(1) << fmt::format(
            "VK ellipsoid promote: axis1={}B axis2={}B axis3={}B center={}B color={}B flags={}B spec={}B idx={}B",
            axisBytes,
            axisBytes,
            axisBytes,
            centerBytes,
            colorBytes,
            flagsBytes,
            specBytes,
            m_indexCount * sizeof(uint32_t));
          entry.vbAxis1 = axis1Dst.buffer;
          entry.vbAxis2 = axis2Dst.buffer;
          entry.vbAxis3 = axis3Dst.buffer;
          entry.vbCenter = centerDst.buffer;
          entry.vbColor = colorDst.buffer;
          entry.vbFlags = flagsDst.buffer;
          entry.vbSpecular = specDst.buffer;
          entry.axis1Offset = axis1Dst.offset;
          entry.axis2Offset = axis2Dst.offset;
          entry.axis3Offset = axis3Dst.offset;
          entry.centerOffset = centerDst.offset;
          entry.colorOffset = colorDst.offset;
          entry.flagsOffset = flagsDst.offset;
          entry.specularOffset = specDst.offset;
          entry.ib = ibDst.buffer;
          entry.ibOffset = ibDst.offset;
          entry.promoted = true;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
        }
      }
    }
  }
}

vk::PipelineVertexInputStateCreateInfo ZVulkanEllipsoidPipelineContext::makeVertexInputState() const
{
  static std::array<vk::VertexInputBindingDescription, 7> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis1
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis2
    vk::VertexInputBindingDescription{.binding = 2,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis3
    vk::VertexInputBindingDescription{.binding = 3,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // center
    vk::VertexInputBindingDescription{.binding = 4,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // color
    vk::VertexInputBindingDescription{.binding = 5,
                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // flags
    vk::VertexInputBindingDescription{.binding = 6,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // specular
  };
  static std::array<vk::VertexInputAttributeDescription, 7> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 1,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 2,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 3,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 4,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 5, .binding = 5, .format = vk::Format::eR32Sfloat, .offset = 0},
    vk::VertexInputAttributeDescription{.location = 6,
                                        .binding = 6,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               }
  };
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  info.pVertexBindingDescriptions = bindings.data();
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

} // namespace nim
