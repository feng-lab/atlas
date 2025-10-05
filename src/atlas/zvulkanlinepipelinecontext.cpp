#include "zvulkanlinepipelinecontext.h"

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
#include "z3dlinerenderer.h"
#include "zvulkanrenderconversions.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace nim {
namespace {

vk::PipelineVertexInputStateCreateInfo makeWideVertexInput()
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(LineWideVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 5> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0},
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
                                        .offset = static_cast<uint32_t>(offsetof(LineWideVertex, flags))}
  };
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
                                                   .stride = static_cast<uint32_t>(sizeof(VulkanThinLineVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 2> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(VulkanThinLineVertex, color))}
  };
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

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

} // namespace

ZVulkanLinePipelineContext::ZVulkanLinePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanLinePipelineContext::~ZVulkanLinePipelineContext() = default;

void ZVulkanLinePipelineContext::resetFrame()
{
  m_wideVertices.clear();
  m_wideIndices.clear();
  m_thinVertices.clear();
  resetDescriptors();
}

void ZVulkanLinePipelineContext::resetDescriptors()
{
  m_dsTexture.reset();
  m_dsLighting.reset();
  m_dsTransforms.reset();
  if (m_descriptorPool) {
    m_descriptorPool->reset();
  }
}

void ZVulkanLinePipelineContext::ensureDescriptorLayouts()
{
  if (m_setTexture && m_setLighting && m_setTransforms) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  if (!m_setTexture) {
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
                                     .stageFlags = vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
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

void ZVulkanLinePipelineContext::ensurePlaceholderTexture() {}

void ZVulkanLinePipelineContext::ensureDescriptorSets(Z3DRendererBase& renderer)
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
  if (m_dsTexture) {
    auto& tex = m_backend.defaultPlaceholderTexture2D();
    auto sampler = m_backend.defaultSampler();
    m_dsTexture->updateTexture(0, tex, sampler);
    m_dsTexture->updateTexture(1, tex, sampler);
    m_dsTexture->updateTexture(2, tex, sampler);
  }
}

void ZVulkanLinePipelineContext::updateUBOs(Z3DRendererBase& renderer, const RenderBatch& batch)
{
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
  const auto& params = renderer.parameterState();
  const glm::mat4& coordTransform = params.coordTransform;
  transforms.pos_transform = coordTransform;

  // Line shaders do not consume the normal matrix; keep it as identity to
  // avoid redundant inverse/transpose work.
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(glm::mat3(1.0f));
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  transforms.parameters = glm::vec4(params.sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);
  m_uboTransforms->copyData(&transforms, sizeof(transforms));

  MaterialUBOStd140 material{};
  material.scene_ambient = sceneState.sceneAmbient;
  material.material_ambient = params.materialAmbient;
  material.material_specular = params.materialSpecular;
  material.material_shininess = params.materialShininess;
  material.alpha = params.opacity;
  m_uboMaterial->copyData(&material, sizeof(material));
}

ZVulkanLinePipelineContext::PipelineInstance&
ZVulkanLinePipelineContext::ensurePipeline(const PipelineKey& key,
                                           const LinePayload& payload,
                                           const vulkan::AttachmentFormats& formats)
{
  (void)payload;
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;

  auto makeBlendAttachments = [](size_t count, vk::PipelineColorBlendAttachmentState templateState) {
    std::vector<vk::PipelineColorBlendAttachmentState> attachments(count, templateState);
    return attachments;
  };

  if (key.useSmooth) {
    std::string fragmentShader = "wideline.frag.spv";
    switch (key.shaderHookType) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        fragmentShader = "dual_peeling_init_wideline.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        fragmentShader = "dual_peeling_peel_wideline.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        fragmentShader = "wavg_init_wideline.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        fragmentShader = "wblended_init_wideline.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        fragmentShader = "wideline.frag.spv";
        break;
    }

    instance.shader = std::make_unique<ZVulkanShader>(device,
                                                      shaderBase + "wideline1.vert.spv",
                                                      shaderBase + fragmentShader,
                                                      std::nullopt);

    const uint32_t useTex = key.useTextureColor ? 1u : 0u;
    const uint32_t roundCap = key.roundCap ? 1u : 0u;
    const uint32_t lighting = 0u;

    std::array<vk::SpecializationMapEntry, 3> specEntries{
      vk::SpecializationMapEntry{.constantID = 98,  .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 99,  .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
      vk::SpecializationMapEntry{.constantID = 100, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)}
    };
    std::array<uint32_t, 3> specData{useTex, roundCap, lighting};
    instance.shader->setSpecializationConstants(
      vk::ShaderStageFlagBits::eFragment,
      std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end()),
      std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                           reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

    std::array<vk::SpecializationMapEntry, 1> vertexEntry{
      vk::SpecializationMapEntry{.constantID = 101, .offset = 0, .size = sizeof(uint32_t)}
    };
    const uint32_t screenAligned = key.screenAligned ? 1u : 0u;
    instance.shader->setSpecializationConstants(
      vk::ShaderStageFlagBits::eVertex,
      std::vector<vk::SpecializationMapEntry>(vertexEntry.begin(), vertexEntry.end()),
      std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&screenAligned),
                           reinterpret_cast<const uint8_t*>(&screenAligned) + sizeof(uint32_t)));

    auto vi = makeWideVertexInput();
    instance.pipeline = device.createPipeline(*instance.shader, vi, vk::PrimitiveTopology::eTriangleList);
    std::vector<vk::DescriptorSetLayout> setLayouts = {**m_setTexture, **m_setLighting, **m_setTransforms};
    instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
    instance.pipeline->setDescriptorSetLayouts(setLayouts);

    vk::PipelineColorBlendAttachmentState baseBlend{};
    baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    baseBlend.blendEnable = VK_FALSE;

    switch (key.shaderHookType) {
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
        auto attachments = makeBlendAttachments(formats.colorFormats.size(), baseBlend);
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
        auto attachments = makeBlendAttachments(formats.colorFormats.size(), baseBlend);
        for (size_t i = 0; i < attachments.size(); ++i) {
          auto& attachment = attachments[i];
          attachment.blendEnable = VK_TRUE;
          if (i == 0) {
            attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
            attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
            attachment.colorBlendOp = vk::BlendOp::eAdd;
            attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.alphaBlendOp = vk::BlendOp::eAdd;
          } else {
            attachment.srcColorBlendFactor = vk::BlendFactor::eZero;
            attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
            attachment.colorBlendOp = vk::BlendOp::eAdd;
            attachment.srcAlphaBlendFactor = vk::BlendFactor::eZero;
            attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
            attachment.alphaBlendOp = vk::BlendOp::eAdd;
          }
        }
        instance.pipeline->setColorBlendAttachments(std::move(attachments));
        instance.pipeline->setDepthTestEnable(true);
        instance.pipeline->setDepthWriteEnable(false);
        break;
      }
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel: {
        auto attachments = makeBlendAttachments(formats.colorFormats.size(), baseBlend);
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
        instance.pipeline->setDepthWriteEnable(false);
        break;
      }
      default:
        break;
    }

    constexpr uint32_t wideLinePCSize = static_cast<uint32_t>(sizeof(glm::mat4) * 2 + sizeof(float) * 6);
    vk::PushConstantRange pushRange{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                    .offset = 0,
                                    .size = wideLinePCSize};
    instance.pipeline->setPushConstantRanges({pushRange});
    instance.pipeline->create();
  } else {
    std::string fragmentShader = "line.frag.spv";
    switch (key.shaderHookType) {
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        // Thin-line specialisations are not available; fall back to normal shader.
        fragmentShader = "line.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        fragmentShader = "line.frag.spv";
        break;
    }

    instance.shader =
      std::make_unique<ZVulkanShader>(device, shaderBase + "line.vert.spv", shaderBase + fragmentShader, std::nullopt);

    auto vi = makeThinVertexInput();
    const vk::PrimitiveTopology topology =
      key.lineStrip ? vk::PrimitiveTopology::eLineStrip : vk::PrimitiveTopology::eLineList;
    instance.pipeline = device.createPipeline(*instance.shader, vi, topology);
    std::vector<vk::DescriptorSetLayout> setLayouts = {**m_setTexture, **m_setLighting, **m_setTransforms};
    instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
    instance.pipeline->setDescriptorSetLayouts(setLayouts);
    instance.pipeline->create();
  }

  auto [insertIt, _] = m_pipelineCache.insert({key, std::move(instance)});
  return insertIt->second;
}

