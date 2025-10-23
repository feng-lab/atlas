#include "zvulkanconepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3drendererstates.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkandescriptorset.h"
#include "zvulkanbuffer.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanbindings.h"
#include "zvulkanuniforms.h"
#include "zsysteminfo.h"
#include "zexception.h"
#include "zlog.h"
#include "z3dconerenderer.h"

#include <algorithm>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdint>

namespace nim {

namespace {

int toCapsMode(ConePayload::CapStyle capStyle)
{
  switch (capStyle) {
    case ConePayload::CapStyle::NoCaps:
      return 0;
    case ConePayload::CapStyle::FlatCaps:
      return 1;
    case ConePayload::CapStyle::RoundCaps:
      return 2;
    case ConePayload::CapStyle::FlatBaseRoundTop:
      return 3;
    case ConePayload::CapStyle::RoundBaseFlatTop:
      return 4;
  }
  return 1;
}

struct ConePushConstants
{
  glm::mat4 projectionMatrix{1.0f};
  float ortho = 0.0f;
  float _pad[3] = {0.0f, 0.0f, 0.0f};
};

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

} // namespace

ZVulkanConePipelineContext::ZVulkanConePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanConePipelineContext::~ZVulkanConePipelineContext() = default;

void ZVulkanConePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_originBuffer = nullptr;
  m_axisBuffer = nullptr;
  m_flagsBuffer = nullptr;
  m_baseColorBuffer = nullptr;
  m_topColorBuffer = nullptr;
  m_originOffset = 0;
  m_axisOffset = 0;
  m_flagsOffset = 0;
  m_baseColorOffset = 0;
  m_topColorOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  // Retire per-frame UBOs so they are not overwritten while still in use by
  // an in-flight frame. Destruction runs after the active submission fence.
  retainUbo(m_uboOIT);
  resetDescriptors();
  m_ddpLightingFrozen = false;
  m_ddpTransformsFrozen = false;
  m_ddpMaterialFrozen = false;
  m_ddpArgsPrepared = false;
  m_ddpArgsOffset = 0;
}

void ZVulkanConePipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  for (auto& sp : m_retainedUbos) {
    auto keep = sp;
    m_backend.scheduleAfterActiveSubmissionFence([keep]() {});
  }
  m_retainedUbos.clear();
}

void ZVulkanConePipelineContext::resetDescriptors()
{
  m_dsPlaceholder.reset();
  m_dsLighting.reset();
  m_dsTransforms.reset();
  m_dsOIT.reset();
}

