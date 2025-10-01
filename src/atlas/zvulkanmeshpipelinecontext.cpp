#include "zvulkanmeshpipelinecontext.h"

#include "z3drenderervulkanbackend.h"
#include "z3drendererbase.h"
#include "z3drendererstates.h"
#include "z3drendercommands.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkantexture.h"
#include "zvulkanuniforms.h"
#include "zsysteminfo.h"
#include "zmesh.h"
#include "z3dtexture.h"
#include "z3dprimitiverenderer.h"
#include "z3dmeshrenderer.h"
#include "zlog.h"
#include "zexception.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace nim {
namespace {

struct MeshVertex
{
  glm::vec3 position{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  glm::vec4 color{1.0f};
  float tex1d = 0.0f;
  glm::vec2 tex2d{0.0f};
  glm::vec3 tex3d{0.0f};
};

vk::PipelineVertexInputStateCreateInfo makeMeshVertexInput()
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(MeshVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 6> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, position))},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, normal))},
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, color))},
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 0,
                                        .format = vk::Format::eR32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, tex1d))},
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, tex2d))},
    vk::VertexInputAttributeDescription{.location = 5,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(MeshVertex, tex3d))}};
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

vk::PrimitiveTopology toVkTopology(ZMesh::Type type)
{
  switch (type) {
    case ZMesh::Type::TRIANGLES:
      return vk::PrimitiveTopology::eTriangleList;
    case ZMesh::Type::TRIANGLE_STRIP:
      return vk::PrimitiveTopology::eTriangleStrip;
    case ZMesh::Type::TRIANGLE_FAN:
      return vk::PrimitiveTopology::eTriangleFan;
    default:
      return vk::PrimitiveTopology::eTriangleList;
  }
}

bool validateTexturePrerequisites(const MeshPayload& payload, const ZMesh& mesh)
{
  const size_t vertexCount = mesh.numVertices();
  switch (payload.colorSource) {
    case MeshPayload::ColorSource::Mesh1DTexture:
      return vertexCount > 0 && mesh.num1DTextureCoordinates() >= vertexCount && payload.texture != nullptr &&
             payload.texture->textureTarget() == GL_TEXTURE_1D;
    case MeshPayload::ColorSource::Mesh2DTexture:
      return vertexCount > 0 && mesh.num2DTextureCoordinates() >= vertexCount && payload.texture != nullptr &&
             payload.texture->textureTarget() == GL_TEXTURE_2D;
    case MeshPayload::ColorSource::Mesh3DTexture:
      return vertexCount > 0 && mesh.num3DTextureCoordinates() >= vertexCount && payload.texture != nullptr &&
             payload.texture->textureTarget() == GL_TEXTURE_3D;
    default:
      return true;
  }
}
const glm::vec4 kFallbackMeshColor{0.0f, 0.0f, 0.0f, 1.0f};

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

} // namespace

ZVulkanMeshPipelineContext::ZVulkanMeshPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanMeshPipelineContext::~ZVulkanMeshPipelineContext() = default;

void ZVulkanMeshPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_draws.clear();
}

