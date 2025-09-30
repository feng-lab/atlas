#include "z3drenderervulkanbackend.h"

#include "z3drendererbase.h"
#include "z3drendercommands.h"
#include "z3dshaderprogram.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanswapchain.h"
#include "zvulkantexture.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "z3drendertarget.h"
#include "z3dscratchresourcepool.h"
#include "zsysteminfo.h"

#include <algorithm>
#include <array>
#include <map>
#include <string_view>
#include <tuple>
#include <vector>

namespace nim {

namespace {

struct LightingUBOStd140 {
  alignas(4) int lighting_enabled = 0;
  alignas(4) int numLights = 0;
  alignas(8) glm::vec2 _pad0{0.0f};
  alignas(16) glm::vec3 fog_color_top{0.0f};
  alignas(4) float fog_end = 0.0f;
  alignas(16) glm::vec3 fog_color_bottom{0.0f};
  alignas(4) float fog_scale = 0.0f;
  alignas(8) float fog_density_log2e = 0.0f;
  alignas(8) float fog_density_density_log2e = 0.0f;
  alignas(8) glm::vec2 screen_dim_RCP{0.0f};
  alignas(8) glm::vec2 _pad1{0.0f};
};

struct TransformsUBOStd140 {
  glm::mat4 projection_view_matrix{1.0f};
  glm::mat4 view_matrix{1.0f};
  glm::mat4 pos_transform{1.0f};
  glm::mat4 pos_transform_normal_matrix{1.0f};
};

struct MaterialUBOStd140 {
  glm::vec4 scene_ambient{0.0f};
  glm::vec4 material_ambient{1.0f};
  glm::vec4 material_specular{0.0f};
  float material_shininess = 32.0f;
  float alpha = 1.0f;
  int use_custom_color = 0;
  float _pad0 = 0.0f;
  glm::vec4 custom_color{1.0f};
};

struct ThinVertex
{
  glm::vec3 pos;
  glm::vec4 color;
};

vk::PipelineVertexInputStateCreateInfo makeWideVertexInput()
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                    .stride = static_cast<uint32_t>(sizeof(LineWideVertex)),
                                                    .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 5> attrs{
    vk::VertexInputAttributeDescription{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(LineWideVertex, p1))},
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(LineWideVertex, c0))},
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(LineWideVertex, c1))},
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 0,
                                        .format = vk::Format::eR32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(LineWideVertex, flags))}};
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

vk::PipelineVertexInputStateCreateInfo makeThinVertexInput()
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                    .stride = static_cast<uint32_t>(sizeof(ThinVertex)),
                                                    .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 2> attrs{
    vk::VertexInputAttributeDescription{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(ThinVertex, color))}};
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

float resolveWideLineWidth(float srcWidth)
{
  return std::max(1.f, srcWidth) - 0.9f;
}

} // namespace

class Z3DRendererVulkanBackend::LinePipelineContext
{
public:
  explicit LinePipelineContext(Z3DRendererVulkanBackend& backend);

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const LinePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

  void resetFrame();

private:
  struct PipelineKey
  {
    bool useSmooth = true;
    bool picking = false;
    bool roundCap = true;
    bool screenAligned = false;
    bool useTextureColor = false;
    bool lineStrip = false;

    auto tie() const
    {
      return std::tie(useSmooth, picking, roundCap, screenAligned, useTextureColor, lineStrip);
    }