void ZVulkanLinePipelineContext::bindDescriptorSets(vk::raii::CommandBuffer& cmd,
                                                    const PipelineInstance& pipeline) const
{
  if (!m_dsLighting || !m_dsTransforms || !m_dsTexture) {
    return;
  }

  std::array<vk::DescriptorSet, 3> sets{m_dsTexture->descriptorSet(),
                                        m_dsLighting->descriptorSet(),
                                        m_dsTransforms->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
}

void ZVulkanLinePipelineContext::uploadWideGeometry(const LinePayload& payload, bool pickingPass)
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
    m_wideVertexBuffer =
      device.createBuffer(allocSize,
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
    m_wideIndexBuffer =
      device.createBuffer(allocSize,
                          vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    m_wideIndexCapacity = allocSize;
  }
  if (indexBytes > 0) {
    m_wideIndexBuffer->copyData(m_wideIndices.data(), indexBytes);
  }
}

void ZVulkanLinePipelineContext::uploadThinGeometry(const LinePayload& payload, bool pickingPass)
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
      m_thinVertices.push_back(VulkanThinLineVertex{positions[i - 1], colorAt(i - 1)});
      m_thinVertices.push_back(VulkanThinLineVertex{positions[i], colorAt(i)});
    }
  } else {
    for (size_t i = 0; i + 1 < positions.size(); i += 2) {
      m_thinVertices.push_back(VulkanThinLineVertex{positions[i], colorAt(i)});
      m_thinVertices.push_back(VulkanThinLineVertex{positions[i + 1], colorAt(i + 1)});
    }
  }

  const size_t vertexBytes = m_thinVertices.size() * sizeof(VulkanThinLineVertex);
  if (vertexBytes == 0) {
    return;
  }

  auto& device = m_backend.device();
  if (!m_thinVertexBuffer || vertexBytes > m_thinVertexCapacity) {
    const size_t allocSize = std::max<size_t>(vertexBytes, sizeof(VulkanThinLineVertex));
    m_thinVertexBuffer =
      device.createBuffer(allocSize,
                          vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    m_thinVertexCapacity = allocSize;
  }

  m_thinVertexBuffer->copyData(m_thinVertices.data(), vertexBytes);
}