void ZVulkanMeshPipelineContext::record(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const MeshPayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd)
{
  if (!payload.renderer || payload.meshes.empty()) {
    return;
  }

  uploadGeometry(payload);
  if (m_draws.empty() || m_vertexCount == 0) {
    return;
  }

  updateLightingUBO(renderer, batch, payload);
  updateTransformUBO(renderer, batch);
  ensureDescriptorSets();
  bindTextureIfNeeded(payload);

  vk::DeviceSize vertexOffset = 0;
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {vertexOffset});
  if (m_indexCount > 0 && m_indexBuffer) {
    cmd.bindIndexBuffer(m_indexBuffer->buffer(), 0, vk::IndexType::eUint32);
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  const bool drawSurface = payload.wireframeMode != MeshPayload::WireframeMode::OnlyWireframe;
  const bool drawWireframe = payload.wireframeMode != MeshPayload::WireframeMode::NoWireframe;

  const FogMode fogMode = renderer.sceneState().fog.mode;

  PipelineInstance* currentPipeline = nullptr;
  if (drawSurface) {
    for (const auto& draw : m_draws) {
      if (!draw.mesh) {
        continue;
      }

      PipelineKey key;
      key.colorSource = payload.colorSource;
      key.meshType = draw.mesh->type();
      key.wireframe = false;
      key.fogMode = fogMode;

      PipelineInstance& pipeline = ensurePipeline(key);
      if (&pipeline != currentPipeline) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
        bindDescriptorSets(cmd, pipeline);
        currentPipeline = &pipeline;
      }

      updateMaterialUBO(renderer,
                        payload,
                        draw.payloadMeshIndex,
                        draw.useFallbackColor,
                        draw.fallbackColor);

      if (draw.indexed && draw.indexCount > 0 && m_indexBuffer) {
        cmd.drawIndexed(draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0);
      } else {
        cmd.draw(draw.vertexCount, 1, draw.firstVertex, 0);
      }
    }
  }

  if (drawWireframe) {
    currentPipeline = nullptr;
    for (const auto& draw : m_draws) {
      if (!draw.mesh) {
        continue;
      }

      PipelineKey key;
      key.colorSource = payload.colorSource;
      key.meshType = draw.mesh->type();
      key.wireframe = true;
      key.fogMode = fogMode;

      PipelineInstance& pipeline = ensurePipeline(key);
      if (&pipeline != currentPipeline) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
        bindDescriptorSets(cmd, pipeline);
        currentPipeline = &pipeline;
      }

      updateMaterialUBO(renderer,
                        payload,
                        draw.payloadMeshIndex,
                        true,
                        payload.wireframeColor);

      if (draw.indexed && draw.indexCount > 0 && m_indexBuffer) {
        cmd.drawIndexed(draw.indexCount, 1, draw.firstIndex, static_cast<int32_t>(draw.firstVertex), 0);
      } else {
        cmd.draw(draw.vertexCount, 1, draw.firstVertex, 0);
      }
    }
  }
}

