#include "zvulkanlinepipelinecontext.h"
#include "zcommandlineflags.h"

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
#include <limits>
#include <string>
#include <tuple>
#include <vector>
#include <cstring>
#include <cstdint>

#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>

ABSL_DECLARE_FLAG(bool, atlas_vk_cache_draw_secondaries);

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

size_t staticSliceBytes(const Z3DRendererVulkanBackend::StaticSlice& slice)
{
  return slice ? slice.size : 0u;
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
  , m_streamUsageTracker(Z3DRendererVulkanBackend::StaticCacheOwner::Line)
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
  m_thinGeometryStreamCache.resetFrame();
  m_thinAppearanceStreamCache.resetFrame();
  m_wideGeometryStreamCache.resetFrame();
  m_wideAppearanceStreamCache.resetFrame();
}

void ZVulkanLinePipelineContext::evictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }

  m_streamUsageTracker.eraseStream(streamKey);

  m_thinGeometryStreamCache.evictStream(streamKey, [this](ThinGeometryCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbPos);
    m_backend.releaseStaticSlice(entry.ib);
  });

  m_thinAppearanceStreamCache.evictStream(streamKey, [this](ThinAppearanceCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbColor);
  });

  m_wideGeometryStreamCache.evictStream(streamKey, [this](WideGeometryCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbP0);
    m_backend.releaseStaticSlice(entry.vbP1);
    m_backend.releaseStaticSlice(entry.vbFlags);
    m_backend.releaseStaticSlice(entry.ib);
  });

  m_wideAppearanceStreamCache.evictStream(streamKey, [this](WideAppearanceCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbC0);
    m_backend.releaseStaticSlice(entry.vbC1);
  });

  for (auto it = m_ddpArgsByStream.begin(); it != m_ddpArgsByStream.end();) {
    if (it->first.streamKey == streamKey) {
      it = m_ddpArgsByStream.erase(it);
    } else {
      ++it;
    }
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

void ZVulkanLinePipelineContext::touchStaticStream(uint64_t streamKey)
{
  m_streamUsageTracker.touch(streamKey, m_backend.currentStaticCacheEpoch());
}

size_t ZVulkanLinePipelineContext::staticBytesForStream(uint64_t streamKey,
                                                        Z3DRendererVulkanBackend::StaticPressureDomain domain) const
{
  return m_thinGeometryStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const ThinGeometryCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbPos);
             }
             return staticSliceBytes(entry.ib);
           }) +
         m_thinAppearanceStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const ThinAppearanceCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbColor);
             }
             return size_t{0};
           }) +
         m_wideGeometryStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const WideGeometryCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbP0) + staticSliceBytes(entry.vbP1) + staticSliceBytes(entry.vbFlags);
             }
             return staticSliceBytes(entry.ib);
           }) +
         m_wideAppearanceStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const WideAppearanceCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbC0) + staticSliceBytes(entry.vbC1);
             }
             return size_t{0};
           });
}

std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
ZVulkanLinePipelineContext::oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain,
                                                        uint64_t protectedEpoch) const
{
  return m_streamUsageTracker.oldestEvictableStaticStream(
    domain,
    protectedEpoch,
    [this](uint64_t streamKey, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
      return staticBytesForStream(streamKey, cacheDomain);
    });
}