    bool operator<(const PipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct PipelineInstance
  {
    std::unique_ptr<ZVulkanShader> shader;
    std::unique_ptr<ZVulkanPipeline> pipeline;
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::optional<vk::raii::DescriptorSetLayout> m_setTexture;
  std::optional<vk::raii::DescriptorSetLayout> m_setLighting;
  std::optional<vk::raii::DescriptorSetLayout> m_setTransforms;
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTexture;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;

  std::unique_ptr<ZVulkanTexture> m_placeholderTexture;
  std::optional<vk::raii::Sampler> m_sampler;

  std::unique_ptr<ZVulkanBuffer> m_uboLighting;
  std::unique_ptr<ZVulkanBuffer> m_uboTransforms;
  std::unique_ptr<ZVulkanBuffer> m_uboMaterial;

  std::unique_ptr<ZVulkanBuffer> m_wideVertexBuffer;
  std::unique_ptr<ZVulkanBuffer> m_wideIndexBuffer;
  std::unique_ptr<ZVulkanBuffer> m_thinVertexBuffer;
  size_t m_wideVertexCapacity = 0;
  size_t m_wideIndexCapacity = 0;
  size_t m_thinVertexCapacity = 0;

  std::vector<LineWideVertex> m_wideVertices;
  std::vector<uint32_t> m_wideIndices;
  std::vector<ThinVertex> m_thinVertices;

  void ensureDescriptorLayouts();
  void ensurePlaceholderTexture();
  void ensureDescriptorSets(Z3DRendererBase& renderer);
  void updateUBOs(Z3DRendererBase& renderer, const RenderBatch& batch);
  PipelineInstance& ensurePipeline(const PipelineKey& key, const LinePayload& payload);
  void bindDescriptorSets(vk::raii::CommandBuffer& cmd, const PipelineInstance& pipeline) const;
  void uploadWideGeometry(const LinePayload& payload, bool pickingPass);
  void uploadThinGeometry(const LinePayload& payload, bool pickingPass);
};

LinePipelineContext::LinePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

void LinePipelineContext::resetFrame()
{
  // Currently no per-frame state to reset.
}

void LinePipelineContext::ensureDescriptorLayouts()
{
  if (m_setTexture && m_setLighting && m_setTransforms) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setTexture) {
    vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = 1, .pBindings = &binding};
    m_setTexture.emplace(vkDevice, createInfo);
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

void LinePipelineContext::ensurePlaceholderTexture()
{
  auto& device = m_backend.device();

  if (!m_placeholderTexture) {
    m_placeholderTexture = device.createTexture(1,
                                                1,
                                                vk::Format::eR8G8B8A8Unorm,
                                                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                                vk::MemoryPropertyFlagBits::eDeviceLocal);
    uint32_t pixel = 0xffffffffu;
    m_placeholderTexture->uploadData(&pixel, sizeof(pixel));
  }

  if (!m_sampler) {
    vk::SamplerCreateInfo samplerInfo{.magFilter = vk::Filter::eLinear,
                                      .minFilter = vk::Filter::eLinear,
                                      .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                      .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                      .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                      .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                      .borderColor = vk::BorderColor::eIntOpaqueWhite};
    m_sampler.emplace(device.context().device(), samplerInfo);
  }
}

void LinePipelineContext::ensureDescriptorSets(Z3DRendererBase& renderer)
{
  (void)renderer;
  ensureDescriptorLayouts();
  auto& device = m_backend.device();

  if (!m_descriptorPool) {
    m_descriptorPool = device.createDescriptorPool();
  }

  if (!m_dsLighting || !m_dsTransforms || !m_dsTexture) {
    auto dsLighting = m_descriptorPool->allocateDescriptorSet(**m_setLighting);
    auto dsTransforms = m_descriptorPool->allocateDescriptorSet(**m_setTransforms);
    auto dsTexture = m_descriptorPool->allocateDescriptorSet(**m_setTexture);
    m_dsLighting = std::make_unique<ZVulkanDescriptorSet>(device, std::move(dsLighting));
    m_dsTransforms = std::make_unique<ZVulkanDescriptorSet>(device, std::move(dsTransforms));
    m_dsTexture = std::make_unique<ZVulkanDescriptorSet>(device, std::move(dsTexture));
  }

  if (m_uboLighting) {
    m_dsLighting->updateUniformBuffer(0, *m_uboLighting);
  }
  if (m_uboTransforms) {
    m_dsTransforms->updateUniformBuffer(0, *m_uboTransforms);
  }
  if (m_uboMaterial) {
    m_dsTransforms->updateUniformBuffer(1, *m_uboMaterial);
  }

  ensurePlaceholderTexture();
  if (m_placeholderTexture) {
    m_dsTexture->updateTexture(0, *m_placeholderTexture, **m_sampler);
  }
}

void LinePipelineContext::updateUBOs(Z3DRendererBase& renderer, const RenderBatch& batch)
{
  auto& device = m_backend.device();

  if (!m_uboLighting) {
    m_uboLighting = device.createBuffer(sizeof(LightingUBOStd140),
                                        vk::BufferUsageFlagBits::eUniformBuffer,
                                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  }
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

  LightingUBOStd140 lighting{};
  glm::vec2 extent = batch.pass.viewport.extent;
  if (extent.x <= 0.f || extent.y <= 0.f) {
    const auto& viewport = renderer.frameState().viewport;
    extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
  }
  if (extent.x > 0.f && extent.y > 0.f) {
    lighting.screen_dim_RCP = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  }

  const auto& sceneState = renderer.sceneState();
  if (sceneState.fog.mode != FogMode::None) {
    lighting.fog_color_top = sceneState.fog.topColor;
    lighting.fog_color_bottom = sceneState.fog.bottomColor;
  }
  m_uboLighting->copyData(&lighting, sizeof(lighting));

  TransformsUBOStd140 transforms{};
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  transforms.view_matrix = eyeState.viewMatrix;
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  transforms.pos_transform = renderer.coordTransform();
  transforms.pos_transform_normal_matrix = glm::mat4(1.0f);
  m_uboTransforms->copyData(&transforms, sizeof(transforms));

  MaterialUBOStd140 material{};
  material.scene_ambient = sceneState.sceneAmbient;
  material.material_ambient = renderer.parameterState().materialAmbient;
  material.material_specular = renderer.parameterState().materialSpecular;
  material.material_shininess = renderer.parameterState().materialShininess;
  material.alpha = renderer.parameterState().opacity;
  m_uboMaterial->copyData(&material, sizeof(material));
}

LinePipelineContext::PipelineInstance&
LinePipelineContext::ensurePipeline(const PipelineKey& key, const LinePayload& payload)
{
  (void)payload;
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = (ZSystemInfo::resourcesDirPath() + "/shader/vulkan/spv/").toStdString();

  PipelineInstance instance;

  if (key.useSmooth) {
    instance.shader = std::make_unique<ZVulkanShader>(device,
                                                     shaderBase + "wideline1.vert.spv",
                                                     shaderBase + "wideline.frag.spv",
                                                     std::nullopt);

    const uint32_t useTex = key.useTextureColor ? 1u : 0u;
    const uint32_t roundCap = key.roundCap ? 1u : 0u;
    const uint32_t lighting = 0u;

    std::array<vk::SpecializationMapEntry, 3> specEntries{
      vk::SpecializationMapEntry{.constantID = 98, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 99, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 100, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}};
    std::array<uint32_t, 3> specData{useTex, roundCap, lighting};
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment,
                                                std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end()),
                                                std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                                                                     reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

    std::array<vk::SpecializationMapEntry, 1> vertexEntry{
      vk::SpecializationMapEntry{.constantID = 101, .offset = 0, .size = sizeof(uint32_t)}};
    const uint32_t screenAligned = key.screenAligned ? 1u : 0u;
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex,
                                                std::vector<vk::SpecializationMapEntry>(vertexEntry.begin(), vertexEntry.end()),
                                                std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&screenAligned),
                                                                     reinterpret_cast<const uint8_t*>(&screenAligned) + sizeof(uint32_t)));