void ZVulkanConePipelineContext::record(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const ConePayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd)
{
  // Ensure last frame's UBOs remain alive until this submission completes.
  flushRetainedUbos();
  VLOG(2) << "record begin: payload sizes base=" << payload.baseAndRadius.size()
          << " axis=" << payload.axisAndTopRadius.size() << " flags=" << payload.flags.size()
          << " baseColors=" << payload.baseColors.size() << " topColors=" << payload.topColors.size()
          << " pickingColors=" << payload.pickingColors.size() << " indices=" << payload.indices.size()
          << " pickingPass=" << payload.pickingPass << " useConeShader2=" << payload.useConeShader2;
  if (payload.baseAndRadius.empty()) {
    return;
  }

  // GL parity: in picking pass, require picking colors sized equal to vertices.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.baseAndRadius.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  VLOG(2) << "uploadGeometry -> vertexCount=" << m_vertexCount << " indexCount=" << m_indexCount;
  if (m_vertexCount == 0) {
    return;
  }

  VLOG(1) << fmt::format(
    "VK cone draw: picking={} useShader2={} verts={} idx={} buf(origin={}, axis={}, flags={}, base={}, top={}) offs(o={}, a={}, f={}, b={}, t={})",
    payload.pickingPass,
    payload.useConeShader2,
    m_vertexCount,
    m_indexCount,
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_originBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_axisBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_flagsBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_baseColorBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_topColorBuffer))),
    m_originOffset,
    m_axisOffset,
    m_flagsOffset,
    m_baseColorOffset,
    m_topColorOffset);

  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = renderer.shaderHookType();

  updateLightingUBO(renderer, batch, payload, pickingPass);
  updateTransformUBO(renderer, batch, payload, pickingPass);
  // OIT params UBO for shaders including oit_params.glslinc
  ensureOITResources();
  {
    glm::vec2 extent = batch.pass.viewport.extent;
    if (extent.x <= 0.0f || extent.y <= 0.0f) {
      const auto& viewportState = renderer.frameState().viewport;
      extent = glm::vec2(static_cast<float>(viewportState.z), static_cast<float>(viewportState.w));
    }
    glm::vec2 screenRcp =
      (extent.x > 0.0f && extent.y > 0.0f) ? glm::vec2(1.0f / extent.x, 1.0f / extent.y) : glm::vec2(0.0f);
    updateOITParamsUBO(renderer, batch, screenRcp);
  }
  // Descriptor sets are primed in beginRender(); avoid record-time rewrites.
  CHECK(m_dsLighting && m_dsTransforms) << "descriptor sets missing (lighting/transforms)";

  ZVulkanDescriptorSet* dsPlaceholderOverride = nullptr;
  ensurePlaceholderTexture();
  const auto& hookPara = renderer.shaderHookPara();
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel && m_setPlaceholder) {
    dsPlaceholderOverride = m_backend.allocateOverrideDescriptorSet(m_setPlaceholder);
    CHECK(dsPlaceholderOverride != nullptr) << "DDP peel: override descriptor allocation failed (fatal)";
    if (dsPlaceholderOverride) {
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                   m_backend.device(),
                                                   "dual-depth-peeling depth blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, depthTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, tex, m_backend.defaultSampler());
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                   m_backend.device(),
                                                   "dual-depth-peeling front blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, frontTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, tex, m_backend.defaultSampler());
      }
    }
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  m_backend.validateFormatsOrCrash(formats, "cone");

  PipelineKey key;
  key.dynamicMaterial = !pickingPass;
  key.useConeShader2 = payload.useConeShader2;
  key.capsMode = toCapsMode(payload.capStyle);
  key.shaderHookType = shaderHook;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  ConePushConstants constants;
  constants.projectionMatrix = eyeState.projectionMatrix;
  constants.ortho = eyeState.isPerspective ? 0.0f : 1.0f;
  CHECK((dsPlaceholderOverride != nullptr) || (m_dsPlaceholder != nullptr))
    << "placeholder descriptor set not initialised";
  std::vector<vk::DescriptorSet> sets;
  const vk::DescriptorSet ds0 =
    dsPlaceholderOverride ? dsPlaceholderOverride->descriptorSet() : m_dsPlaceholder->descriptorSet();
  sets.push_back(ds0);
  sets.push_back(m_dsLighting->descriptorSet());
  sets.push_back(m_dsTransforms->descriptorSet());

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = {viewport};
  drawSpec.scissors = {scissor};
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetInputs;
  drawSpec.descriptorSets = sets;
  // Dynamic offsets order: lighting (set1,b0), transforms (set2,b0), material (set2,b1)
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

  drawSpec.vertexBuffers = {m_originBuffer, m_axisBuffer, m_flagsBuffer, m_baseColorBuffer, m_topColorBuffer};
  drawSpec.vertexOffsets = {m_originOffset, m_axisOffset, m_flagsOffset, m_baseColorOffset, m_topColorOffset};
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
  drawSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(ConePushConstants));
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
        } payload{drawSpec.indexCount,
                  drawSpec.instanceCount,
                  drawSpec.firstIndex,
                  drawSpec.vertexOffset,
                  drawSpec.firstInstance};
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &payload, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), m_ddpArgsOffset, slice);
      } else {
        struct Cmd
        {
          uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
        } payload{drawSpec.vertexCount, drawSpec.instanceCount, drawSpec.firstVertex, drawSpec.firstInstance};
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &payload, sizeof(Cmd));
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
      CHECK(m_ddpArgsPrepared) << "Cone DDP peel: args not prepared in init";
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

void ZVulkanConePipelineContext::ensureDescriptorLayouts()
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

