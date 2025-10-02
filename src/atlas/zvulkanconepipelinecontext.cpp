#include "zvulkanconepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3drendererstates.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkanbuffer.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanuniforms.h"
#include "zsysteminfo.h"
#include "zlog.h"
#include "z3dconerenderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

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
  float pad0 = 0.0f;
  float pad1 = 0.0f;
  float pad2 = 0.0f;
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

  uploadGeometry(payload);
  if (m_vertexCount == 0) {
    return;
  }

  const bool pickingPass = payload.pickingPass;

  updateLightingUBO(renderer, batch, payload, pickingPass);
  updateTransformUBO(renderer, batch, payload, pickingPass);
  ensureDescriptorSets();

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.dynamicMaterial = !pickingPass;
  key.capsMode = toCapsMode(payload.capStyle);
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  vk::DeviceSize offsets = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offsets});
  if (m_indexCount > 0 && m_indexBuffer) {
    cmd.bindIndexBuffer(m_indexBuffer->buffer(), 0, vk::IndexType::eUint32);
  }

  if (m_dsLighting && m_dsTransforms) {
    std::array<vk::DescriptorSet, 2> sets{m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 1, sets, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];

  ConePushConstants constants;
  constants.projectionMatrix = eyeState.projectionMatrix;
  constants.ortho = eyeState.isPerspective ? 0.0f : 1.0f;
  cmd.pushConstants<ConePushConstants>(pipeline.pipeline->pipelineLayout(),
                                       vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       constants);

  if (m_indexCount > 0 && m_indexBuffer) {
    cmd.drawIndexed(static_cast<uint32_t>(m_indexCount), 1, 0, 0, 0);
  } else {
    cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
  }
}

void ZVulkanConePipelineContext::ensureDescriptorLayouts()
{
  if (m_setPlaceholder && m_setLighting && m_setTransforms) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setPlaceholder) {
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 0, .pBindings = nullptr};
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
                                     .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
      vk::DescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = vk::DescriptorType::eUniformBuffer,
                                     .descriptorCount = 1,
                                     .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment}};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setTransforms.emplace(vkDevice, createInfo);
  }
}

void ZVulkanConePipelineContext::ensureDescriptorSets()
{
  ensureDescriptorLayouts();

  auto& device = m_backend.device();
  if (!m_descriptorPool) {
    m_descriptorPool = device.createDescriptorPool();
  }

  if (!m_dsLighting) {
    auto dsLighting = m_descriptorPool->allocateDescriptorSet(**m_setLighting);
    m_dsLighting = std::make_unique<ZVulkanDescriptorSet>(device, std::move(dsLighting));
  }

  if (!m_dsTransforms) {
    auto dsTransforms = m_descriptorPool->allocateDescriptorSet(**m_setTransforms);
    m_dsTransforms = std::make_unique<ZVulkanDescriptorSet>(device, std::move(dsTransforms));
  }

  if (m_dsLighting && m_uboLighting) {
    m_dsLighting->updateUniformBuffer(0, *m_uboLighting);
  }
  if (m_dsTransforms && m_uboTransforms && m_uboMaterial) {
    m_dsTransforms->updateUniformBuffer(0, *m_uboTransforms);
    m_dsTransforms->updateUniformBuffer(1, *m_uboMaterial);
  }
}

void ZVulkanConePipelineContext::updateLightingUBO(Z3DRendererBase& renderer,
                                                   const RenderBatch& batch,
                                                   const ConePayload& payload,
                                                   bool pickingPass)
{
  auto& device = m_backend.device();
  if (!m_uboLighting) {
    m_uboLighting = device.createBuffer(sizeof(LightingUBOStd140),
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
  lighting.fog_scale = scene.fog.range.y > scene.fog.range.x ?
                         1.0f / std::max(scene.fog.range.y - scene.fog.range.x, 1e-6f) :
                         0.0f;
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
    m_uboTransforms = device.createBuffer(sizeof(TransformsUBOStd140),
                                          vk::BufferUsageFlagBits::eUniformBuffer,
                                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_uboMaterial) {
    m_uboMaterial = device.createBuffer(sizeof(MaterialUBOStd140),
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
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "cone.vert.spv",
                                                    shaderBase + "cone.frag.spv",
                                                    std::nullopt);

  std::array<vk::SpecializationMapEntry, 1> specEntries{
    vk::SpecializationMapEntry{.constantID = 90, .offset = 0, .size = sizeof(int)}};
  std::array<int, 1> specData{key.capsMode};
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                              std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end()),
                                              std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                                                                   reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  std::vector<vk::DescriptorSetLayout> layouts{**m_setPlaceholder, **m_setLighting, **m_setTransforms};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);

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
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, origin))},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, axis))},
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 0,
                                        .format = vk::Format::eR32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, flags))},
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, colorBase))},
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ConeVertex, colorTop))}};

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanConePipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(ConeVertex);
  if (requiredBytes <= m_vertexCapacity) {
    return;
  }

  size_t newCapacity = std::max(requiredBytes, m_vertexCapacity == 0 ? requiredBytes : m_vertexCapacity * 2);
  auto& device = m_backend.device();
  m_vertexBuffer = device.createBuffer(newCapacity,
                                       vk::BufferUsageFlagBits::eVertexBuffer,
                                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_vertexCapacity = newCapacity;
}

void ZVulkanConePipelineContext::ensureIndexCapacity(size_t indexCount)
{
  const size_t requiredBytes = indexCount * sizeof(uint32_t);
  if (requiredBytes <= m_indexCapacity) {
    return;
  }

  size_t newCapacity = std::max(requiredBytes, m_indexCapacity == 0 ? requiredBytes : m_indexCapacity * 2);
  auto& device = m_backend.device();
  m_indexBuffer = device.createBuffer(newCapacity,
                                      vk::BufferUsageFlagBits::eIndexBuffer,
                                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_indexCapacity = newCapacity;
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

  ensureVertexCapacity(m_vertexCount);
  if (m_indexCount > 0) {
    ensureIndexCapacity(m_indexCount);
  }

  auto* vertices = reinterpret_cast<ConeVertex*>(m_vertexBuffer->map(0, m_vertexCount * sizeof(ConeVertex)));
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
    vertex.flags = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
  }
  m_vertexBuffer->unmap();

  if (m_indexCount > 0 && m_indexBuffer) {
    m_indexBuffer->copyData(payload.indices.data(), m_indexCount * sizeof(uint32_t));
  }
}

} // namespace nim
