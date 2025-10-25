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
#include "zvulkandescriptorset.h"
#include "zvulkantexture.h"
#include "zvulkanuniforms.h"
#include "zvulkanbindings.h"
#include "zsysteminfo.h"
#include "z3dlinerenderer.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <cstring>
#include <cstdint>

namespace nim {
namespace {

vk::PipelineVertexInputStateCreateInfo makeWideVertexInput()
{
  static std::array<vk::VertexInputBindingDescription, 5> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // p0
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // p1
    vk::VertexInputBindingDescription{.binding = 2,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // c0
    vk::VertexInputBindingDescription{.binding = 3,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // c1
    vk::VertexInputBindingDescription{.binding = 4,
                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // flags
  };
  static std::array<vk::VertexInputAttributeDescription, 5> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 1,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 2,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 3,
                                        .binding = 3,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 4, .binding = 4, .format = vk::Format::eR32Sfloat, .offset = 0}
  };
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  info.pVertexBindingDescriptions = bindings.data();
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

vk::PipelineVertexInputStateCreateInfo makeThinVertexInput()
{
  static std::array<vk::VertexInputBindingDescription, 2> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // position
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // color
  };
  static std::array<vk::VertexInputAttributeDescription, 2> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 1,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0}
  };
  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  info.pVertexBindingDescriptions = bindings.data();
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
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
  m_thinPosBuffer = nullptr;
  m_thinColorBuffer = nullptr;
  m_thinPosOffset = 0;
  m_thinColorOffset = 0;
  m_thinUploadVertexCount = 0;
  m_thinUploadIndexBuffer = nullptr;
  m_thinUploadIndexOffset = 0;
  m_thinUploadIndexCount = 0;
  m_wideP0Buffer = nullptr;
  m_wideP1Buffer = nullptr;
  m_wideC0Buffer = nullptr;
  m_wideC1Buffer = nullptr;
  m_wideFlagsBuffer = nullptr;
  m_wideP0Offset = 0;
  m_wideP1Offset = 0;
  m_wideC0Offset = 0;
  m_wideC1Offset = 0;
  m_wideFlagsOffset = 0;
  m_wideUploadIndexOffset = 0;
  m_wideUploadIndexCount = 0;
  m_wideIndexBuffer = nullptr;
  // Retire per-frame UBOs to avoid cross-frame write-after-use hazards when
  // more than one frame is in flight. The buffers are released after the
  // current submission fence signals so the previous frame can safely finish
  // reading them on the GPU.
  retainUbo(m_uboOIT);
  resetDescriptors();
  m_ddpTransformsFrozen = false;
  m_ddpMaterialFrozen = false;
}

void ZVulkanLinePipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  // Hand retained UBOs to the backend so destruction happens after the active
  // submission fence signals. Capture shared_ptr by value to extend lifetime.
  for (auto& sp : m_retainedUbos) {
    auto keep = sp; // copy shared ownership into the closure
    m_backend.scheduleAfterActiveSubmissionFence([keep]() {});
  }
  m_retainedUbos.clear();
}

void ZVulkanLinePipelineContext::resetDescriptors()
{
  m_dsTexture.reset();
  m_dsLighting.reset();
  m_dsTransforms.reset();
  m_dsOIT.reset();
}

void ZVulkanLinePipelineContext::ensureDescriptorLayouts()
{
  if (!m_setTexture) {
    auto& vkDevice = m_backend.device().context().device();
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
    m_setLighting = m_backend.lightingDescriptorSetLayout();
  }
  if (!m_setTransforms) {
    m_setTransforms = m_backend.transformDescriptorSetLayout();
  }
  if (!m_setOIT) {
    m_setOIT = m_backend.oitDescriptorSetLayout();
  }
}

void ZVulkanLinePipelineContext::ensurePlaceholderTexture() {}

void ZVulkanLinePipelineContext::ensureDescriptorSets(Z3DRendererBase& renderer)
{
  (void)renderer;
  ensureDescriptorLayouts();
  if (!m_dsLighting) {
    m_dsLighting = m_backend.allocateFrameDescriptorSet(m_setLighting);
  }
  if (!m_dsTransforms) {
    m_dsTransforms = m_backend.allocateFrameDescriptorSet(m_setTransforms);
  }
  if (!m_dsTexture) {
    m_dsTexture = m_backend.allocateFrameDescriptorSet(**m_setTexture);
  }

  // Ensure OIT UBO and descriptor set (only bound for WB init when needed)
  if (!m_uboOIT) {
    m_uboOIT = m_backend.device().createBuffer(sizeof(OITParamsUBOStd140),
                                               vk::BufferUsageFlagBits::eUniformBuffer,
                                               vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent);
  }
  if (!m_dsOIT && m_setOIT) {
    m_dsOIT = m_backend.allocateFrameDescriptorSet(m_setOIT);
  }
  if (m_dsOIT && m_uboOIT) {
    m_dsOIT->writeUniformBufferOnce(0, *m_uboOIT);
    // Prime DDP flag SSBO only when not recording.
    if (!m_backend.isRecording() && m_backend.ddpIndirectCountEnabled()) {
      if (auto* buf = m_backend.ddpChangedFlagBufferObj()) {
        m_dsOIT->writeStorageBufferOnce(nim::vkbind::kBindingOITDDPFlag, *buf);
      }
    }
  }

  // Bind per-frame uniform arena buffer to dynamic UBO bindings once.
  if (m_dsLighting) {
    m_dsLighting->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(LightingUBOStd140));
  }
  if (m_dsTransforms) {
    m_dsTransforms->writeUniformBufferDynamicOnce(0, m_backend.uniformArenaBuffer(), sizeof(TransformsUBOStd140));
    m_dsTransforms->writeUniformBufferDynamicOnce(1, m_backend.uniformArenaBuffer(), sizeof(MaterialUBOStd140));
  }

  ensurePlaceholderTexture();
  if (m_dsTexture) {
    auto& tex = m_backend.defaultPlaceholderTexture2D();
    auto sampler = m_backend.defaultSampler();
    m_dsTexture->writeTextureOnce(0, tex, sampler);
    m_dsTexture->writeTextureOnce(1, tex, sampler);
    m_dsTexture->writeTextureOnce(2, tex, sampler);
  }
}