void ZVulkanConePipelineContext::ensureDescriptorSets()
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

  // Prime dynamic UBO bindings to the per-frame uniform arena
  if (m_dsLighting) {
    m_dsLighting->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(LightingUBOStd140));
  }
  if (m_dsTransforms) {
    m_dsTransforms->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(TransformsUBOStd140));
    m_dsTransforms->writeUniformBufferDynamicOnce(1, m_backend.uniformArenaBuffer(), sizeof(MaterialUBOStd140));
  }

  // OIT params UBO (regular, not dynamic)
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (m_dsOIT && m_uboOIT) {
    m_dsOIT->writeUniformBufferOnce(vkbind::kBindingOITParamsUBO, *m_uboOIT);
    if (!m_backend.isRecording()) {
      if (auto* buf = m_backend.ddpChangedFlagBufferObj()) {
        m_dsOIT->writeStorageBufferOnce(vkbind::kBindingOITDDPFlag, *buf);
      }
    }
  }
}

void ZVulkanConePipelineContext::ensureOITResources()
{
  ensureDescriptorLayouts();
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_dsOIT && m_setOIT) {
    m_dsOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }
}

void ZVulkanConePipelineContext::updateOITParamsUBO(Z3DRendererBase& renderer,
                                                    const RenderBatch& batch,
                                                    const glm::vec2& screenDimRcp)
{
  (void)batch;
  if (!m_uboOIT) {
    return;
  }
  OITParamsUBOStd140 oit{};
  oit.screen_dim_RCP = screenDimRcp;
  const float n = renderer.viewState().nearClip;
  const float f = renderer.viewState().farClip;
  const float denom = std::max(f - n, 1e-6f);
  oit.ze_to_zw_a = (f * n) / denom;
  oit.ze_to_zw_b = 0.5f * (f + n) / denom + 0.5f;
  oit.weighted_blended_depth_scale = renderer.sceneState().weightedBlendedDepthScale;
  m_uboOIT->copyData(&oit, sizeof(oit));
}

void ZVulkanConePipelineContext::ensurePlaceholderTexture() {}

