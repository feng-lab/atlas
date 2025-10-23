#include "zvulkanspherepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dsphererenderer.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkandescriptorset.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbindings.h"
#include "zvulkanuniforms.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zsysteminfo.h"
#include "zlog.h"
#include "zexception.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>
#include <cstring>
#include <cstdint>

namespace nim {

namespace {

float computeBoxCorrection(float fovyDegrees)
{
  if (fovyDegrees <= 90.0f) {
    return 1.0027f + 0.000111f * fovyDegrees + 0.000098f * fovyDegrees * fovyDegrees;
  }
  return 2.02082f - 0.033935f * fovyDegrees + 0.00037854f * fovyDegrees * fovyDegrees;
}

struct SpherePushConstants
{
  float sizeScale = 1.0f;
  float boxCorrection = 1.0f;
  float ortho = 0.0f;
  float weighted_a = 0.0f;
  float weighted_b = 0.0f;
  float weighted_depth_scale = 0.0f;
  float pad0 = 0.0f;
};

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

} // namespace

ZVulkanSpherePipelineContext::ZVulkanSpherePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanSpherePipelineContext::~ZVulkanSpherePipelineContext() = default;

void ZVulkanSpherePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_centerRadiusBuffer = nullptr;
  m_colorBuffer = nullptr;
  m_specularBuffer = nullptr;
  m_flagsBuffer = nullptr;
  m_centerRadiusOffset = 0;
  m_colorOffset = 0;
  m_flagsOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  // Retire per-frame UBOs so previous frames can complete safely on GPU
  // when multiple frames are in flight. Destruction is deferred until the
  // active submission fence signals.
  retainUbo(m_uboOIT);
  resetDescriptors();
  m_ddpLightingFrozen = false;
  m_ddpTransformsFrozen = false;
  m_ddpMaterialFrozen = false;
  m_ddpArgsPrepared = false;
  m_ddpArgsOffset = 0;
}

void ZVulkanSpherePipelineContext::flushRetainedUbos()
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

void ZVulkanSpherePipelineContext::resetDescriptors()
{
  m_dsPlaceholder.reset();
  m_dsLighting.reset();
  m_dsTransforms.reset();
  m_dsOIT.reset();
}