    auto vi = makeWideVertexInput();
    instance.pipeline = device.createPipeline(*instance.shader, vi, vk::PrimitiveTopology::eTriangleList);
    std::vector<vk::DescriptorSetLayout> setLayouts = {**m_setTexture, **m_setLighting, **m_setTransforms};
    instance.pipeline->setDescriptorSetLayouts(setLayouts);

    vk::PushConstantRange pushRange{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                    .offset = 0,
                                    .size = static_cast<uint32_t>(sizeof(glm::mat4) * 2 + sizeof(float) * 2)};
    instance.pipeline->setPushConstantRanges({pushRange});
    instance.pipeline->create();
  } else {
    instance.shader = std::make_unique<ZVulkanShader>(device,
                                                     shaderBase + "line.vert.spv",
                                                     shaderBase + "line.frag.spv",
                                                     std::nullopt);

    auto vi = makeThinVertexInput();
    const vk::PrimitiveTopology topology = key.lineStrip ? vk::PrimitiveTopology::eLineStrip : vk::PrimitiveTopology::eLineList;
    instance.pipeline = device.createPipeline(*instance.shader, vi, topology);
    std::vector<vk::DescriptorSetLayout> setLayouts = {**m_setTexture, **m_setLighting, **m_setTransforms};
    instance.pipeline->setDescriptorSetLayouts(setLayouts);
    instance.pipeline->create();
  }