void ZVulkanConePipelineContext::updateLightingUBO(Z3DRendererBase& renderer,
                                                   const RenderBatch& batch,
                                                   const ConePayload& payload,
                                                   bool pickingPass)
{
  const auto hook = renderer.shaderHookType();
  const bool ddp = (hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                    hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (ddp && m_ddpLightingFrozen) {
    return;
  }
  LightingUBOStd140 lighting{};
  const auto& scene = renderer.sceneState();

  size_t availableLights = scene.lighting.positions.size();
  availableLights = std::min(availableLights, scene.lighting.ambient.size());
  availableLights = std::min(availableLights, scene.lighting.diffuse.size());
  availableLights = std::min(availableLights, scene.lighting.specular.size());
  availableLights = std::min(availableLights, scene.lighting.attenuation.size());
  availableLights = std::min(availableLights, scene.lighting.spotCutoff.size());
  availableLights = std::min(availableLights, scene.lighting.spotExponent.size());
  availableLights = std::min(availableLights, scene.lighting.spotDirection.size());
  availableLights = std::min(availableLights, static_cast<size_t>(scene.lighting.lightCount));

  lighting.numLights = static_cast<int>(std::min(availableLights, lighting.lights.size()));
  const bool enableLighting = !pickingPass && payload.wantsLighting;
  lighting.lighting_enabled = enableLighting ? 1 : 0;

  glm::vec2 extent = batch.pass.viewport.extent;
  if (extent.x <= 0.0f || extent.y <= 0.0f) {
    const auto& viewport = renderer.frameState().viewport;
    extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
  }
  if (extent.x > 0.0f && extent.y > 0.0f) {
    lighting.screen_dim_RCP = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  }

  lighting.fog_color_top = scene.fog.topColor;
  lighting.fog_color_bottom = scene.fog.bottomColor;
  lighting.fog_end = scene.fog.range.y;
  lighting.fog_scale =
    scene.fog.range.y > scene.fog.range.x ? 1.0f / std::max(scene.fog.range.y - scene.fog.range.x, 1e-6f) : 0.0f;
  constexpr float kLog2e = 1.44269504088896340735992468100189214f;
  lighting.fog_density_log2e = scene.fog.density * kLog2e;
  lighting.fog_density_density_log2e = scene.fog.density * scene.fog.density * kLog2e;

  for (int i = 0; i < lighting.numLights; ++i) {
    const size_t idx = static_cast<size_t>(i);
    lighting.lights[i].position = scene.lighting.positions[idx];
    lighting.lights[i].ambient = scene.lighting.ambient[idx];
    lighting.lights[i].diffuse = scene.lighting.diffuse[idx];
    lighting.lights[i].specular = scene.lighting.specular[idx];
    lighting.lights[i].attenuation = scene.lighting.attenuation[idx];
    lighting.lights[i].spotCutoff = scene.lighting.spotCutoff[idx];
    lighting.lights[i].spotExponent = scene.lighting.spotExponent[idx];
    lighting.lights[i].spotDirection = scene.lighting.spotDirection[idx];
  }

  {
    auto slice = m_backend.suballocateUniform(sizeof(LightingUBOStd140));
    std::memcpy(slice.mapped, &lighting, sizeof(lighting));
    m_dynLightingOffset = slice.offset;
    if (ddp) {
      m_ddpLightingFrozen = true;
    }
  }
}

void ZVulkanConePipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                    const RenderBatch& batch,
                                                    const ConePayload& payload,
                                                    bool pickingPass)
{
  const auto hook = renderer.shaderHookType();
  const bool ddp = (hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                    hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  CHECK(payload.params != nullptr) << "Cone payload missing params";

  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];

  TransformsUBOStd140 transforms{};
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  transforms.view_matrix = eyeState.viewMatrix;
  const glm::mat4 model =
    (payload.followCoordTransform && payload.params) ? payload.params->coordTransform : glm::mat4(1.0f);
  transforms.pos_transform = model;

  const glm::mat4 combined = eyeState.viewMatrix * model;
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(combined)));
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(normalMatrix);
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  const float sizeScale = (payload.followSizeScale && payload.params) ? payload.params->sizeScale : 1.0f;
  transforms.parameters = glm::vec4(sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);
  if (!(ddp && m_ddpTransformsFrozen)) {
    auto slice = m_backend.suballocateUniform(sizeof(TransformsUBOStd140));
    std::memcpy(slice.mapped, &transforms, sizeof(transforms));
    m_dynTransformsOffset = slice.offset;
    if (ddp) {
      m_ddpTransformsFrozen = true;
    }
  }

  MaterialUBOStd140 material{};
  const auto& scene = renderer.sceneState();
  material.scene_ambient = scene.sceneAmbient;
  material.material_ambient = payload.params->materialAmbient;
  material.material_specular = pickingPass ? glm::vec4(0.0f) : payload.params->materialSpecular;
  material.material_shininess = pickingPass ? 0.0f : payload.params->materialShininess;
  material.alpha = (pickingPass || !payload.followOpacity || !payload.params) ? 1.0f : payload.params->opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);
  if (!(ddp && m_ddpMaterialFrozen)) {
    auto slice = m_backend.suballocateUniform(sizeof(MaterialUBOStd140));
    std::memcpy(slice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = slice.offset;
    if (ddp) {
      m_ddpMaterialFrozen = true;
    }
  }

  VLOG(2) << fmt::format("VK cone params: sizeScale={:.3f} alpha={:.3f} picking={} ortho={}",
                         payload.params->sizeScale,
                         material.alpha,
                         pickingPass,
                         (eyeState.isPerspective ? 0 : 1));
}

