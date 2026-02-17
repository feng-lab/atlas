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
#include "zvulkantexture.h"
#include "zvulkanclipplanes.h"
#include "zvulkanuniforms.h"
#include "zvulkanbindings.h"
#include "zsysteminfo.h"
#include "z3dlinerenderer.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanstaticpromotionutils.h"
#include "zrenderthreadexecutor_tls.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <cstring>
#include <cstdint>

#include <folly/coro/Task.h>

DECLARE_bool(atlas_vk_cache_draw_secondaries);

namespace nim {
namespace {

const RendererViewState& viewStateForBatch(const Z3DRendererBase& renderer, const RenderBatch& batch)
{
  if (batch.viewStateOverride) {
    return *batch.viewStateOverride;
  }
  return renderer.viewState();
}

glm::mat4 viewportMatrixFor(const ViewportDesc& viewport)
{
  CHECK(viewport.extent.x > 0.0f && viewport.extent.y > 0.0f) << "viewportMatrixFor requires a non-empty viewport";
  const float l = viewport.origin.x;
  const float b = viewport.origin.y;
  const float r = l + viewport.extent.x;
  const float t = b + viewport.extent.y;
  constexpr float n = 0.f;
  constexpr float f = 1.f;
  return glm::mat4(glm::vec4((r - l) / 2.f, 0.f, 0.f, 0.f),
                   glm::vec4(0.f, (t - b) / 2.f, 0.f, 0.f),
                   glm::vec4(0.f, 0.f, (f - n) / 2.f, 0.f),
                   glm::vec4((r + l) / 2.f, (t + b) / 2.f, (f + n) / 2.f, 1.f));
}

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

bool clipPlanesEqual(const ClipPlanesState& a, const ClipPlanesState& b)
{
  if (a.captured != b.captured) {
    return false;
  }
  if (a.enabled != b.enabled) {
    return false;
  }
  if (a.planeCount != b.planeCount) {
    return false;
  }

  const uint32_t count = std::min<uint32_t>(a.planeCount, static_cast<uint32_t>(a.planes.size()));
  for (uint32_t i = 0; i < count; ++i) {
    const glm::vec4 pa = a.planes[i];
    const glm::vec4 pb = b.planes[i];
    if (pa.x != pb.x || pa.y != pb.y || pa.z != pb.z || pa.w != pb.w) {
      return false;
    }
  }
  return true;
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
  // No OIT UBO retention required.
  m_usedThinStaticVBThisFrame = false;
  m_ddpArgsByStream.clear();
  m_thinStaticCopyPendingKeys.clear();
  m_wideStaticCopyPendingKeys.clear();
}

void ZVulkanLinePipelineContext::evictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }

  for (auto it = m_thinStaticCopyPendingKeys.begin(); it != m_thinStaticCopyPendingKeys.end();) {
    if (it->streamKey == streamKey) {
      it = m_thinStaticCopyPendingKeys.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = m_wideStaticCopyPendingKeys.begin(); it != m_wideStaticCopyPendingKeys.end();) {
    if (it->streamKey == streamKey) {
      it = m_wideStaticCopyPendingKeys.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = m_thinStaticCache.begin(); it != m_thinStaticCache.end();) {
    if (it->first.streamKey != streamKey) {
      ++it;
      continue;
    }
    auto& entry = it->second;
    m_backend.releaseStaticSlice(entry.vbPos);
    m_backend.releaseStaticSlice(entry.vbColor);
    m_backend.releaseStaticSlice(entry.ib);
    it = m_thinStaticCache.erase(it);
  }

  for (auto it = m_wideStaticCache.begin(); it != m_wideStaticCache.end();) {
    if (it->first.streamKey != streamKey) {
      ++it;
      continue;
    }
    auto& entry = it->second;
    m_backend.releaseStaticSlice(entry.vbP0);
    m_backend.releaseStaticSlice(entry.vbP1);
    m_backend.releaseStaticSlice(entry.vbC0);
    m_backend.releaseStaticSlice(entry.vbC1);
    m_backend.releaseStaticSlice(entry.vbFlags);
    m_backend.releaseStaticSlice(entry.ib);
    it = m_wideStaticCache.erase(it);
  }

  for (auto it = m_thinSecondaryCache.begin(); it != m_thinSecondaryCache.end();) {
    if (it->first.streamKey == streamKey) {
      it = m_thinSecondaryCache.erase(it);
    } else {
      ++it;
    }
  }

  for (auto& [frameKey, cache] : m_uboCacheByFrameKey) {
    (void)frameKey;
    cache.byStream.erase(streamKey);
  }
}

void ZVulkanLinePipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  const auto fence = m_backend.awaitActiveSubmissionFence("VK line retained UBO lifetime");
  auto keepAlive = currentRenderThreadExecutorKeepAlive("VK line retained UBO lifetime");
  // Hand retained UBOs to the backend so destruction happens after the active
  // submission fence signals. Capture shared_ptr by value to extend lifetime.
  for (auto& sp : m_retainedUbos) {
    m_backend.spawnDetachedTask(
      keepAlive,
      [fence, keep = sp]() mutable -> folly::coro::Task<void> {
        co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(fence);
        co_return;
      }(),
      "VK line retained UBO lifetime");
  }
  m_retainedUbos.clear();
}

void ZVulkanLinePipelineContext::updateUBOs(Z3DRendererBase& renderer,
                                            const RenderBatch& batch,
                                            const LinePayload& payload)
{
  // Use shared per-frame lighting UBO dynamic offset
  m_dynLightingOffset =
    payload.pickingPass ? m_backend.framePickingLightingOffset() : m_backend.frameSharedLightingOffset();

  const auto& viewState = viewStateForBatch(renderer, batch);
  const auto& eyeState = viewState.eyes[static_cast<size_t>(batch.eye)];

  // Frame transforms are shared per eye and updated once per submission in beginRender().
  // If the batch carries a viewStateOverride snapshot, allocate a dedicated
  // frame UBO slice for this draw.
  if (batch.viewStateOverride) {
    FrameTransformsUBOStd140 xf{};
    xf.projection_view_matrix = eyeState.projectionViewMatrix;
    xf.view_matrix = eyeState.viewMatrix;
    xf.projection_matrix = eyeState.projectionMatrix;
    xf.inverse_projection_matrix = eyeState.inverseProjectionMatrix;
    const float orthoFlag = eyeState.isPerspective ? 0.0f : 1.0f;
    xf.parameters = glm::vec4(0.0f, orthoFlag, 0.0f, 0.0f);
    auto slice = m_backend.suballocateUniformFor(payload, sizeof(FrameTransformsUBOStd140));
    std::memcpy(slice.mapped, &xf, sizeof(xf));
    m_dynFrameTransformsOffset = slice.offset;
  } else {
    m_dynFrameTransformsOffset = m_backend.frameTransformsOffset(batch.eye);
  }

  ObjectTransformsUBOStd140 transforms{};
  CHECK(payload.paramsCaptured) << "Line payload missing params";
  const glm::mat4 model = payload.followCoordTransform ? payload.params.coordTransform : glm::mat4(1.0f);
  transforms.pos_transform = model;

  // Line shaders do not consume the normal matrix; keep it as identity to
  // avoid redundant inverse/transpose work.
  transforms.pos_transform_normal_matrix = encodeMat3ToStd140(glm::mat3(1.0f));
  const float sizeScale = payload.followSizeScale ? payload.params.sizeScale : 1.0f;
  transforms.parameters = glm::vec4(sizeScale, 0.0f, 0.0f, 0.0f);
  vulkan::applyBatchClipPlanesToTransforms(batch, transforms);

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params.materialAmbient;
  material.material_specular = payload.params.materialSpecular;
  material.material_shininess = payload.params.materialShininess;
  material.alpha = (payload.pickingPass || !payload.followOpacity) ? 1.0f : payload.params.opacity;

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Line updateUBOs called without an active Vulkan frame-slot key";
  if (payload.streamKey != 0) {
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    auto& entries = frameCache.byStream[payload.streamKey];

    for (auto& cached : entries) {
      if (cached.pickingPass != payload.pickingPass) {
        continue;
      }

      m_dynObjectTransformsOffset = cached.objectTransformsOffset;
      m_dynMaterialOffset = cached.materialOffset;

      if (cached.params == payload.params && cached.followCoordTransform == payload.followCoordTransform &&
          cached.followSizeScale == payload.followSizeScale && cached.followOpacity == payload.followOpacity &&
          clipPlanesEqual(cached.clipPlanes, batch.clipPlanes)) {
        return;
      }

      std::memcpy(m_backend.persistentUniformMappedAt(cached.objectTransformsOffset, sizeof(ObjectTransformsUBOStd140)),
                  &transforms,
                  sizeof(transforms));
      std::memcpy(m_backend.persistentUniformMappedAt(cached.materialOffset, sizeof(MaterialUBOStd140)),
                  &material,
                  sizeof(material));

      cached.params = payload.params;
      cached.followCoordTransform = payload.followCoordTransform;
      cached.followSizeScale = payload.followSizeScale;
      cached.followOpacity = payload.followOpacity;
      cached.clipPlanes = batch.clipPlanes;
      return;
    }

    auto transformsSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
    std::memcpy(transformsSlice.mapped, &transforms, sizeof(transforms));
    m_dynObjectTransformsOffset = transformsSlice.offset;

    auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
    std::memcpy(materialSlice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = materialSlice.offset;

    UboCacheEntry entry{};
    entry.pickingPass = payload.pickingPass;
    entry.params = payload.params;
    entry.followCoordTransform = payload.followCoordTransform;
    entry.followSizeScale = payload.followSizeScale;
    entry.followOpacity = payload.followOpacity;
    entry.clipPlanes = batch.clipPlanes;
    entry.objectTransformsOffset = transformsSlice.offset;
    entry.materialOffset = materialSlice.offset;
    entries.push_back(std::move(entry));
    return;
  }

  // Fallback (should be rare): no stream key, so we cannot assign a stable slice.
  auto transformsSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
  std::memcpy(transformsSlice.mapped, &transforms, sizeof(transforms));
  m_dynObjectTransformsOffset = transformsSlice.offset;

  auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
  std::memcpy(materialSlice.mapped, &material, sizeof(material));
  m_dynMaterialOffset = materialSlice.offset;
}

ZVulkanLinePipelineContext::PipelineInstance&
ZVulkanLinePipelineContext::ensurePipeline(const PipelineKey& key,
                                           const LinePayload& payload,
                                           const vulkan::AttachmentFormats& formats)
{
  (void)payload;
  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Line pipeline missing bindless descriptor set layout";
  const vk::DescriptorSetLayout lightingLayout = m_backend.lightingDescriptorSetLayout();
  CHECK(lightingLayout) << "Line pipeline missing lighting descriptor set layout";
  const vk::DescriptorSetLayout transformsLayout = m_backend.transformDescriptorSetLayout();
  CHECK(transformsLayout) << "Line pipeline missing transforms descriptor set layout";
  const vk::DescriptorSetLayout oitLayout = m_backend.oitDescriptorSetLayout();
  CHECK(oitLayout) << "Line pipeline missing OIT descriptor set layout";
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
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        fragmentShader = "ppll_count_wideline.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        fragmentShader = "ppll_store_wideline.frag.spv";
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
    std::vector<vk::DescriptorSetLayout> setLayouts = {bindlessLayout, lightingLayout, transformsLayout};
    if ((key.shaderHookType == Z3DRendererBase::ShaderHookType::WeightedBlendedInit ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore)) {
      setLayouts.push_back(oitLayout);
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

    auto enablePremultipliedAlphaBlend = [&]() {
      auto attachments = makeBlendAttachments(formats.colorFormats.size(), baseBlend);
      for (auto& attachment : attachments) {
        attachment.blendEnable = true;
        attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
        attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        // Match glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
        attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
    };

    switch (key.shaderHookType) {
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        // Exact OIT PPLL: depth test against opaque depth, but do not write depth.
        instance.pipeline->setDepthTestEnable(true);
        instance.pipeline->setDepthWriteEnable(false);
        break;
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
        // Match GL WA init: depth-test against the (loaded) opaque depth buffer
        // when a depth attachment is present, but do not write depth.
        instance.pipeline->setDepthTestEnable(key.depthFormat.has_value());
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
            // Match GL DDP peel: this attachment uses MAX blending as well.
            attachment.colorBlendOp = vk::BlendOp::eMax;
            attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
            attachment.alphaBlendOp = vk::BlendOp::eMax;
          } else {
            attachment.blendEnable = false;
          }
        }
        instance.pipeline->setColorBlendAttachments(std::move(attachments));
        // Match GL DDP: depth-tested against the (loaded) opaque depth buffer, but do not write depth.
        instance.pipeline->setDepthTestEnable(true);
        instance.pipeline->setDepthWriteEnable(false);
        break;
      }
      default:
        // Match OpenGL compositor selection-box pass: wide lines output
        // premultiplied alpha coverage for anti-aliasing and must blend over
        // the existing scene color. Keep picking pass as overwrite to preserve
        // ID colors.
        if (!key.picking) {
          enablePremultipliedAlphaBlend();
        }
        break;
    }

    vk::PushConstantRange pushRange{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                    .offset = 0,
                                    .size = static_cast<uint32_t>(sizeof(WideLinePushConstants))};
    instance.pipeline->setPushConstantRanges({pushRange});
    instance.pipeline->create();
  } else {
    std::string fragmentShader = "line.frag.spv";
    switch (key.shaderHookType) {
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        fragmentShader = "ppll_count_line.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        fragmentShader = "ppll_store_line.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        fragmentShader = "dual_peeling_init_line.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        fragmentShader = "dual_peeling_peel_line.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        fragmentShader = "wavg_init_line.frag.spv";
        break;
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        fragmentShader = "wblended_init_line.frag.spv";
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
    std::vector<vk::DescriptorSetLayout> setLayouts = {bindlessLayout, lightingLayout, transformsLayout};
    if ((key.shaderHookType == Z3DRendererBase::ShaderHookType::WeightedBlendedInit ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount ||
         key.shaderHookType == Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore)) {
      setLayouts.push_back(oitLayout);
    }
    instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
    instance.pipeline->setDescriptorSetLayouts(setLayouts);
    // Thin lines may produce implementation-dependent winding; disable culling
    // to ensure visibility regardless of vertex order.
    instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);

    // Dual-depth-peeling peel shader consumes bindless blender indices via push constants.
    vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                  .offset = 0,
                                  .size = static_cast<uint32_t>(sizeof(DDPPeelPushConstants))};
    instance.pipeline->setPushConstantRanges({pcRange});
    if (key.shaderHookType == Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount ||
        key.shaderHookType == Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore) {
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
    }
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
          // Match GL DDP peel: this attachment uses MAX blending as well.
          attachment.colorBlendOp = vk::BlendOp::eMax;
          attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.alphaBlendOp = vk::BlendOp::eMax;
        } else {
          attachment.blendEnable = false;
        }
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
      // Match GL DDP: depth-tested against the (loaded) opaque depth buffer, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
    } else if (!key.picking) {
      // Match glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) for line overlays
      // and any non-opaque line draws. Picking must overwrite to keep IDs.
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(formats.colorFormats.size());
      for (auto& attachment : attachments) {
        attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        attachment.blendEnable = true;
        attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
        attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
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

  // Determine vertex count from smooth P0/P1 positions.
  // These streams must be aligned 1:1; mismatches indicate a bug in the CPU
  // smooth-line generation path.
  CHECK(payload.smoothP0Positions.size() == payload.smoothP1Positions.size())
    << "Vulkan wide-line backend skipping batch: smooth position buffers mismatch (p0="
    << payload.smoothP0Positions.size() << " p1=" << payload.smoothP1Positions.size() << ").";
  const size_t vertexCount = payload.smoothP0Positions.size();
  if (vertexCount == 0) {
    return;
  }

  // When provided (non-empty), smooth color streams must also match vertexCount.
  CHECK(payload.smoothP0Colors.empty() || payload.smoothP0Colors.size() == vertexCount)
    << "Vulkan wide-line backend skipping batch: smooth P0 color buffer mismatch (c0=" << payload.smoothP0Colors.size()
    << " vtx=" << vertexCount << ").";
  CHECK(payload.smoothP1Colors.empty() || payload.smoothP1Colors.size() == vertexCount)
    << "Vulkan wide-line backend skipping batch: smooth P1 color buffer mismatch (c1=" << payload.smoothP1Colors.size()
    << " vtx=" << vertexCount << ").";
  CHECK(payload.smoothPickingColors.empty() || payload.smoothPickingColors.size() == vertexCount)
    << "Vulkan wide-line backend skipping batch: smooth picking color buffer mismatch (pick="
    << payload.smoothPickingColors.size() << " vtx=" << vertexCount << ").";

  const size_t indexCount = payload.smoothIndices.size();

  // Fast path: if this stream was promoted to device-local static buffers and
  // nothing changed, bind the static buffers and skip per-frame staging/memcpy.
  if (payload.streamKey != 0) {
    WideCacheKey key{payload.streamKey, pickingPass};
    if (!m_wideStaticCopyPendingKeys.contains(key)) {
      auto it = m_wideStaticCache.find(key);
      if (it != m_wideStaticCache.end()) {
        WideCacheEntry& entry = it->second;
        const bool sizeSame = (entry.vertexCount == vertexCount) && (entry.indexCount == indexCount);
        bool gensSame = (entry.p0Gen == payload.smoothP0PositionsGen) &&
                        (entry.p1Gen == payload.smoothP1PositionsGen) && (entry.flagsGen == payload.smoothFlagsGen) &&
                        (entry.indexGen == (payload.smoothIndicesGen ? payload.smoothIndicesGen : payload.indicesGen));
        if (pickingPass) {
          gensSame = gensSame && (entry.pickGen == payload.smoothPickingColorsGen);
        } else {
          gensSame =
            gensSame && (entry.c0Gen == payload.smoothP0ColorsGen) && (entry.c1Gen == payload.smoothP1ColorsGen);
        }

        if (entry.promoted && sizeSame && gensSame && entry.vbP0 && entry.vbP1 && entry.vbC0 && entry.vbC1 &&
            entry.vbFlags && (indexCount == 0 || entry.ib)) {
          m_wideP0Buffer = entry.vbP0.buffer;
          m_wideP1Buffer = entry.vbP1.buffer;
          m_wideC0Buffer = entry.vbC0.buffer;
          m_wideC1Buffer = entry.vbC1.buffer;
          m_wideFlagsBuffer = entry.vbFlags.buffer;
          m_wideP0Offset = entry.vbP0.offset;
          m_wideP1Offset = entry.vbP1.offset;
          m_wideC0Offset = entry.vbC0.offset;
          m_wideC1Offset = entry.vbC1.offset;
          m_wideFlagsOffset = entry.vbFlags.offset;
          m_wideUploadIndexCount = entry.indexCount;
          if (indexCount > 0) {
            m_wideIndexBuffer = entry.ib.buffer;
            m_wideUploadIndexOffset = entry.ib.offset;
          } else {
            m_wideIndexBuffer = VK_NULL_HANDLE;
            m_wideUploadIndexOffset = 0;
          }
          vulkan::pinStaticSlicesForActiveSubmission(
            m_backend,
            {&entry.vbP0, &entry.vbP1, &entry.vbC0, &entry.vbC1, &entry.vbFlags, &entry.ib});
          entry.usedStaticOnce = true;
          return;
        }
      }
    }
  }

  const size_t pBytes = vertexCount * sizeof(glm::vec3);
  const size_t cBytes = vertexCount * sizeof(glm::vec4);
  const size_t fBytes = vertexCount * sizeof(float);
  m_backend.reserveUploadSlices({
    {pBytes,                        alignof(glm::vec3)},
    {pBytes,                        alignof(glm::vec3)},
    {cBytes,                        alignof(glm::vec4)},
    {cBytes,                        alignof(glm::vec4)},
    {fBytes,                        alignof(float)    },
    {indexCount * sizeof(uint32_t), alignof(uint32_t) }
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
    const auto promoteToStatics = [&](WideCacheEntry& dstEntry) -> bool {
      const size_t idxBytes = indexCount * sizeof(uint32_t);
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(m_backend,
                                                   {
                                                     {&dstEntry.vbP0,    &p0Slice,    pBytes,   alignof(glm::vec3), false},
                                                     {&dstEntry.vbP1,    &p1Slice,    pBytes,   alignof(glm::vec3), false},
                                                     {&dstEntry.vbC0,    &c0Slice,    cBytes,   alignof(glm::vec4), false},
                                                     {&dstEntry.vbC1,    &c1Slice,    cBytes,   alignof(glm::vec4), false},
                                                     {&dstEntry.vbFlags, &flagsSlice, fBytes,   alignof(float),     false},
                                                     {&dstEntry.ib,      &idxSlice,   idxBytes, alignof(uint32_t),  true }
      },
                                                   &stagedBytes)) {
        return false;
      }
      m_backend.addLineBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK line wide promote: p0={}B p1={}B c0={}B c1={}B flags={}B idx={}B",
                             pBytes,
                             pBytes,
                             cBytes,
                             cBytes,
                             fBytes,
                             idxBytes);
      return true;
    };
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
      auto [inserted, _] = m_wideStaticCache.emplace(key, entry);
      WideCacheEntry& insertedEntry = inserted->second;

      // UX: stage into device-local statics on first sight so the next frame
      // can bind fast device-local buffers when geometry is steady.
      if (promoteToStatics(insertedEntry)) {
        insertedEntry.promoted = true;
        insertedEntry.usedStaticOnce = false;
        // Do not bind statics this frame; keep upload slices. Statics bind next frame.
        m_wideStaticCopyPendingKeys.insert(key);
        return;
      }
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

      // A previous draw in this submission scheduled upload->static copies for
      // this stream; do not bind statics until the next submission because the
      // copies are flushed after rendering ends.
      if (m_wideStaticCopyPendingKeys.contains(key)) {
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
        return;
      }

      if (entry.promoted && !sizeSame) {
        vulkan::releaseStaticSlices(m_backend,
                                    {&entry.vbP0, &entry.vbP1, &entry.vbC0, &entry.vbC1, &entry.vbFlags, &entry.ib});
        entry.promoted = false;
        entry.usedStaticOnce = false;
        entry.unchangedFrames = 0;
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

        // Recreate static slices immediately so the next steady frame can bind
        // device-local buffers without a multi-frame warmup.
        if (promoteToStatics(entry)) {
          entry.promoted = true;
          entry.usedStaticOnce = false;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_wideStaticCopyPendingKeys.insert(key);
          return;
        }
        return;
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

        // If this stream is still changing before we've ever bound the static
        // buffers, drop the statics and fall back to upload-only mode to avoid
        // paying upload->device copies every frame without a benefit.
        if (anyChanged && !entry.usedStaticOnce) {
          vulkan::releaseStaticSlices(m_backend,
                                      {&entry.vbP0, &entry.vbP1, &entry.vbC0, &entry.vbC1, &entry.vbFlags, &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
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
          return;
        }

        if (entry.p0Gen != payload.smoothP0PositionsGen) {
          m_backend.scheduleStaticCopy(entry.vbP0.buffer, entry.vbP0.offset, p0Slice, false);
        }
        if (entry.p1Gen != payload.smoothP1PositionsGen) {
          m_backend.scheduleStaticCopy(entry.vbP1.buffer, entry.vbP1.offset, p1Slice, false);
        }
        if (pickingPass) {
          if (entry.pickGen != payload.smoothPickingColorsGen) {
            m_backend.scheduleStaticCopy(entry.vbC0.buffer, entry.vbC0.offset, c0Slice, false);
            m_backend.scheduleStaticCopy(entry.vbC1.buffer, entry.vbC1.offset, c1Slice, false);
          }
        } else {
          if (entry.c0Gen != payload.smoothP0ColorsGen) {
            m_backend.scheduleStaticCopy(entry.vbC0.buffer, entry.vbC0.offset, c0Slice, false);
          }
          if (entry.c1Gen != payload.smoothP1ColorsGen) {
            m_backend.scheduleStaticCopy(entry.vbC1.buffer, entry.vbC1.offset, c1Slice, false);
          }
        }
        if (entry.flagsGen != payload.smoothFlagsGen) {
          m_backend.scheduleStaticCopy(entry.vbFlags.buffer, entry.vbFlags.offset, flagsSlice, false);
        }
        if (entry.indexGen != (payload.smoothIndicesGen ? payload.smoothIndicesGen : payload.indicesGen) &&
            m_wideUploadIndexCount > 0) {
          m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, idxSlice, true);
        }

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

        if (anyChanged) {
          m_wideStaticCopyPendingKeys.insert(key);
          return; // use upload slices for this frame
        }

        m_wideP0Buffer = entry.vbP0.buffer;
        m_wideP1Buffer = entry.vbP1.buffer;
        m_wideC0Buffer = entry.vbC0.buffer;
        m_wideC1Buffer = entry.vbC1.buffer;
        m_wideFlagsBuffer = entry.vbFlags.buffer;
        m_wideP0Offset = entry.vbP0.offset;
        m_wideP1Offset = entry.vbP1.offset;
        m_wideC0Offset = entry.vbC0.offset;
        m_wideC1Offset = entry.vbC1.offset;
        m_wideFlagsOffset = entry.vbFlags.offset;
        m_wideIndexBuffer = entry.ib ? entry.ib.buffer : VK_NULL_HANDLE;
        m_wideUploadIndexOffset = entry.ib.offset;
        vulkan::pinStaticSlicesForActiveSubmission(
          m_backend,
          {&entry.vbP0, &entry.vbP1, &entry.vbC0, &entry.vbC1, &entry.vbFlags, &entry.ib});
        entry.usedStaticOnce = true;
        return;
      }

      // Not promoted: keep observed state up-to-date so unchangedFrames can
      // reach the promotion threshold after a data change.
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

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        if (promoteToStatics(entry)) {
          entry.promoted = true;
          entry.usedStaticOnce = false;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_wideStaticCopyPendingKeys.insert(key);
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

  const uint32_t expectedIndexCount = payload.isLineStrip ? static_cast<uint32_t>(positions.size()) : 0u;
  const uint32_t colorGen = pickingPass ? payload.pickingColorsGen : payload.colorsGen;

  // Fast path: if this stream was promoted to device-local static buffers and
  // nothing changed, bind the static buffers and skip per-frame staging/memcpy.
  if (payload.streamKey != 0) {
    ThinCacheKey key{payload.streamKey, pickingPass, payload.isLineStrip};
    if (!m_thinStaticCopyPendingKeys.contains(key)) {
      auto it = m_thinStaticCache.find(key);
      if (it != m_thinStaticCache.end()) {
        ThinCacheEntry& entry = it->second;
        const bool sizeSame = (entry.vertexCount == vertexCount) && (entry.indexCount == expectedIndexCount);
        const bool gensSame = (entry.positionsGen == payload.positionsGen) && (entry.indexGen == payload.indicesGen) &&
                              (entry.colorsGen == colorGen);
        if (entry.promoted && sizeSame && gensSame && entry.vbPos && entry.vbColor &&
            (!payload.isLineStrip || entry.ib)) {
          m_thinUploadVertexCount = vertexCount;
          m_thinPosBuffer = entry.vbPos.buffer;
          m_thinColorBuffer = entry.vbColor.buffer;
          m_thinPosOffset = entry.vbPos.offset;
          m_thinColorOffset = entry.vbColor.offset;
          m_thinUploadIndexCount = expectedIndexCount;
          if (payload.isLineStrip) {
            m_thinUploadIndexBuffer = entry.ib.buffer;
            m_thinUploadIndexOffset = entry.ib.offset;
          } else {
            m_thinUploadIndexBuffer = VK_NULL_HANDLE;
            m_thinUploadIndexOffset = 0;
          }
          vulkan::pinStaticSlicesForActiveSubmission(m_backend, {&entry.vbPos, &entry.vbColor, &entry.ib});
          m_usedThinStaticVBThisFrame = true;
          entry.usedStaticOnce = true;
          return;
        }
      }
    }
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
    const auto promoteToStatics = [&](ThinCacheEntry& dstEntry) -> bool {
      const size_t idxBytes = payload.isLineStrip ? static_cast<size_t>(m_thinUploadIndexCount) * sizeof(uint32_t) : 0u;
      Z3DRendererVulkanBackend::UploadSlice idxUpload{};
      const Z3DRendererVulkanBackend::UploadSlice* idxSrc = nullptr;
      if (idxBytes > 0) {
        if (!m_thinUploadIndexBuffer) {
          return false;
        }
        idxUpload =
          Z3DRendererVulkanBackend::UploadSlice{m_thinUploadIndexBuffer, m_thinUploadIndexOffset, nullptr, idxBytes};
        idxSrc = &idxUpload;
      }

      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(
            m_backend,
            {
              {&dstEntry.vbPos,   &posSlice, posBytes, alignof(glm::vec3), false},
              {&dstEntry.vbColor, &colSlice, colBytes, alignof(glm::vec4), false},
              {&dstEntry.ib,      idxSrc,    idxBytes, alignof(uint32_t),  true }
      },
            &stagedBytes)) {
        return false;
      }

      m_backend.addLineBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK line thin promote: pos={}B col={}B idx={}B", posBytes, colBytes, idxBytes);
      return true;
    };
    if (it == m_thinStaticCache.end()) {
      ThinCacheEntry entry{};
      entry.vertexCount = vertexCount;
      entry.indexCount = m_thinUploadIndexCount;
      entry.positionsGen = payload.positionsGen;
      entry.colorsGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
      entry.indexGen = payload.indicesGen;
      auto [inserted, _] = m_thinStaticCache.emplace(key, entry);
      ThinCacheEntry& insertedEntry = inserted->second;

      // UX: stage into device-local statics on first sight so the next frame can
      // bind fast device-local buffers when geometry is steady.
      if (promoteToStatics(insertedEntry)) {
        insertedEntry.promoted = true;
        insertedEntry.usedStaticOnce = false;
        // Do not bind statics this frame; keep upload slices. Statics bind next frame.
        m_thinStaticCopyPendingKeys.insert(key);
        return;
      }
    } else {
      ThinCacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == vertexCount) && (entry.indexCount == m_thinUploadIndexCount);
      const bool gensSame = (entry.positionsGen == payload.positionsGen) && (entry.indexGen == payload.indicesGen) &&
                            (entry.colorsGen == colorGen);
      if (sizeSame && gensSame) {
        entry.unchangedFrames++;
      } else {
        entry.unchangedFrames = 0;
      }

      // A previous draw in this submission scheduled upload->static copies for
      // this stream; do not bind statics until the next submission because the
      // copies are flushed after rendering ends.
      if (m_thinStaticCopyPendingKeys.contains(key)) {
        entry.vertexCount = vertexCount;
        entry.indexCount = m_thinUploadIndexCount;
        entry.positionsGen = payload.positionsGen;
        entry.colorsGen = colorGen;
        entry.indexGen = payload.indicesGen;
        return;
      }

      if (entry.promoted && !sizeSame) {
        vulkan::releaseStaticSlices(m_backend, {&entry.vbPos, &entry.vbColor, &entry.ib});
        entry.promoted = false;
        entry.usedStaticOnce = false;
        entry.unchangedFrames = 0;
        entry.vertexCount = vertexCount;
        entry.indexCount = m_thinUploadIndexCount;
        entry.positionsGen = payload.positionsGen;
        entry.colorsGen = colorGen;
        entry.indexGen = payload.indicesGen;

        // Recreate static slices immediately so the next steady frame can bind
        // device-local buffers without a multi-frame warmup.
        if (promoteToStatics(entry)) {
          entry.promoted = true;
          entry.usedStaticOnce = false;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_thinStaticCopyPendingKeys.insert(key);
          return;
        }
        return;
      }

      if (entry.promoted && sizeSame) {
        bool anyChanged = (entry.positionsGen != payload.positionsGen) || (entry.colorsGen != colorGen) ||
                          (payload.isLineStrip && entry.indexGen != payload.indicesGen);

        // If this stream is still changing before we've ever bound the static
        // buffers, drop the statics and fall back to upload-only mode to avoid
        // paying upload->device copies every frame without a benefit.
        if (anyChanged && !entry.usedStaticOnce) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbPos, &entry.vbColor, &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          entry.vertexCount = vertexCount;
          entry.indexCount = m_thinUploadIndexCount;
          entry.positionsGen = payload.positionsGen;
          entry.colorsGen = colorGen;
          entry.indexGen = payload.indicesGen;
          return;
        }

        if (entry.positionsGen != payload.positionsGen) {
          m_backend.scheduleStaticCopy(entry.vbPos.buffer, entry.vbPos.offset, posSlice, false);
        }
        if (entry.colorsGen != colorGen) {
          m_backend.scheduleStaticCopy(entry.vbColor.buffer, entry.vbColor.offset, colSlice, false);
        }
        if (payload.isLineStrip && entry.indexGen != payload.indicesGen && m_thinUploadIndexBuffer) {
          Z3DRendererVulkanBackend::UploadSlice iUpload{m_thinUploadIndexBuffer,
                                                        m_thinUploadIndexOffset,
                                                        nullptr,
                                                        static_cast<size_t>(m_thinUploadIndexCount) * sizeof(uint32_t)};
          if (entry.ib) {
            m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, iUpload, true);
          }
        }

        entry.vertexCount = vertexCount;
        entry.indexCount = m_thinUploadIndexCount;
        entry.positionsGen = payload.positionsGen;
        entry.colorsGen = colorGen;
        entry.indexGen = payload.indicesGen;

        if (anyChanged) {
          m_thinStaticCopyPendingKeys.insert(key);
          return; // use upload slices for this frame
        }

        m_thinPosBuffer = entry.vbPos.buffer;
        m_thinColorBuffer = entry.vbColor.buffer;
        m_thinPosOffset = entry.vbPos.offset;
        m_thinColorOffset = entry.vbColor.offset;
        if (payload.isLineStrip && entry.ib) {
          m_thinUploadIndexBuffer = entry.ib.buffer;
          m_thinUploadIndexOffset = entry.ib.offset;
        }
        vulkan::pinStaticSlicesForActiveSubmission(m_backend, {&entry.vbPos, &entry.vbColor, &entry.ib});
        entry.usedStaticOnce = true;
        return;
      }

      entry.vertexCount = vertexCount;
      entry.indexCount = m_thinUploadIndexCount;
      entry.positionsGen = payload.positionsGen;
      entry.colorsGen = colorGen;
      entry.indexGen = payload.indicesGen;

      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        if (promoteToStatics(entry)) {
          entry.promoted = true;
          entry.usedStaticOnce = false;
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_thinStaticCopyPendingKeys.insert(key);
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
  // Descriptor sets are primed by the backend in beginRender(); avoid record-time rewrites.
  const vk::DescriptorSet dsLighting = m_backend.sharedLightingDescriptorSet();
  const vk::DescriptorSet dsTransforms = m_backend.sharedTransformsDescriptorSetPersistent();
  CHECK(dsLighting && dsTransforms) << "Line pipeline shared descriptor sets missing (lighting/transforms)";

  CHECK(batch.shaderHook.captured) << "Line batch missing shader hook snapshot";
  const auto shaderHook = batch.shaderHook.type;
  const auto& hookPara = batch.shaderHook.para;
  DDPPeelPushConstants ddpPc{};
  const bool usesDdpPeelPc = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (usesDdpPeelPc) {
    CHECK(hookPara.dualDepthPeelingDepthBlenderHandle.valid()) << "Line DDP peel requires a valid depth blender handle";
    CHECK(hookPara.dualDepthPeelingFrontBlenderHandle.valid()) << "Line DDP peel requires a valid front blender handle";
    auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                               m_backend.device(),
                                               "line dual-depth-peeling depth blender");
    auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                               m_backend.device(),
                                               "line dual-depth-peeling front blender");
    ddpPc.ddpDepthBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(depthTex, "line ddp depth blender");
    ddpPc.ddpFrontBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(frontTex, "line ddp front blender");
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

  // Build descriptor sets for draw-only recording
  const std::array<vk::DescriptorSet, 3> descriptorSets{m_backend.bindlessSampledImageDescriptorSet(),
                                                        dsLighting,
                                                        dsTransforms};
  // Dynamic offsets added to drawSpec after it is constructed below.

  uint32_t expectedSets = static_cast<uint32_t>(descriptorSets.size());
  std::array<vk::DescriptorSet, 1> oitDescriptorSets{};
  std::array<ZVulkanDescriptorBindInfo, 1> extraBinds{};
  uint32_t extraBindCount = 0;
  if ((shaderHook == Z3DRendererBase::ShaderHookType::WeightedBlendedInit ||
       shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
       shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel ||
       shaderHook == Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount ||
       shaderHook == Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore)) {
    ZVulkanDescriptorBindInfo oitBind{};
    oitBind.firstSet = vkbind::kSetOIT;
    oitDescriptorSets[0] = m_backend.sharedOITDescriptorSet();
    oitBind.sets = oitDescriptorSets;
    extraBinds[0] = oitBind;
    extraBindCount = 1;
    expectedSets = std::max(expectedSets, vkbind::kSetOIT + 1);
  }

  if (payload.useSmoothLine) {
    uploadWideGeometry(payload, pickingPass);
    if (m_wideP0Buffer == VK_NULL_HANDLE || m_wideUploadIndexCount == 0) {
      return;
    }

    if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
      // Prepare device-local indirect args during init; peel passes must not
      // schedule upload->device copies inside dynamic rendering (copies are
      // flushed after the init pass ends).
      CHECK(payload.streamKey != 0) << "Line DDP init: missing streamKey";
      auto& ddp = m_ddpArgsByStream[payload.streamKey];
      const auto widths = payload.perSegmentWidths;
      const uint32_t segmentCount = m_wideUploadIndexCount / 6u;
      const bool perSegment = !widths.empty();
      const uint32_t drawSegments =
        perSegment ? std::min<uint32_t>(segmentCount, static_cast<uint32_t>(widths.size())) : 0u;

      if (ddp.widePrepared) {
        CHECK(ddp.widePerSegment == perSegment) << "Line DDP init: wide args mode mismatch for streamKey";
        if (perSegment) {
          CHECK(ddp.wideOffsets.size() == static_cast<size_t>(drawSegments))
            << "Line DDP init: wide args segment count mismatch for streamKey";
        } else {
          CHECK(ddp.wideOffsets.size() == 1u) << "Line DDP init: wide args table mismatch for streamKey";
          CHECK(ddp.wideIndexCount == m_wideUploadIndexCount)
            << "Line DDP init: wide args index count mismatch for streamKey";
        }
      } else {
        CHECK(static_cast<VkBuffer>(m_backend.ddpDeviceArgsBuffer()) != VK_NULL_HANDLE)
          << "Line DDP init: device args buffer missing (fatal)";
        ddp.widePerSegment = perSegment;
        if (perSegment) {
          ddp.wideOffsets.assign(drawSegments, vk::DeviceSize(0));
          ddp.wideSegments = drawSegments;
          ddp.wideIndexCount = 6u;
          for (uint32_t i = 0; i < drawSegments; ++i) {
            struct Cmd
            {
              uint32_t indexCount, instanceCount, firstIndex;
              int32_t vertexOffset;
              uint32_t firstInstance;
            } drawCmd{6u, 1u, i * 6u, static_cast<int32_t>(i * 4u), 0u};
            const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
            auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
            if (slice.buffer && slice.mapped) {
              std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
            }
            m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
            ddp.wideOffsets[i] = off;
          }
        } else {
          ddp.wideOffsets.assign(1u, vk::DeviceSize(0));
          ddp.wideSegments = segmentCount;
          ddp.wideIndexCount = m_wideUploadIndexCount;
          struct Cmd
          {
            uint32_t indexCount, instanceCount, firstIndex;
            int32_t vertexOffset;
            uint32_t firstInstance;
          } drawCmd{m_wideUploadIndexCount, 1u, 0u, 0, 0u};
          const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
          auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
          if (slice.buffer && slice.mapped) {
            std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
          }
          m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
          ddp.wideOffsets[0] = off;
        }
        ddp.widePrepared = true;
      }
    }

    ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
    drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
    drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
    drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
    drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
    drawSpec.descriptorSetFirst = 0;
    drawSpec.descriptorSets = descriptorSets;
    // Dynamic offsets order must match set/binding order:
    // - lighting (set1,b0)
    // - frame transforms (set2,b0)
    // - object transforms (set2,b1)
    // - material (set2,b2)
    const std::array<uint32_t, 4> dynamicOffsets{static_cast<uint32_t>(m_dynLightingOffset),
                                                 static_cast<uint32_t>(m_dynFrameTransformsOffset),
                                                 static_cast<uint32_t>(m_dynObjectTransformsOffset),
                                                 static_cast<uint32_t>(m_dynMaterialOffset)};
    drawSpec.dynamicOffsets = dynamicOffsets;
    drawSpec.extraDescriptorBinds = std::span<const ZVulkanDescriptorBindInfo>(extraBinds.data(), extraBindCount);
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

      WideLinePushConstants pc{};
      const glm::mat4 vpMatrix = viewportMatrixFor(batch.pass.viewport);
      pc.viewport_matrix = vpMatrix;
      pc.viewport_matrix_inverse = glm::inverse(vpMatrix);
      CHECK(payload.paramsCaptured) << "Line payload missing params";
      // Match GL: allow renderers to opt out of applying the global sizeScale
      // multiplier (used by overlays like selection bounding boxes / gizmos).
      pc.size_scale = payload.followSizeScale ? payload.params.sizeScale : 1.0f;
      pc.line_texture = 0;
      pc.ddpDepthBlender = usesDdpPeelPc ? ddpPc.ddpDepthBlender : 0;
      pc.ddpFrontBlender = usesDdpPeelPc ? ddpPc.ddpFrontBlender : 0;

      const auto widths = payload.perSegmentWidths;
      const float dpr = renderer.sceneState().devicePixelRatio;
      const bool msaa2x2 =
        (renderer.sceneState().multisample == GeometryMSAAMode::MSAA2x2) && payload.enableMultisample;
      const float sizeScale = pc.size_scale;
      const bool ddpPeelIndirect =
        (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
      const DDPArgs* ddpArgs = nullptr;
      if (ddpPeelIndirect) {
        auto it = m_ddpArgsByStream.find(payload.streamKey);
        CHECK(it != m_ddpArgsByStream.end()) << "Line DDP peel: args not prepared in init";
        ddpArgs = &it->second;
        CHECK(ddpArgs->widePrepared) << "Line DDP peel: wide args not prepared in init";
        const bool perSegment = !widths.empty();
        CHECK(ddpArgs->widePerSegment == perSegment) << "Line DDP peel: wide args mode mismatch for streamKey";
      }
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
        if (ddpPeelIndirect) {
          CHECK(ddpArgs != nullptr);
          CHECK(ddpArgs->wideOffsets.size() == static_cast<size_t>(drawSegments))
            << "Line DDP peel: wide args segment table mismatch";
        }
        for (uint32_t i = 0; i < drawSegments; ++i) {
          pc.line_width = widths[i];
          cb.pushConstants<WideLinePushConstants>(pipelineLayout,
                                                  vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                                  0,
                                                  pc);
          if (ddpPeelIndirect) {
            CHECK(ddpArgs != nullptr);
            const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
            const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
            const vk::DeviceSize off = ddpArgs->wideOffsets[i];
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
        cb.pushConstants<WideLinePushConstants>(pipelineLayout,
                                                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                                0,
                                                pc);
        if (ddpPeelIndirect) {
          CHECK(ddpArgs != nullptr);
          CHECK(ddpArgs->wideOffsets.size() == 1u) << "Line DDP peel: wide args table mismatch";
          const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
          const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
          cb.drawIndexedIndirectCount(argsBuf,
                                      ddpArgs->wideOffsets[0],
                                      cntBuf,
                                      0,
                                      1,
                                      sizeof(VkDrawIndexedIndirectCommand));
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
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = 0;
  drawSpec.descriptorSets = descriptorSets;
  const std::array<uint32_t, 4> dynamicOffsets{static_cast<uint32_t>(m_dynLightingOffset),
                                               static_cast<uint32_t>(m_dynFrameTransformsOffset),
                                               static_cast<uint32_t>(m_dynObjectTransformsOffset),
                                               static_cast<uint32_t>(m_dynMaterialOffset)};
  drawSpec.dynamicOffsets = dynamicOffsets;
  drawSpec.extraDescriptorBinds = std::span<const ZVulkanDescriptorBindInfo>(extraBinds.data(), extraBindCount);
  drawSpec.expectedDescriptorSetCount = expectedSets;

  if (usesDdpPeelPc && !payload.useSmoothLine) {
    drawSpec.pushConstantsData = &ddpPc;
    drawSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(ddpPc));
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    drawSpec.requirePushConstants = true;
  }

  const std::array<vk::Buffer, 2> vertexBuffers{m_thinPosBuffer, m_thinColorBuffer};
  const std::array<vk::DeviceSize, 2> vertexOffsets{m_thinPosOffset, m_thinColorOffset};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  if (payload.isLineStrip && m_thinUploadIndexBuffer && m_thinUploadIndexCount > 0) {
    drawSpec.indexBuffer = m_thinUploadIndexBuffer;
    drawSpec.indexOffset = m_thinUploadIndexOffset;
    drawSpec.indexType = vk::IndexType::eUint32;
    drawSpec.indexCount = m_thinUploadIndexCount;
  } else {
    drawSpec.vertexCount = m_thinUploadVertexCount;
  }
  drawSpec.instanceCount = 1;

  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    // Prepare device-local indirect args during init; copies are flushed after
    // the init pass ends. Peel passes reference the prepared offset.
    CHECK(payload.streamKey != 0) << "Line DDP init: missing streamKey";
    auto& ddp = m_ddpArgsByStream[payload.streamKey];
    const bool indexed = (drawSpec.indexCount > 0);
    if (ddp.thinPrepared) {
      CHECK(ddp.thinIndexed == indexed) << "Line DDP init: thin args indexed mismatch for streamKey";
      if (indexed) {
        CHECK(ddp.thinIndexCount == drawSpec.indexCount)
          << "Line DDP init: thin args index count mismatch for streamKey";
      } else {
        CHECK(ddp.thinVertexCount == drawSpec.vertexCount)
          << "Line DDP init: thin args vertex count mismatch for streamKey";
      }
    } else {
      CHECK(static_cast<VkBuffer>(m_backend.ddpDeviceArgsBuffer()) != VK_NULL_HANDLE)
        << "Line DDP init: device args buffer missing (fatal)";
      if (indexed) {
        struct Cmd
        {
          uint32_t indexCount, instanceCount, firstIndex;
          int32_t vertexOffset;
          uint32_t firstInstance;
        } drawCmd{drawSpec.indexCount,
                  drawSpec.instanceCount,
                  drawSpec.firstIndex,
                  drawSpec.vertexOffset,
                  drawSpec.firstInstance};
        const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
        ddp.thinOffset = off;
        ddp.thinIndexed = true;
        ddp.thinIndexCount = drawSpec.indexCount;
      } else {
        struct Cmd
        {
          uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
        } drawCmd{drawSpec.vertexCount, drawSpec.instanceCount, drawSpec.firstVertex, drawSpec.firstInstance};
        const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
        ddp.thinOffset = off;
        ddp.thinIndexed = false;
        ddp.thinVertexCount = drawSpec.vertexCount;
      }
      ddp.thinPrepared = true;
    }
  }

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Line record called without an active Vulkan frame-slot key";

  const std::array<uint64_t, 2> vertexBufferSegmentIds{
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[0]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[1]),
  };
  uint64_t indexBufferSegmentId = 0;
  if (drawSpec.indexCount > 0 && drawSpec.indexBuffer) {
    indexBufferSegmentId = m_backend.staticArenaSegmentIdForBuffer(drawSpec.indexBuffer);
  }
  const bool allStaticSegments = (vertexBufferSegmentIds[0] != 0) && (vertexBufferSegmentIds[1] != 0) &&
                                 ((drawSpec.indexCount == 0) || (indexBufferSegmentId != 0));

  const bool ddpHook = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                        shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  const bool hasOit = (extraBindCount != 0);
  if (FLAGS_atlas_vk_cache_draw_secondaries &&
      m_backend.device().context().supportsInlineAndSecondaryDynamicRendering() && payload.streamKey != 0 &&
      m_usedThinStaticVBThisFrame && allStaticSegments && !ddpHook) {
    m_backend.notifyDrawSecondaryCacheAttempt();

    ThinSecondaryCacheKey cacheKey{};
    cacheKey.frameKey = frameKey;
    cacheKey.streamKey = payload.streamKey;
    cacheKey.picking = pickingPass;
    cacheKey.lineStrip = payload.isLineStrip;
    cacheKey.shaderHookType = shaderHook;
    cacheKey.eye = batch.eye;
    cacheKey.hasOit = hasOit;
    cacheKey.oitRingIndex = hasOit ? m_backend.sharedOITDescriptorSetRingIndex() : 0u;

    ThinSecondarySignature signature{};
    signature.pipeline = drawSpec.pipelineHandle;
    signature.layout = drawSpec.pipelineLayoutHandle;
    signature.baseDescriptorSets = descriptorSets;
    signature.baseDescriptorGenerations = {m_backend.bindlessSampledImageDescriptorSetGeneration(),
                                           m_backend.sharedLightingDescriptorSetGeneration(),
                                           m_backend.sharedTransformsDescriptorSetPersistentGeneration()};
    signature.hasOit = hasOit;
    if (hasOit) {
      signature.oitDescriptorSet = m_backend.sharedOITDescriptorSet();
      signature.oitDescriptorGeneration = m_backend.sharedOITDescriptorSetGeneration();
    }
    signature.dynamicOffsets = dynamicOffsets;
    signature.vertexBuffers = vertexBuffers;
    signature.vertexOffsets = vertexOffsets;
    signature.vertexBufferSegmentIds = vertexBufferSegmentIds;
    signature.indexBuffer = drawSpec.indexBuffer;
    signature.indexOffset = drawSpec.indexOffset;
    signature.indexBufferSegmentId = indexBufferSegmentId;
    signature.indexType = drawSpec.indexType;
    signature.indexCount = drawSpec.indexCount;
    signature.vertexCount = drawSpec.vertexCount;
    signature.viewport = viewport;
    signature.scissor = scissor;

    auto itCache = m_thinSecondaryCache.find(cacheKey);
    if (itCache != m_thinSecondaryCache.end()) {
      m_backend.notifyDrawSecondaryCacheKeyFound();
      ThinSecondaryCacheEntry& entry = itCache->second;
      const vk::CommandBuffer rawSecondary = entry.commandBuffer;
      if (entry.recorded && rawSecondary != vk::CommandBuffer{}) {
        if (entry.signature == signature) {
          m_backend.notifyDrawSecondaryCacheHit();
          m_backend.notifyDrawSecondaryCacheExecute();
          cmd.executeCommands({rawSecondary});
          m_backend.notifyDrawSubmitted();
          return;
        }
      }
      uint32_t mask = 0;
      const auto& prev = entry.signature;
      if (prev.pipeline != signature.pipeline) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kPipeline;
      }
      if (prev.layout != signature.layout) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kLayout;
      }
      if (prev.baseDescriptorSets != signature.baseDescriptorSets) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kBaseDescriptorSets;
      }
      if (prev.baseDescriptorGenerations != signature.baseDescriptorGenerations) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kBaseDescriptorGenerations;
      }
      if (prev.hasOit != signature.hasOit) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitDescriptorPresence;
      }
      if (prev.oitDescriptorSet != signature.oitDescriptorSet) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitDescriptorSet;
      }
      if (prev.oitDescriptorGeneration != signature.oitDescriptorGeneration) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kOitDescriptorGeneration;
      }
      if (prev.dynamicOffsets != signature.dynamicOffsets) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kDynamicOffsets;
      }
      if (prev.vertexBuffers != signature.vertexBuffers) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kVertexBuffers;
      }
      if (prev.vertexOffsets != signature.vertexOffsets) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kVertexOffsets;
      }
      if (prev.vertexBufferSegmentIds != signature.vertexBufferSegmentIds) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kVertexBufferLifetime;
      }
      if (prev.indexBuffer != signature.indexBuffer || prev.indexOffset != signature.indexOffset ||
          prev.indexType != signature.indexType) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kIndexState;
      }
      if (prev.indexBufferSegmentId != signature.indexBufferSegmentId) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kIndexBufferLifetime;
      }
      if (prev.indexCount != signature.indexCount || prev.vertexCount != signature.vertexCount) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kCounts;
      }
      if (prev.viewport.x != signature.viewport.x || prev.viewport.y != signature.viewport.y ||
          prev.viewport.width != signature.viewport.width || prev.viewport.height != signature.viewport.height ||
          prev.viewport.minDepth != signature.viewport.minDepth ||
          prev.viewport.maxDepth != signature.viewport.maxDepth) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kViewport;
      }
      if (prev.scissor.offset.x != signature.scissor.offset.x || prev.scissor.offset.y != signature.scissor.offset.y ||
          prev.scissor.extent.width != signature.scissor.extent.width ||
          prev.scissor.extent.height != signature.scissor.extent.height) {
        mask |= Z3DRendererVulkanBackend::DrawSecondarySignatureMismatchMask::kScissor;
      }
      m_backend.notifyDrawSecondaryCacheSignatureMismatchMask(mask);
    }

    ThinSecondaryCacheEntry& entry = m_thinSecondaryCache[cacheKey];
    entry.signature = signature;
    m_backend.notifyDrawSecondaryCacheBuild();

    vk::SampleCountFlagBits rasterSamples = vk::SampleCountFlagBits::e1;
    bool samplesCaptured = false;
    for (const auto& attachment : batch.pass.colorAttachments) {
      if (!attachment.handle.valid()) {
        continue;
      }
      const auto& tex =
        vulkan::textureFromHandle(attachment.handle, m_backend.device(), "line secondary inheritance color");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Line secondary: mismatched MSAA sample counts in attachments";
      }
    }
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      const auto& tex = vulkan::textureFromHandle(batch.pass.depthAttachment->handle,
                                                  m_backend.device(),
                                                  "line secondary inheritance depth");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Line secondary: mismatched MSAA sample counts in attachments";
      }
    }

    const vk::Format* colorFormatsPtr = formats.colorFormats.empty() ? nullptr : formats.colorFormats.data();
    const vk::Format depthFormat = formats.depthFormat.value_or(vk::Format::eUndefined);
    vk::CommandBufferInheritanceRenderingInfo renderingInheritance{};
    renderingInheritance.rasterizationSamples = rasterSamples;
    renderingInheritance.colorAttachmentCount = static_cast<uint32_t>(formats.colorFormats.size());
    renderingInheritance.pColorAttachmentFormats = colorFormatsPtr;
    renderingInheritance.depthAttachmentFormat = depthFormat;
    renderingInheritance.stencilAttachmentFormat = vk::Format::eUndefined;

    vk::CommandBufferInheritanceInfo inheritance{};
    inheritance.pNext = &renderingInheritance;

    ZVulkanSecondaryBuildInfo secondaryInfo{};
    secondaryInfo.device = &m_backend.device().context().device();
    secondaryInfo.commandPool = m_backend.device().context().commandPool();
    secondaryInfo.inheritance = inheritance;

    entry.commandBuffer = buildStaticSecondary(secondaryInfo, [&](vk::raii::CommandBuffer& secondaryCmd) {
      ZVulkanPipelineCommandRecorder secondaryRecorder(secondaryCmd);
      secondaryRecorder.recordGraphicsDraw(drawSpec);
    });
    entry.recorded = true;

    m_backend.notifyDrawSecondaryCacheExecute();
    cmd.executeCommands({static_cast<vk::CommandBuffer>(entry.commandBuffer)});
    return;
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);
  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    const bool indexed = (drawSpec.indexCount > 0);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      auto it = m_ddpArgsByStream.find(payload.streamKey);
      CHECK(it != m_ddpArgsByStream.end()) << "Line DDP peel: args not prepared in init";
      const DDPArgs& ddp = it->second;
      CHECK(ddp.thinPrepared) << "Line DDP peel: thin args not prepared in init";
      CHECK(ddp.thinIndexed == indexed) << "Line DDP peel: thin args indexed mismatch for streamKey";
      if (indexed) {
        CHECK(ddp.thinIndexCount == drawSpec.indexCount) << "Line DDP peel: thin index count mismatch";
        c.drawIndexedIndirectCount(argsBuf, ddp.thinOffset, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
      } else {
        CHECK(ddp.thinVertexCount == drawSpec.vertexCount) << "Line DDP peel: thin vertex count mismatch";
        c.drawIndirectCount(argsBuf, ddp.thinOffset, cntBuf, 0, 1, sizeof(VkDrawIndirectCommand));
      }
    });
  } else {
    recorder.recordGraphicsDraw(drawSpec);
  }
}

} // namespace nim