  auto [insertIt, _] = m_pipelineCache.insert({key, std::move(instance)});
  return insertIt->second;
}

void LinePipelineContext::bindDescriptorSets(vk::raii::CommandBuffer& cmd, const PipelineInstance& pipeline) const
{
  if (!m_dsLighting || !m_dsTransforms || !m_dsTexture) {
    return;
  }

  std::array<vk::DescriptorSet, 3> sets{m_dsTexture->descriptorSet(),
                                        m_dsLighting->descriptorSet(),
                                        m_dsTransforms->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
}

void LinePipelineContext::uploadWideGeometry(const LinePayload& payload, bool pickingPass)
{
  m_wideVertices.clear();
  m_wideIndices.clear();

  if (!payload.renderer) {
    return;
  }

  payload.renderer->buildWideLineGeometry(m_wideVertices, m_wideIndices);
  if (m_wideVertices.empty() || m_wideIndices.empty()) {
    return;
  }

  if (pickingPass) {
    if (!payload.smoothPickingColors.empty()) {
      const auto pickingSpan = payload.smoothPickingColors;
      for (size_t i = 0; i < m_wideVertices.size() && i < pickingSpan.size(); ++i) {
        m_wideVertices[i].c0 = pickingSpan[i];
        m_wideVertices[i].c1 = pickingSpan[i];
      }
    } else if (!payload.pickingColors.empty()) {
      const auto pickingSpan = payload.pickingColors;
      for (size_t i = 0; i < m_wideVertices.size(); ++i) {
        const size_t colorIndex = std::min(i, pickingSpan.size() - 1);
        m_wideVertices[i].c0 = pickingSpan[colorIndex];
        m_wideVertices[i].c1 = pickingSpan[colorIndex];
      }
    }
  }

  auto& device = m_backend.device();

  const size_t vertexBytes = m_wideVertices.size() * sizeof(LineWideVertex);
  if (!m_wideVertexBuffer || vertexBytes > m_wideVertexCapacity) {
    const size_t allocSize = std::max<size_t>(vertexBytes, sizeof(LineWideVertex));
    m_wideVertexBuffer = device.createBuffer(allocSize,
                                             vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                             vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    m_wideVertexCapacity = allocSize;
  }
  if (vertexBytes > 0) {
    m_wideVertexBuffer->copyData(m_wideVertices.data(), vertexBytes);
  }

  const size_t indexBytes = m_wideIndices.size() * sizeof(uint32_t);
  if (!m_wideIndexBuffer || indexBytes > m_wideIndexCapacity) {
    const size_t allocSize = std::max<size_t>(indexBytes, sizeof(uint32_t));
    m_wideIndexBuffer = device.createBuffer(allocSize,
                                            vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    m_wideIndexCapacity = allocSize;
  }
  if (indexBytes > 0) {
    m_wideIndexBuffer->copyData(m_wideIndices.data(), indexBytes);
  }
}

void LinePipelineContext::uploadThinGeometry(const LinePayload& payload, bool pickingPass)
{
  m_thinVertices.clear();

  const auto positions = payload.positions;
  if (positions.size() < 2) {
    return;
  }

  const auto colorSpan = pickingPass ? payload.pickingColors : payload.colors;
  const bool hasColors = !colorSpan.empty();
  const glm::vec4 defaultColor(0.f, 0.f, 0.f, 1.f);

  auto colorAt = [&](size_t index) {
    if (!hasColors) {
      return defaultColor;
    }
    const size_t clamped = std::min(index, colorSpan.size() - 1);
    return colorSpan[clamped];
  };

  if (payload.isLineStrip) {
    for (size_t i = 1; i < positions.size(); ++i) {
      m_thinVertices.push_back(ThinVertex{positions[i - 1], colorAt(i - 1)});
      m_thinVertices.push_back(ThinVertex{positions[i], colorAt(i)});
    }
  } else {
    for (size_t i = 0; i + 1 < positions.size(); i += 2) {
      m_thinVertices.push_back(ThinVertex{positions[i], colorAt(i)});
      m_thinVertices.push_back(ThinVertex{positions[i + 1], colorAt(i + 1)});
    }
  }

  const size_t vertexBytes = m_thinVertices.size() * sizeof(ThinVertex);
  if (vertexBytes == 0) {
    return;
  }

  auto& device = m_backend.device();
  if (!m_thinVertexBuffer || vertexBytes > m_thinVertexCapacity) {
    const size_t allocSize = std::max<size_t>(vertexBytes, sizeof(ThinVertex));
    m_thinVertexBuffer = device.createBuffer(allocSize,
                                             vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                                             vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    m_thinVertexCapacity = allocSize;
  }

  m_thinVertexBuffer->copyData(m_thinVertices.data(), vertexBytes);
}

void LinePipelineContext::record(Z3DRendererBase& renderer,
                                 const RenderBatch& batch,
                                 const LinePayload& payload,
                                 const vk::Viewport& viewport,
                                 const vk::Rect2D& scissor,
                                 vk::raii::CommandBuffer& cmd)
{
  if (!payload.renderer) {
    return;
  }

  const bool pickingPass = payload.pickingPass;

  updateUBOs(renderer, batch);
  ensureDescriptorSets(renderer);

  PipelineKey key;
  key.useSmooth = payload.useSmoothLine;
  key.picking = pickingPass;
  key.roundCap = payload.roundCap;
  key.screenAligned = payload.screenAligned;
  key.useTextureColor = false; // TODO: integrate line textures for Vulkan backend
  key.lineStrip = payload.isLineStrip;

  auto& pipeline = ensurePipeline(key, payload);

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  bindDescriptorSets(cmd, pipeline);

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  if (payload.useSmoothLine) {
    uploadWideGeometry(payload, pickingPass);
    if (m_wideVertices.empty() || !m_wideVertexBuffer || !m_wideIndexBuffer) {
      return;
    }

    vk::DeviceSize offset = 0;
    cmd.bindVertexBuffers(0, {m_wideVertexBuffer->buffer()}, {offset});
    cmd.bindIndexBuffer(m_wideIndexBuffer->buffer(), 0, vk::IndexType::eUint32);

    struct WideLinePC
    {
      glm::mat4 viewport_matrix{1.0f};
      glm::mat4 viewport_matrix_inverse{1.0f};
      float line_width = 1.0f;
      float size_scale = 1.0f;
    } pc;

    const auto& frameState = renderer.frameState();
    pc.viewport_matrix = frameState.viewportMatrix;
    pc.viewport_matrix_inverse = frameState.inverseViewportMatrix;
    pc.size_scale = renderer.sizeScale();

    const auto widths = payload.perSegmentWidths;
    if (!widths.empty()) {
      const uint32_t segmentCount = static_cast<uint32_t>(m_wideIndices.size() / 6);
      const uint32_t drawSegments = std::min<uint32_t>(segmentCount, static_cast<uint32_t>(widths.size()));
      for (uint32_t i = 0; i < drawSegments; ++i) {
        pc.line_width = resolveWideLineWidth(widths[i]);
        cmd.pushConstants<WideLinePC>(pipeline.pipeline->pipelineLayout(),
                                      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                      0,
                                      pc);
        cmd.drawIndexed(6, 1, i * 6, i * 4, 0);
      }
    } else {
      pc.line_width = resolveWideLineWidth(payload.srcLineWidth);
      cmd.pushConstants<WideLinePC>(pipeline.pipeline->pipelineLayout(),
                                    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                    0,
                                    pc);
      cmd.drawIndexed(static_cast<uint32_t>(m_wideIndices.size()), 1, 0, 0, 0);
    }

  } else {
    uploadThinGeometry(payload, pickingPass);
    if (m_thinVertices.empty() || !m_thinVertexBuffer) {
      return;
    }

    vk::DeviceSize offset = 0;
    cmd.bindVertexBuffers(0, {m_thinVertexBuffer->buffer()}, {offset});
    cmd.draw(static_cast<uint32_t>(m_thinVertices.size()), 1, 0, 0);
  }
}

} // namespace

Z3DRendererVulkanBackend::Z3DRendererVulkanBackend()
  : m_lineContext(std::make_unique<LinePipelineContext>(*this))
{}

Z3DRendererVulkanBackend::~Z3DRendererVulkanBackend() = default;

void Z3DRendererVulkanBackend::setGlobalShaderParameters(Z3DRendererBase& renderer,
                                                         Z3DShaderProgram& shader,
                                                         Z3DEye eye)
{
  (void)renderer;
  (void)shader;
  (void)eye;
  LOG_FIRST_N(WARNING, 1) << "Vulkan backend does not provide GLSL shader parameter bindings";
}

std::string Z3DRendererVulkanBackend::generateHeader(const Z3DRendererBase& renderer) const
{
  (void)renderer;
  return std::string();
}

std::string Z3DRendererVulkanBackend::generateGeomHeader(const Z3DRendererBase& renderer) const
{
  (void)renderer;
  return std::string();
}

void Z3DRendererVulkanBackend::beginRender(Z3DRendererBase& renderer)
{
  if (m_lineContext) {
    m_lineContext->resetFrame();
  }
  ensureDevice();

  const auto& viewport = renderer.frameState().viewport;
  const uint32_t width = viewport.z;
  const uint32_t height = viewport.w;

  if (width == 0U || height == 0U) {
    m_activeCommandBuffer.reset();
    return;
  }

  ensureSwapChain(width, height);
  if (!m_swapChain) {
    LOG(ERROR) << "Vulkan backend failed to create swap chain";
    m_activeCommandBuffer.reset();
    return;
  }

  m_activeCommandBuffer = m_swapChain->beginFrame();

  renderer.setActiveSurfaceForNextPass(describeSurfaceFromSwapChain());
}

void Z3DRendererVulkanBackend::endRender(Z3DRendererBase& renderer)
{
  (void)renderer;
  if (m_swapChain && m_activeCommandBuffer) {
    m_swapChain->endFrame(*m_activeCommandBuffer);
  }
  m_activeCommandBuffer.reset();
}

namespace {

vk::Viewport toVkViewport(const ViewportDesc& viewport)
{
  return vk::Viewport(viewport.origin.x,
                      viewport.origin.y,
                      std::max(0.0f, viewport.extent.x),
                      std::max(0.0f, viewport.extent.y),
                      viewport.minDepth,
                      viewport.maxDepth);
}

vk::Rect2D toVkScissor(const BackendPassDesc& pass)
{
  if (pass.enableScissor) {
    return vk::Rect2D({static_cast<int32_t>(pass.scissorRect.x), static_cast<int32_t>(pass.scissorRect.y)},
                      {static_cast<uint32_t>(std::max(0.0f, pass.scissorRect.z)),
                       static_cast<uint32_t>(std::max(0.0f, pass.scissorRect.w))});
  }
  return vk::Rect2D({static_cast<int32_t>(pass.viewport.origin.x), static_cast<int32_t>(pass.viewport.origin.y)},
                    {static_cast<uint32_t>(std::max(0.0f, pass.viewport.extent.x)),
                     static_cast<uint32_t>(std::max(0.0f, pass.viewport.extent.y))});
}

std::string_view describeGeometry(const GeometryPayload& geometry)
{
  if (std::holds_alternative<std::monostate>(geometry)) {
    return "none";
  }
  if (std::holds_alternative<LinePayload>(geometry)) {
    return "line";
  }
  if (std::holds_alternative<MeshPayload>(geometry)) {
    return "mesh";
  }
  if (std::holds_alternative<EllipsoidPayload>(geometry)) {
    return "ellipsoid";
  }
  if (std::holds_alternative<ConePayload>(geometry)) {
    return "cone";
  }
  return "unknown";
}

} // namespace

RendererFrameState::ActiveSurface Z3DRendererVulkanBackend::describeSurfaceFromSwapChain()
{
  RendererFrameState::ActiveSurface surface;

  if (!m_swapChain) {
    return surface;
  }

  AttachmentDesc colorAttachment;
  colorAttachment.handle.backend = AttachmentBackend::Vulkan;
  colorAttachment.handle.index = 0;
  colorAttachment.handle.id = reinterpret_cast<uint64_t>(&m_swapChain->colorAttachment());
  colorAttachment.loadOp = LoadOp::Clear;
  colorAttachment.storeOp = StoreOp::Store;
  colorAttachment.clearValue.color = glm::vec4(0.f, 0.f, 0.f, 1.f);
  surface.colorAttachments.push_back(colorAttachment);

  AttachmentDesc depthAttachment;
  depthAttachment.handle.backend = AttachmentBackend::Vulkan;
  depthAttachment.handle.index = 0;
  depthAttachment.handle.id = reinterpret_cast<uint64_t>(&m_swapChain->depthAttachment());
  depthAttachment.loadOp = LoadOp::Clear;
  depthAttachment.storeOp = StoreOp::Store;
  depthAttachment.clearValue.depth = 1.0f;
  depthAttachment.clearValue.stencil = 0;
  surface.depthAttachment = depthAttachment;

  return surface;
}

void Z3DRendererVulkanBackend::processBatches(Z3DRendererBase& renderer, const RendererCPUState& state)
{
  if (!m_activeCommandBuffer || state.batches.empty()) {
    return;
  }

  auto& cmd = *m_activeCommandBuffer;

  for (const auto& batch : state.batches) {
    std::vector<vk::RenderingAttachmentInfo> colorAttachments;
    colorAttachments.reserve(batch.pass.colorAttachments.size());

    auto convertLoadOp = [](LoadOp op) {
      switch (op) {
        case LoadOp::Clear:
          return vk::AttachmentLoadOp::eClear;
        case LoadOp::Load:
          return vk::AttachmentLoadOp::eLoad;
        case LoadOp::DontCare:
        default:
          return vk::AttachmentLoadOp::eDontCare;
      }
    };

    auto convertStoreOp = [](StoreOp op) {
      switch (op) {
        case StoreOp::Store:
          return vk::AttachmentStoreOp::eStore;
        case StoreOp::DontCare:
        default:
          return vk::AttachmentStoreOp::eDontCare;
      }
    };

    auto makeColorAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
      if (attachment.handle.backend != AttachmentBackend::Vulkan || attachment.handle.id == 0) {
        return std::nullopt;
      }
      auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
      if (!texture) {
        return std::nullopt;
      }

      const auto desiredLayout = vk::ImageLayout::eColorAttachmentOptimal;
      texture->transitionLayout(cmd, texture->layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture->imageView();
      info.imageLayout = desiredLayout;
      info.loadOp = convertLoadOp(attachment.loadOp);
      info.storeOp = convertStoreOp(attachment.storeOp);
      vk::ClearValue clear{};
      clear.color = vk::ClearColorValue(std::array<float, 4>{attachment.clearValue.color.r,
                                                             attachment.clearValue.color.g,
                                                             attachment.clearValue.color.b,
                                                             attachment.clearValue.color.a});
      info.clearValue = clear;
      return info;
    };

    auto makeDepthAttachment = [&](const AttachmentDesc& attachment) -> std::optional<vk::RenderingAttachmentInfo> {
      if (attachment.handle.backend != AttachmentBackend::Vulkan || attachment.handle.id == 0) {
        return std::nullopt;
      }
      auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
      if (!texture) {
        return std::nullopt;
      }

      const auto desiredLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
      texture->transitionLayout(cmd, texture->layout(), desiredLayout);

      vk::RenderingAttachmentInfo info;
      info.imageView = texture->imageView();
      info.imageLayout = desiredLayout;
      info.loadOp = convertLoadOp(attachment.loadOp);
      info.storeOp = convertStoreOp(attachment.storeOp);
      vk::ClearValue clear{};
      clear.depthStencil = vk::ClearDepthStencilValue(attachment.clearValue.depth, attachment.clearValue.stencil);
      info.clearValue = clear;
      return info;
    };

    for (const auto& attachment : batch.pass.colorAttachments) {
      if (auto vkAttachment = makeColorAttachment(attachment)) {
        colorAttachments.push_back(*vkAttachment);
      }
    }

    std::optional<vk::RenderingAttachmentInfo> depthAttachmentInfo;
    if (batch.pass.depthAttachment) {
      depthAttachmentInfo = makeDepthAttachment(*batch.pass.depthAttachment);
    }

    if (colorAttachments.empty() && !depthAttachmentInfo) {
      LOG_FIRST_N(WARNING, 5) << "Vulkan backend skipping batch with no Vulkan-compatible attachments.";
      continue;
    }

    const auto vkViewport = toVkViewport(batch.pass.viewport);
    const auto vkScissor = toVkScissor(batch.pass);

    vk::RenderingInfo renderingInfo;
    renderingInfo.renderArea = vkScissor;
    renderingInfo.layerCount = 1;
    renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    renderingInfo.pDepthAttachment = depthAttachmentInfo ? &*depthAttachmentInfo : nullptr;

    cmd.beginRendering(renderingInfo);

    bool handled = false;
    if (const auto* line = std::get_if<LinePayload>(&batch.geometry)) {
      if (line->renderer) {
        if (!m_lineContext) {
          m_lineContext = std::make_unique<LinePipelineContext>(*this);
        }
        m_lineContext->record(renderer, batch, *line, vkViewport, vkScissor, cmd);
        handled = true;
      }
    }

    if (!handled) {
      cmd.setViewport(0, vkViewport);
      cmd.setScissor(0, vkScissor);
      LOG_FIRST_N(WARNING, 5) << "Vulkan backend has not yet implemented draw emission for geometry type '"
                              << describeGeometry(batch.geometry) << "'.";
    }

    cmd.endRendering();
  }
}

bool Z3DRendererVulkanBackend::supportsCommandLists() const
{
  return true;
}

RendererFrameState::ActiveSurface
Z3DRendererVulkanBackend::describeSurfaceFromRenderTarget(const Z3DRenderTarget& /*target*/)
{
  // Vulkan backend has no direct mapping for GL render targets; return empty surface.
  return RendererFrameState::ActiveSurface{};
}

RendererFrameState::ActiveSurface Z3DRendererVulkanBackend::describeSurfaceFromLease(
  const Z3DScratchResourcePool::RenderTargetLease& lease)
{
  RendererFrameState::ActiveSurface surface;
  if (!lease.hasVulkanImage()) {
    return surface;
  }

  auto& scratch = lease.vulkanScratchImage();
  for (const auto& attachment : lease.descriptor.attachments) {
    AttachmentDesc desc;
    desc.handle.backend = AttachmentBackend::Vulkan;
    desc.handle.index = attachment.index;

    if (attachment.kind == ScratchAttachmentKind::Color) {
      auto* texture = scratch.colorAttachment(attachment.index);
      if (!texture) {
        continue;
      }
      desc.handle.id = reinterpret_cast<uint64_t>(texture);
      surface.colorAttachments.push_back(desc);
    } else {
      auto* texture = scratch.depthAttachment();
      if (!texture) {
        continue;
      }
      desc.handle.id = reinterpret_cast<uint64_t>(texture);
      surface.depthAttachment = desc;
    }
  }

  return surface;
}

ZVulkanDevice& Z3DRendererVulkanBackend::device()
{
  ensureDevice();
  CHECK(m_device != nullptr);
  return *m_device;
}

const ZVulkanDevice& Z3DRendererVulkanBackend::device() const
{
  CHECK(m_device != nullptr);
  return *m_device;
}

ZVulkanSwapChain& Z3DRendererVulkanBackend::swapChain()
{
  CHECK(m_swapChain != nullptr);
  return *m_swapChain;
}

const ZVulkanSwapChain& Z3DRendererVulkanBackend::swapChain() const
{
  CHECK(m_swapChain != nullptr);
  return *m_swapChain;
}

vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer()
{
  CHECK(m_activeCommandBuffer.has_value());
  return *m_activeCommandBuffer;
}

const vk::raii::CommandBuffer& Z3DRendererVulkanBackend::commandBuffer() const
{
  CHECK(m_activeCommandBuffer.has_value());
  return *m_activeCommandBuffer;
}

void Z3DRendererVulkanBackend::ensureDevice()
{
  if (!m_context) {
    m_context = std::make_unique<ZVulkanContext>();
  }
  if (!m_device && m_context) {
    m_device = m_context->createDevice();
  }
}

void Z3DRendererVulkanBackend::ensureSwapChain(uint32_t width, uint32_t height)
{
  if (width == 0U || height == 0U) {
    return;
  }

  ensureDevice();
  if (!m_device) {
    return;
  }

  if (!m_swapChain) {
    m_swapChain = m_device->createSwapChain(width, height);
    m_swapChainExtent = glm::uvec2(width, height);
    return;
  }

  if (m_swapChainExtent.x != width || m_swapChainExtent.y != height) {
    m_swapChain->resize(width, height);
    m_swapChainExtent = glm::uvec2(width, height);
  }
}

std::unique_ptr<Z3DRendererBackend> createVulkanRendererBackend()
{
  return std::make_unique<Z3DRendererVulkanBackend>();
}

} // namespace nim
