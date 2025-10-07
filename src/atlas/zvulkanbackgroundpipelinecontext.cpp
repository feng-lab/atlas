#include "zvulkanbackgroundpipelinecontext.h"

#include "z3dbackgroundrenderer.h"
#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkandescriptorset.h"
#include "zvulkanbuffer.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanuniforms.h"
#include "zsysteminfo.h"
#include "zexception.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace nim {

namespace {

struct BackgroundPushConstants
{
  glm::vec2 screen_dim_RCP{0.0f};
  glm::vec2 pad0{0.0f};
  glm::vec4 color1{1.0f};
  glm::vec4 color2{0.0f};
  glm::vec4 region{0.0f, 1.0f, 0.0f, 1.0f};
};

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

} // namespace

ZVulkanBackgroundPipelineContext::ZVulkanBackgroundPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanBackgroundPipelineContext::~ZVulkanBackgroundPipelineContext() = default;

void ZVulkanBackgroundPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  resetDescriptors();
}

void ZVulkanBackgroundPipelineContext::resetDescriptors()
{
  m_dsLighting.reset();
  m_dsTransforms.reset();
}

void ZVulkanBackgroundPipelineContext::record(Z3DRendererBase& renderer,
                                              const RenderBatch& batch,
                                              const BackgroundPayload& payload,
                                              const vk::Viewport& viewport,
                                              const vk::Rect2D& scissor,
                                              vk::raii::CommandBuffer& cmd)
{
  if (!payload.renderer) {
    return;
  }

  // Shared fullscreen quad geometry
  m_vertexCount = 4;

  updateLightingUBO(renderer, batch, payload);
  updateTransformUBO(renderer, batch, payload);
  ensureDescriptorSets();
  CHECK(m_dsLighting && m_dsTransforms) << "Background pipeline descriptor sets missing (lighting/transforms)";

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.mode = payload.mode;
  key.orientation = payload.orientation;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  auto& quad = m_backend.fullscreenQuadVertexBuffer();
  cmd.bindVertexBuffers(0, {quad.buffer()}, {vk::DeviceSize(0)});

  std::array<vk::DescriptorSet, 2> sets{m_dsLighting->descriptorSet(), m_dsTransforms->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 1, sets, {});

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  glm::vec2 extent(viewport.width, viewport.height);
  if (extent.x <= 0.f || extent.y <= 0.f) {
    const auto& viewportRect = renderer.frameState().viewport;
    extent = glm::vec2(static_cast<float>(viewportRect.z), static_cast<float>(viewportRect.w));
  }

  BackgroundPushConstants constants;
  if (extent.x > 0.f && extent.y > 0.f) {
    constants.screen_dim_RCP = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  }
  constants.color1 = payload.color1;
  constants.color2 = payload.color2;
  constants.region = payload.region;

  cmd.pushConstants<BackgroundPushConstants>(pipeline.pipeline->pipelineLayout(),
                                             vk::ShaderStageFlagBits::eFragment,
                                             0,
                                             constants);

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanBackgroundPipelineContext::ensureDescriptorLayouts()
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
}

void ZVulkanBackgroundPipelineContext::ensureDescriptorSets()
{
  ensureDescriptorLayouts();

  if (!m_dsLighting) {
    m_dsLighting = m_backend.allocateFrameDescriptorSet(**m_setLighting);
  }

  if (!m_dsTransforms) {
    m_dsTransforms = m_backend.allocateFrameDescriptorSet(**m_setTransforms);
  }

  // Ensure UBO buffers exist even before per-batch updates
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

  if (m_dsLighting && m_uboLighting) {
    m_dsLighting->writeUniformBufferOnce(0, *m_uboLighting);
  }
  if (m_dsTransforms && m_uboTransforms && m_uboMaterial) {
    m_dsTransforms->writeUniformBufferOnce(0, *m_uboTransforms);
    m_dsTransforms->writeUniformBufferOnce(1, *m_uboMaterial);
  }
}

void ZVulkanBackgroundPipelineContext::updateLightingUBO(Z3DRendererBase& renderer,
                                                         const RenderBatch& batch,
                                                         const BackgroundPayload& payload)
{
  (void)payload;
  auto& device = m_backend.device();
  if (!m_uboLighting) {
    m_uboLighting =
      device.createBuffer(sizeof(LightingUBOStd140),
                          vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }

  LightingUBOStd140 lighting{};
  const auto& scene = renderer.sceneState();

  lighting.numLights = 0;
  lighting.lighting_enabled = 0;

  glm::vec2 extent = batch.pass.viewport.extent;
  if (extent.x <= 0.f || extent.y <= 0.f) {
    const auto& viewportRect = renderer.frameState().viewport;
    extent = glm::vec2(static_cast<float>(viewportRect.z), static_cast<float>(viewportRect.w));
  }
  if (extent.x > 0.f && extent.y > 0.f) {
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

  m_uboLighting->copyData(&lighting, sizeof(lighting));
}

void ZVulkanBackgroundPipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                          const RenderBatch& batch,
                                                          const BackgroundPayload& payload)
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
  material.scene_ambient = renderer.sceneState().sceneAmbient;
  material.material_ambient = params.materialAmbient;
  material.material_specular = glm::vec4(0.0f);
  material.material_shininess = 0.0f;
  material.alpha = 1.0f;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);

  m_uboMaterial->copyData(&material, sizeof(material));
}

ZVulkanBackgroundPipelineContext::PipelineInstance&
ZVulkanBackgroundPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
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
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "background.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  std::vector<vk::DescriptorSetLayout> layouts{**m_setPlaceholder, **m_setLighting, **m_setTransforms};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  // Background should not participate in depth testing/writes
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);

  std::array<vk::SpecializationMapEntry, 5> entries{
    vk::SpecializationMapEntry{.constantID = 30, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 31, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 32, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 33, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 34, .offset = 4 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };

  const bool isUniform = key.mode == BackgroundMode::Uniform;
  const bool gradientL2R =
    key.mode == BackgroundMode::Gradient && key.orientation == BackgroundGradientOrientation::LeftToRight;
  const bool gradientR2L =
    key.mode == BackgroundMode::Gradient && key.orientation == BackgroundGradientOrientation::RightToLeft;
  const bool gradientT2B =
    key.mode == BackgroundMode::Gradient && key.orientation == BackgroundGradientOrientation::TopToBottom;
  const bool gradientB2T =
    key.mode == BackgroundMode::Gradient && key.orientation == BackgroundGradientOrientation::BottomToTop;

  std::array<uint32_t, 5> specData{static_cast<uint32_t>(isUniform),
                                   static_cast<uint32_t>(gradientL2R),
                                   static_cast<uint32_t>(gradientR2L),
                                   static_cast<uint32_t>(gradientT2B),
                                   static_cast<uint32_t>(gradientB2T)};

  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                         reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(BackgroundPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanBackgroundPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(BackgroundVertex)),
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

void ZVulkanBackgroundPipelineContext::ensureVertexCapacity(size_t) {}
void ZVulkanBackgroundPipelineContext::uploadGeometry() {}

} // namespace nim