void ZVulkanLinePipelineContext::updateUBOs(Z3DRendererBase& renderer,
                                            const RenderBatch& batch,
                                            const LinePayload& payload)
{
  (void)batch;
  (void)payload;
  // Use shared per-frame lighting UBO dynamic offset
  m_dynLightingOffset = m_backend.frameSharedLightingOffset();

  TransformsUBOStd140 transforms{};
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  transforms.view_matrix = eyeState.viewMatrix;
  transforms.projection_view_matrix = eyeState.projectionViewMatrix;
  CHECK(payload.params != nullptr) << "Line payload missing params";
  const glm::mat4 model =
    (payload.followCoordTransform && payload.params) ? payload.params->coordTransform : glm::mat4(1.0f);
  transforms.pos_transform = model;

  // Line shaders do not consume the normal matrix; keep it as identity to
  // avoid redundant inverse/transpose work.
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(glm::mat3(1.0f));
  transforms.projection_matrix = eyeState.projectionMatrix;
  transforms.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
  const float sizeScale = (payload.followSizeScale && payload.params) ? payload.params->sizeScale : 1.0f;
  transforms.parameters = glm::vec4(sizeScale, eyeState.isPerspective ? 0.0f : 1.0f, 0.0f, 0.0f);
  const auto hook = renderer.shaderHookType();
  const bool ddp = (hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                    hook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
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
  material.alpha = (payload.pickingPass || !payload.followOpacity || !payload.params) ? 1.0f : payload.params->opacity;
  if (!(ddp && m_ddpMaterialFrozen)) {
    auto slice = m_backend.suballocateUniform(sizeof(MaterialUBOStd140));
    std::memcpy(slice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = slice.offset;
    if (ddp) {
      m_ddpMaterialFrozen = true;
    }
  }

  VLOG(2) << fmt::format("VK line params: sizeScale={:.3f} alpha={:.3f} ortho={} picking={}",
                         payload.params->sizeScale,
                         payload.params->opacity,
                         (eyeState.isPerspective ? 0 : 1),
                         payload.pickingPass);

  // OIT params: used by weighted-blended wideline init
  if (m_uboOIT) {
    OITParamsUBOStd140 oit{};
    glm::vec2 rcp = glm::vec2(0.0f);
    glm::vec2 extent = batch.pass.viewport.extent;
    if (extent.x > 0.0f && extent.y > 0.0f) {
      rcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
    } else {
      const auto& viewport = renderer.frameState().viewport;
      if (viewport.z > 0 && viewport.w > 0) {
        rcp = glm::vec2(1.0f / viewport.z, 1.0f / viewport.w);
      }
    }
    oit.screen_dim_RCP = rcp;
    const float n = renderer.viewState().nearClip;
    const float f = renderer.viewState().farClip;
    const float denom = std::max(f - n, 1e-6f);
    oit.ze_to_zw_a = (f * n) / denom;
    oit.ze_to_zw_b = 0.5f * (f + n) / denom + 0.5f;
    oit.weighted_blended_depth_scale = renderer.sceneState().weightedBlendedDepthScale;
    m_uboOIT->copyData(&oit, sizeof(oit));
  }
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
    const uint32_t lighting = key.picking ? 0u : 1u;

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
    std::vector<vk::DescriptorSetLayout> setLayouts = {**m_setTexture, m_setLighting, m_setTransforms};
    if ((key.shaderHookType == Z3DRendererBase::ShaderHookType::WeightedBlendedInit ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) &&
        m_setOIT) {
      setLayouts.push_back(m_setOIT);
    }
    instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
    instance.pipeline->setDescriptorSetLayouts(setLayouts);
    // Wide-line quads are generated in screen space; disable culling to avoid
    // implementation-dependent winding eliminating triangles.
    instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);

    vk::PipelineColorBlendAttachmentState baseBlend{};
    baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    baseBlend.blendEnable = false;

    switch (key.shaderHookType) {
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
        auto attachments = makeBlendAttachments(formats.colorFormats.size(), baseBlend);
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
        auto attachments = makeBlendAttachments(formats.colorFormats.size(), baseBlend);
        for (size_t i = 0; i < attachments.size(); ++i) {
          auto& attachment = attachments[i];
          attachment.blendEnable = true;
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
        for (size_t i = 0; i < attachments.size(); ++i) {
          auto& attachment = attachments[i];
          if (i == 0 || i == 3) {
            attachment.blendEnable = true;
            attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
            attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
            attachment.colorBlendOp = vk::BlendOp::eMax;
            attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.alphaBlendOp = vk::BlendOp::eMax;
          } else if (i == 1 || i == 4) {
            attachment.blendEnable = true;
            attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
            attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
            attachment.colorBlendOp = vk::BlendOp::eMax;
            attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.alphaBlendOp = vk::BlendOp::eMax;
          } else if (i == 2 || i == 5) {
            attachment.blendEnable = true;
            attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
            attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
            attachment.colorBlendOp = vk::BlendOp::eAdd;
            attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.alphaBlendOp = vk::BlendOp::eAdd;
          } else {
            attachment.blendEnable = false;
          }
        }
        instance.pipeline->setColorBlendAttachments(std::move(attachments));
        instance.pipeline->setDepthTestEnable(false);
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
    std::vector<vk::DescriptorSetLayout> setLayouts = {**m_setTexture, m_setLighting, m_setTransforms};
    instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
    instance.pipeline->setDescriptorSetLayouts(setLayouts);
    // Thin lines may produce implementation-dependent winding; disable culling
    // to ensure visibility regardless of vertex order.
    instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
    if (key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
        key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(formats.colorFormats.size());
      for (size_t i = 0; i < attachments.size(); ++i) {
        auto& attachment = attachments[i];
        attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        if (i == 0 || i == 3) {
          attachment.blendEnable = true;
          attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
          attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
          attachment.colorBlendOp = vk::BlendOp::eMax;
          attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 1 || i == 4) {
          attachment.blendEnable = true;
          attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
          attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
          attachment.colorBlendOp = vk::BlendOp::eMax;
          attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 2 || i == 5) {
          attachment.blendEnable = true;
          attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
          attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
          attachment.colorBlendOp = vk::BlendOp::eAdd;
          attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.alphaBlendOp = vk::BlendOp::eAdd;
        } else {
          attachment.blendEnable = false;
        }
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
      instance.pipeline->setDepthTestEnable(false);
      instance.pipeline->setDepthWriteEnable(false);
    }
    instance.pipeline->create();
  }

  auto [insertIt, _] = m_pipelineCache.insert({key, std::move(instance)});
  return insertIt->second;
}

void ZVulkanLinePipelineContext::uploadWideGeometry(const LinePayload& payload, bool pickingPass)
{
  // Build wide-line SoA buffers directly from payload spans; copy indices from payload.smoothIndices.
  m_wideP0Buffer = nullptr;
  m_wideP1Buffer = nullptr;
  m_wideC0Buffer = nullptr;
  m_wideC1Buffer = nullptr;
  m_wideFlagsBuffer = nullptr;
  m_wideP0Offset = 0;
  m_wideP1Offset = 0;
  m_wideC0Offset = 0;
  m_wideC1Offset = 0;
  m_wideFlagsOffset = 0;
  m_wideUploadIndexOffset = 0;

  // Determine vertex count from smooth P0/P1 positions (clamped to the smallest span).
  const size_t nP0 = payload.smoothP0Positions.size();
  const size_t nP1 = payload.smoothP1Positions.size();
  size_t vertexCount = std::min(nP0, nP1);
  if (vertexCount == 0) {
    return;
  }

  const size_t pBytes = vertexCount * sizeof(glm::vec3);
  const size_t cBytes = vertexCount * sizeof(glm::vec4);
  const size_t fBytes = vertexCount * sizeof(float);
  m_backend.reserveUploadSlices({
    {pBytes,                                          alignof(glm::vec3)},
    {pBytes,                                          alignof(glm::vec3)},
    {cBytes,                                          alignof(glm::vec4)},
    {cBytes,                                          alignof(glm::vec4)},
    {fBytes,                                          alignof(float)    },
    {payload.smoothIndices.size() * sizeof(uint32_t), alignof(uint32_t) }
  });
  auto p0Slice = m_backend.suballocateUpload(pBytes, alignof(glm::vec3));
  auto p1Slice = m_backend.suballocateUpload(pBytes, alignof(glm::vec3));
  auto c0Slice = m_backend.suballocateUpload(cBytes, alignof(glm::vec4));
  auto c1Slice = m_backend.suballocateUpload(cBytes, alignof(glm::vec4));
  auto flagsSlice = m_backend.suballocateUpload(fBytes, alignof(float));
  if (!p0Slice.buffer || !p0Slice.mapped || !p1Slice.buffer || !p1Slice.mapped || !c0Slice.buffer || !c0Slice.mapped ||
      !c1Slice.buffer || !c1Slice.mapped || !flagsSlice.buffer || !flagsSlice.mapped) {
    return;
  }

  // Copy positions
  std::memcpy(p0Slice.mapped, payload.smoothP0Positions.data(), pBytes);
  std::memcpy(p1Slice.mapped, payload.smoothP1Positions.data(), pBytes);

  const bool hasP0Colors = !payload.smoothP0Colors.empty();
  const bool hasP1Colors = !payload.smoothP1Colors.empty();
  const glm::vec4 defaultColor(0.f, 0.f, 0.f, 1.f);
  if (pickingPass) {
    if (!payload.smoothPickingColors.empty()) {
      const size_t n = std::min(vertexCount, payload.smoothPickingColors.size());
      std::memcpy(c0Slice.mapped, payload.smoothPickingColors.data(), n * sizeof(glm::vec4));
      std::memcpy(c1Slice.mapped, payload.smoothPickingColors.data(), n * sizeof(glm::vec4));
    } else if (!payload.pickingColors.empty()) {
      // Expand pickingColors to both c0/c1
      auto* c0Out = static_cast<glm::vec4*>(c0Slice.mapped);
      auto* c1Out = static_cast<glm::vec4*>(c1Slice.mapped);
      for (size_t i = 0; i < vertexCount; ++i) {
        const size_t idx = std::min(i, payload.pickingColors.size() - 1);
        c0Out[i] = payload.pickingColors[idx];
        c1Out[i] = payload.pickingColors[idx];
      }
    } else {
      return;
    }
  } else {
    auto* c0Out = static_cast<glm::vec4*>(c0Slice.mapped);
    auto* c1Out = static_cast<glm::vec4*>(c1Slice.mapped);
    for (size_t i = 0; i < vertexCount; ++i) {
      const glm::vec4 c0 = hasP0Colors ? payload.smoothP0Colors[i] : defaultColor;
      const glm::vec4 c1 = hasP1Colors ? payload.smoothP1Colors[i] : c0;
      c0Out[i] = c0;
      c1Out[i] = c1;
    }
  }
  // Flags
  auto* fOut = static_cast<float*>(flagsSlice.mapped);
  for (size_t i = 0; i < vertexCount; ++i) {
    fOut[i] = (i < payload.smoothFlags.size()) ? payload.smoothFlags[i] : 0.0f;
  }

  // Indices come directly from payload.smoothIndices
  const size_t indexCount = payload.smoothIndices.size();
  if (indexCount == 0) {
    // Still keep vertex slice for potential degenerate case, but no draw will occur without indices
    m_wideP0Buffer = p0Slice.buffer;
    m_wideP1Buffer = p1Slice.buffer;
    m_wideC0Buffer = c0Slice.buffer;
    m_wideC1Buffer = c1Slice.buffer;
    m_wideFlagsBuffer = flagsSlice.buffer;
    m_wideP0Offset = p0Slice.offset;
    m_wideP1Offset = p1Slice.offset;
    m_wideC0Offset = c0Slice.offset;
    m_wideC1Offset = c1Slice.offset;
    m_wideFlagsOffset = flagsSlice.offset;
    m_wideUploadIndexOffset = 0;
    return;
  }

  auto idxSlice = m_backend.suballocateUpload(indexCount * sizeof(uint32_t), alignof(uint32_t));
  if (!idxSlice.buffer || !idxSlice.mapped) {
    return;
  }
  std::memcpy(idxSlice.mapped, payload.smoothIndices.data(), indexCount * sizeof(uint32_t));

  m_wideP0Buffer = p0Slice.buffer;
  m_wideP1Buffer = p1Slice.buffer;
  m_wideC0Buffer = c0Slice.buffer;
  m_wideC1Buffer = c1Slice.buffer;
  m_wideFlagsBuffer = flagsSlice.buffer;
  m_wideP0Offset = p0Slice.offset;
  m_wideP1Offset = p1Slice.offset;
  m_wideC0Offset = c0Slice.offset;
  m_wideC1Offset = c1Slice.offset;
  m_wideFlagsOffset = flagsSlice.offset;
  m_wideUploadIndexOffset = idxSlice.offset;
  m_wideUploadIndexCount = static_cast<uint32_t>(indexCount);
  m_wideIndexBuffer = idxSlice.buffer; // arena: indices reside in arena
  // Account staging bytes for lines (wide)
  m_backend.addLineBytesStaged(2 * pBytes + 2 * cBytes + fBytes + indexCount * sizeof(uint32_t));

  // Static promotion (wide)
  {
    CHECK(payload.streamKey != 0) << "Line payload missing streamKey";
    WideCacheKey key{payload.streamKey, payload.pickingPass};
    auto it = m_wideStaticCache.find(key);
    const int kPromotionThreshold = 2;
    if (it == m_wideStaticCache.end()) {
      WideCacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(vertexCount);
      entry.indexCount = static_cast<uint32_t>(indexCount);
      entry.p0Gen = payload.smoothP0PositionsGen;
      entry.p1Gen = payload.smoothP1PositionsGen;
      entry.flagsGen = payload.smoothFlagsGen;
      if (pickingPass) {
        entry.pickGen = payload.smoothPickingColorsGen;
      } else {
        entry.c0Gen = payload.smoothP0ColorsGen;
        entry.c1Gen = payload.smoothP1ColorsGen;
      }
      entry.indexGen = payload.smoothIndicesGen ? payload.smoothIndicesGen : payload.indicesGen;
      m_wideStaticCache.emplace(key, entry);
    } else {
      WideCacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == vertexCount) && (entry.indexCount == indexCount);
      bool gensSame = (entry.p0Gen == payload.smoothP0PositionsGen) && (entry.p1Gen == payload.smoothP1PositionsGen) &&
                      (entry.flagsGen == payload.smoothFlagsGen) &&
                      (entry.indexGen == (payload.smoothIndicesGen ? payload.smoothIndicesGen : payload.indicesGen));
      if (pickingPass) {
        gensSame = gensSame && (entry.pickGen == payload.smoothPickingColorsGen);
      } else {
        gensSame = gensSame && (entry.c0Gen == payload.smoothP0ColorsGen) && (entry.c1Gen == payload.smoothP1ColorsGen);
      }
      if (sizeSame && gensSame) {
        entry.unchangedFrames++;
      } else {
        entry.unchangedFrames = 0;
      }

      if (entry.promoted && sizeSame) {
        bool anyChanged =
          (entry.p0Gen != payload.smoothP0PositionsGen) || (entry.p1Gen != payload.smoothP1PositionsGen) ||
          (entry.flagsGen != payload.smoothFlagsGen) ||
          (entry.indexGen != (payload.smoothIndicesGen ? payload.smoothIndicesGen : payload.indicesGen));
        if (pickingPass) {
          anyChanged = anyChanged || (entry.pickGen != payload.smoothPickingColorsGen);
        } else {
          anyChanged =
            anyChanged || (entry.c0Gen != payload.smoothP0ColorsGen) || (entry.c1Gen != payload.smoothP1ColorsGen);
        }
        if (entry.p0Gen != payload.smoothP0PositionsGen) {
          m_backend.scheduleStaticCopy(entry.vbP0, entry.p0Offset, p0Slice, false);
        }
        if (entry.p1Gen != payload.smoothP1PositionsGen) {
          m_backend.scheduleStaticCopy(entry.vbP1, entry.p1Offset, p1Slice, false);
        }
        if (pickingPass) {
          if (entry.pickGen != payload.smoothPickingColorsGen) {
            m_backend.scheduleStaticCopy(entry.vbC0, entry.c0Offset, c0Slice, false);
            m_backend.scheduleStaticCopy(entry.vbC1, entry.c1Offset, c1Slice, false);
          }
        } else {
          if (entry.c0Gen != payload.smoothP0ColorsGen) {
            m_backend.scheduleStaticCopy(entry.vbC0, entry.c0Offset, c0Slice, false);
          }
          if (entry.c1Gen != payload.smoothP1ColorsGen) {
            m_backend.scheduleStaticCopy(entry.vbC1, entry.c1Offset, c1Slice, false);
          }
        }
        if (entry.flagsGen != payload.smoothFlagsGen) {
          m_backend.scheduleStaticCopy(entry.vbFlags, entry.flagsOffset, flagsSlice, false);
        }
        if (entry.indexGen != (payload.smoothIndicesGen ? payload.smoothIndicesGen : payload.indicesGen) &&
            m_wideUploadIndexCount > 0) {
          m_backend.scheduleStaticCopy(entry.ib, entry.ibOffset, idxSlice, true);
        }
        if (!anyChanged) {
          m_wideP0Buffer = entry.vbP0;
          m_wideP1Buffer = entry.vbP1;
          m_wideC0Buffer = entry.vbC0;
          m_wideC1Buffer = entry.vbC1;
          m_wideFlagsBuffer = entry.vbFlags;
          m_wideP0Offset = entry.p0Offset;
          m_wideP1Offset = entry.p1Offset;
          m_wideC0Offset = entry.c0Offset;
          m_wideC1Offset = entry.c1Offset;
          m_wideFlagsOffset = entry.flagsOffset;
          m_wideIndexBuffer = entry.ib ? entry.ib : VK_NULL_HANDLE;
          m_wideUploadIndexOffset = entry.ibOffset;
          return;
        }
        return; // use upload slices for this frame
      }

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        auto p0Dst = m_backend.allocateStaticVB(pBytes, alignof(glm::vec3));
        auto p1Dst = m_backend.allocateStaticVB(pBytes, alignof(glm::vec3));
        auto c0Dst = m_backend.allocateStaticVB(cBytes, alignof(glm::vec4));
        auto c1Dst = m_backend.allocateStaticVB(cBytes, alignof(glm::vec4));
        auto fDst = m_backend.allocateStaticVB(fBytes, alignof(float));
        auto ibDst = m_backend.allocateStaticIB(indexCount * sizeof(uint32_t), alignof(uint32_t));
        if (p0Dst.buffer && p1Dst.buffer && c0Dst.buffer && c1Dst.buffer && fDst.buffer && ibDst.buffer) {
          m_backend.scheduleStaticCopy(p0Dst.buffer, p0Dst.offset, p0Slice, false);
          m_backend.scheduleStaticCopy(p1Dst.buffer, p1Dst.offset, p1Slice, false);
          m_backend.scheduleStaticCopy(c0Dst.buffer, c0Dst.offset, c0Slice, false);
          m_backend.scheduleStaticCopy(c1Dst.buffer, c1Dst.offset, c1Slice, false);
          m_backend.scheduleStaticCopy(fDst.buffer, fDst.offset, flagsSlice, false);
          m_backend.scheduleStaticCopy(ibDst.buffer, ibDst.offset, idxSlice, true);
          entry.vbP0 = p0Dst.buffer;
          entry.vbP1 = p1Dst.buffer;
          entry.vbC0 = c0Dst.buffer;
          entry.vbC1 = c1Dst.buffer;
          entry.vbFlags = fDst.buffer;
          entry.p0Offset = p0Dst.offset;
          entry.p1Offset = p1Dst.offset;
          entry.c0Offset = c0Dst.offset;
          entry.c1Offset = c1Dst.offset;
          entry.flagsOffset = fDst.offset;
          entry.ib = ibDst.buffer;
          entry.ibOffset = ibDst.offset;
          entry.p0Gen = payload.smoothP0PositionsGen;
          entry.p1Gen = payload.smoothP1PositionsGen;
          entry.flagsGen = payload.smoothFlagsGen;
          if (pickingPass) {
            entry.pickGen = payload.smoothPickingColorsGen;
          } else {
            entry.c0Gen = payload.smoothP0ColorsGen;
            entry.c1Gen = payload.smoothP1ColorsGen;
          }
          entry.indexGen = payload.smoothIndicesGen ? payload.smoothIndicesGen : payload.indicesGen;
          entry.promoted = true;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_backend.addLineBytesStaged(2 * pBytes + 2 * cBytes + fBytes + indexCount * sizeof(uint32_t));
          VLOG(1) << fmt::format("VK line wide promote: p0={}B p1={}B c0={}B c1={}B flags={}B idx={}B",
                                 pBytes,
                                 pBytes,
                                 cBytes,
                                 cBytes,
                                 fBytes,
                                 indexCount * sizeof(uint32_t));
          return;
        }
      }
    }
  }
}

void ZVulkanLinePipelineContext::uploadThinGeometry(const LinePayload& payload, bool pickingPass)
{
  const auto positions = payload.positions;
  if (positions.size() < 2) {
    m_thinUploadVertexCount = 0;
    m_thinPosBuffer = VK_NULL_HANDLE;
    m_thinColorBuffer = VK_NULL_HANDLE;
    m_thinPosOffset = 0;
    m_thinColorOffset = 0;
    m_thinUploadIndexBuffer = VK_NULL_HANDLE;
    m_thinUploadIndexOffset = 0;
    m_thinUploadIndexCount = 0;
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

  // Compute vertex count and allocate directly from the per-frame upload arena
  uint32_t vertexCount = 0;
  if (payload.isLineStrip) {
    vertexCount = static_cast<uint32_t>(positions.size());
  } else {
    vertexCount = static_cast<uint32_t>((positions.size() / 2) * 2);
  }
  const size_t posBytes = static_cast<size_t>(vertexCount) * sizeof(glm::vec3);
  const size_t colBytes = static_cast<size_t>(vertexCount) * sizeof(glm::vec4);
  m_backend.reserveUploadSlices({
    {posBytes,                                                                            alignof(glm::vec3)},
    {colBytes,                                                                            alignof(glm::vec4)},
    {payload.isLineStrip ? static_cast<size_t>(positions.size()) * sizeof(uint32_t) : 0u, alignof(uint32_t) }
  });
  if (vertexCount == 0) {
    m_thinUploadVertexCount = 0;
    m_thinPosBuffer = VK_NULL_HANDLE;
    m_thinColorBuffer = VK_NULL_HANDLE;
    m_thinPosOffset = 0;
    m_thinColorOffset = 0;
    return;
  }

  auto posSlice = m_backend.suballocateUpload(posBytes, alignof(glm::vec3));
  auto colSlice = m_backend.suballocateUpload(colBytes, alignof(glm::vec4));
  if (!posSlice.buffer || !posSlice.mapped || !colSlice.buffer || !colSlice.mapped) {
    m_thinUploadVertexCount = 0;
    m_thinPosBuffer = VK_NULL_HANDLE;
    m_thinColorBuffer = VK_NULL_HANDLE;
    m_thinPosOffset = 0;
    m_thinColorOffset = 0;
    return;
  }

  auto* posOut = static_cast<glm::vec3*>(posSlice.mapped);
  auto* colOut = static_cast<glm::vec4*>(colSlice.mapped);
  uint32_t out = 0;
  if (payload.isLineStrip) {
    for (size_t i = 0; i < positions.size(); ++i) {
      posOut[out] = positions[i];
      colOut[out] = colorAt(i);
      out++;
    }
    // Build index buffer 0..N-1 for line strip
    const uint32_t idxCount = static_cast<uint32_t>(positions.size());
    auto idxSlice = m_backend.suballocateUpload(idxCount * sizeof(uint32_t), alignof(uint32_t));
    if (idxSlice.buffer && idxSlice.mapped) {
      auto* idst = static_cast<uint32_t*>(idxSlice.mapped);
      for (uint32_t i = 0; i < idxCount; ++i) {
        idst[i] = i;
      }
      m_thinUploadIndexBuffer = idxSlice.buffer;
      m_thinUploadIndexOffset = idxSlice.offset;
      m_thinUploadIndexCount = idxCount;
    }
  } else {
    for (size_t i = 0; i + 1 < positions.size(); i += 2) {
      posOut[out] = positions[i];
      colOut[out] = colorAt(i);
      out++;
      posOut[out] = positions[i + 1];
      colOut[out] = colorAt(i + 1);
      out++;
    }
    m_thinUploadIndexBuffer = VK_NULL_HANDLE;
    m_thinUploadIndexOffset = 0;
    m_thinUploadIndexCount = 0;
  }

  m_thinUploadVertexCount = vertexCount;
  m_thinPosBuffer = posSlice.buffer;
  m_thinColorBuffer = colSlice.buffer;
  m_thinPosOffset = posSlice.offset;
  m_thinColorOffset = colSlice.offset;

  // Static promotion (thin)
  {
    CHECK(payload.streamKey != 0) << "Line payload missing streamKey";
    ThinCacheKey key{payload.streamKey, payload.pickingPass, payload.isLineStrip};
    auto it = m_thinStaticCache.find(key);
    const int kPromotionThreshold = 2;
    if (it == m_thinStaticCache.end()) {
      ThinCacheEntry entry{};
      entry.vertexCount = vertexCount;
      entry.indexCount = m_thinUploadIndexCount;
      entry.positionsGen = payload.positionsGen;
      entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
      entry.indexGen = payload.indicesGen;
      m_thinStaticCache.emplace(key, entry);
    } else {
      ThinCacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == vertexCount) && (entry.indexCount == m_thinUploadIndexCount);
      uint32_t colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
      const bool gensSame = (entry.positionsGen == payload.positionsGen) && (entry.indexGen == payload.indicesGen) &&
                            (entry.colorsGen == colorGen);
      if (sizeSame && gensSame) {
        entry.unchangedFrames++;
      } else {
        entry.unchangedFrames = 0;
      }

      if (entry.promoted && sizeSame) {
        bool anyChanged = (entry.positionsGen != payload.positionsGen) || (entry.colorsGen != colorGen) ||
                          (payload.isLineStrip && entry.indexGen != payload.indicesGen);
        if (entry.positionsGen != payload.positionsGen) {
          m_backend.scheduleStaticCopy(entry.vbPos, entry.posOffset, posSlice, false);
        }
        if (entry.colorsGen != colorGen) {
          m_backend.scheduleStaticCopy(entry.vbColor, entry.colorOffset, colSlice, false);
        }
        if (payload.isLineStrip && entry.indexGen != payload.indicesGen && m_thinUploadIndexBuffer) {
          Z3DRendererVulkanBackend::UploadSlice iUpload{m_thinUploadIndexBuffer,
                                                        m_thinUploadIndexOffset,
                                                        nullptr,
                                                        static_cast<size_t>(m_thinUploadIndexCount) * sizeof(uint32_t)};
          if (entry.ib) {
            m_backend.scheduleStaticCopy(entry.ib, entry.ibOffset, iUpload, true);
          }
        }
        if (!anyChanged) {
          m_thinPosBuffer = entry.vbPos;
          m_thinColorBuffer = entry.vbColor;
          m_thinPosOffset = entry.posOffset;
          m_thinColorOffset = entry.colorOffset;
          if (payload.isLineStrip && entry.ib) {
            m_thinUploadIndexBuffer = entry.ib;
            m_thinUploadIndexOffset = entry.ibOffset;
          }
          return;
        }
        return; // use upload slices for this frame
      }

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        auto posDst = m_backend.allocateStaticVB(posBytes, alignof(glm::vec3));
        auto colDst = m_backend.allocateStaticVB(colBytes, alignof(glm::vec4));
        Z3DRendererVulkanBackend::StaticSlice ibDst{};
        if (payload.isLineStrip && m_thinUploadIndexCount > 0) {
          ibDst = m_backend.allocateStaticIB(static_cast<size_t>(m_thinUploadIndexCount) * sizeof(uint32_t),
                                             alignof(uint32_t));
        }
        if (posDst.buffer && colDst.buffer && (!payload.isLineStrip || ibDst.buffer)) {
          m_backend.scheduleStaticCopy(posDst.buffer, posDst.offset, posSlice, false);
          m_backend.scheduleStaticCopy(colDst.buffer, colDst.offset, colSlice, false);
          if (payload.isLineStrip && m_thinUploadIndexCount > 0) {
            Z3DRendererVulkanBackend::UploadSlice iUpload{m_thinUploadIndexBuffer,
                                                          m_thinUploadIndexOffset,
                                                          nullptr,
                                                          static_cast<size_t>(m_thinUploadIndexCount) *
                                                            sizeof(uint32_t)};
            m_backend.scheduleStaticCopy(ibDst.buffer, ibDst.offset, iUpload, /*isIndexBuffer=*/true);
          }
          entry.vbPos = posDst.buffer;
          entry.vbColor = colDst.buffer;
          entry.posOffset = posDst.offset;
          entry.colorOffset = colDst.offset;
          entry.ib = ibDst.buffer;
          entry.ibOffset = ibDst.offset;
          entry.vertexCount = vertexCount;
          entry.indexCount = m_thinUploadIndexCount;
          entry.positionsGen = payload.positionsGen;
          entry.colorsGen = colorGen;
          entry.indexGen = payload.indicesGen;
          entry.promoted = true;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_backend.addLineBytesStaged(
            posBytes + colBytes +
            (payload.isLineStrip ? static_cast<size_t>(m_thinUploadIndexCount) * sizeof(uint32_t) : 0u));
          VLOG(1) << fmt::format("VK line thin promote: pos={}B col={}B idx={}B",
                                 posBytes,
                                 colBytes,
                                 payload.isLineStrip ? static_cast<size_t>(m_thinUploadIndexCount) * sizeof(uint32_t)
                                                     : 0u);
          return;
        }
      }
    }
  }
}

