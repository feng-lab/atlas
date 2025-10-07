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
  float weighted_a = 0.0f;
  float weighted_b = 0.0f;
  float weighted_depth_scale = 0.0f;
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
  m_vertexUploadBuffer = VK_NULL_HANDLE;
  m_vertexUploadOffset = 0;
  m_indexUploadBuffer = VK_NULL_HANDLE;
  m_indexUploadOffset = 0;
  resetDescriptors();
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
  if (!payload.renderer || payload.baseAndRadius.empty()) {
    return;
  }

  // GL parity: in picking pass, require picking colors sized equal to vertices.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.baseAndRadius.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  if (m_vertexCount == 0) {
    return;
  }

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
  ensureDescriptorSets();
  CHECK(m_dsLighting && m_dsTransforms) << "Cone pipeline descriptor sets missing (lighting/transforms)";

  ZVulkanDescriptorSet* dsPlaceholderOverride = nullptr;
  ensurePlaceholderTexture();
  const auto& hookPara = renderer.shaderHookPara();
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel && m_setPlaceholder) {
    dsPlaceholderOverride = m_backend.allocateOverrideDescriptorSet(**m_setPlaceholder);
    CHECK(dsPlaceholderOverride != nullptr) << "Cone DDP peel: override descriptor allocation failed (fatal)";
    if (dsPlaceholderOverride) {
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                   m_backend.device(),
                                                   "cone dual-depth-peeling depth blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, depthTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPDepthBlender, tex, m_backend.defaultSampler());
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                   m_backend.device(),
                                                   "cone dual-depth-peeling front blender");
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, frontTex, m_backend.defaultSampler());
      } else {
        auto& tex = m_backend.defaultPlaceholderTexture2D();
        dsPlaceholderOverride->updateTexture(vkbind::kBindingDDPFrontBlender, tex, m_backend.defaultSampler());
      }
    }
  }

  const auto formats = vulkan::extractAttachmentFormats(batch);

  if (!m_backend.validateFormatsOrSkip(formats, "cone")) {
    return;
  }

  PipelineKey key;
  key.dynamicMaterial = !pickingPass;
  key.capsMode = toCapsMode(payload.capStyle);
  key.shaderHookType = shaderHook;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  vk::DeviceSize offsets = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_vertexUploadBuffer}, {m_vertexUploadOffset});
  if (m_indexCount > 0 && m_indexUploadBuffer) {
    cmd.bindIndexBuffer(m_indexUploadBuffer, m_indexUploadOffset, vk::IndexType::eUint32);
  }

  CHECK((dsPlaceholderOverride != nullptr) || (m_dsPlaceholder != nullptr))
    << "Cone pipeline placeholder descriptor set not initialised";
  if ((dsPlaceholderOverride || m_dsPlaceholder) && m_dsLighting && m_dsTransforms) {
    const vk::DescriptorSet ds0 = dsPlaceholderOverride ? dsPlaceholderOverride->descriptorSet()
                                                        : m_dsPlaceholder->descriptorSet();
    std::array<vk::DescriptorSet, 3> sets{ds0, m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), vkbind::kSetInputs, sets, {});
  }
  if (m_dsOIT) {
    std::array<vk::DescriptorSet, 1> ds3{m_dsOIT->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), vkbind::kSetOITParams, ds3, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];

  ConePushConstants constants;
  constants.projectionMatrix = eyeState.projectionMatrix;
  constants.ortho = eyeState.isPerspective ? 0.0f : 1.0f;
  if (shaderHook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit) {
    const float n = renderer.viewState().nearClip;
    const float f = renderer.viewState().farClip;
    const float denom = std::max(f - n, 1e-6f);
    constants.weighted_a = (f * n) / denom;
    constants.weighted_b = 0.5f * (f + n) / denom + 0.5f;
    constants.weighted_depth_scale = renderer.sceneState().weightedBlendedDepthScale;
  }
  cmd.pushConstants<ConePushConstants>(pipeline.pipeline->pipelineLayout(),
                                       vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       constants);

  if (m_indexCount > 0 && m_indexUploadBuffer) {
    cmd.drawIndexed(static_cast<uint32_t>(m_indexCount), 1, 0, 0, 0);
  } else {
    cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
  }
}