ZVulkanConePipelineContext::PipelineInstance&
ZVulkanConePipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  ensureDescriptorLayouts();

  PipelineInstance instance;

  auto selectFragmentShader = [&](Z3DRendererBase::ShaderHookType hook) -> std::string {
    const char* suffix = key.useConeShader2 ? "_2" : "";
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return std::string("dual_peeling_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return std::string("dual_peeling_peel_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return std::string("wavg_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return std::string("wblended_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return std::string("cone") + suffix + ".frag.spv";
    }
  };

  const std::string vertName = key.useConeShader2 ? "cone_2.vert.spv" : "cone.vert.spv";
  VLOG(1) << "selecting shaders vert='" << (shaderBase + vertName) << "' frag='"
          << (shaderBase + selectFragmentShader(key.shaderHookType)) << "' capsMode=" << key.capsMode
          << " dynamicMaterial=" << key.dynamicMaterial << " shaderHook=" << static_cast<int>(key.shaderHookType);
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + vertName,
                                                    shaderBase + selectFragmentShader(key.shaderHookType),
                                                    std::nullopt);

  std::array<vk::SpecializationMapEntry, 1> specEntries{
    vk::SpecializationMapEntry{.constantID = 90, .offset = 0, .size = sizeof(int)}
  };
  std::array<int, 1> specData{key.capsMode};
  // Apply CAPS_MODE to both vertex and fragment (cone_2.vert depends on it)
  {
    auto entries = std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end());
    auto dataVec = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                                        reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData));
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex, entries, dataVec);
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, entries, dataVec);
  }

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  std::vector<vk::DescriptorSetLayout> layouts{m_setPlaceholder, m_setLighting, m_setTransforms, m_setOIT};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  // Screen-space cone quads (cone_2.vert) end up with CW winding under Vulkan
  // due to clip-space conventions. Treat CW as front to match GL behavior.
  instance.pipeline->setFrontFace(vk::FrontFace::eClockwise);

  vk::PipelineColorBlendAttachmentState baseBlend{};
  baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  baseBlend.blendEnable = false;

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(formats.colorFormats.size(), baseBlend);
      for (auto& attachment : attachments) {
        attachment.blendEnable = true;
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
          state.blendEnable = true;
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
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 1 || i == 4) {
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 2 || i == 5) {
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eAdd;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eAdd;
        } else {
          state.blendEnable = false;
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
                              .size = static_cast<uint32_t>(sizeof(ConePushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanConePipelineContext::makeVertexInputState() const
{
  static std::array<vk::VertexInputBindingDescription, 5> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // origin
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis
    vk::VertexInputBindingDescription{.binding = 2,
                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // flags
    vk::VertexInputBindingDescription{.binding = 3,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // base color
    vk::VertexInputBindingDescription{.binding = 4,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // top color
  };
  static std::array<vk::VertexInputAttributeDescription, 5> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 1,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 2, .binding = 2, .format = vk::Format::eR32Sfloat, .offset = 0},
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 3,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 4,
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

void ZVulkanConePipelineContext::uploadGeometry(const ConePayload& payload)
{
  m_vertexCount = payload.baseAndRadius.size();
  m_indexCount = payload.indices.size();

  VLOG(2) << "uploadGeometry begin: vertexCount=" << m_vertexCount << " indexCount=" << m_indexCount
          << " axisCount=" << payload.axisAndTopRadius.size() << " flagsCount=" << payload.flags.size();

  if (m_vertexCount == 0) {
    return;
  }

  CHECK(payload.axisAndTopRadius.size() == m_vertexCount && payload.flags.size() == m_vertexCount)
    << "Vulkan cone backend skipping batch: attribute buffers are incomplete.";

  // Allocate SoA slices
  const size_t v4Bytes = m_vertexCount * sizeof(glm::vec4);
  const size_t fBytes = m_vertexCount * sizeof(float);
  m_backend.reserveUploadSlices({
    {v4Bytes,                         alignof(glm::vec4)},
    {v4Bytes,                         alignof(glm::vec4)},
    {fBytes,                          alignof(float)    },
    {v4Bytes,                         alignof(glm::vec4)},
    {v4Bytes,                         alignof(glm::vec4)},
    {m_indexCount * sizeof(uint32_t), alignof(uint32_t) }
  });
  VLOG(2) << "uploadGeometry allocating: v4Bytes=" << v4Bytes << " fBytes=" << fBytes
          << " idxBytes=" << (m_indexCount * sizeof(uint32_t));
  auto originSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  auto axisSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  auto flagsSlice = m_backend.suballocateUpload(fBytes, alignof(float));
  auto baseColorSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  auto topColorSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  VLOG(2) << "Slices: origin(buf=" << static_cast<bool>(originSlice.buffer)
          << ", mapped=" << (originSlice.mapped != nullptr) << ") axis(buf=" << static_cast<bool>(axisSlice.buffer)
          << ", mapped=" << (axisSlice.mapped != nullptr) << ") flags(buf=" << static_cast<bool>(flagsSlice.buffer)
          << ", mapped=" << (flagsSlice.mapped != nullptr)
          << ") baseColor(buf=" << static_cast<bool>(baseColorSlice.buffer)
          << ", mapped=" << (baseColorSlice.mapped != nullptr)
          << ") topColor(buf=" << static_cast<bool>(topColorSlice.buffer)
          << ", mapped=" << (topColorSlice.mapped != nullptr) << ")";
  if (!originSlice.buffer || !originSlice.mapped || !axisSlice.buffer || !axisSlice.mapped || !flagsSlice.buffer ||
      !flagsSlice.mapped || !baseColorSlice.buffer || !baseColorSlice.mapped || !topColorSlice.buffer ||
      !topColorSlice.mapped) {
    VLOG(2) << "uploadGeometry: one or more upload slices are invalid (null buffer or mapping). Aborting batch.";
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }
  VLOG(2) << "uploadGeometry: memcpy origins/axis (" << v4Bytes << " bytes each)";
  auto* originOut = static_cast<glm::vec4*>(originSlice.mapped);
  auto* axisOut = static_cast<glm::vec4*>(axisSlice.mapped);
  auto* flagsOut = static_cast<float*>(flagsSlice.mapped);
  auto* baseColorOut = static_cast<glm::vec4*>(baseColorSlice.mapped);
  auto* topColorOut = static_cast<glm::vec4*>(topColorSlice.mapped);
  std::memcpy(originOut, payload.baseAndRadius.data(), v4Bytes);
  std::memcpy(axisOut, payload.axisAndTopRadius.data(), v4Bytes);
  VLOG(2) << "uploadGeometry: filled origin/axis, writing flags/colors";
  for (size_t i = 0; i < m_vertexCount; ++i) {
    float flagsValue = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
    // GL parity: keep flags encoding as produced by the GL path.
    // - For cone.vert (8-vertex path), flags are encoded as (right<<8 | up<<4 | out)
    // - For cone_2.vert (quad path), flags are encoded as (right<<4 | up)
    flagsOut[i] = flagsValue;
  }
  if (payload.pickingPass) {
    for (size_t i = 0; i < m_vertexCount; ++i) {
      glm::vec4 c = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
      baseColorOut[i] = c;
      topColorOut[i] = c;
    }
  } else {
    for (size_t i = 0; i < m_vertexCount; ++i) {
      glm::vec4 base = (i < payload.baseColors.size()) ? payload.baseColors[i] : glm::vec4(1.0f);
      baseColorOut[i] = base;
      glm::vec4 top = base;
      if (!payload.sameColorForBaseAndTop && i < payload.topColors.size()) {
        top = payload.topColors[i];
      }
      topColorOut[i] = top;
    }
  }

  if (m_indexCount > 0) {
    VLOG(2) << "uploadGeometry: allocating/staging indices bytes=" << (m_indexCount * sizeof(uint32_t));
    auto iSlice = m_backend.suballocateUpload(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
    if (iSlice.buffer && iSlice.mapped) {
      std::memcpy(iSlice.mapped, payload.indices.data(), m_indexCount * sizeof(uint32_t));
      m_indexUploadBuffer = iSlice.buffer;
      m_indexUploadOffset = iSlice.offset;
      VLOG(2) << "uploadGeometry: indices staged at offset=" << m_indexUploadOffset;
    } else {
      VLOG(2) << "uploadGeometry: index upload slice invalid; drawing non-indexed";
      m_indexCount = 0;
    }
  } else {
    m_indexUploadBuffer = nullptr;
    m_indexUploadOffset = 0;
  }
  m_originBuffer = originSlice.buffer;
  m_axisBuffer = axisSlice.buffer;
  m_flagsBuffer = flagsSlice.buffer;
  m_baseColorBuffer = baseColorSlice.buffer;
  m_topColorBuffer = topColorSlice.buffer;
  m_originOffset = originSlice.offset;
  m_axisOffset = axisSlice.offset;
  m_flagsOffset = flagsSlice.offset;
  m_baseColorOffset = baseColorSlice.offset;
  m_topColorOffset = topColorSlice.offset;
  auto bufToU64 = [](vk::Buffer b) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(b)));
  };
  VLOG(2) << "uploadGeometry: VB buffers origin=" << bufToU64(m_originBuffer) << " axis=" << bufToU64(m_axisBuffer)
          << " flags=" << bufToU64(m_flagsBuffer) << " baseCol=" << bufToU64(m_baseColorBuffer)
          << " topCol=" << bufToU64(m_topColorBuffer);
  VLOG(2) << "uploadGeometry: VB offsets origin=" << m_originOffset << " axis=" << m_axisOffset
          << " flags=" << m_flagsOffset << " baseCol=" << m_baseColorOffset << " topCol=" << m_topColorOffset;

  // Attempt static promotion
  {
    CHECK(payload.streamKey != 0) << "Cone payload missing streamKey";
    CacheKey key{payload.streamKey, payload.pickingPass};
    auto it = m_staticCache.find(key);
    const int kPromotionThreshold = 2;
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.baseGen = payload.baseGen;
      entry.axisGen = payload.axisGen;
      entry.baseColorGen = payload.baseColorGen;
      entry.topColorGen = payload.topColorGen;
      entry.pickingColorsGen = payload.pickingColorsGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
      m_staticCache.emplace(key, entry);
    } else {
      CacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
      const bool shapeSame = entry.baseGen == payload.baseGen && entry.axisGen == payload.axisGen &&
                             entry.flagsGen == payload.flagsGen && entry.indexGen == payload.indexGen;
      if (sizeSame && shapeSame) {
        entry.unchangedFrames++;
      } else {
        entry.unchangedFrames = 0;
      }

      if (entry.promoted && sizeSame) {
        bool anyChanged = false;
        const bool baseColorChanged =
          (entry.baseColorGen != payload.baseColorGen) || (entry.pickingColorsGen != payload.pickingColorsGen);
        if (baseColorChanged) {
          m_backend.scheduleStaticCopy(entry.vbBaseColor, entry.baseColorOffset, baseColorSlice, false);
          entry.baseColorGen = payload.baseColorGen;
          entry.pickingColorsGen = payload.pickingColorsGen;
          anyChanged = true;
        }
        const bool topColorChanged = (!payload.sameColorForBaseAndTop && entry.topColorGen != payload.topColorGen) ||
                                     (payload.sameColorForBaseAndTop && baseColorChanged);
        if (topColorChanged && entry.vbTopColor) {
          m_backend.scheduleStaticCopy(entry.vbTopColor, entry.topColorOffset, topColorSlice, false);
          anyChanged = true;
        }
        entry.topColorGen = payload.topColorGen;
        if (entry.axisGen != payload.axisGen) {
          m_backend.scheduleStaticCopy(entry.vbAxis, entry.axisOffset, axisSlice, false);
          entry.axisGen = payload.axisGen;
          anyChanged = true;
        }
        if (entry.baseGen != payload.baseGen) {
          m_backend.scheduleStaticCopy(entry.vbOrigin, entry.originOffset, originSlice, false);
          entry.baseGen = payload.baseGen;
          anyChanged = true;
        }
        if (entry.flagsGen != payload.flagsGen) {
          m_backend.scheduleStaticCopy(entry.vbFlags, entry.flagsOffset, flagsSlice, false);
          entry.flagsGen = payload.flagsGen;
          anyChanged = true;
        }
        if (entry.indexGen != payload.indexGen && m_indexCount > 0 && m_indexUploadBuffer) {
          Z3DRendererVulkanBackend::UploadSlice iUpload{m_indexUploadBuffer,
                                                        m_indexUploadOffset,
                                                        nullptr,
                                                        m_indexCount * sizeof(uint32_t)};
          m_backend.scheduleStaticCopy(entry.ib, entry.ibOffset, iUpload, true);
          entry.indexGen = payload.indexGen;
          anyChanged = true;
        }
        entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
        entry.indexCount = static_cast<uint32_t>(m_indexCount);
        if (!anyChanged) {
          m_originBuffer = entry.vbOrigin;
          m_axisBuffer = entry.vbAxis;
          m_flagsBuffer = entry.vbFlags;
          m_baseColorBuffer = entry.vbBaseColor;
          m_topColorBuffer = entry.vbTopColor;
          m_originOffset = entry.originOffset;
          m_axisOffset = entry.axisOffset;
          m_flagsOffset = entry.flagsOffset;
          m_baseColorOffset = entry.baseColorOffset;
          m_topColorOffset = entry.topColorOffset;
          if (entry.indexCount > 0 && entry.ib) {
            m_indexUploadBuffer = entry.ib;
            m_indexUploadOffset = entry.ibOffset;
          }
        } else {
          // Defer promotion copies to the next frame; use upload slices now.
          return;
        }
        return;
      }

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        auto originDst = m_backend.allocateStaticVB(v4Bytes, alignof(glm::vec4));
        auto axisDst = m_backend.allocateStaticVB(v4Bytes, alignof(glm::vec4));
        auto flagsDst = m_backend.allocateStaticVB(fBytes, alignof(float));
        auto baseColorDst = m_backend.allocateStaticVB(v4Bytes, alignof(glm::vec4));
        auto topColorDst = m_backend.allocateStaticVB(v4Bytes, alignof(glm::vec4));
        Z3DRendererVulkanBackend::StaticSlice ibDst{};
        if (m_indexCount > 0) {
          ibDst = m_backend.allocateStaticIB(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
        }
        if (originDst.buffer && axisDst.buffer && flagsDst.buffer && baseColorDst.buffer && topColorDst.buffer &&
            (m_indexCount == 0 || ibDst.buffer)) {
          m_backend.scheduleStaticCopy(originDst.buffer, originDst.offset, originSlice, false);
          m_backend.scheduleStaticCopy(axisDst.buffer, axisDst.offset, axisSlice, false);
          m_backend.scheduleStaticCopy(flagsDst.buffer, flagsDst.offset, flagsSlice, false);
          m_backend.scheduleStaticCopy(baseColorDst.buffer, baseColorDst.offset, baseColorSlice, false);
          m_backend.scheduleStaticCopy(topColorDst.buffer, topColorDst.offset, topColorSlice, false);
          if (m_indexCount > 0) {
            Z3DRendererVulkanBackend::UploadSlice iUpload{m_indexUploadBuffer,
                                                          m_indexUploadOffset,
                                                          nullptr,
                                                          m_indexCount * sizeof(uint32_t)};
            m_backend.scheduleStaticCopy(ibDst.buffer, ibDst.offset, iUpload, /*isIndexBuffer=*/true);
          }
          VLOG(1) << fmt::format("VK cone promote: origin={}B axis={}B flags={}B baseColor={}B topColor={}B idx={}B",
                                 v4Bytes,
                                 v4Bytes,
                                 fBytes,
                                 v4Bytes,
                                 v4Bytes,
                                 m_indexCount * sizeof(uint32_t));
          entry.vbOrigin = originDst.buffer;
          entry.vbAxis = axisDst.buffer;
          entry.vbFlags = flagsDst.buffer;
          entry.vbBaseColor = baseColorDst.buffer;
          entry.vbTopColor = topColorDst.buffer;
          entry.originOffset = originDst.offset;
          entry.axisOffset = axisDst.offset;
          entry.flagsOffset = flagsDst.offset;
          entry.baseColorOffset = baseColorDst.offset;
          entry.topColorOffset = topColorDst.offset;
          entry.ib = ibDst.buffer;
          entry.ibOffset = ibDst.offset;
          entry.promoted = true;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
        }
      }
    }
  }
}

} // namespace nim