size_t ZVulkanLinePipelineContext::evictStaticStreamForPressure(uint64_t streamKey)
{
  const size_t vbBytes = staticBytesForStream(streamKey, Z3DRendererVulkanBackend::StaticPressureDomain::Vertex);
  const size_t ibBytes = staticBytesForStream(streamKey, Z3DRendererVulkanBackend::StaticPressureDomain::Index);
  evictStream(streamKey);
  return vbBytes + ibBytes;
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
    m_backend.spawnDetachedTask(keepAlive,
                                folly::coro::co_invoke([fence, keep = sp]() mutable -> folly::coro::Task<void> {
                                  co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(fence);
                                  co_return;
                                }),
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
    const uint32_t submissionId = m_backend.activeSubmissionId();
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    auto& entries = frameCache.byStream[payload.streamKey];
    UboCacheEntry* reusableEntry = nullptr;

    for (auto& cached : entries) {
      if (cached.pickingPass != payload.pickingPass) {
        continue;
      }

      if (cached.params == payload.params && cached.followCoordTransform == payload.followCoordTransform &&
          cached.followSizeScale == payload.followSizeScale && cached.followOpacity == payload.followOpacity &&
          clipPlanesEqual(cached.clipPlanes, batch.clipPlanes)) {
        cached.lastSubmissionId = submissionId;
        m_dynObjectTransformsOffset = cached.objectTransformsOffset;
        m_dynMaterialOffset = cached.materialOffset;
        return;
      }

      if (cached.lastSubmissionId != submissionId && reusableEntry == nullptr) {
        reusableEntry = &cached;
      }
    }

    if (reusableEntry != nullptr) {
      std::memcpy(
        m_backend.persistentUniformMappedAt(reusableEntry->objectTransformsOffset, sizeof(ObjectTransformsUBOStd140)),
        &transforms,
        sizeof(transforms));
      std::memcpy(m_backend.persistentUniformMappedAt(reusableEntry->materialOffset, sizeof(MaterialUBOStd140)),
                  &material,
                  sizeof(material));

      reusableEntry->params = payload.params;
      reusableEntry->followCoordTransform = payload.followCoordTransform;
      reusableEntry->followSizeScale = payload.followSizeScale;
      reusableEntry->followOpacity = payload.followOpacity;
      reusableEntry->clipPlanes = batch.clipPlanes;
      reusableEntry->lastSubmissionId = submissionId;
      m_dynObjectTransformsOffset = reusableEntry->objectTransformsOffset;
      m_dynMaterialOffset = reusableEntry->materialOffset;
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
    entry.lastSubmissionId = submissionId;
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
  const size_t pBytes = vertexCount * sizeof(glm::vec3);
  const size_t cBytes = vertexCount * sizeof(glm::vec4);
  const size_t fBytes = vertexCount * sizeof(float);
  const uint32_t wideIndexGen = payload.smoothIndicesGen ? payload.smoothIndicesGen : payload.indicesGen;
  const bool hasStreamKey = payload.streamKey != 0;
  const WideGeometryCacheKey geometryKey{payload.streamKey, payload.streamSegmentOrdinal};
  const WideAppearanceCacheKey appearanceKey{payload.streamKey, payload.streamSegmentOrdinal, pickingPass};

  WideGeometryCacheEntry* boundGeometryStaticEntry = nullptr;
  WideAppearanceCacheEntry* boundAppearanceStaticEntry = nullptr;
  bool geometryBound = false;
  bool appearanceBound = false;

  auto bindGeometryStatic = [&](WideGeometryCacheEntry& entry) {
    m_wideP0Buffer = entry.vbP0.buffer;
    m_wideP1Buffer = entry.vbP1.buffer;
    m_wideFlagsBuffer = entry.vbFlags.buffer;
    m_wideP0Offset = entry.vbP0.offset;
    m_wideP1Offset = entry.vbP1.offset;
    m_wideFlagsOffset = entry.vbFlags.offset;
    m_wideUploadIndexCount = entry.indexCount;
    if (indexCount > 0) {
      m_wideIndexBuffer = entry.ib.buffer;
      m_wideUploadIndexOffset = entry.ib.offset;
    } else {
      m_wideIndexBuffer = VK_NULL_HANDLE;
      m_wideUploadIndexOffset = 0;
    }
    boundGeometryStaticEntry = &entry;
    geometryBound = true;
  };
  auto bindAppearanceStatic = [&](WideAppearanceCacheEntry& entry) {
    m_wideC0Buffer = entry.vbC0.buffer;
    m_wideC1Buffer = entry.vbC1.buffer;
    m_wideC0Offset = entry.vbC0.offset;
    m_wideC1Offset = entry.vbC1.offset;
    boundAppearanceStaticEntry = &entry;
    appearanceBound = true;
  };

  if (hasStreamKey) {
    if (m_wideGeometryStreamCache.hasPendingCopy(geometryKey)) {
      if (const WidePendingGeometryUploadBinding* pending = m_wideGeometryStreamCache.findPendingUpload(geometryKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == vertexCount && pending->indexCount == indexCount;
        const bool gensSame = pending->p0Gen == payload.smoothP0PositionsGen &&
                              pending->p1Gen == payload.smoothP1PositionsGen &&
                              pending->flagsGen == payload.smoothFlagsGen && pending->indexGen == wideIndexGen;
        const bool buffersOk = pending->p0.buffer && pending->p1.buffer && pending->flags.buffer &&
                               ((indexCount == 0) || pending->index.buffer);
        if (sizeSame && gensSame && buffersOk) {
          m_wideP0Buffer = pending->p0.buffer;
          m_wideP1Buffer = pending->p1.buffer;
          m_wideFlagsBuffer = pending->flags.buffer;
          m_wideP0Offset = pending->p0.offset;
          m_wideP1Offset = pending->p1.offset;
          m_wideFlagsOffset = pending->flags.offset;
          m_wideUploadIndexCount = pending->indexCount;
          if (indexCount > 0) {
            m_wideIndexBuffer = pending->index.buffer;
            m_wideUploadIndexOffset = pending->index.offset;
          } else {
            m_wideIndexBuffer = VK_NULL_HANDLE;
            m_wideUploadIndexOffset = 0;
          }
          geometryBound = true;
        }
      }
    }
    if (!geometryBound && !m_wideGeometryStreamCache.hasPendingCopy(geometryKey)) {
      if (WideGeometryCacheEntry* entry = m_wideGeometryStreamCache.findEntry(geometryKey); entry != nullptr) {
        const bool sizeSame = (entry->vertexCount == vertexCount) && (entry->indexCount == indexCount);
        const bool gensSame = (entry->p0Gen == payload.smoothP0PositionsGen) &&
                              (entry->p1Gen == payload.smoothP1PositionsGen) &&
                              (entry->flagsGen == payload.smoothFlagsGen) && (entry->indexGen == wideIndexGen);
        if (entry->promoted && sizeSame && gensSame && entry->vbP0 && entry->vbP1 && entry->vbFlags &&
            (indexCount == 0 || entry->ib)) {
          bindGeometryStatic(*entry);
        }
      }
    }

    if (m_wideAppearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (const WidePendingAppearanceUploadBinding* pending =
            m_wideAppearanceStreamCache.findPendingUpload(appearanceKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == vertexCount;
        bool gensSame = true;
        if (pickingPass) {
          gensSame = pending->pickGen == payload.smoothPickingColorsGen;
        } else {
          gensSame = (pending->c0Gen == payload.smoothP0ColorsGen) && (pending->c1Gen == payload.smoothP1ColorsGen);
        }
        const bool buffersOk = pending->c0.buffer && pending->c1.buffer;
        if (sizeSame && gensSame && buffersOk) {
          m_wideC0Buffer = pending->c0.buffer;
          m_wideC1Buffer = pending->c1.buffer;
          m_wideC0Offset = pending->c0.offset;
          m_wideC1Offset = pending->c1.offset;
          appearanceBound = true;
        }
      }
    }
    if (!appearanceBound && !m_wideAppearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (WideAppearanceCacheEntry* entry = m_wideAppearanceStreamCache.findEntry(appearanceKey); entry != nullptr) {
        const bool sizeSame = entry->vertexCount == vertexCount;
        bool gensSame = true;
        if (pickingPass) {
          gensSame = entry->pickGen == payload.smoothPickingColorsGen;
        } else {
          gensSame = (entry->c0Gen == payload.smoothP0ColorsGen) && (entry->c1Gen == payload.smoothP1ColorsGen);
        }
        if (entry->promoted && sizeSame && gensSame && entry->vbC0 && entry->vbC1) {
          bindAppearanceStatic(*entry);
        }
      }
    }
  }

  if (geometryBound && appearanceBound) {
    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                 {&boundGeometryStaticEntry->vbP0,
                                                  &boundGeometryStaticEntry->vbP1,
                                                  &boundGeometryStaticEntry->vbFlags,
                                                  &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundAppearanceStaticEntry->vbC0, &boundAppearanceStaticEntry->vbC1});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    return;
  }

  const bool needGeometryUpload = !geometryBound;
  const bool needAppearanceUpload = !appearanceBound;
  // A pass may reuse only geometry or only appearance statics while uploading the
  // other half. Protect the selected static slices before upload reservation or
  // promotion can invoke the global residency broker.
  if (boundGeometryStaticEntry != nullptr) {
    vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                               {&boundGeometryStaticEntry->vbP0,
                                                &boundGeometryStaticEntry->vbP1,
                                                &boundGeometryStaticEntry->vbFlags,
                                                &boundGeometryStaticEntry->ib});
    boundGeometryStaticEntry->usedStaticOnce = true;
    touchStaticStream(geometryKey.streamKey);
  }
  if (boundAppearanceStaticEntry != nullptr) {
    vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                               {&boundAppearanceStaticEntry->vbC0, &boundAppearanceStaticEntry->vbC1});
    boundAppearanceStaticEntry->usedStaticOnce = true;
    touchStaticStream(appearanceKey.streamKey);
  }
  m_backend.reserveUploadSlices({
    {needGeometryUpload ? pBytes : 0u,                        alignof(glm::vec3)},
    {needGeometryUpload ? pBytes : 0u,                        alignof(glm::vec3)},
    {needAppearanceUpload ? cBytes : 0u,                      alignof(glm::vec4)},
    {needAppearanceUpload ? cBytes : 0u,                      alignof(glm::vec4)},
    {needGeometryUpload ? fBytes : 0u,                        alignof(float)    },
    {needGeometryUpload ? indexCount * sizeof(uint32_t) : 0u, alignof(uint32_t) }
  });
  Z3DRendererVulkanBackend::UploadSlice p0Slice{};
  Z3DRendererVulkanBackend::UploadSlice p1Slice{};
  Z3DRendererVulkanBackend::UploadSlice c0Slice{};
  Z3DRendererVulkanBackend::UploadSlice c1Slice{};
  Z3DRendererVulkanBackend::UploadSlice flagsSlice{};
  Z3DRendererVulkanBackend::UploadSlice idxSlice{};
  if (needGeometryUpload) {
    p0Slice = m_backend.suballocateUpload(pBytes, alignof(glm::vec3));
    p1Slice = m_backend.suballocateUpload(pBytes, alignof(glm::vec3));
    flagsSlice = m_backend.suballocateUpload(fBytes, alignof(float));
  }
  if (needAppearanceUpload) {
    c0Slice = m_backend.suballocateUpload(cBytes, alignof(glm::vec4));
    c1Slice = m_backend.suballocateUpload(cBytes, alignof(glm::vec4));
  }
  if (needGeometryUpload && indexCount > 0) {
    idxSlice = m_backend.suballocateUpload(indexCount * sizeof(uint32_t), alignof(uint32_t));
  }
  if ((needGeometryUpload &&
       (!p0Slice.buffer || !p0Slice.mapped || !p1Slice.buffer || !p1Slice.mapped || !flagsSlice.buffer ||
        !flagsSlice.mapped || (indexCount > 0 && (!idxSlice.buffer || !idxSlice.mapped)))) ||
      (needAppearanceUpload && (!c0Slice.buffer || !c0Slice.mapped || !c1Slice.buffer || !c1Slice.mapped))) {
    return;
  }

  const bool hasP0Colors = !payload.smoothP0Colors.empty();
  const bool hasP1Colors = !payload.smoothP1Colors.empty();
  const glm::vec4 defaultColor(0.f, 0.f, 0.f, 1.f);
  if (needGeometryUpload) {
    std::memcpy(p0Slice.mapped, payload.smoothP0Positions.data(), pBytes);
    std::memcpy(p1Slice.mapped, payload.smoothP1Positions.data(), pBytes);
    auto* fOut = static_cast<float*>(flagsSlice.mapped);
    for (size_t i = 0; i < vertexCount; ++i) {
      fOut[i] = (i < payload.smoothFlags.size()) ? payload.smoothFlags[i] : 0.0f;
    }
    m_wideP0Buffer = p0Slice.buffer;
    m_wideP1Buffer = p1Slice.buffer;
    m_wideFlagsBuffer = flagsSlice.buffer;
    m_wideP0Offset = p0Slice.offset;
    m_wideP1Offset = p1Slice.offset;
    m_wideFlagsOffset = flagsSlice.offset;
    m_wideUploadIndexCount = static_cast<uint32_t>(indexCount);
    if (indexCount > 0) {
      auto* out = static_cast<uint32_t*>(idxSlice.mapped);
      CHECK(payload.indexValueBias <= std::numeric_limits<uint32_t>::max())
        << "Line payload indexValueBias exceeds uint32_t range";
      const uint32_t indexValueBias = payload.indexValueBias;
      for (size_t i = 0; i < indexCount; ++i) {
        CHECK(payload.smoothIndices[i] >= indexValueBias)
          << "Segmented wide-line index underflow during Vulkan upload rebase";
        out[i] = payload.smoothIndices[i] - indexValueBias;
      }
      m_wideIndexBuffer = idxSlice.buffer;
      m_wideUploadIndexOffset = idxSlice.offset;
    } else {
      m_wideIndexBuffer = VK_NULL_HANDLE;
      m_wideUploadIndexOffset = 0;
    }
  }
  if (needAppearanceUpload) {
    if (pickingPass) {
      if (!payload.smoothPickingColors.empty()) {
        const size_t n = std::min(vertexCount, payload.smoothPickingColors.size());
        std::memcpy(c0Slice.mapped, payload.smoothPickingColors.data(), n * sizeof(glm::vec4));
        std::memcpy(c1Slice.mapped, payload.smoothPickingColors.data(), n * sizeof(glm::vec4));
      } else if (!payload.pickingColors.empty()) {
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
    m_wideC0Buffer = c0Slice.buffer;
    m_wideC1Buffer = c1Slice.buffer;
    m_wideC0Offset = c0Slice.offset;
    m_wideC1Offset = c1Slice.offset;
  }

  const size_t uploadBytesStaged = (needGeometryUpload ? (2 * pBytes + fBytes + indexCount * sizeof(uint32_t)) : 0u) +
                                   (needAppearanceUpload ? (2 * cBytes) : 0u);
  m_backend.addLineBytesStaged(uploadBytesStaged);

  if (hasStreamKey) {
    auto rememberPendingGeometryUpload = [&]() {
      if (!needGeometryUpload) {
        return;
      }
      WidePendingGeometryUploadBinding binding{};
      binding.p0 = p0Slice;
      binding.p1 = p1Slice;
      binding.flags = flagsSlice;
      binding.index = idxSlice;
      binding.vertexCount = static_cast<uint32_t>(vertexCount);
      binding.indexCount = static_cast<uint32_t>(indexCount);
      binding.p0Gen = payload.smoothP0PositionsGen;
      binding.p1Gen = payload.smoothP1PositionsGen;
      binding.flagsGen = payload.smoothFlagsGen;
      binding.indexGen = wideIndexGen;
      m_wideGeometryStreamCache.rememberPendingUploadBinding(geometryKey, binding);
    };
    auto rememberPendingAppearanceUpload = [&]() {
      if (!needAppearanceUpload) {
        return;
      }
      WidePendingAppearanceUploadBinding binding{};
      binding.c0 = c0Slice;
      binding.c1 = c1Slice;
      binding.vertexCount = static_cast<uint32_t>(vertexCount);
      binding.c0Gen = payload.smoothP0ColorsGen;
      binding.c1Gen = payload.smoothP1ColorsGen;
      binding.pickGen = payload.smoothPickingColorsGen;
      m_wideAppearanceStreamCache.rememberPendingUploadBinding(appearanceKey, binding);
    };

    const int kPromotionThreshold = 2;
    auto promoteGeometryToStatics = [&](WideGeometryCacheEntry& entry) -> bool {
      if (!needGeometryUpload) {
        return false;
      }
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(m_backend,
                                                   {
                                                     {&entry.vbP0,    &p0Slice,             pBytes, alignof(glm::vec3), false},
                                                     {&entry.vbP1,    &p1Slice,             pBytes, alignof(glm::vec3), false},
                                                     {&entry.vbFlags, &flagsSlice,          fBytes, alignof(float),     false},
                                                     {&entry.ib,
                                                      indexCount > 0 ? &idxSlice : nullptr,
                                                      indexCount * sizeof(uint32_t),
                                                      alignof(uint32_t),
                                                      true                                                                   }
      },
                                                   &stagedBytes)) {
        return false;
      }
      m_backend.addLineBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK line wide geometry promote: p0={}B p1={}B flags={}B idx={}B",
                             pBytes,
                             pBytes,
                             fBytes,
                             indexCount * sizeof(uint32_t));
      return true;
    };
    auto promoteAppearanceToStatics = [&](WideAppearanceCacheEntry& entry) -> bool {
      if (!needAppearanceUpload) {
        return false;
      }
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(m_backend,
                                                   {
                                                     {&entry.vbC0, &c0Slice, cBytes, alignof(glm::vec4), false},
                                                     {&entry.vbC1, &c1Slice, cBytes, alignof(glm::vec4), false},
      },
                                                   &stagedBytes)) {
        return false;
      }
      m_backend.addLineBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK line wide appearance promote: c0={}B c1={}B", cBytes, cBytes);
      return true;
    };

    {
      auto [entry, inserted] = m_wideGeometryStreamCache.tryEmplaceEntry(geometryKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevIndexCount = entry.indexCount;
      const uint32_t prevP0Gen = entry.p0Gen;
      const uint32_t prevP1Gen = entry.p1Gen;
      const uint32_t prevFlagsGen = entry.flagsGen;
      const uint32_t prevIndexGen = entry.indexGen;
      const bool sizeSame = (prevVertexCount == vertexCount) && (prevIndexCount == indexCount);
      const bool gensSame = (prevP0Gen == payload.smoothP0PositionsGen) &&
                            (prevP1Gen == payload.smoothP1PositionsGen) && (prevFlagsGen == payload.smoothFlagsGen) &&
                            (prevIndexGen == wideIndexGen);
      entry.unchangedFrames = (!inserted && sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      if (needGeometryUpload) {
        if (m_wideGeometryStreamCache.hasPendingCopy(geometryKey)) {
          rememberPendingGeometryUpload();
        } else if (inserted) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_wideGeometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbP0, &entry.vbP1, &entry.vbFlags, &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_wideGeometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool p0Changed = prevP0Gen != payload.smoothP0PositionsGen;
          const bool p1Changed = prevP1Gen != payload.smoothP1PositionsGen;
          const bool flagsChanged = prevFlagsGen != payload.smoothFlagsGen;
          const bool indexChanged = prevIndexGen != wideIndexGen;
          const bool anyChanged = p0Changed || p1Changed || flagsChanged || indexChanged;
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbP0, &entry.vbP1, &entry.vbFlags, &entry.ib});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (p0Changed) {
              m_backend.scheduleStaticCopy(entry.vbP0.buffer, entry.vbP0.offset, p0Slice, false);
              restagedBytes += pBytes;
            }
            if (p1Changed) {
              m_backend.scheduleStaticCopy(entry.vbP1.buffer, entry.vbP1.offset, p1Slice, false);
              restagedBytes += pBytes;
            }
            if (flagsChanged) {
              m_backend.scheduleStaticCopy(entry.vbFlags.buffer, entry.vbFlags.offset, flagsSlice, false);
              restagedBytes += fBytes;
            }
            if (indexChanged && indexCount > 0) {
              m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, idxSlice, true);
              restagedBytes += indexCount * sizeof(uint32_t);
            }
            if (restagedBytes > 0) {
              m_backend.addLineBytesStaged(restagedBytes);
            }
            m_wideGeometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_wideGeometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        }
      }

      entry.vertexCount = static_cast<uint32_t>(vertexCount);
      entry.indexCount = static_cast<uint32_t>(indexCount);
      entry.p0Gen = payload.smoothP0PositionsGen;
      entry.p1Gen = payload.smoothP1PositionsGen;
      entry.flagsGen = payload.smoothFlagsGen;
      entry.indexGen = wideIndexGen;
    }

    {
      auto [entry, inserted] = m_wideAppearanceStreamCache.tryEmplaceEntry(appearanceKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevC0Gen = entry.c0Gen;
      const uint32_t prevC1Gen = entry.c1Gen;
      const uint32_t prevPickGen = entry.pickGen;
      const bool sizeSame = prevVertexCount == vertexCount;
      bool gensSame = true;
      if (pickingPass) {
        gensSame = prevPickGen == payload.smoothPickingColorsGen;
      } else {
        gensSame = (prevC0Gen == payload.smoothP0ColorsGen) && (prevC1Gen == payload.smoothP1ColorsGen);
      }
      entry.unchangedFrames = (!inserted && sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      if (needAppearanceUpload) {
        if (m_wideAppearanceStreamCache.hasPendingCopy(appearanceKey)) {
          rememberPendingAppearanceUpload();
        } else if (inserted) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_wideAppearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbC0, &entry.vbC1});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_wideAppearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && sizeSame) {
          bool anyChanged = false;
          if (pickingPass) {
            anyChanged = prevPickGen != payload.smoothPickingColorsGen;
          } else {
            anyChanged = (prevC0Gen != payload.smoothP0ColorsGen) || (prevC1Gen != payload.smoothP1ColorsGen);
          }
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbC0, &entry.vbC1});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (pickingPass) {
              m_backend.scheduleStaticCopy(entry.vbC0.buffer, entry.vbC0.offset, c0Slice, false);
              m_backend.scheduleStaticCopy(entry.vbC1.buffer, entry.vbC1.offset, c1Slice, false);
              restagedBytes += 2 * cBytes;
            } else {
              if (prevC0Gen != payload.smoothP0ColorsGen) {
                m_backend.scheduleStaticCopy(entry.vbC0.buffer, entry.vbC0.offset, c0Slice, false);
                restagedBytes += cBytes;
              }
              if (prevC1Gen != payload.smoothP1ColorsGen) {
                m_backend.scheduleStaticCopy(entry.vbC1.buffer, entry.vbC1.offset, c1Slice, false);
                restagedBytes += cBytes;
              }
            }
            if (restagedBytes > 0) {
              m_backend.addLineBytesStaged(restagedBytes);
            }
            m_wideAppearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_wideAppearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        }
      }

      entry.vertexCount = static_cast<uint32_t>(vertexCount);
      entry.c0Gen = payload.smoothP0ColorsGen;
      entry.c1Gen = payload.smoothP1ColorsGen;
      entry.pickGen = payload.smoothPickingColorsGen;
    }

    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                 {&boundGeometryStaticEntry->vbP0,
                                                  &boundGeometryStaticEntry->vbP1,
                                                  &boundGeometryStaticEntry->vbFlags,
                                                  &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundAppearanceStaticEntry->vbC0, &boundAppearanceStaticEntry->vbC1});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
  }
}