void ZVulkanMeshPipelineContext::ensureDescriptorLayouts()
{
  if (m_setTextures && m_setLighting && m_setTransforms) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setTextures) {
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    m_setTextures.emplace(vkDevice, createInfo);
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

void ZVulkanMeshPipelineContext::ensurePlaceholderTextures()
{
  auto& device = m_backend.device();

  if (!m_placeholder1D) {
    auto info = ZVulkanTexture::CreateInfo::make1D(1,
                                                   vk::Format::eR8G8B8A8Unorm,
                                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_placeholder1D = device.createTexture(info);
    const uint32_t pixel = 0xffffffffu;
    m_placeholder1D->uploadData(&pixel, sizeof(pixel));
  }

  if (!m_placeholder2D) {
    auto info = ZVulkanTexture::CreateInfo::make2D(1,
                                                   1,
                                                   vk::Format::eR8G8B8A8Unorm,
                                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_placeholder2D = device.createTexture(info);
    const uint32_t pixel = 0xffffffffu;
    m_placeholder2D->uploadData(&pixel, sizeof(pixel));
  }

  if (!m_placeholder3D) {
    auto info = ZVulkanTexture::CreateInfo::make3D(1,
                                                   1,
                                                   1,
                                                   vk::Format::eR8G8B8A8Unorm,
                                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_placeholder3D = device.createTexture(info);
    const uint32_t pixel = 0xffffffffu;
    m_placeholder3D->uploadData(&pixel, sizeof(pixel));
  }
}

void ZVulkanMeshPipelineContext::ensureDescriptorSets()
{
  ensureDescriptorLayouts();
  ensurePlaceholderTextures();

  auto& device = m_backend.device();

  if (!m_descriptorPool) {
    m_descriptorPool = device.createDescriptorPool();
  }

  if (!m_dsTextures) {
    auto dsTextures = m_descriptorPool->allocateDescriptorSet(**m_setTextures);
    m_dsTextures = std::make_unique<ZVulkanDescriptorSet>(device, std::move(dsTextures));
  }
  if (!m_dsLighting) {
    auto dsLighting = m_descriptorPool->allocateDescriptorSet(**m_setLighting);
    m_dsLighting = std::make_unique<ZVulkanDescriptorSet>(device, std::move(dsLighting));
  }
  if (!m_dsTransforms) {
    auto dsTransforms = m_descriptorPool->allocateDescriptorSet(**m_setTransforms);
    m_dsTransforms = std::make_unique<ZVulkanDescriptorSet>(device, std::move(dsTransforms));
  }

  if (m_placeholder1D && m_placeholder2D && m_placeholder3D && m_dsTextures) {
    m_dsTextures->updateTexture(0, *m_placeholder1D);
    m_dsTextures->updateTexture(1, *m_placeholder2D);
    m_dsTextures->updateTexture(2, *m_placeholder3D);
  }

  if (m_dsLighting && m_uboLighting) {
    m_dsLighting->updateUniformBuffer(0, *m_uboLighting);
  }
  if (m_dsTransforms && m_uboTransforms && m_uboMaterial) {
    m_dsTransforms->updateUniformBuffer(0, *m_uboTransforms);
    m_dsTransforms->updateUniformBuffer(1, *m_uboMaterial);
  }
}

void ZVulkanMeshPipelineContext::updateLightingUBO(Z3DRendererBase& renderer,
                                                   const RenderBatch& batch,
                                                   const MeshPayload& payload)
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
  lighting.lighting_enabled = lighting.numLights > 0 && payload.renderer && payload.renderer->needLighting() ? 1 : 0;

  const glm::vec2 extent = batch.pass.viewport.extent;
  if (extent.x > 0.0f && extent.y > 0.0f) {
    lighting.screen_dim_RCP = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  } else {
    const auto& viewport = renderer.frameState().viewport;
    if (viewport.z > 0 && viewport.w > 0) {
      lighting.screen_dim_RCP = glm::vec2(1.0f / viewport.z, 1.0f / viewport.w);
    }
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

void ZVulkanMeshPipelineContext::updateTransformUBO(Z3DRendererBase& renderer, const RenderBatch& batch)
{
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

  TransformsUBOStd140 transforms{};
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  transforms.view_matrix = eyeState.viewMatrix;
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  transforms.pos_transform = renderer.parameterState().coordTransform;

  const glm::mat4 combined = eyeState.viewMatrix * transforms.pos_transform;
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(combined)));
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(normalMatrix);
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  const auto& params = renderer.parameterState();
  transforms.parameters = glm::vec4(params.sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);

  m_uboTransforms->copyData(&transforms, sizeof(transforms));

  MaterialUBOStd140 material{};
  const auto& scene = renderer.sceneState();
  material.scene_ambient = scene.sceneAmbient;
  material.material_ambient = params.materialAmbient;
  material.material_specular = params.materialSpecular;
  material.material_shininess = params.materialShininess;
  material.alpha = params.opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);
  m_uboMaterial->copyData(&material, sizeof(material));
}

void ZVulkanMeshPipelineContext::updateMaterialUBO(Z3DRendererBase& renderer,
                                                    const MeshPayload& payload,
                                                    size_t meshIndex,
                                                    bool useFallbackColor,
                                                    const glm::vec4& fallbackColor)
{
  if (!m_uboMaterial) {
    return;
  }

  MaterialUBOStd140 material{};
  const auto& scene = renderer.sceneState();
  const auto& params = renderer.parameterState();
  material.scene_ambient = scene.sceneAmbient;
  material.material_ambient = params.materialAmbient;
  material.material_specular = params.materialSpecular;
  material.material_shininess = params.materialShininess;
  material.alpha = params.opacity;

  bool useCustomColor = false;
  glm::vec4 colorValue = fallbackColor;

  if (useFallbackColor) {
    useCustomColor = true;
  } else if (payload.colorSource == MeshPayload::ColorSource::CustomColor &&
             meshIndex < payload.meshColors.size()) {
    useCustomColor = true;
    colorValue = payload.meshColors[meshIndex];
  }

  material.use_custom_color = useCustomColor ? 1 : 0;
  material.custom_color = colorValue;

  m_uboMaterial->copyData(&material, sizeof(material));
}

void ZVulkanMeshPipelineContext::bindDescriptorSets(vk::raii::CommandBuffer& cmd, const PipelineInstance& pipeline) const
{
  if (!m_dsTextures || !m_dsLighting || !m_dsTransforms) {
    return;
  }

  std::array<vk::DescriptorSet, 3> sets{m_dsTextures->descriptorSet(),
                                        m_dsLighting->descriptorSet(),
                                        m_dsTransforms->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
}

ZVulkanMeshPipelineContext::PipelineInstance&
ZVulkanMeshPipelineContext::ensurePipeline(const PipelineKey& key)
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
                                                    shaderBase + "mesh.vert.spv",
                                                    shaderBase + "mesh.frag.spv",
                                                    std::nullopt);

  const uint32_t useMeshColor = key.colorSource == MeshPayload::ColorSource::MeshColor ? 1u : 0u;
  const uint32_t use1D = key.colorSource == MeshPayload::ColorSource::Mesh1DTexture ? 1u : 0u;
  const uint32_t use2D = key.colorSource == MeshPayload::ColorSource::Mesh2DTexture ? 1u : 0u;
  const uint32_t use3D = key.colorSource == MeshPayload::ColorSource::Mesh3DTexture ? 1u : 0u;

  std::array<vk::SpecializationMapEntry, 4> vertexEntries{
    vk::SpecializationMapEntry{.constantID = 40, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 41, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 42, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 43, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)}};
  std::array<uint32_t, 4> vertexData{useMeshColor, use1D, use2D, use3D};
  const uint8_t* vertexPtr = reinterpret_cast<const uint8_t*>(vertexData.data());
  std::vector<uint8_t> vertexBytes(vertexPtr, vertexPtr + sizeof(vertexData));
  std::vector<vk::SpecializationMapEntry> vertexSpecs(vertexEntries.begin(), vertexEntries.end());
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex, vertexSpecs, vertexBytes);

  const uint32_t useLinearFog = key.fogMode == FogMode::Linear ? 1u : 0u;
  const uint32_t useExpFog = key.fogMode == FogMode::Exponential ? 1u : 0u;
  const uint32_t useExp2Fog = key.fogMode == FogMode::ExponentialSquared ? 1u : 0u;

  std::array<vk::SpecializationMapEntry, 7> fragmentEntries{
    vk::SpecializationMapEntry{.constantID = 40, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 41, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 42, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 43, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 20, .offset = 4 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 21, .offset = 5 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 22, .offset = 6 * sizeof(uint32_t), .size = sizeof(uint32_t)}};
  std::array<uint32_t, 7> fragmentData{useMeshColor, use1D, use2D, use3D, useLinearFog, useExpFog, useExp2Fog};
  const uint8_t* fragmentPtr = reinterpret_cast<const uint8_t*>(fragmentData.data());
  std::vector<uint8_t> fragmentBytes(fragmentPtr, fragmentPtr + sizeof(fragmentData));
  std::vector<vk::SpecializationMapEntry> fragmentSpecs(fragmentEntries.begin(), fragmentEntries.end());
  instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, fragmentSpecs, fragmentBytes);

  auto vertexInput = makeMeshVertexInput();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, toVkTopology(key.meshType));
  std::vector<vk::DescriptorSetLayout> layouts{**m_setTextures, **m_setLighting, **m_setTransforms};
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);

  if (key.wireframe) {
    instance.pipeline->setPolygonMode(vk::PolygonMode::eLine);
    instance.pipeline->setDepthBias(true, -1.0f, -1.0f);
    instance.pipeline->setLineWidth(1.0f);
  }

  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

void ZVulkanMeshPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(MeshVertex);
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

void ZVulkanMeshPipelineContext::ensureIndexCapacity(size_t indexCount)
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

void ZVulkanMeshPipelineContext::uploadGeometry(const MeshPayload& payload)
{
  m_draws.clear();
  m_vertexCount = 0;
  m_indexCount = 0;

  if (payload.meshes.empty()) {
    return;
  }

  if (payload.colorSource == MeshPayload::ColorSource::CustomColor &&
      payload.meshColors.size() < payload.meshes.size()) {
    LOG_FIRST_N(WARNING, 5) << "Vulkan mesh backend skipping batch: custom color array is incomplete.";
    return;
  }

  size_t totalVertices = 0;
  size_t totalIndices = 0;

  for (size_t i = 0; i < payload.meshes.size(); ++i) {
    ZMesh* mesh = payload.meshes[i];
    if (!mesh) {
      continue;
    }

    if (!validateTexturePrerequisites(payload, *mesh)) {
      LOG_FIRST_N(WARNING, 5) << "Vulkan mesh backend skipping batch: texture prerequisites not met.";
      m_draws.clear();
      return;
    }

    if (mesh->numVertices() == 0) {
      continue;
    }

    if (mesh->numNormals() != mesh->numVertices()) {
      mesh->generateNormals();
    }

    totalVertices += mesh->numVertices();
    if (!mesh->indices().empty()) {
      totalIndices += mesh->indices().size();
    }
  }

  if (totalVertices == 0) {
    return;
  }

  ensureVertexCapacity(totalVertices);
  if (totalIndices > 0) {
    ensureIndexCapacity(totalIndices);
  }

  auto* vertexPtr = reinterpret_cast<MeshVertex*>(m_vertexBuffer->map(0, totalVertices * sizeof(MeshVertex)));
  uint32_t* indexPtr = nullptr;
  if (totalIndices > 0 && m_indexBuffer) {
    indexPtr = reinterpret_cast<uint32_t*>(m_indexBuffer->map(0, totalIndices * sizeof(uint32_t)));
  }

  size_t vertexCursor = 0;
  size_t indexCursor = 0;

  for (size_t meshIdx = 0; meshIdx < payload.meshes.size(); ++meshIdx) {
    ZMesh* mesh = payload.meshes[meshIdx];
    if (!mesh || mesh->numVertices() == 0) {
      continue;
    }

    MeshDraw draw{};
    draw.mesh = mesh;
    draw.payloadMeshIndex = meshIdx;
    draw.firstVertex = static_cast<uint32_t>(vertexCursor);
    draw.vertexCount = static_cast<uint32_t>(mesh->numVertices());

    const auto& positions = mesh->vertices();
    const auto& normals = mesh->normals();
    const auto& colors = mesh->colors();
    const auto& tex1D = mesh->textureCoordinates1D();
    const auto& tex2D = mesh->textureCoordinates2D();
    const auto& tex3D = mesh->textureCoordinates3D();

    const bool hasVertexColors = payload.colorSource == MeshPayload::ColorSource::MeshColor &&
                                 colors.size() >= mesh->numVertices();
    const bool fallbackColorNeeded =
      payload.colorSource == MeshPayload::ColorSource::MeshColor && !hasVertexColors;

    for (size_t v = 0; v < mesh->numVertices(); ++v) {
      MeshVertex& dst = vertexPtr[vertexCursor + v];
      dst.position = positions[v];
      dst.normal = normals.size() > v ? normals[v] : glm::vec3(0.0f, 0.0f, 1.0f);
      if (hasVertexColors) {
        dst.color = colors[v];
      } else {
        dst.color = glm::vec4(1.0f);
      }

      if (payload.colorSource == MeshPayload::ColorSource::Mesh1DTexture && tex1D.size() > v) {
        dst.tex1d = tex1D[v];
      }
      if (payload.colorSource == MeshPayload::ColorSource::Mesh2DTexture && tex2D.size() > v) {
        dst.tex2d = tex2D[v];
      }
      if (payload.colorSource == MeshPayload::ColorSource::Mesh3DTexture && tex3D.size() > v) {
        dst.tex3d = tex3D[v];
      }
    }

    vertexCursor += mesh->numVertices();

    const auto& indices = mesh->indices();
    if (!indices.empty() && indexPtr) {
      draw.indexed = true;
      draw.firstIndex = static_cast<uint32_t>(indexCursor);
      draw.indexCount = static_cast<uint32_t>(indices.size());
      for (size_t idx = 0; idx < indices.size(); ++idx) {
        indexPtr[indexCursor + idx] = static_cast<uint32_t>(indices[idx]) + draw.firstVertex;
      }
      indexCursor += indices.size();
    }

    draw.useFallbackColor = fallbackColorNeeded;
    draw.fallbackColor = fallbackColorNeeded ? kFallbackMeshColor : glm::vec4(1.0f);

    m_draws.push_back(draw);
  }

  m_vertexBuffer->unmap();
  if (indexPtr && m_indexBuffer) {
    m_indexBuffer->unmap();
  }

  m_vertexCount = totalVertices;
  m_indexCount = totalIndices;
}