void ZVulkanLinePipelineContext::record(Z3DRendererBase& renderer,
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

  const auto shaderHook = renderer.shaderHookType();
  if (m_dsTexture && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    const auto& hookPara = renderer.shaderHookPara();
    if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
      auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                 m_backend.device(),
                                                 "line dual-depth-peeling depth blender");
      m_dsTexture->updateTexture(1, depthTex, **m_sampler);
    }
    if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
      auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                 m_backend.device(),
                                                 "line dual-depth-peeling front blender");
      m_dsTexture->updateTexture(2, frontTex, **m_sampler);
    }
  } else if (m_dsTexture) {
    auto& tex = m_backend.defaultPlaceholderTexture2D();
    auto sampler = m_backend.defaultSampler();
    m_dsTexture->updateTexture(1, tex, sampler);
    m_dsTexture->updateTexture(2, tex, sampler);
  }

  PipelineKey key;
  key.useSmooth = payload.useSmoothLine;
  key.picking = pickingPass;
  key.roundCap = payload.roundCap;
  key.screenAligned = payload.screenAligned;
  key.useTextureColor = false; // TODO: integrate line textures for Vulkan backend
  key.lineStrip = payload.isLineStrip;
  key.shaderHookType = shaderHook;

  const auto formats = vulkan::extractAttachmentFormats(batch);
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  auto& pipeline = ensurePipeline(key, payload, formats);

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
      float weighted_a = 0.0f;
      float weighted_b = 0.0f;
      float weighted_depth_scale = 0.0f;
      float _pad = 0.0f;
    } pc;

    const auto& frameState = renderer.frameState();
    pc.viewport_matrix = frameState.viewportMatrix;
    pc.viewport_matrix_inverse = frameState.inverseViewportMatrix;
    pc.size_scale = renderer.parameterState().sizeScale;

    const auto widths = payload.perSegmentWidths;
    if (!widths.empty()) {
      const uint32_t segmentCount = static_cast<uint32_t>(m_wideIndices.size() / 6);
      const uint32_t drawSegments = std::min<uint32_t>(segmentCount, static_cast<uint32_t>(widths.size()));
      for (uint32_t i = 0; i < drawSegments; ++i) {
        pc.line_width = resolveWideLineWidth(widths[i]);
        if (shaderHook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit) {
          const float n = renderer.viewState().nearClip;
          const float f = renderer.viewState().farClip;
          const float denom = std::max(f - n, 1e-6f);
          pc.weighted_a = (f * n) / denom;
          pc.weighted_b = 0.5f * (f + n) / denom + 0.5f;
          pc.weighted_depth_scale = renderer.sceneState().weightedBlendedDepthScale;
        } else {
          pc.weighted_a = 0.0f;
          pc.weighted_b = 0.0f;
          pc.weighted_depth_scale = 0.0f;
        }

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

} // namespace nim