void ZVulkanLinePipelineContext::uploadThinGeometry(const LinePayload& payload, bool pickingPass)
{
  m_usedThinStaticVBThisFrame = false;
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
  const size_t posBytes = static_cast<size_t>(vertexCount) * sizeof(glm::vec3);
  const size_t colBytes = static_cast<size_t>(vertexCount) * sizeof(glm::vec4);
  const ThinGeometryCacheKey geometryKey{payload.streamKey, payload.streamSegmentOrdinal, payload.isLineStrip};
  const ThinAppearanceCacheKey appearanceKey{payload.streamKey,
                                             payload.streamSegmentOrdinal,
                                             pickingPass,
                                             payload.isLineStrip};
  const bool hasStreamKey = payload.streamKey != 0;

  ThinGeometryCacheEntry* boundGeometryStaticEntry = nullptr;
  ThinAppearanceCacheEntry* boundAppearanceStaticEntry = nullptr;
  bool geometryBound = false;
  bool appearanceBound = false;

  auto bindGeometryStatic = [&](ThinGeometryCacheEntry& entry) {
    m_thinUploadVertexCount = vertexCount;
    m_thinPosBuffer = entry.vbPos.buffer;
    m_thinPosOffset = entry.vbPos.offset;
    m_thinUploadIndexCount = entry.indexCount;
    if (payload.isLineStrip) {
      m_thinUploadIndexBuffer = entry.ib.buffer;
      m_thinUploadIndexOffset = entry.ib.offset;
    } else {
      m_thinUploadIndexBuffer = VK_NULL_HANDLE;
      m_thinUploadIndexOffset = 0;
    }
    boundGeometryStaticEntry = &entry;
    geometryBound = true;
  };
  auto bindAppearanceStatic = [&](ThinAppearanceCacheEntry& entry) {
    m_thinColorBuffer = entry.vbColor.buffer;
    m_thinColorOffset = entry.vbColor.offset;
    boundAppearanceStaticEntry = &entry;
    appearanceBound = true;
  };

  if (hasStreamKey) {
    if (m_thinGeometryStreamCache.hasPendingCopy(geometryKey)) {
      if (const ThinPendingGeometryUploadBinding* pending = m_thinGeometryStreamCache.findPendingUpload(geometryKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == vertexCount && pending->indexCount == expectedIndexCount;
        const bool gensSame = pending->positionsGen == payload.positionsGen && pending->indexGen == payload.indicesGen;
        const bool buffersOk = pending->pos.buffer && (!payload.isLineStrip || pending->index.buffer);
        if (sizeSame && gensSame && buffersOk) {
          m_thinUploadVertexCount = vertexCount;
          m_thinPosBuffer = pending->pos.buffer;
          m_thinPosOffset = pending->pos.offset;
          m_thinUploadIndexCount = pending->indexCount;
          if (payload.isLineStrip) {
            m_thinUploadIndexBuffer = pending->index.buffer;
            m_thinUploadIndexOffset = pending->index.offset;
          } else {
            m_thinUploadIndexBuffer = VK_NULL_HANDLE;
            m_thinUploadIndexOffset = 0;
          }
          geometryBound = true;
        }
      }
    }
    if (!geometryBound && !m_thinGeometryStreamCache.hasPendingCopy(geometryKey)) {
      if (ThinGeometryCacheEntry* entry = m_thinGeometryStreamCache.findEntry(geometryKey); entry != nullptr) {
        const bool sizeSame = (entry->vertexCount == vertexCount) && (entry->indexCount == expectedIndexCount);
        const bool gensSame = (entry->positionsGen == payload.positionsGen) && (entry->indexGen == payload.indicesGen);
        if (entry->promoted && sizeSame && gensSame && entry->vbPos && (!payload.isLineStrip || entry->ib)) {
          bindGeometryStatic(*entry);
        }
      }
    }

    if (m_thinAppearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (const ThinPendingAppearanceUploadBinding* pending =
            m_thinAppearanceStreamCache.findPendingUpload(appearanceKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == vertexCount;
        const bool gensSame = pending->colorsGen == colorGen;
        const bool buffersOk = pending->color.buffer;
        if (sizeSame && gensSame && buffersOk) {
          m_thinColorBuffer = pending->color.buffer;
          m_thinColorOffset = pending->color.offset;
          appearanceBound = true;
        }
      }
    }
    if (!appearanceBound && !m_thinAppearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (ThinAppearanceCacheEntry* entry = m_thinAppearanceStreamCache.findEntry(appearanceKey); entry != nullptr) {
        const bool sizeSame = entry->vertexCount == vertexCount;
        const bool gensSame = entry->colorsGen == colorGen;
        if (entry->promoted && sizeSame && gensSame && entry->vbColor) {
          bindAppearanceStatic(*entry);
        }
      }
    }
  }

  if (geometryBound && appearanceBound) {
    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                 {&boundGeometryStaticEntry->vbPos, &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend, {&boundAppearanceStaticEntry->vbColor});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    m_usedThinStaticVBThisFrame = (boundGeometryStaticEntry != nullptr) && (boundAppearanceStaticEntry != nullptr);
    return;
  }

  // A pass may reuse only geometry or only appearance statics while uploading the
  // other half. Protect the selected static slices before upload reservation or
  // promotion can invoke the global residency broker.
  if (boundGeometryStaticEntry != nullptr) {
    vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                               {&boundGeometryStaticEntry->vbPos, &boundGeometryStaticEntry->ib});
    boundGeometryStaticEntry->usedStaticOnce = true;
    touchStaticStream(geometryKey.streamKey);
  }
  if (boundAppearanceStaticEntry != nullptr) {
    vulkan::pinStaticSlicesForActiveSubmission(m_backend, {&boundAppearanceStaticEntry->vbColor});
    boundAppearanceStaticEntry->usedStaticOnce = true;
    touchStaticStream(appearanceKey.streamKey);
  }
  m_backend.reserveUploadSlices({
    {!geometryBound ? posBytes : 0u,                                                                          alignof(glm::vec3)},
    {!appearanceBound ? colBytes : 0u,                                                                        alignof(glm::vec4)},
    {(!geometryBound && payload.isLineStrip) ? static_cast<size_t>(positions.size()) * sizeof(uint32_t) : 0u,
     alignof(uint32_t)                                                                                                          }
  });
  if (vertexCount == 0) {
    m_thinUploadVertexCount = 0;
    m_thinPosBuffer = VK_NULL_HANDLE;
    m_thinColorBuffer = VK_NULL_HANDLE;
    m_thinPosOffset = 0;
    m_thinColorOffset = 0;
    return;
  }

  Z3DRendererVulkanBackend::UploadSlice posSlice{};
  Z3DRendererVulkanBackend::UploadSlice colSlice{};
  Z3DRendererVulkanBackend::UploadSlice idxSlice{};
  if (!geometryBound) {
    posSlice = m_backend.suballocateUpload(posBytes, alignof(glm::vec3));
  }
  if (!appearanceBound) {
    colSlice = m_backend.suballocateUpload(colBytes, alignof(glm::vec4));
  }
  if (!geometryBound && payload.isLineStrip) {
    idxSlice = m_backend.suballocateUpload(static_cast<size_t>(positions.size()) * sizeof(uint32_t), alignof(uint32_t));
  }
  if ((!geometryBound &&
       (!posSlice.buffer || !posSlice.mapped || (payload.isLineStrip && (!idxSlice.buffer || !idxSlice.mapped)))) ||
      (!appearanceBound && (!colSlice.buffer || !colSlice.mapped))) {
    m_thinUploadVertexCount = 0;
    m_thinPosBuffer = VK_NULL_HANDLE;
    m_thinColorBuffer = VK_NULL_HANDLE;
    m_thinPosOffset = 0;
    m_thinColorOffset = 0;
    return;
  }

  if (!geometryBound || !appearanceBound) {
    uint32_t out = 0;
    auto* posOut = !geometryBound ? static_cast<glm::vec3*>(posSlice.mapped) : nullptr;
    auto* colOut = !appearanceBound ? static_cast<glm::vec4*>(colSlice.mapped) : nullptr;
    if (payload.isLineStrip) {
      for (size_t i = 0; i < positions.size(); ++i) {
        if (!geometryBound) {
          posOut[out] = positions[i];
        }
        if (!appearanceBound) {
          colOut[out] = colorAt(i);
        }
        out++;
      }
      if (!geometryBound) {
        auto* idst = static_cast<uint32_t*>(idxSlice.mapped);
        for (uint32_t i = 0; i < expectedIndexCount; ++i) {
          idst[i] = i;
        }
      }
    } else {
      for (size_t i = 0; i + 1 < positions.size(); i += 2) {
        if (!geometryBound) {
          posOut[out] = positions[i];
        }
        if (!appearanceBound) {
          colOut[out] = colorAt(i);
        }
        out++;
        if (!geometryBound) {
          posOut[out] = positions[i + 1];
        }
        if (!appearanceBound) {
          colOut[out] = colorAt(i + 1);
        }
        out++;
      }
    }
  }

  if (!geometryBound) {
    m_thinUploadVertexCount = vertexCount;
    m_thinPosBuffer = posSlice.buffer;
    m_thinPosOffset = posSlice.offset;
    m_thinUploadIndexCount = expectedIndexCount;
    if (payload.isLineStrip) {
      m_thinUploadIndexBuffer = idxSlice.buffer;
      m_thinUploadIndexOffset = idxSlice.offset;
    } else {
      m_thinUploadIndexBuffer = VK_NULL_HANDLE;
      m_thinUploadIndexOffset = 0;
      m_thinUploadIndexCount = 0;
    }
  }
  if (!appearanceBound) {
    m_thinColorBuffer = colSlice.buffer;
    m_thinColorOffset = colSlice.offset;
  }

  const size_t uploadBytesStaged =
    (!geometryBound ? posBytes : 0u) + (!appearanceBound ? colBytes : 0u) +
    ((!geometryBound && payload.isLineStrip) ? static_cast<size_t>(expectedIndexCount) * sizeof(uint32_t) : 0u);
  m_backend.addLineBytesStaged(uploadBytesStaged);

  if (hasStreamKey) {
    auto rememberPendingGeometryUpload = [&]() {
      if (geometryBound) {
        return;
      }
      ThinPendingGeometryUploadBinding binding{};
      binding.pos = posSlice;
      binding.index = idxSlice;
      binding.vertexCount = vertexCount;
      binding.indexCount = expectedIndexCount;
      binding.positionsGen = payload.positionsGen;
      binding.indexGen = payload.indicesGen;
      m_thinGeometryStreamCache.rememberPendingUploadBinding(geometryKey, binding);
    };
    auto rememberPendingAppearanceUpload = [&]() {
      if (appearanceBound) {
        return;
      }
      ThinPendingAppearanceUploadBinding binding{};
      binding.color = colSlice;
      binding.vertexCount = vertexCount;
      binding.colorsGen = colorGen;
      m_thinAppearanceStreamCache.rememberPendingUploadBinding(appearanceKey, binding);
    };

    const int kPromotionThreshold = 2;
    auto promoteGeometryToStatics = [&](ThinGeometryCacheEntry& entry) -> bool {
      if (geometryBound) {
        return false;
      }
      const size_t idxBytes = payload.isLineStrip ? static_cast<size_t>(expectedIndexCount) * sizeof(uint32_t) : 0u;
      const Z3DRendererVulkanBackend::UploadSlice* idxSrc = payload.isLineStrip ? &idxSlice : nullptr;
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(m_backend,
                                                   {
                                                     {&entry.vbPos, &posSlice, posBytes, alignof(glm::vec3), false},
                                                     {&entry.ib,    idxSrc,    idxBytes, alignof(uint32_t),  true }
      },
                                                   &stagedBytes)) {
        return false;
      }
      m_backend.addLineBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK line thin geometry promote: pos={}B idx={}B", posBytes, idxBytes);
      return true;
    };
    auto promoteAppearanceToStatics = [&](ThinAppearanceCacheEntry& entry) -> bool {
      if (appearanceBound) {
        return false;
      }
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(m_backend,
                                                   {
                                                     {&entry.vbColor, &colSlice, colBytes, alignof(glm::vec4), false},
      },
                                                   &stagedBytes)) {
        return false;
      }
      m_backend.addLineBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK line thin appearance promote: col={}B", colBytes);
      return true;
    };

    {
      auto [entry, inserted] = m_thinGeometryStreamCache.tryEmplaceEntry(geometryKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevIndexCount = entry.indexCount;
      const uint32_t prevPositionsGen = entry.positionsGen;
      const uint32_t prevIndexGen = entry.indexGen;
      const bool sizeSame = (prevVertexCount == vertexCount) && (prevIndexCount == expectedIndexCount);
      const bool gensSame = (prevPositionsGen == payload.positionsGen) && (prevIndexGen == payload.indicesGen);
      entry.unchangedFrames = (!inserted && sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      if (!geometryBound) {
        if (m_thinGeometryStreamCache.hasPendingCopy(geometryKey)) {
          rememberPendingGeometryUpload();
        } else if (inserted) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_thinGeometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbPos, &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_thinGeometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool anyChanged =
            (prevPositionsGen != payload.positionsGen) || (payload.isLineStrip && prevIndexGen != payload.indicesGen);
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbPos, &entry.ib});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (prevPositionsGen != payload.positionsGen) {
              m_backend.scheduleStaticCopy(entry.vbPos.buffer, entry.vbPos.offset, posSlice, false);
              restagedBytes += posBytes;
            }
            if (payload.isLineStrip && prevIndexGen != payload.indicesGen) {
              m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, idxSlice, true);
              restagedBytes += static_cast<size_t>(expectedIndexCount) * sizeof(uint32_t);
            }
            if (restagedBytes > 0) {
              m_backend.addLineBytesStaged(restagedBytes);
            }
            m_thinGeometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_thinGeometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        }
      }

      entry.vertexCount = vertexCount;
      entry.indexCount = expectedIndexCount;
      entry.positionsGen = payload.positionsGen;
      entry.indexGen = payload.indicesGen;
    }

    {
      auto [entry, inserted] = m_thinAppearanceStreamCache.tryEmplaceEntry(appearanceKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevColorsGen = entry.colorsGen;
      const bool sizeSame = prevVertexCount == vertexCount;
      const bool gensSame = prevColorsGen == colorGen;
      entry.unchangedFrames = (!inserted && sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      if (!appearanceBound) {
        if (m_thinAppearanceStreamCache.hasPendingCopy(appearanceKey)) {
          rememberPendingAppearanceUpload();
        } else if (inserted) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_thinAppearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbColor});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_thinAppearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool anyChanged = prevColorsGen != colorGen;
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbColor});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            m_backend.scheduleStaticCopy(entry.vbColor.buffer, entry.vbColor.offset, colSlice, false);
            m_backend.addLineBytesStaged(colBytes);
            m_thinAppearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_thinAppearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        }
      }

      entry.vertexCount = vertexCount;
      entry.colorsGen = colorGen;
    }

    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                 {&boundGeometryStaticEntry->vbPos, &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend, {&boundAppearanceStaticEntry->vbColor});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    m_usedThinStaticVBThisFrame = (boundGeometryStaticEntry != nullptr) && (boundAppearanceStaticEntry != nullptr);
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
      auto& ddp = m_ddpArgsByStream[DDPStreamKey{payload.streamKey, payload.streamSegmentOrdinal}];
      auto prepareWideDdpArgs = [&](DDPArgs& args, const char* context) {
        const auto widths = payload.perSegmentWidths;
        const uint32_t segmentCount = m_wideUploadIndexCount / 6u;
        const bool perSegment = !widths.empty();
        const uint32_t drawSegments =
          perSegment ? std::min<uint32_t>(segmentCount, static_cast<uint32_t>(widths.size())) : 0u;

        if (args.widePrepared) {
          CHECK(args.widePerSegment == perSegment) << context << ": wide args mode mismatch for streamKey";
          if (perSegment) {
            CHECK(args.wideOffsets.size() == static_cast<size_t>(drawSegments))
              << context << ": wide args segment count mismatch for streamKey";
          } else {
            CHECK(args.wideOffsets.size() == 1u) << context << ": wide args table mismatch for streamKey";
            CHECK(args.wideIndexCount == m_wideUploadIndexCount)
              << context << ": wide args index count mismatch for streamKey";
          }
          return;
        }

        CHECK(m_backend.ddpDeviceArgsBuffer() != vk::Buffer{}) << context << ": device args buffer missing";
        args.widePerSegment = perSegment;
        if (perSegment) {
          args.wideOffsets.assign(drawSegments, vk::DeviceSize(0));
          args.wideSegments = drawSegments;
          args.wideIndexCount = 6u;
          for (uint32_t i = 0; i < drawSegments; ++i) {
            struct Cmd
            {
              uint32_t indexCount, instanceCount, firstIndex;
              int32_t vertexOffset;
              uint32_t firstInstance;
            } drawCmd{6u, 1u, i * 6u, static_cast<int32_t>(i * 4u), 0u};
            const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
            auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
            CHECK(slice.buffer && slice.mapped) << context << ": failed to allocate wide per-segment args upload slice";
            std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
            m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
            args.wideOffsets[i] = off;
          }
        } else {
          args.wideOffsets.assign(1u, vk::DeviceSize(0));
          args.wideSegments = segmentCount;
          args.wideIndexCount = m_wideUploadIndexCount;
          struct Cmd
          {
            uint32_t indexCount, instanceCount, firstIndex;
            int32_t vertexOffset;
            uint32_t firstInstance;
          } drawCmd{m_wideUploadIndexCount, 1u, 0u, 0, 0u};
          const vk::DeviceSize off = m_backend.ddpAllocDeviceArgsSlot(sizeof(Cmd));
          auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
          CHECK(slice.buffer && slice.mapped) << context << ": failed to allocate wide args upload slice";
          std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
          m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
          args.wideOffsets[0] = off;
        }
        args.widePrepared = true;
      };
      prepareWideDdpArgs(ddp, "Line DDP init");
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
      const bool supersample2x2 =
        (renderer.sceneState().geometryAAMode == GeometryAAMode::Supersample2x2) && payload.followSupersampling;
      const float sizeScale = pc.size_scale;
      const bool ddpPeelIndirect =
        (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
      const DDPArgs* ddpArgs = nullptr;
      if (ddpPeelIndirect) {
        CHECK(payload.streamKey != 0) << "Line DDP peel: missing streamKey";
        auto& ddp = m_ddpArgsByStream[DDPStreamKey{payload.streamKey, payload.streamSegmentOrdinal}];
        const bool perSegment = !widths.empty();
        const uint32_t segmentCount = m_wideUploadIndexCount / 6u;
        const uint32_t drawSegments =
          perSegment ? std::min<uint32_t>(segmentCount, static_cast<uint32_t>(widths.size())) : 0u;
        if (!ddp.widePrepared) {
          CHECK(m_backend.ddpDeviceArgsBuffer() != vk::Buffer{}) << "Line DDP peel: device args buffer missing";
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
              CHECK(slice.buffer && slice.mapped) << "Line DDP peel: failed to allocate wide args upload slice";
              std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
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
            CHECK(slice.buffer && slice.mapped) << "Line DDP peel: failed to allocate wide args upload slice";
            std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
            m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
            ddp.wideOffsets[0] = off;
          }
          ddp.widePrepared = true;
        } else {
          CHECK(ddp.widePerSegment == perSegment) << "Line DDP peel: wide args mode mismatch for streamKey";
          ddpArgs = &ddp;
        }
      }
      if (!widths.empty()) {
        VLOG(1) << fmt::format(
          "VK wide line: segments={} dpr={:.3f} supersample2x2={} sizeScale={:.3f} resolvedLineWidth={:.3f}",
          m_wideUploadIndexCount / 6u,
          dpr,
          supersample2x2,
          sizeScale,
          payload.resolvedLineWidth);
        const uint32_t segmentCount = m_wideUploadIndexCount / 6u;
        const uint32_t drawSegments = std::min<uint32_t>(segmentCount, static_cast<uint32_t>(widths.size()));
        if (ddpArgs != nullptr) {
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
          if (ddpArgs != nullptr) {
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
        if (ddpArgs != nullptr) {
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

  auto prepareThinDdpArgs = [&](DDPArgs& ddp, bool indexed, const char* context) {
    if (ddp.thinPrepared) {
      CHECK(ddp.thinIndexed == indexed) << context << ": thin args indexed mismatch for streamKey";
      if (indexed) {
        CHECK(ddp.thinIndexCount == drawSpec.indexCount) << context << ": thin args index count mismatch for streamKey";
      } else {
        CHECK(ddp.thinVertexCount == drawSpec.vertexCount)
          << context << ": thin args vertex count mismatch for streamKey";
      }
      return;
    }

    CHECK(m_backend.ddpDeviceArgsBuffer() != vk::Buffer{}) << context << ": device args buffer missing";
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
      CHECK(slice.buffer && slice.mapped) << context << ": failed to allocate thin indexed args upload slice";
      std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
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
      CHECK(slice.buffer && slice.mapped) << context << ": failed to allocate thin args upload slice";
      std::memcpy(slice.mapped, &drawCmd, sizeof(Cmd));
      m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), off, slice);
      ddp.thinOffset = off;
      ddp.thinIndexed = false;
      ddp.thinVertexCount = drawSpec.vertexCount;
    }
    ddp.thinPrepared = true;
  };

  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    // Prepare device-local indirect args during init; copies are flushed after
    // the init pass ends. Peel passes reference the prepared offset.
    CHECK(payload.streamKey != 0) << "Line DDP init: missing streamKey";
    auto& ddp = m_ddpArgsByStream[DDPStreamKey{payload.streamKey, payload.streamSegmentOrdinal}];
    const bool indexed = (drawSpec.indexCount > 0);
    prepareThinDdpArgs(ddp, indexed, "Line DDP init");
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
  if (absl::GetFlag(FLAGS_atlas_vk_cache_draw_secondaries) &&
      m_backend.device().context().supportsInlineAndSecondaryDynamicRendering() && payload.streamKey != 0 &&
      m_usedThinStaticVBThisFrame && allStaticSegments && !ddpHook) {
    m_backend.notifyDrawSecondaryCacheAttempt();

    ThinSecondaryCacheKey cacheKey{};
    cacheKey.frameKey = frameKey;
    cacheKey.streamKey = payload.streamKey;
    cacheKey.streamSegmentOrdinal = payload.streamSegmentOrdinal;
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
    CHECK(payload.streamKey != 0) << "Line DDP peel: missing streamKey";
    auto& ddp = m_ddpArgsByStream[DDPStreamKey{payload.streamKey, payload.streamSegmentOrdinal}];
    if (!ddp.thinPrepared) {
      prepareThinDdpArgs(ddp, indexed, "Line DDP peel");
      recorder.recordGraphicsDraw(drawSpec);
      return;
    }
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
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