std::optional<ZVulkanMeshPipelineContext::TextureBinding>
ZVulkanMeshPipelineContext::bindTextureIfNeeded(const MeshPayload& payload)
{
  if (!m_dsTextures || !payload.texture) {
    return std::nullopt;
  }

  ZVulkanTexture* vkTexture = ensureTextureUpload(*payload.texture);
  if (!vkTexture) {
    return std::nullopt;
  }

  switch (payload.colorSource) {
    case MeshPayload::ColorSource::Mesh1DTexture:
      m_dsTextures->updateTexture(0, *vkTexture);
      return TextureBinding{vkTexture, 0};
    case MeshPayload::ColorSource::Mesh2DTexture:
      m_dsTextures->updateTexture(1, *vkTexture);
      return TextureBinding{vkTexture, 1};
    case MeshPayload::ColorSource::Mesh3DTexture:
      m_dsTextures->updateTexture(2, *vkTexture);
      return TextureBinding{vkTexture, 2};
    default:
      return std::nullopt;
  }
}

ZVulkanTexture* ZVulkanMeshPipelineContext::ensureTextureUpload(const Z3DTexture& source)
{
  const auto it = m_textureCache.find(&source);
  if (it != m_textureCache.end()) {
    return it->second.get();
  }

  std::optional<vk::Format> format;
  if (source.dataFormat() == GL_RGBA && source.dataType() == GL_UNSIGNED_BYTE) {
    format = vk::Format::eR8G8B8A8Unorm;
  }

  if (!format) {
    LOG_FIRST_N(WARNING, 5) << "Skipping unsupported mesh texture format for Vulkan backend.";
    return nullptr;
  }

  const size_t byteSize = source.textureSizeOnGPU();
  if (byteSize == 0) {
    return nullptr;
  }

  std::vector<uint8_t> pixels(byteSize);
  source.downloadTextureToBuffer(source.dataFormat(), source.dataType(), pixels.data());

  auto& device = m_backend.device();
  std::unique_ptr<ZVulkanTexture> vkTexture;

  switch (source.textureTarget()) {
    case GL_TEXTURE_1D: {
      auto info = ZVulkanTexture::CreateInfo::make1D(static_cast<uint32_t>(source.width()),
                                                     *format,
                                                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                                     vk::MemoryPropertyFlagBits::eDeviceLocal);
      vkTexture = device.createTexture(info);
      break;
    }
    case GL_TEXTURE_2D: {
      auto info = ZVulkanTexture::CreateInfo::make2D(static_cast<uint32_t>(source.width()),
                                                     static_cast<uint32_t>(source.height()),
                                                     *format,
                                                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                                     vk::MemoryPropertyFlagBits::eDeviceLocal);
      vkTexture = device.createTexture(info);
      break;
    }
    case GL_TEXTURE_3D: {
      auto info = ZVulkanTexture::CreateInfo::make3D(static_cast<uint32_t>(source.width()),
                                                     static_cast<uint32_t>(source.height()),
                                                     static_cast<uint32_t>(source.depth()),
                                                     *format,
                                                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                                     vk::MemoryPropertyFlagBits::eDeviceLocal);
      vkTexture = device.createTexture(info);
      break;
    }
    default:
      LOG_FIRST_N(WARNING, 3) << "Unsupported texture target for Vulkan mesh pipeline.";
      return nullptr;
  }

  if (!vkTexture) {
    return nullptr;
  }

  vkTexture->uploadData(pixels.data(), pixels.size());

  auto [inserted, _] = m_textureCache.emplace(&source, std::move(vkTexture));
  return inserted->second.get();
}

} // namespace nim