void ZVulkanLinePipelineContext::record(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const LinePayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd)
{
  // Ensure previously used UBOs survive until this submission completes.
  flushRetainedUbos();
  // No-op if payload empty

  // GL parity: skip if no geometry, and in picking pass skip when picking colors
  // are absent or mismatched in count.
  if (payload.positions.empty()) {
    return;
  }
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.positions.size()) {
      return;
    }
  }

  const bool pickingPass = payload.pickingPass;

  updateUBOs(renderer, batch, payload);
  // Descriptor sets are primed in beginRender(); avoid record-time rewrites.
  CHECK(m_dsLighting && m_dsTransforms) << "Line pipeline descriptor sets missing (lighting/transforms)";

  const auto shaderHook = renderer.shaderHookType();
  vk::DescriptorSet texOverride{};
  if (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel && m_setTexture) {
    // Allocate a per-draw override descriptor set for DDP peel inputs to avoid update-after-bind.
    auto* ds = m_backend.allocateOverrideDescriptorSet(**m_setTexture);
    CHECK(ds != nullptr) << "Line DDP peel: override descriptor allocation failed (fatal)";
    if (ds) {
      auto& tex = m_backend.defaultPlaceholderTexture2D();
      auto sampler = m_backend.defaultSampler();
      ds->updateTexture(0, tex, sampler);
      const auto& hookPara = renderer.shaderHookPara();
      if (hookPara.dualDepthPeelingDepthBlenderHandle.valid()) {
        auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                                   m_backend.device(),
                                                   "line dual-depth-peeling depth blender");
        ds->updateTexture(1, depthTex, sampler);
      } else {
        ds->updateTexture(1, tex, sampler);
      }
      if (hookPara.dualDepthPeelingFrontBlenderHandle.valid()) {
        auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                                   m_backend.device(),
                                                   "line dual-depth-peeling front blender");
        ds->updateTexture(2, frontTex, sampler);
      } else {
        ds->updateTexture(2, tex, sampler);
      }
      texOverride = ds->descriptorSet();
    }
  }
  if (shaderHook != Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    CHECK(m_dsTexture != nullptr) << "Line pipeline texture descriptor set not initialised";
  }
  // No record-time rewrites of persistent texture set; initialized in ensureDescriptorSets()

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

  // Build descriptor sets for draw-only recording
  std::vector<vk::DescriptorSet> descriptorSets;
  descriptorSets.reserve(3);
  const vk::DescriptorSet dsTex =
    texOverride ? texOverride : (m_dsTexture ? m_dsTexture->descriptorSet() : vk::DescriptorSet{});
  CHECK(dsTex) << "Line pipeline texture descriptor set not initialised";
  descriptorSets.push_back(dsTex);
  descriptorSets.push_back(m_dsLighting->descriptorSet());
  descriptorSets.push_back(m_dsTransforms->descriptorSet());
  // Dynamic offsets added to drawSpec after it is constructed below.

  uint32_t expectedSets = static_cast<uint32_t>(descriptorSets.size());
  std::vector<ZVulkanDescriptorBindInfo> extraBinds;
  if ((shaderHook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit ||
       shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
       shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) &&
      m_dsOIT) {
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = vkbind::kSetOITParams;
    oitBind.sets = {m_dsOIT->descriptorSet()};
    extraBinds.push_back(oitBind);
    expectedSets = std::max(expectedSets, vkbind::kSetOITParams + 1);
  }

  if (payload.useSmoothLine) {
    uploadWideGeometry(payload, pickingPass);
    if (m_wideP0Buffer == VK_NULL_HANDLE || m_wideUploadIndexCount == 0) {
      return;
    }

    ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
    drawSpec.viewports = {viewport};
    drawSpec.scissors = {scissor};
    drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    drawSpec.descriptorSetFirst = 0;
    drawSpec.descriptorSets = descriptorSets;
    // Dynamic offsets order must match set/binding order: lighting (set1,b0), transforms (set2,b0), material (set2,b1)
    drawSpec.dynamicOffsets = {static_cast<uint32_t>(m_dynLightingOffset),
                               static_cast<uint32_t>(m_dynTransformsOffset),
                               static_cast<uint32_t>(m_dynMaterialOffset)};
    drawSpec.extraDescriptorBinds = extraBinds;
    drawSpec.expectedDescriptorSetCount = expectedSets;

    const vk::PipelineLayout pipelineLayout = pipeline.pipeline->pipelineLayout();

    ZVulkanPipelineCommandRecorder recorder(cmd);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& cb) {
      std::array<vk::Buffer, 5> wbufs{m_wideP0Buffer,
                                      m_wideP1Buffer,
                                      m_wideC0Buffer,
                                      m_wideC1Buffer,
                                      m_wideFlagsBuffer};
      std::array<vk::DeviceSize, 5> woffs{m_wideP0Offset,
                                          m_wideP1Offset,
                                          m_wideC0Offset,
                                          m_wideC1Offset,
                                          m_wideFlagsOffset};
      cb.bindVertexBuffers(0, wbufs, woffs);
      vk::Buffer idxBuf = m_wideIndexBuffer ? m_wideIndexBuffer : m_wideP0Buffer;
      cb.bindIndexBuffer(idxBuf, m_wideUploadIndexOffset, vk::IndexType::eUint32);

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
      CHECK(payload.params != nullptr) << "Line payload missing params";
      pc.size_scale = payload.params->sizeScale;

      const auto widths = payload.perSegmentWidths;
      const float dpr = renderer.sceneState().devicePixelRatio;
      const bool msaa2x2 =
        (renderer.sceneState().multisample == GeometryMSAAMode::MSAA2x2) && payload.enableMultisample;
      const float sizeScale = payload.params->sizeScale;
      if (!widths.empty()) {
        VLOG(1) << fmt::format(
          "VK wide line: segments={} dpr={:.3f} msaa2x2={} sizeScale={:.3f} resolvedLineWidth={:.3f}",
          m_wideUploadIndexCount / 6u,
          dpr,
          msaa2x2,
          sizeScale,
          payload.resolvedLineWidth);
        const uint32_t segmentCount = m_wideUploadIndexCount / 6u;
        const uint32_t drawSegments = std::min<uint32_t>(segmentCount, static_cast<uint32_t>(widths.size()));
        for (uint32_t i = 0; i < drawSegments; ++i) {
          pc.line_width = widths[i];
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
          cb.pushConstants<WideLinePC>(pipelineLayout,
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       pc);
          if (m_backend.ddpIndirectCountEnabled() &&
              shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
            // Device-local args per segment
            struct Cmd
            {
              uint32_t indexCount, instanceCount, firstIndex;
              int32_t vertexOffset;
              uint32_t firstInstance;
            } cmd{6, 1, i * 6, static_cast<int32_t>(i * 4), 0};
            const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
            auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
            if (slice.buffer && slice.mapped) {
              std::memcpy(slice.mapped, &cmd, sizeof(Cmd));
            }
            m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
            const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
            const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
            cb.drawIndexedIndirectCount(argsBuf, off, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
          } else {
            cb.drawIndexed(6, 1, i * 6, i * 4, 0);
          }
        }
      } else {
        pc.line_width = payload.resolvedLineWidth;
        VLOG(1) << fmt::format("VK wide line: single width resolvedLineWidth={:.3f} sizeScale={:.3f}",
                               payload.resolvedLineWidth,
                               sizeScale);
        cb.pushConstants<WideLinePC>(pipelineLayout,
                                     vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                     0,
                                     pc);
        if (m_backend.ddpIndirectCountEnabled() &&
            shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
          struct Cmd
          {
            uint32_t indexCount, instanceCount, firstIndex;
            int32_t vertexOffset;
            uint32_t firstInstance;
          } cmd{m_wideUploadIndexCount, 1, 0, 0, 0};
          const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
          auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
          if (slice.buffer && slice.mapped) {
            std::memcpy(slice.mapped, &cmd, sizeof(Cmd));
          }
          m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
          const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
          const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
          cb.drawIndexedIndirectCount(argsBuf, off, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
        } else {
          cb.drawIndexed(m_wideUploadIndexCount, 1, 0, 0, 0);
        }
      }
    });
    return;
  }

  uploadThinGeometry(payload, pickingPass);
  if (m_thinUploadVertexCount == 0 || m_thinPosBuffer == VK_NULL_HANDLE) {
    return;
  }

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = {viewport};
  drawSpec.scissors = {scissor};
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = 0;
  drawSpec.descriptorSets = descriptorSets;
  drawSpec.dynamicOffsets = {static_cast<uint32_t>(m_dynLightingOffset),
                             static_cast<uint32_t>(m_dynTransformsOffset),
                             static_cast<uint32_t>(m_dynMaterialOffset)};
  drawSpec.extraDescriptorBinds = extraBinds;
  drawSpec.expectedDescriptorSetCount = expectedSets;

  drawSpec.vertexBuffers = {m_thinPosBuffer, m_thinColorBuffer};
  drawSpec.vertexOffsets = {m_thinPosOffset, m_thinColorOffset};
  if (payload.isLineStrip && m_thinUploadIndexBuffer && m_thinUploadIndexCount > 0) {
    drawSpec.indexBuffer = m_thinUploadIndexBuffer;
    drawSpec.indexOffset = m_thinUploadIndexOffset;
    drawSpec.indexType = vk::IndexType::eUint32;
    drawSpec.indexCount = m_thinUploadIndexCount;
  } else {
    drawSpec.vertexCount = m_thinUploadVertexCount;
  }
  drawSpec.instanceCount = 1;

  ZVulkanPipelineCommandRecorder recorder(cmd);
  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    const bool indexed = (drawSpec.indexCount > 0);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      if (indexed) {
        struct Cmd
        {
          uint32_t indexCount, instanceCount, firstIndex;
          int32_t vertexOffset;
          uint32_t firstInstance;
        } cmd{drawSpec.indexCount, 1, 0, 0, 0};
        const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &cmd, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(argsBuf, off, slice);
        c.drawIndexedIndirectCount(argsBuf, off, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
      } else {
        struct Cmd
        {
          uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
        } cmd{drawSpec.vertexCount, 1, 0, 0};
        const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &cmd, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(argsBuf, off, slice);
        c.drawIndirectCount(argsBuf, off, cntBuf, 0, 1, sizeof(Cmd));
      }
    });
  } else {
    recorder.recordGraphicsDraw(drawSpec);
  }
}

} // namespace nim