void ZVulkanSpherePipelineContext::record(Z3DRendererBase& renderer,
                                          const RenderBatch& batch,
                                          const SpherePayload& payload,
                                          const vk::Viewport& viewport,
                                          const vk::Rect2D& scissor,
                                          vk::raii::CommandBuffer& cmd)
{
  // Ensure last frame's UBOs remain alive until this submission completes.
  flushRetainedUbos();
  if (payload.pointsAndRadius.empty()) {
    return;
  }

  // GL parity: in picking pass, require picking colors sized equal to vertices.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.pointsAndRadius.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  if (m_vertexCount == 0) {
    return;
  }

  VLOG(1) << fmt::format(
    "VK sphere draw: picking={} verts={} idx={} buf(center={}, color={}, spec={}, flags={}) offs(cr={}, c={}, s={}, f={})",
    payload.pickingPass,
    m_vertexCount,
    m_indexCount,
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_centerRadiusBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_colorBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_specularBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_flagsBuffer))),
    m_centerRadiusOffset,
    m_colorOffset,
    m_specularOffset,
    m_flagsOffset);

  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = renderer.shaderHookType();

  updateLightingUBO(renderer, batch, payload, pickingPass);
  updateTransformUBO(renderer, batch, payload, pickingPass);
  // OIT params UBO for shaders that include oit_params.glslinc
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
  CHECK(m_dsLighting && m_dsTransforms) << "Sphere pipeline descriptor sets missing (lighting/transforms)";

  ZVulkanDescriptorSet* dsPlaceholderOverride = nullptr;
  const auto& hookPara = renderer.shaderHookPara();
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel && m_setPlaceholder) {
    dsPlaceholderOverride = m_backend.allocateOverrideDescriptorSet(m_setPlaceholder);
    CHECK(dsPlaceholderOverride != nullptr) << "Sphere DDP peel: override descriptor allocation failed (fatal)";
    if (dsPlaceholderOverride) {
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                   m_backend.device(),
                                                   "sphere dual-depth-peeling depth blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, depthTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, tex, m_backend.defaultSampler());
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                   m_backend.device(),
                                                   "sphere dual-depth-peeling front blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, frontTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, tex, m_backend.defaultSampler());
      }
    }
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  m_backend.validateFormatsOrCrash(formats, "sphere");

  PipelineKey key;
  key.dynamicMaterial = payload.useDynamicMaterial && !pickingPass;
  key.shaderHookType = shaderHook;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  // Draw-only recording under backend-managed segment: no attachment handling here

  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];

  CHECK(payload.params != nullptr) << "Sphere payload missing params";
  SpherePushConstants constants;
  constants.sizeScale = payload.params->sizeScale;
  constants.boxCorrection = computeBoxCorrection(glm::degrees(eyeState.fieldOfView));
  constants.ortho = eyeState.isPerspective ? 0.0f : 1.0f;
  if (shaderHook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit) {
    const float n = renderer.viewState().nearClip;
    const float f = renderer.viewState().farClip;
    const float denom = std::max(f - n, 1e-6f);
    constants.weighted_a = (f * n) / denom;
    constants.weighted_b = 0.5f * (f + n) / denom + 0.5f;
    constants.weighted_depth_scale = renderer.sceneState().weightedBlendedDepthScale;
  } else {
    constants.weighted_a = 0.0f;
    constants.weighted_b = 0.0f;
    constants.weighted_depth_scale = 0.0f;
  }

  CHECK((dsPlaceholderOverride != nullptr) || (m_dsPlaceholder != nullptr))
    << "Sphere pipeline placeholder descriptor set not initialised";
  std::vector<vk::DescriptorSet> boundSets;
  boundSets.reserve(3);
  const vk::DescriptorSet ds0 =
    dsPlaceholderOverride ? dsPlaceholderOverride->descriptorSet() : m_dsPlaceholder->descriptorSet();
  boundSets.push_back(ds0);
  boundSets.push_back(m_dsLighting->descriptorSet());
  boundSets.push_back(m_dsTransforms->descriptorSet());

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = {viewport};
  drawSpec.scissors = {scissor};
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetInputs;
  drawSpec.descriptorSets = boundSets;

  uint32_t expectedSets = static_cast<uint32_t>(boundSets.size());
  if (m_dsOIT) {
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = vkbind::kSetOITParams;
    oitBind.sets = {m_dsOIT->descriptorSet()};
    drawSpec.extraDescriptorBinds.push_back(std::move(oitBind));
    expectedSets = std::max(expectedSets, vkbind::kSetOITParams + 1);
  }
  drawSpec.expectedDescriptorSetCount = expectedSets;

  drawSpec.vertexBuffers = {m_centerRadiusBuffer, m_colorBuffer, m_flagsBuffer, m_specularBuffer};
  drawSpec.vertexOffsets = {m_centerRadiusOffset, m_colorOffset, m_flagsOffset, m_specularOffset};
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
  drawSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(SpherePushConstants));
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;
  // Dynamic offsets for (set1,b0), (set2,b0), (set2,b1)
  drawSpec.dynamicOffsets = {static_cast<uint32_t>(m_dynLightingOffset),
                             static_cast<uint32_t>(m_dynTransformsOffset),
                             static_cast<uint32_t>(m_dynMaterialOffset)};

  ZVulkanPipelineCommandRecorder recorder(cmd);
  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    // Prepare device-local indirect args during init; copies are flushed after init pass ends.
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
    // Use the prebuilt device-local args for indirect count draw
    const bool indexed = (drawSpec.indexCount > 0);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      CHECK(m_ddpArgsPrepared) << "Sphere DDP peel: args not prepared in init";
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

void ZVulkanSpherePipelineContext::ensureDescriptorLayouts()
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

void ZVulkanSpherePipelineContext::ensureDescriptorSets()
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
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  // Dynamic UBOs are already primed above; no per-frame writes needed here.
  if (m_dsOIT && m_uboOIT) {
    m_dsOIT->writeUniformBufferOnce(vkbind::kBindingOITParamsUBO, *m_uboOIT);
    if (!m_backend.isRecording()) {
      if (auto* buf = m_backend.ddpChangedFlagBufferObj()) {
        m_dsOIT->writeStorageBufferOnce(vkbind::kBindingOITDDPFlag, *buf);
      }
    }
  }
}

void ZVulkanSpherePipelineContext::ensureOITResources()
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