void ZVulkanConePipelineContext::ensureDescriptorLayouts()
{
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setPlaceholder) {
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
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setPlaceholder.emplace(vkDevice, createInfo);
  }

  if (!m_setLighting) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_setLighting.emplace(vkDevice, createInfo);
  }

  if (!m_setTransforms) {
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
      vk::DescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = vk::DescriptorType::eUniformBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eUniformBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags =
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setTransforms.emplace(vkDevice, createInfo);
  }

  if (!m_setOIT) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_setOIT.emplace(vkDevice, createInfo);
  }
}

void ZVulkanConePipelineContext::ensureDescriptorSets()
{
  ensureDescriptorLayouts();

  if (!m_dsPlaceholder) m_dsPlaceholder = m_backend.allocateFrameDescriptorSet(**m_setPlaceholder);
  if (!m_dsLighting) m_dsLighting = m_backend.allocateFrameDescriptorSet(**m_setLighting);
  if (!m_dsTransforms) m_dsTransforms = m_backend.allocateFrameDescriptorSet(**m_setTransforms);
  if (!m_dsOIT && m_setOIT) m_dsOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);

  ensurePlaceholderTexture();
  if (m_dsPlaceholder) {
    auto& tex = m_backend.defaultPlaceholderTexture2D();
    m_dsPlaceholder->writeTextureOnce(0, tex, m_backend.defaultSampler());
    m_dsPlaceholder->writeTextureOnce(1, tex, m_backend.defaultSampler());
  }

  // Ensure UBO buffers exist prior to recording
  auto& device = m_backend.device();
  if (!m_uboLighting) {
    m_uboLighting =
      device.createBuffer(sizeof(LightingUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_uboTransforms) {
    m_uboTransforms =
      device.createBuffer(sizeof(TransformsUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_uboMaterial) {
    m_uboMaterial =
      device.createBuffer(sizeof(MaterialUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  if (m_dsLighting && m_uboLighting) {
    m_dsLighting->writeUniformBufferOnce(0, *m_uboLighting);
  }
  if (m_dsTransforms && m_uboTransforms && m_uboMaterial) {
    m_dsTransforms->writeUniformBufferOnce(0, *m_uboTransforms);
    m_dsTransforms->writeUniformBufferOnce(1, *m_uboMaterial);
  }
  if (m_dsOIT && m_uboOIT) {
    m_dsOIT->writeUniformBufferOnce(vkbind::kBindingOITParamsUBO, *m_uboOIT);
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
    m_dsOIT = m_backend.allocateFrameDescriptorSet(**m_setOIT);
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
  auto& device = m_backend.device();
  if (!m_uboLighting) {
    m_uboLighting =
      device.createBuffer(sizeof(LightingUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
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
  const bool enableLighting = !pickingPass && payload.renderer && payload.renderer->needLighting();
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

  m_uboLighting->copyData(&lighting, sizeof(lighting));
}

void ZVulkanConePipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                    const RenderBatch& batch,
                                                    const ConePayload& payload,
                                                    bool pickingPass)
{
  (void)payload;
  auto& device = m_backend.device();
  if (!m_uboTransforms) {
    m_uboTransforms =
      device.createBuffer(sizeof(TransformsUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_uboMaterial) {
    m_uboMaterial =
      device.createBuffer(sizeof(MaterialUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  const auto& params = renderer.parameterState();

  TransformsUBOStd140 transforms{};
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  transforms.view_matrix = eyeState.viewMatrix;
  transforms.pos_transform = params.coordTransform;

  const glm::mat4 combined = eyeState.viewMatrix * params.coordTransform;
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(combined)));
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(normalMatrix);
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  transforms.parameters = glm::vec4(params.sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);
  m_uboTransforms->copyData(&transforms, sizeof(transforms));

  MaterialUBOStd140 material{};
  const auto& scene = renderer.sceneState();
  material.scene_ambient = scene.sceneAmbient;
  material.material_ambient = params.materialAmbient;
  material.material_specular = pickingPass ? glm::vec4(0.0f) : params.materialSpecular;
  material.material_shininess = pickingPass ? 0.0f : params.materialShininess;
  material.alpha = pickingPass ? 1.0f : params.opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);
  m_uboMaterial->copyData(&material, sizeof(material));
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

  auto selectFragmentShader = [](Z3DRendererBase::ShaderHookType hook) -> std::string {
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return "dual_peeling_init_cone.frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return "dual_peeling_peel_cone.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return "wavg_init_cone.frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return "wblended_init_cone.frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return "cone.frag.spv";
    }
  };

  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "cone.vert.spv",
                                                    shaderBase + selectFragmentShader(key.shaderHookType),
                                                    std::nullopt);

  std::array<vk::SpecializationMapEntry, 1> specEntries{
    vk::SpecializationMapEntry{.constantID = 90, .offset = 0, .size = sizeof(int)}
  };
  std::array<int, 1> specData{key.capsMode};
  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                         reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  std::vector<vk::DescriptorSetLayout> layouts{**m_setPlaceholder, **m_setLighting, **m_setTransforms, **m_setOIT};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

  vk::PipelineColorBlendAttachmentState baseBlend{};
  baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  baseBlend.blendEnable = VK_FALSE;

  switch (key.shaderHookType) {
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
                              .size = static_cast<uint32_t>(sizeof(ConePushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanConePipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(ConeVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 5> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, origin))   },
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, axis))     },
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 0,
                                        .format = vk::Format::eR32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, flags))    },
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, colorBase))},
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, colorTop)) }
  };

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanConePipelineContext::uploadGeometry(const ConePayload& payload)
{
  m_vertexCount = payload.baseAndRadius.size();
  m_indexCount = payload.indices.size();

  if (m_vertexCount == 0) {
    return;
  }

  if (payload.axisAndTopRadius.size() != m_vertexCount || payload.flags.size() != m_vertexCount) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan cone backend skipping batch: attribute buffers are incomplete.";
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }

  auto vSlice = m_backend.suballocateUpload(m_vertexCount * sizeof(ConeVertex), alignof(ConeVertex));
  if (!vSlice.buffer || !vSlice.mapped) {
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }
  auto* vertices = static_cast<ConeVertex*>(vSlice.mapped);
  for (size_t i = 0; i < m_vertexCount; ++i) {
    auto& vertex = vertices[i];
    vertex.origin = payload.baseAndRadius[i];
    vertex.axis = payload.axisAndTopRadius[i];
    if (i < payload.baseColors.size()) {
      vertex.colorBase = payload.baseColors[i];
    } else {
      vertex.colorBase = glm::vec4(1.0f);
    }
    if (payload.pickingPass) {
      if (i < payload.pickingColors.size()) {
        vertex.colorBase = payload.pickingColors[i];
        vertex.colorTop = payload.pickingColors[i];
      } else {
        vertex.colorTop = vertex.colorBase;
      }
    } else if (!payload.sameColorForBaseAndTop && i < payload.topColors.size()) {
      vertex.colorTop = payload.topColors[i];
    } else {
      vertex.colorTop = vertex.colorBase;
    }
    float flagsValue = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
    if (payload.useConeShader2) {
      float right = std::floor(flagsValue / 16.0f);
      float up = flagsValue - right * 16.0f;
      right = std::clamp(right, 0.0f, 1.0f);
      up = std::clamp(up, 0.0f, 1.0f);
      float forward = up;
      flagsValue = right * 256.0f + up * 16.0f + forward;
    }
    vertex.flags = flagsValue;
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
  m_vertexUploadBuffer = vSlice.buffer;
  m_vertexUploadOffset = vSlice.offset;
}

} // namespace nim