void ZVulkanSpherePipelineContext::updateOITParamsUBO(Z3DRendererBase& renderer,
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

void ZVulkanSpherePipelineContext::ensurePlaceholderTexture() {}

void ZVulkanSpherePipelineContext::updateLightingUBO(Z3DRendererBase& renderer,
                                                     const RenderBatch& batch,
                                                     const SpherePayload& payload,
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

void ZVulkanSpherePipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                      const RenderBatch& batch,
                                                      const SpherePayload& payload,
                                                      bool pickingPass)
{
  const auto hook = renderer.shaderHookType();
  const bool ddp = (hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                    hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  CHECK(payload.params != nullptr) << "Sphere payload missing params";

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
  material.material_specular =
    pickingPass || !payload.useDynamicMaterial ? glm::vec4(0.0f) : payload.params->materialSpecular;
  material.material_shininess = pickingPass || !payload.useDynamicMaterial ? 0.0f : payload.params->materialShininess;
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

  VLOG(2) << fmt::format("VK sphere params: sizeScale={:.3f} alpha={:.3f} picking={} ortho={}",
                         payload.params->sizeScale,
                         material.alpha,
                         pickingPass,
                         (eyeState.isPerspective ? 0 : 1));
}

ZVulkanSpherePipelineContext::PipelineInstance&
ZVulkanSpherePipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
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
        return "dual_peeling_init_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return "dual_peeling_peel_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return "wavg_init_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return "wblended_init_sphere.frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return "sphere.frag.spv";
    }
  };

  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "sphere.vert.spv",
                                                    shaderBase + selectFragmentShader(key.shaderHookType),
                                                    std::nullopt);

  // Dynamic material specialization (match GL's DYNAMIC_MATERIAL_PROPERTY)
  {
    const uint32_t useDynamic = key.dynamicMaterial ? 1u : 0u;
    std::array<vk::SpecializationMapEntry, 1> entries{
      vk::SpecializationMapEntry{.constantID = 60, .offset = 0, .size = sizeof(uint32_t)}
    };
    std::array<uint32_t, 1> data{useDynamic};
    auto entriesVec = std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end());
    auto dataVec = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(data.data()),
                                        reinterpret_cast<const uint8_t*>(data.data()) + sizeof(data));
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex, entriesVec, dataVec);
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, entriesVec, dataVec);
  }

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
  baseBlend.blendEnable = false;

  auto makeAttachments =
    [&](vk::BlendFactor srcColor, vk::BlendFactor dstColor, vk::BlendFactor srcAlpha, vk::BlendFactor dstAlpha) {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(formats.colorFormats.size(), baseBlend);
      for (auto& attachment : attachments) {
        attachment.blendEnable = true;
        attachment.srcColorBlendFactor = srcColor;
        attachment.dstColorBlendFactor = dstColor;
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        attachment.srcAlphaBlendFactor = srcAlpha;
        attachment.dstAlphaBlendFactor = dstAlpha;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
    };

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
      makeAttachments(vk::BlendFactor::eOne, vk::BlendFactor::eOne, vk::BlendFactor::eOne, vk::BlendFactor::eOne);
      instance.pipeline->setDepthTestEnable(false);
      instance.pipeline->setDepthWriteEnable(false);
      break;
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

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(SpherePushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanSpherePipelineContext::makeVertexInputState() const
{
  static std::array<vk::VertexInputBindingDescription, 4> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // centerRadius
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // color
    vk::VertexInputBindingDescription{.binding = 2,
                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // flags
    vk::VertexInputBindingDescription{.binding = 3,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // specular/shininess
  };
  static std::array<vk::VertexInputAttributeDescription, 4> attrs{
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
                                        .offset = 0                                                               }
  };
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  info.pVertexBindingDescriptions = bindings.data();
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanSpherePipelineContext::uploadGeometry(const SpherePayload& payload)
{
  m_vertexCount = payload.pointsAndRadius.size();
  m_indexCount = payload.indices.size();

  if (m_vertexCount == 0) {
    return;
  }

  CHECK(payload.flags.size() == m_vertexCount) << "Vulkan sphere backend skipping batch: flag buffer is incomplete.";

  const size_t crBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t colBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t spBytes = m_vertexCount * sizeof(glm::vec4);
  const size_t flBytes = m_vertexCount * sizeof(float);
  m_backend.reserveUploadSlices({
    {crBytes,                         alignof(glm::vec4)},
    {colBytes,                        alignof(glm::vec4)},
    {spBytes,                         alignof(glm::vec4)},
    {flBytes,                         alignof(float)    },
    {m_indexCount * sizeof(uint32_t), alignof(uint32_t) }
  });

  auto crSlice = m_backend.suballocateUpload(crBytes, alignof(glm::vec4));
  auto colSlice = m_backend.suballocateUpload(colBytes, alignof(glm::vec4));
  auto spSlice = m_backend.suballocateUpload(spBytes, alignof(glm::vec4));
  auto flSlice = m_backend.suballocateUpload(flBytes, alignof(float));
  if (!crSlice.buffer || !crSlice.mapped || !colSlice.buffer || !colSlice.mapped || !spSlice.buffer ||
      !spSlice.mapped || !flSlice.buffer || !flSlice.mapped) {
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }
  std::memcpy(crSlice.mapped, payload.pointsAndRadius.data(), crBytes);
  if (payload.pickingPass) {
    auto* cOut = static_cast<glm::vec4*>(colSlice.mapped);
    for (size_t i = 0; i < m_vertexCount; ++i) {
      cOut[i] = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
    }
  } else {
    auto* cOut = static_cast<glm::vec4*>(colSlice.mapped);
    for (size_t i = 0; i < m_vertexCount; ++i) {
      cOut[i] = (i < payload.colors.size()) ? payload.colors[i] : glm::vec4(1.0f);
    }
  }
  // Specular/shininess: per-vertex when using dynamic material; otherwise zeros
  {
    auto* spOut = static_cast<glm::vec4*>(spSlice.mapped);
    if (!payload.pickingPass && payload.useDynamicMaterial) {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        spOut[i] = (i < payload.specularAndShininess.size()) ? payload.specularAndShininess[i] : glm::vec4(0.0f);
      }
    } else {
      std::memset(spOut, 0, spBytes);
    }
  }
  auto* fOut = static_cast<float*>(flSlice.mapped);
  for (size_t i = 0; i < m_vertexCount; ++i) {
    fOut[i] = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
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
    m_indexUploadBuffer = nullptr;
    m_indexUploadOffset = 0;
  }
  m_centerRadiusBuffer = crSlice.buffer;
  m_colorBuffer = colSlice.buffer;
  m_specularBuffer = spSlice.buffer;
  m_flagsBuffer = flSlice.buffer;
  m_centerRadiusOffset = crSlice.offset;
  m_colorOffset = colSlice.offset;
  m_specularOffset = spSlice.offset;
  m_flagsOffset = flSlice.offset;

  // Attempt static promotion
  {
    CHECK(payload.streamKey != 0) << "Sphere payload missing streamKey";
    CacheKey key{payload.streamKey, payload.pickingPass, payload.useDynamicMaterial};
    auto it = m_staticCache.find(key);
    const int kPromotionThreshold = 2;
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.centersGen = payload.centersGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
      entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
      entry.specularGen = payload.specularGen;
      m_staticCache.emplace(key, entry);
    } else {
      CacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == m_vertexCount) && (entry.indexCount == m_indexCount);
      const bool gensSame = entry.centersGen == payload.centersGen && entry.flagsGen == payload.flagsGen &&
                            entry.indexGen == payload.indexGen &&
                            entry.colorsGen == (payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen) &&
                            entry.specularGen == payload.specularGen;
      entry.unchangedFrames = (sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      if (entry.promoted && sizeSame) {
        bool anyChanged = false;
        if (entry.centersGen != payload.centersGen) {
          m_backend.scheduleStaticCopy(entry.vbCenterRadius, entry.centerRadiusOffset, crSlice, false);
          anyChanged = true;
        }
        if (entry.colorsGen != (payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen)) {
          m_backend.scheduleStaticCopy(entry.vbColor, entry.colorOffset, colSlice, false);
          anyChanged = true;
        }
        if (entry.specularGen != payload.specularGen) {
          m_backend.scheduleStaticCopy(entry.vbSpecular, entry.specularOffset, spSlice, false);
          anyChanged = true;
        }
        if (entry.flagsGen != payload.flagsGen) {
          m_backend.scheduleStaticCopy(entry.vbFlags, entry.flagsOffset, flSlice, false);
          anyChanged = true;
        }
        if (entry.indexGen != payload.indexGen && m_indexUploadBuffer && m_indexCount > 0) {
          Z3DRendererVulkanBackend::UploadSlice idx{m_indexUploadBuffer,
                                                    m_indexUploadOffset,
                                                    nullptr,
                                                    m_indexCount * sizeof(uint32_t)};
          m_backend.scheduleStaticCopy(entry.ib, entry.ibOffset, idx, true);
          anyChanged = true;
        }
        // If anything changed, defer restaging to the next frame to avoid
        // hazards. This frame binds upload slices; at the next steady frame
        // promotion/restage will occur.
        if (anyChanged) {
          return;
        }
        // Safety: if we restaged any stream this frame, bind the upload slices
        // for this draw and let the static buffers take effect on the next
        // frame. This avoids driver-dependent hazards on buffer copies.
        if (!anyChanged) {
          m_centerRadiusBuffer = entry.vbCenterRadius;
          m_colorBuffer = entry.vbColor;
          m_specularBuffer = entry.vbSpecular;
          m_flagsBuffer = entry.vbFlags;
          m_centerRadiusOffset = entry.centerRadiusOffset;
          m_colorOffset = entry.colorOffset;
          m_specularOffset = entry.specularOffset;
          m_flagsOffset = entry.flagsOffset;
          if (entry.indexCount > 0 && entry.ib) {
            m_indexUploadBuffer = entry.ib;
            m_indexUploadOffset = entry.ibOffset;
          }
        }
        return;
      }

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        auto crDst = m_backend.allocateStaticVB(crBytes, alignof(glm::vec4));
        auto colDst = m_backend.allocateStaticVB(colBytes, alignof(glm::vec4));
        auto flDst = m_backend.allocateStaticVB(flBytes, alignof(float));
        auto spDst = m_backend.allocateStaticVB(spBytes, alignof(glm::vec4));
        Z3DRendererVulkanBackend::StaticSlice ibDst{};
        if (m_indexCount > 0) {
          ibDst = m_backend.allocateStaticIB(m_indexCount * sizeof(uint32_t), alignof(uint32_t));
        }
        if (crDst.buffer && colDst.buffer && flDst.buffer && spDst.buffer && (m_indexCount == 0 || ibDst.buffer)) {
          size_t staged = 0;
          m_backend.scheduleStaticCopy(crDst.buffer, crDst.offset, crSlice, false);
          staged += crBytes;
          m_backend.scheduleStaticCopy(colDst.buffer, colDst.offset, colSlice, false);
          staged += colBytes;
          m_backend.scheduleStaticCopy(spDst.buffer, spDst.offset, spSlice, false);
          staged += spBytes;
          m_backend.scheduleStaticCopy(flDst.buffer, flDst.offset, flSlice, false);
          staged += flBytes;
          if (m_indexCount > 0) {
            Z3DRendererVulkanBackend::UploadSlice idx{m_indexUploadBuffer,
                                                      m_indexUploadOffset,
                                                      nullptr,
                                                      m_indexCount * sizeof(uint32_t)};
            m_backend.scheduleStaticCopy(ibDst.buffer, ibDst.offset, idx, true);
            staged += m_indexCount * sizeof(uint32_t);
          }
          if (staged > 0) {
            m_backend.addSphereBytesStaged(staged);
          }
          entry.vbCenterRadius = crDst.buffer;
          entry.vbColor = colDst.buffer;
          entry.vbSpecular = spDst.buffer;
          entry.vbFlags = flDst.buffer;
          entry.centerRadiusOffset = crDst.offset;
          entry.colorOffset = colDst.offset;
          entry.specularOffset = spDst.offset;
          entry.flagsOffset = flDst.offset;
          entry.ib = ibDst.buffer;
          entry.ibOffset = ibDst.offset;
          entry.centersGen = payload.centersGen;
          entry.flagsGen = payload.flagsGen;
          entry.indexGen = payload.indexGen;
          entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
          entry.specularGen = payload.specularGen;
          entry.promoted = true;
          VLOG(1) << fmt::format("VK sphere promote: centerRadius={}B color={}B flags={}B idx={}B",
                                 crBytes,
                                 colBytes,
                                 flBytes,
                                 m_indexCount * sizeof(uint32_t));
          return;
        }
      }
    }
  }
}

} // namespace nim
