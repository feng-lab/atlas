#include "zvulkanspherepipelinecontext.h"
#include "zcommandlineflags.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dsphererenderer.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbindings.h"
#include "zvulkanclipplanes.h"
#include "zvulkanuniforms.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanstaticpromotionutils.h"
#include "zlog.h"
#include "zexception.h"
#include "zrenderthreadexecutor_tls.h"

#include <QString>
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>
#include <cstring>
#include <cstdint>
#include <utility>

#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>

ABSL_DECLARE_FLAG(bool, atlas_vk_cache_draw_secondaries);

namespace nim {

namespace {

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

bool clipPlanesEqual(const ClipPlanesState& a, const ClipPlanesState& b)
{
  if (a.enabled != b.enabled || a.planeCount != b.planeCount) {
    return false;
  }
  auto vec4Equal = [](const glm::vec4& x, const glm::vec4& y) {
    return x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w;
  };
  const size_t count = std::min(static_cast<size_t>(a.planeCount), kRenderBatchMaxClipPlanes);
  for (size_t i = 0; i < count; ++i) {
    if (!vec4Equal(a.planes[i], b.planes[i])) {
      return false;
    }
  }
  return true;
}

size_t staticSliceBytes(const Z3DRendererVulkanBackend::StaticSlice& slice)
{
  return slice ? slice.size : 0u;
}

} // namespace

ZVulkanSpherePipelineContext::ZVulkanSpherePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
  , m_streamUsageTracker(Z3DRendererVulkanBackend::StaticCacheOwner::Sphere)
{}

ZVulkanSpherePipelineContext::~ZVulkanSpherePipelineContext() = default;

void ZVulkanSpherePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_usedStaticVBThisFrame = false;
  m_centerRadiusBuffer = nullptr;
  m_colorBuffer = nullptr;
  m_specularBuffer = nullptr;
  m_flagsBuffer = nullptr;
  m_centerRadiusOffset = 0;
  m_colorOffset = 0;
  m_specularOffset = 0;
  m_flagsOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  // Per-stream UBO offsets are cached per frame-slot in the backend's persistent
  // uniform arena; keep them across submissions to stabilize dynamic offsets.
  m_ddpArgsByStream.clear();
  m_geometryStreamCache.resetFrame();
  m_appearanceStreamCache.resetFrame();
}

void ZVulkanSpherePipelineContext::evictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }

  m_streamUsageTracker.eraseStream(streamKey);

  m_geometryStreamCache.evictStream(streamKey, [this](GeometryCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbCenterRadius);
    m_backend.releaseStaticSlice(entry.vbFlags);
    m_backend.releaseStaticSlice(entry.ib);
  });
  m_appearanceStreamCache.evictStream(streamKey, [this](AppearanceCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbColor);
    m_backend.releaseStaticSlice(entry.vbSpecular);
  });

  for (auto it = m_secondaryCache.begin(); it != m_secondaryCache.end();) {
    if (it->first.streamKey == streamKey) {
      it = m_secondaryCache.erase(it);
    } else {
      ++it;
    }
  }

  for (auto& [frameKey, cache] : m_uboCacheByFrameKey) {
    (void)frameKey;
    cache.byStream.erase(streamKey);
  }

  for (auto it = m_ddpArgsByStream.begin(); it != m_ddpArgsByStream.end();) {
    if (it->first.streamKey == streamKey) {
      it = m_ddpArgsByStream.erase(it);
    } else {
      ++it;
    }
  }
}

void ZVulkanSpherePipelineContext::touchStaticStream(uint64_t streamKey)
{
  m_streamUsageTracker.touch(streamKey, m_backend.currentStaticCacheEpoch());
}

size_t ZVulkanSpherePipelineContext::staticBytesForStream(uint64_t streamKey,
                                                          Z3DRendererVulkanBackend::StaticPressureDomain domain) const
{
  return m_geometryStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const GeometryCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbCenterRadius) + staticSliceBytes(entry.vbFlags);
             }
             return staticSliceBytes(entry.ib);
           }) +
         m_appearanceStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const AppearanceCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbColor) + staticSliceBytes(entry.vbSpecular);
             }
             return size_t{0};
           });
}

std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
ZVulkanSpherePipelineContext::oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain,
                                                          uint64_t protectedEpoch) const
{
  return m_streamUsageTracker.oldestEvictableStaticStream(
    domain,
    protectedEpoch,
    [this](uint64_t streamKey, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
      return staticBytesForStream(streamKey, cacheDomain);
    });
}

size_t ZVulkanSpherePipelineContext::evictStaticStreamForPressure(uint64_t streamKey)
{
  const size_t vbBytes = staticBytesForStream(streamKey, Z3DRendererVulkanBackend::StaticPressureDomain::Vertex);
  const size_t ibBytes = staticBytesForStream(streamKey, Z3DRendererVulkanBackend::StaticPressureDomain::Index);
  evictStream(streamKey);
  return vbBytes + ibBytes;
}

void ZVulkanSpherePipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  const auto fence = m_backend.awaitActiveSubmissionFence("VK sphere retained UBO lifetime");
  auto keepAlive = currentRenderThreadExecutorKeepAlive("VK sphere retained UBO lifetime");
  for (auto& sp : m_retainedUbos) {
    m_backend.spawnDetachedTask(keepAlive,
                                folly::coro::co_invoke([fence, keep = sp]() mutable -> folly::coro::Task<void> {
                                  co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(fence);
                                  co_return;
                                }),
                                "VK sphere retained UBO lifetime");
  }
  m_retainedUbos.clear();
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

  // Per-draw diagnostic; keep behind VLOG(2) so --v=1 remains usable for perf runs.
  VLOG(2) << fmt::format(
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

  CHECK(batch.shaderHook.captured) << "Sphere batch missing shader hook snapshot";
  const bool pickingPass = payload.pickingPass;
  const auto shaderHook = batch.shaderHook.type;

  // Match OpenGL: lighting is controlled per-renderer (Z3DPrimitiveRenderer::m_needLighting),
  // not purely by whether the scene has lights. Use the "no lighting" UBO slice whenever the
  // payload opts out of lighting, so unlit overlays (e.g. transform gizmos) render with flat
  // premultiplied colors like the GL backend.
  m_dynLightingOffset = (payload.pickingPass || !payload.wantsLighting) ? m_backend.framePickingLightingOffset()
                                                                        : m_backend.frameSharedLightingOffset();
  updateTransformUBO(renderer, batch, payload, pickingPass);
  // Descriptor sets are primed by the backend in beginRender(); avoid record-time updates.
  const vk::DescriptorSet dsLighting = m_backend.sharedLightingDescriptorSet();
  const vk::DescriptorSet dsTransforms = m_backend.sharedTransformsDescriptorSetPersistent();
  CHECK(dsLighting && dsTransforms) << "Sphere pipeline shared descriptor sets missing (lighting/transforms)";
  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Sphere pipeline record called without an active Vulkan frame-slot key";

  const auto& hookPara = batch.shaderHook.para;
  DDPPeelPushConstants ddpPc{};
  const bool usesDdpPeelPc = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (usesDdpPeelPc) {
    CHECK(hookPara.dualDepthPeelingDepthBlenderHandle.valid())
      << "Sphere DDP peel requires a valid depth blender handle";
    CHECK(hookPara.dualDepthPeelingFrontBlenderHandle.valid())
      << "Sphere DDP peel requires a valid front blender handle";
    auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                               m_backend.device(),
                                               "sphere dual-depth-peeling depth blender");
    auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                               m_backend.device(),
                                               "sphere dual-depth-peeling front blender");
    ddpPc.ddpDepthBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(depthTex, "sphere ddp depth blender");
    ddpPc.ddpFrontBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(frontTex, "sphere ddp front blender");
  }

  const auto& formatsOpt = m_backend.currentSegmentFormats();
  CHECK(formatsOpt.has_value()) << "Sphere pipeline context requires an active Vulkan rendering segment";
  const auto& formats = *formatsOpt;

  m_backend.validateFormatsOrCrash(formats, "sphere");

  PipelineKey key;
  key.dynamicMaterial = payload.useDynamicMaterial && !pickingPass;
  key.shaderHookType = shaderHook;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  auto& pipeline = ensurePipeline(key);

  // Draw-only recording under backend-managed segment: no attachment handling here

  const std::array<vk::DescriptorSet, 3> allBoundSets{m_backend.bindlessSampledImageDescriptorSet(),
                                                      dsLighting,
                                                      dsTransforms};
  const std::span<const vk::DescriptorSet> boundSets =
    usesDdpPeelPc ? std::span<const vk::DescriptorSet>(allBoundSets)
                  : std::span<const vk::DescriptorSet>(allBoundSets).subspan(vkbind::kSetLighting);

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = usesDdpPeelPc ? vkbind::kSetBindlessSampledImages : vkbind::kSetLighting;
  drawSpec.descriptorSets = boundSets;

  uint32_t expectedSets = drawSpec.descriptorSetFirst + static_cast<uint32_t>(boundSets.size());
  std::array<vk::DescriptorSet, 1> oitSets{m_backend.sharedOITDescriptorSet()};
  std::array<ZVulkanDescriptorBindInfo, 1> extraDescriptorBinds{};
  extraDescriptorBinds[0].firstSet = vkbind::kSetOIT;
  extraDescriptorBinds[0].sets = oitSets;
  extraDescriptorBinds[0].dynamicOffsets = {};
  drawSpec.extraDescriptorBinds = extraDescriptorBinds;
  expectedSets = std::max(expectedSets, vkbind::kSetOIT + 1u);
  drawSpec.expectedDescriptorSetCount = expectedSets;

  const std::array<vk::Buffer, 4> vertexBuffers{m_centerRadiusBuffer, m_colorBuffer, m_flagsBuffer, m_specularBuffer};
  const std::array<vk::DeviceSize, 4> vertexOffsets{m_centerRadiusOffset,
                                                    m_colorOffset,
                                                    m_flagsOffset,
                                                    m_specularOffset};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  if (m_indexCount > 0 && m_indexUploadBuffer) {
    drawSpec.indexBuffer = m_indexUploadBuffer;
    drawSpec.indexOffset = m_indexUploadOffset;
    drawSpec.indexType = vk::IndexType::eUint32;
    drawSpec.indexCount = static_cast<uint32_t>(m_indexCount);
  } else {
    drawSpec.vertexCount = static_cast<uint32_t>(m_vertexCount);
  }
  drawSpec.instanceCount = 1;

  const std::array<uint64_t, 4> vertexBufferSegmentIds{
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[0]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[1]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[2]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[3]),
  };
  uint64_t indexBufferSegmentId = 0;
  if (drawSpec.indexCount > 0 && drawSpec.indexBuffer) {
    indexBufferSegmentId = m_backend.staticArenaSegmentIdForBuffer(drawSpec.indexBuffer);
  }
  const bool allStaticSegments = (vertexBufferSegmentIds[0] != 0) && (vertexBufferSegmentIds[1] != 0) &&
                                 (vertexBufferSegmentIds[2] != 0) && (vertexBufferSegmentIds[3] != 0) &&
                                 ((drawSpec.indexCount == 0) || (indexBufferSegmentId != 0));
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

  if (usesDdpPeelPc) {
    drawSpec.pushConstantsData = &ddpPc;
    drawSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(ddpPc));
    drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
    drawSpec.requirePushConstants = true;
  }

  const bool ddpHook = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                        shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (absl::GetFlag(FLAGS_atlas_vk_cache_draw_secondaries) &&
      m_backend.device().context().supportsInlineAndSecondaryDynamicRendering() && payload.streamKey != 0 &&
      m_usedStaticVBThisFrame && allStaticSegments && !ddpHook) {
    m_backend.notifyDrawSecondaryCacheAttempt();
    SecondaryCacheKey cacheKey{};
    cacheKey.frameKey = frameKey;
    cacheKey.streamKey = payload.streamKey;
    cacheKey.streamSegmentOrdinal = payload.streamSegmentOrdinal;
    cacheKey.picking = pickingPass;
    cacheKey.dynamicMaterial = key.dynamicMaterial;
    cacheKey.shaderHookType = shaderHook;
    cacheKey.eye = batch.eye;
    cacheKey.oitRingIndex = m_backend.sharedOITDescriptorSetRingIndex();

    SecondarySignature signature{};
    signature.pipeline = drawSpec.pipelineHandle;
    signature.layout = drawSpec.pipelineLayoutHandle;
    signature.baseDescriptorSets = {dsLighting, dsTransforms};
    signature.baseDescriptorGenerations = {m_backend.sharedLightingDescriptorSetGeneration(),
                                           m_backend.sharedTransformsDescriptorSetPersistentGeneration()};
    signature.hasOit = true;
    signature.oitDescriptorSet = m_backend.sharedOITDescriptorSet();
    signature.oitDescriptorGeneration = m_backend.sharedOITDescriptorSetGeneration();
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

    auto itCache = m_secondaryCache.find(cacheKey);
    if (itCache != m_secondaryCache.end()) {
      m_backend.notifyDrawSecondaryCacheKeyFound();
      SecondaryCacheEntry& entry = itCache->second;
      const vk::CommandBuffer rawSecondary = entry.commandBuffer;
      if (entry.recorded && rawSecondary != vk::CommandBuffer{}) {
        if (entry.signature == signature) {
          m_backend.notifyDrawSecondaryCacheHit();
          m_backend.notifyDrawSecondaryCacheExecute(entry.signature.pipeline);
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

    SecondaryCacheEntry& entry = m_secondaryCache[cacheKey];
    entry.signature = signature;
    m_backend.notifyDrawSecondaryCacheBuild();

    vk::SampleCountFlagBits rasterSamples = vk::SampleCountFlagBits::e1;
    bool samplesCaptured = false;
    for (const auto& attachment : batch.pass.colorAttachments) {
      if (!attachment.handle.valid()) {
        continue;
      }
      const auto& tex =
        vulkan::textureFromHandle(attachment.handle, m_backend.device(), "sphere secondary inheritance color");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Sphere secondary: mismatched MSAA sample counts in attachments";
      }
    }
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      const auto& tex = vulkan::textureFromHandle(batch.pass.depthAttachment->handle,
                                                  m_backend.device(),
                                                  "sphere secondary inheritance depth");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Sphere secondary: mismatched MSAA sample counts in attachments";
      }
    }

    const bool hasColors = !formats.colorFormats.empty();
    const vk::Format* colorFormatsPtr = hasColors ? formats.colorFormats.data() : nullptr;
    const vk::Format dsFormat = formats.depthFormat.value_or(vk::Format::eUndefined);
    vk::CommandBufferInheritanceRenderingInfo renderingInheritance{};
    renderingInheritance.rasterizationSamples = rasterSamples;
    renderingInheritance.colorAttachmentCount = static_cast<uint32_t>(formats.colorFormats.size());
    renderingInheritance.pColorAttachmentFormats = colorFormatsPtr;
    renderingInheritance.depthAttachmentFormat = dsFormat;
    // Atlas uses depth-only dynamic rendering segments (pStencilAttachment == nullptr).
    // Keep stencilAttachmentFormat UNDEFINED to match vkCmdBeginRendering.
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

    m_backend.notifyDrawSecondaryCacheExecute(entry.signature.pipeline);
    cmd.executeCommands({static_cast<vk::CommandBuffer>(entry.commandBuffer)});
    return;
  }

  ZVulkanPipelineCommandRecorder recorder(cmd);
  auto prepareDdpArgs = [&](DDPArgs& ddp, bool indexed, const char* context) {
    if (ddp.prepared) {
      CHECK(ddp.indexed == indexed) << context << ": indexed mode mismatch for streamKey";
      CHECK(ddp.indexCount == drawSpec.indexCount) << context << ": index count mismatch for streamKey";
      CHECK(ddp.vertexCount == drawSpec.vertexCount) << context << ": vertex count mismatch for streamKey";
      return;
    }

    const vk::Buffer argsBuffer = m_backend.ddpDeviceArgsBuffer();
    CHECK(argsBuffer != vk::Buffer{}) << context << ": device args buffer missing";
    ddp.offset =
      m_backend.ddpAllocDeviceArgsSlot(indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand));
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
      CHECK(slice.buffer != vk::Buffer{} && slice.mapped != nullptr)
        << context << ": failed to allocate indexed args upload slice";
      std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
      m_backend.scheduleStaticCopyIndirect(argsBuffer, ddp.offset, slice);
    } else {
      struct Cmd
      {
        uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
      } cmdPayload{drawSpec.vertexCount, drawSpec.instanceCount, drawSpec.firstVertex, drawSpec.firstInstance};
      auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
      CHECK(slice.buffer != vk::Buffer{} && slice.mapped != nullptr)
        << context << ": failed to allocate args upload slice";
      std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
      m_backend.scheduleStaticCopyIndirect(argsBuffer, ddp.offset, slice);
    }
    ddp.prepared = true;
    ddp.indexed = indexed;
    ddp.vertexCount = drawSpec.vertexCount;
    ddp.indexCount = drawSpec.indexCount;
  };

  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    // Prepare device-local indirect args during init; copies are flushed after init pass ends.
    const bool indexed = (drawSpec.indexCount > 0);
    CHECK(payload.streamKey != 0) << "Sphere DDP init: missing streamKey";
    DDPArgs& ddp = m_ddpArgsByStream[DDPStreamKey{payload.streamKey, payload.streamSegmentOrdinal}];
    prepareDdpArgs(ddp, indexed, "Sphere DDP init");
    recorder.recordGraphicsDraw(drawSpec);
  } else if (m_backend.ddpIndirectCountEnabled() &&
             shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    // Use the prebuilt device-local args for indirect count draw
    const bool indexed = (drawSpec.indexCount > 0);
    CHECK(payload.streamKey != 0) << "Sphere DDP peel: missing streamKey";
    DDPArgs& ddp = m_ddpArgsByStream[DDPStreamKey{payload.streamKey, payload.streamSegmentOrdinal}];
    if (!ddp.prepared) {
      // First peel in a new DDP chunk/submission prepares args for later peel
      // passes in the same chunk. The current pass draws directly because the
      // upload->indirect copy is flushed after this raster node ends.
      prepareDdpArgs(ddp, indexed, "Sphere DDP peel");
      recorder.recordGraphicsDraw(drawSpec);
      return;
    }
    CHECK(ddp.indexed == indexed) << "Sphere DDP peel: indexed mode mismatch for streamKey";
    CHECK(ddp.indexCount == drawSpec.indexCount) << "Sphere DDP peel: index count mismatch";
    CHECK(ddp.vertexCount == drawSpec.vertexCount) << "Sphere DDP peel: vertex count mismatch";
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      CHECK(argsBuf != vk::Buffer{}) << "Sphere DDP peel: device args buffer missing";
      CHECK(cntBuf != vk::Buffer{}) << "Sphere DDP peel: indirect count buffer missing";
      if (indexed) {
        c.drawIndexedIndirectCount(argsBuf, ddp.offset, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
      } else {
        c.drawIndirectCount(argsBuf, ddp.offset, cntBuf, 0, 1, sizeof(VkDrawIndirectCommand));
      }
    });
  } else {
    recorder.recordGraphicsDraw(drawSpec);
  }
}

// Lighting UBO is shared per frame; no per-batch update required.

void ZVulkanSpherePipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                      const RenderBatch& batch,
                                                      const SpherePayload& payload,
                                                      bool pickingPass)
{
  CHECK(batch.shaderHook.captured) << "Sphere batch missing shader hook snapshot";
  CHECK(payload.paramsCaptured) << "Sphere payload missing params";
  (void)renderer;

  // Frame transforms are shared per eye and updated once per submission in beginRender().
  m_dynFrameTransformsOffset = m_backend.frameTransformsOffset(batch.eye);

  ObjectTransformsUBOStd140 objectTransforms{};
  const glm::mat4 model = payload.followCoordTransform ? payload.params.coordTransform : glm::mat4(1.0f);
  objectTransforms.pos_transform = model;
  // Sphere shaders do not consume the normal matrix; keep identity to avoid
  // redundant inverse/transpose work on static geometry.
  objectTransforms.pos_transform_normal_matrix = encodeMat3ToStd140(glm::mat3(1.0f));
  const float sizeScale = payload.followSizeScale ? payload.params.sizeScale : 1.0f;
  objectTransforms.parameters = glm::vec4(sizeScale, 0.0f, 0.0f, 0.0f);
  vulkan::applyBatchClipPlanesToTransforms(batch, objectTransforms);

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params.materialAmbient;
  // Match OpenGL path: when dynamic-material is disabled, spheres should still
  // use the renderer's uniform material properties (specular/shininess).
  // When dynamic-material is enabled, the shader reads per-vertex material
  // values, but keeping the uniform values consistent is harmless and avoids
  // surprising toggling behaviour.
  material.material_specular = pickingPass ? glm::vec4(0.0f) : payload.params.materialSpecular;
  material.material_shininess = pickingPass ? 0.0f : payload.params.materialShininess;
  material.alpha = (pickingPass || !payload.followOpacity) ? 1.0f : payload.params.opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Sphere updateTransformUBO called without an active Vulkan frame-slot key";
  if (payload.streamKey != 0) {
    const uint64_t cacheGeneration = m_backend.activeUniformCacheGeneration();
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    auto& entries = frameCache.byStream[payload.streamKey];
    UboCacheEntry* reusableEntry = nullptr;

    for (auto& cached : entries) {
      if (cached.pickingPass != pickingPass) {
        continue;
      }

      if (cached.params == payload.params && cached.followCoordTransform == payload.followCoordTransform &&
          cached.followSizeScale == payload.followSizeScale && cached.followOpacity == payload.followOpacity &&
          clipPlanesEqual(cached.clipPlanes, batch.clipPlanes)) {
        cached.lastUniformCacheGeneration = cacheGeneration;
        m_dynObjectTransformsOffset = cached.objectTransformsOffset;
        m_dynMaterialOffset = cached.materialOffset;
        return;
      }

      if (cached.lastUniformCacheGeneration != cacheGeneration && reusableEntry == nullptr) {
        reusableEntry = &cached;
      }
    }

    if (reusableEntry != nullptr) {
      std::memcpy(
        m_backend.persistentUniformMappedAt(reusableEntry->objectTransformsOffset, sizeof(ObjectTransformsUBOStd140)),
        &objectTransforms,
        sizeof(objectTransforms));
      std::memcpy(m_backend.persistentUniformMappedAt(reusableEntry->materialOffset, sizeof(MaterialUBOStd140)),
                  &material,
                  sizeof(material));

      reusableEntry->params = payload.params;
      reusableEntry->followCoordTransform = payload.followCoordTransform;
      reusableEntry->followSizeScale = payload.followSizeScale;
      reusableEntry->followOpacity = payload.followOpacity;
      reusableEntry->clipPlanes = batch.clipPlanes;
      reusableEntry->lastUniformCacheGeneration = cacheGeneration;
      m_dynObjectTransformsOffset = reusableEntry->objectTransformsOffset;
      m_dynMaterialOffset = reusableEntry->materialOffset;
      return;
    }

    auto objectSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
    std::memcpy(objectSlice.mapped, &objectTransforms, sizeof(objectTransforms));
    m_dynObjectTransformsOffset = objectSlice.offset;

    auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
    std::memcpy(materialSlice.mapped, &material, sizeof(material));
    m_dynMaterialOffset = materialSlice.offset;

    UboCacheEntry entry{};
    entry.pickingPass = pickingPass;
    entry.params = payload.params;
    entry.followCoordTransform = payload.followCoordTransform;
    entry.followSizeScale = payload.followSizeScale;
    entry.followOpacity = payload.followOpacity;
    entry.clipPlanes = batch.clipPlanes;
    entry.objectTransformsOffset = objectSlice.offset;
    entry.materialOffset = materialSlice.offset;
    entry.lastUniformCacheGeneration = cacheGeneration;
    entries.push_back(std::move(entry));
    return;
  }

  // Fallback (should be rare): no stream key, so we cannot assign a stable slice.
  auto objectSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(ObjectTransformsUBOStd140));
  std::memcpy(objectSlice.mapped, &objectTransforms, sizeof(objectTransforms));
  m_dynObjectTransformsOffset = objectSlice.offset;

  auto materialSlice = m_backend.suballocatePersistentUniformFor(payload, sizeof(MaterialUBOStd140));
  std::memcpy(materialSlice.mapped, &material, sizeof(material));
  m_dynMaterialOffset = materialSlice.offset;
}

ZVulkanSpherePipelineContext::PipelineInstance& ZVulkanSpherePipelineContext::ensurePipeline(const PipelineKey& key)
{
  const auto found = m_pipelines.find(key);
  if (found != m_pipelines.end()) {
    return found->second;
  }

  const auto selectFragmentShader = [](Z3DRendererBase::ShaderHookType hook) -> QString {
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return QStringLiteral("dual_peeling_init_sphere.frag.spv");
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return QStringLiteral("dual_peeling_peel_sphere.frag.spv");
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        return QStringLiteral("ppll_count_sphere.frag.spv");
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        return QStringLiteral("ppll_store_sphere.frag.spv");
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return QStringLiteral("wavg_init_sphere.frag.spv");
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return QStringLiteral("wblended_init_sphere.frag.spv");
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return QStringLiteral("sphere.frag.spv");
    }
  };

  auto& device = m_backend.device();
  auto shader =
    std::make_unique<ZVulkanShader>(device,
                                    ZVulkanShader::spirvResourcePath(QStringLiteral("sphere.vert.spv")),
                                    ZVulkanShader::spirvResourcePath(selectFragmentShader(key.shaderHookType)),
                                    std::nullopt);

  // Dynamic material specialization (match GL's DYNAMIC_MATERIAL_PROPERTY).
  const std::array<vk::SpecializationMapEntry, 1> dynamicMaterialEntries{
    vk::SpecializationMapEntry{.constantID = 60, .offset = 0, .size = sizeof(uint32_t)}
  };
  const std::array<uint32_t, 1> dynamicMaterialData{key.dynamicMaterial ? 1u : 0u};
  const std::vector<vk::SpecializationMapEntry> specializationEntries(dynamicMaterialEntries.begin(),
                                                                      dynamicMaterialEntries.end());
  const std::vector<uint8_t> specializationData(reinterpret_cast<const uint8_t*>(dynamicMaterialData.data()),
                                                reinterpret_cast<const uint8_t*>(dynamicMaterialData.data()) +
                                                  sizeof(dynamicMaterialData));
  shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex, specializationEntries, specializationData);
  shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, specializationEntries, specializationData);

  const std::array vertexBindings{
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
  const std::array vertexAttributes{
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
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexBindings.size());
  vertexInput.pVertexBindingDescriptions = vertexBindings.data();
  vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
  vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();

  auto pipeline = device.createPipeline(*shader, vertexInput, vk::PrimitiveTopology::eTriangleList);

  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Sphere pipeline missing bindless descriptor set layout";
  const vk::DescriptorSetLayout lightingLayout = m_backend.lightingDescriptorSetLayout();
  CHECK(lightingLayout) << "Sphere pipeline missing lighting descriptor set layout";
  const vk::DescriptorSetLayout transformsLayout = m_backend.transformDescriptorSetLayout();
  CHECK(transformsLayout) << "Sphere pipeline missing transforms descriptor set layout";
  const vk::DescriptorSetLayout oitLayout = m_backend.oitDescriptorSetLayout();
  CHECK(oitLayout) << "Sphere pipeline missing OIT descriptor set layout";
  pipeline->setDescriptorSetLayouts({bindlessLayout, lightingLayout, transformsLayout, oitLayout});
  pipeline->setAttachmentFormats(key.colorFormats, key.depthFormat);
  pipeline->setCullMode(vk::CullModeFlagBits::eNone);

  // Dual-depth-peeling peel shader consumes bindless blender indices via push constants.
  pipeline->setPushConstantRanges({
    vk::PushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                          .offset = 0,
                          .size = static_cast<uint32_t>(sizeof(DDPPeelPushConstants))}
  });

  const auto baseBlend = vulkan::toVkBlendAttachment(BlendState{});
  std::vector<vk::PipelineColorBlendAttachmentState> blendAttachments(key.colorFormats.size(), baseBlend);

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
      // Exact OIT PPLL: depth test against opaque depth, but do not write depth.
      pipeline->setDepthTestEnable(true);
      pipeline->setDepthWriteEnable(false);
      break;
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
      for (auto& attachment : blendAttachments) {
        attachment.blendEnable = true;
        attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
        attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
      }
      // Match GL WA init: depth-test against the (loaded) opaque depth buffer
      // when a depth attachment is present, but do not write depth.
      pipeline->setDepthTestEnable(key.depthFormat.has_value());
      pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::WeightedBlendedInit: {
      for (size_t i = 0; i < blendAttachments.size(); ++i) {
        auto& attachment = blendAttachments[i];
        attachment.blendEnable = true;
        if (i == 0) {
          attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
          attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
          attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
        } else {
          attachment.srcColorBlendFactor = vk::BlendFactor::eZero;
          attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
          attachment.srcAlphaBlendFactor = vk::BlendFactor::eZero;
          attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
        }
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
      }
      pipeline->setDepthTestEnable(true);
      pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
    case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel: {
      for (size_t i = 0; i < blendAttachments.size(); ++i) {
        auto& attachment = blendAttachments[i];
        if (i < 6) {
          attachment.blendEnable = true;
          attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
          attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
          attachment.colorBlendOp = vk::BlendOp::eMax;
          attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          attachment.alphaBlendOp = vk::BlendOp::eMax;
        }
      }
      // Match GL DDP: depth-tested against the (loaded) opaque depth buffer, but do not write depth.
      pipeline->setDepthTestEnable(true);
      pipeline->setDepthWriteEnable(false);
      break;
    }
    default:
      break;
  }

  pipeline->setColorBlendAttachments(std::move(blendAttachments));
  pipeline->create();

  PipelineInstance instance;
  instance.shader = std::move(shader);
  instance.pipeline = std::move(pipeline);
  auto [inserted, didInsert] = m_pipelines.emplace(key, std::move(instance));
  CHECK(didInsert) << "Vulkan sphere pipeline insertion failed after a cache miss";
  return inserted->second;
}

void ZVulkanSpherePipelineContext::uploadGeometry(const SpherePayload& payload)
{
  m_usedStaticVBThisFrame = false;
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
  const size_t idxBytes = m_indexCount * sizeof(uint32_t);
  const bool hasStreamKey = payload.streamKey != 0;
  const GeometryCacheKey geometryKey{payload.streamKey, payload.streamSegmentOrdinal};
  const AppearanceCacheKey appearanceKey{payload.streamKey,
                                         payload.streamSegmentOrdinal,
                                         payload.pickingPass,
                                         payload.useDynamicMaterial};
  const bool dynamicSpecular = !payload.pickingPass && payload.useDynamicMaterial;

  GeometryCacheEntry* boundGeometryStaticEntry = nullptr;
  AppearanceCacheEntry* boundAppearanceStaticEntry = nullptr;
  bool geometryBound = false;
  bool appearanceBound = false;

  auto bindGeometryStatic = [&](GeometryCacheEntry& entry) {
    m_centerRadiusBuffer = entry.vbCenterRadius.buffer;
    m_flagsBuffer = entry.vbFlags.buffer;
    m_centerRadiusOffset = entry.vbCenterRadius.offset;
    m_flagsOffset = entry.vbFlags.offset;
    if (m_indexCount > 0) {
      m_indexUploadBuffer = entry.ib.buffer;
      m_indexUploadOffset = entry.ib.offset;
    } else {
      m_indexUploadBuffer = vk::Buffer{};
      m_indexUploadOffset = 0;
    }
    boundGeometryStaticEntry = &entry;
    geometryBound = true;
  };
  auto bindAppearanceStatic = [&](AppearanceCacheEntry& entry) {
    m_colorBuffer = entry.vbColor.buffer;
    m_specularBuffer = entry.vbSpecular.buffer;
    m_colorOffset = entry.vbColor.offset;
    m_specularOffset = entry.vbSpecular.offset;
    boundAppearanceStaticEntry = &entry;
    appearanceBound = true;
  };

  if (hasStreamKey) {
    if (m_geometryStreamCache.hasPendingCopy(geometryKey)) {
      if (const PendingGeometryUploadBinding* pending = m_geometryStreamCache.findPendingUpload(geometryKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == m_vertexCount && pending->indexCount == m_indexCount;
        const bool gensSame = pending->centersGen == payload.centersGen && pending->flagsGen == payload.flagsGen &&
                              pending->indexGen == payload.indexGen;
        const bool buffersOk =
          pending->centerRadius.buffer && pending->flags.buffer && ((m_indexCount == 0) || pending->index.buffer);
        if (sizeSame && gensSame && buffersOk) {
          m_centerRadiusBuffer = pending->centerRadius.buffer;
          m_flagsBuffer = pending->flags.buffer;
          m_centerRadiusOffset = pending->centerRadius.offset;
          m_flagsOffset = pending->flags.offset;
          if (m_indexCount > 0) {
            m_indexUploadBuffer = pending->index.buffer;
            m_indexUploadOffset = pending->index.offset;
          } else {
            m_indexUploadBuffer = vk::Buffer{};
            m_indexUploadOffset = 0;
          }
          geometryBound = true;
        }
      }
    }
    if (!geometryBound && !m_geometryStreamCache.hasPendingCopy(geometryKey)) {
      if (GeometryCacheEntry* entry = m_geometryStreamCache.findEntry(geometryKey); entry != nullptr) {
        const bool sizeSame = (entry->vertexCount == m_vertexCount) && (entry->indexCount == m_indexCount);
        const bool gensSame = (entry->centersGen == payload.centersGen) && (entry->flagsGen == payload.flagsGen) &&
                              (entry->indexGen == payload.indexGen);
        if (entry->promoted && sizeSame && gensSame && entry->vbCenterRadius && entry->vbFlags &&
            (m_indexCount == 0 || entry->ib)) {
          bindGeometryStatic(*entry);
        }
      }
    }

    if (m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (const PendingAppearanceUploadBinding* pending = m_appearanceStreamCache.findPendingUpload(appearanceKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == m_vertexCount;
        const bool colorSame = payload.pickingPass ? (pending->pickingColorsGen == payload.pickingColorsGen)
                                                   : (pending->colorsGen == payload.colorsGen);
        const bool specularSame = !dynamicSpecular || (pending->specularGen == payload.specularGen);
        const bool buffersOk = pending->color.buffer && pending->specular.buffer;
        if (sizeSame && colorSame && specularSame && buffersOk) {
          m_colorBuffer = pending->color.buffer;
          m_specularBuffer = pending->specular.buffer;
          m_colorOffset = pending->color.offset;
          m_specularOffset = pending->specular.offset;
          appearanceBound = true;
        }
      }
    }
    if (!appearanceBound && !m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (AppearanceCacheEntry* entry = m_appearanceStreamCache.findEntry(appearanceKey); entry != nullptr) {
        const bool sizeSame = entry->vertexCount == m_vertexCount;
        const bool colorSame = payload.pickingPass ? (entry->pickingColorsGen == payload.pickingColorsGen)
                                                   : (entry->colorsGen == payload.colorsGen);
        const bool specularSame = !dynamicSpecular || (entry->specularGen == payload.specularGen);
        if (entry->promoted && sizeSame && colorSame && specularSame && entry->vbColor && entry->vbSpecular) {
          bindAppearanceStatic(*entry);
        }
      }
    }
  }

  if (geometryBound && appearanceBound) {
    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundGeometryStaticEntry->vbCenterRadius, &boundGeometryStaticEntry->vbFlags, &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundAppearanceStaticEntry->vbColor, &boundAppearanceStaticEntry->vbSpecular});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    m_usedStaticVBThisFrame = (boundGeometryStaticEntry != nullptr) && (boundAppearanceStaticEntry != nullptr);
    return;
  }

  const bool needGeometryUpload = !geometryBound;
  const bool needAppearanceUpload = !appearanceBound;
  // A pass may reuse only geometry or only appearance statics while uploading the
  // other half. Protect the selected static slices before upload reservation or
  // promotion can invoke the global residency broker.
  if (boundGeometryStaticEntry != nullptr) {
    vulkan::pinStaticSlicesForActiveSubmission(
      m_backend,
      {&boundGeometryStaticEntry->vbCenterRadius, &boundGeometryStaticEntry->vbFlags, &boundGeometryStaticEntry->ib});
    boundGeometryStaticEntry->usedStaticOnce = true;
    touchStaticStream(geometryKey.streamKey);
  }
  if (boundAppearanceStaticEntry != nullptr) {
    vulkan::pinStaticSlicesForActiveSubmission(
      m_backend,
      {&boundAppearanceStaticEntry->vbColor, &boundAppearanceStaticEntry->vbSpecular});
    boundAppearanceStaticEntry->usedStaticOnce = true;
    touchStaticStream(appearanceKey.streamKey);
  }
  m_backend.reserveUploadSlices({
    {needGeometryUpload ? crBytes : 0u,    alignof(glm::vec4)},
    {needAppearanceUpload ? colBytes : 0u, alignof(glm::vec4)},
    {needAppearanceUpload ? spBytes : 0u,  alignof(glm::vec4)},
    {needGeometryUpload ? flBytes : 0u,    alignof(float)    },
    {needGeometryUpload ? idxBytes : 0u,   alignof(uint32_t) }
  });

  Z3DRendererVulkanBackend::UploadSlice crSlice{};
  Z3DRendererVulkanBackend::UploadSlice colSlice{};
  Z3DRendererVulkanBackend::UploadSlice spSlice{};
  Z3DRendererVulkanBackend::UploadSlice flSlice{};
  Z3DRendererVulkanBackend::UploadSlice indexSlice{};
  if (needGeometryUpload) {
    crSlice = m_backend.suballocateUpload(crBytes, alignof(glm::vec4));
    flSlice = m_backend.suballocateUpload(flBytes, alignof(float));
  }
  if (needAppearanceUpload) {
    colSlice = m_backend.suballocateUpload(colBytes, alignof(glm::vec4));
    spSlice = m_backend.suballocateUpload(spBytes, alignof(glm::vec4));
  }
  if (needGeometryUpload && idxBytes > 0) {
    indexSlice = m_backend.suballocateUpload(idxBytes, alignof(uint32_t));
  }

  if ((needGeometryUpload && (!crSlice.buffer || !crSlice.mapped || !flSlice.buffer || !flSlice.mapped ||
                              (idxBytes > 0 && (!indexSlice.buffer || !indexSlice.mapped)))) ||
      (needAppearanceUpload && (!colSlice.buffer || !colSlice.mapped || !spSlice.buffer || !spSlice.mapped))) {
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }

  if (needGeometryUpload) {
    std::memcpy(crSlice.mapped, payload.pointsAndRadius.data(), crBytes);
    auto* fOut = static_cast<float*>(flSlice.mapped);
    for (size_t i = 0; i < m_vertexCount; ++i) {
      fOut[i] = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
    }
    if (idxBytes > 0) {
      auto* dst = static_cast<uint32_t*>(indexSlice.mapped);
      for (size_t i = 0; i < m_indexCount; ++i) {
        CHECK(payload.indices[i] >= payload.indexValueBias) << "Sphere segment index rebasing underflow";
        dst[i] = payload.indices[i] - payload.indexValueBias;
      }
    }
    m_centerRadiusBuffer = crSlice.buffer;
    m_flagsBuffer = flSlice.buffer;
    m_centerRadiusOffset = crSlice.offset;
    m_flagsOffset = flSlice.offset;
    if (idxBytes > 0) {
      m_indexUploadBuffer = indexSlice.buffer;
      m_indexUploadOffset = indexSlice.offset;
    } else {
      m_indexUploadBuffer = vk::Buffer{};
      m_indexUploadOffset = 0;
    }
  }

  if (needAppearanceUpload) {
    auto* cOut = static_cast<glm::vec4*>(colSlice.mapped);
    if (payload.pickingPass) {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        cOut[i] = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
      }
    } else {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        cOut[i] = (i < payload.colors.size()) ? payload.colors[i] : glm::vec4(1.0f);
      }
    }
    auto* spOut = static_cast<glm::vec4*>(spSlice.mapped);
    if (dynamicSpecular) {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        spOut[i] = (i < payload.specularAndShininess.size()) ? payload.specularAndShininess[i] : glm::vec4(0.0f);
      }
    } else {
      std::memset(spOut, 0, spBytes);
    }
    m_colorBuffer = colSlice.buffer;
    m_specularBuffer = spSlice.buffer;
    m_colorOffset = colSlice.offset;
    m_specularOffset = spSlice.offset;
  }

  if (hasStreamKey) {
    auto rememberPendingGeometryUpload = [&]() {
      if (!needGeometryUpload) {
        return;
      }
      PendingGeometryUploadBinding binding{};
      binding.centerRadius = crSlice;
      binding.flags = flSlice;
      binding.index = indexSlice;
      binding.vertexCount = static_cast<uint32_t>(m_vertexCount);
      binding.indexCount = static_cast<uint32_t>(m_indexCount);
      binding.centersGen = payload.centersGen;
      binding.flagsGen = payload.flagsGen;
      binding.indexGen = payload.indexGen;
      m_geometryStreamCache.rememberPendingUploadBinding(geometryKey, binding);
    };
    auto rememberPendingAppearanceUpload = [&]() {
      if (!needAppearanceUpload) {
        return;
      }
      PendingAppearanceUploadBinding binding{};
      binding.color = colSlice;
      binding.specular = spSlice;
      binding.vertexCount = static_cast<uint32_t>(m_vertexCount);
      binding.colorsGen = payload.colorsGen;
      binding.pickingColorsGen = payload.pickingColorsGen;
      binding.specularGen = payload.specularGen;
      m_appearanceStreamCache.rememberPendingUploadBinding(appearanceKey, binding);
    };

    const int kPromotionThreshold = 2;
    auto promoteGeometryToStatics = [&](GeometryCacheEntry& entry) -> bool {
      if (!needGeometryUpload) {
        return false;
      }
      const Z3DRendererVulkanBackend::UploadSlice* idxSrc = idxBytes > 0 ? &indexSlice : nullptr;
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(
            m_backend,
            {
              {&entry.vbCenterRadius, &crSlice, crBytes,  alignof(glm::vec4), false},
              {&entry.vbFlags,        &flSlice, flBytes,  alignof(float),     false},
              {&entry.ib,             idxSrc,   idxBytes, alignof(uint32_t),  true }
      },
            &stagedBytes)) {
        return false;
      }
      m_backend.addSphereBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK sphere geometry promote: centerRadius={}B flags={}B idx={}B",
                             crBytes,
                             flBytes,
                             idxBytes);
      return true;
    };
    auto promoteAppearanceToStatics = [&](AppearanceCacheEntry& entry) -> bool {
      if (!needAppearanceUpload) {
        return false;
      }
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(m_backend,
                                                   {
                                                     {&entry.vbColor,    &colSlice, colBytes, alignof(glm::vec4), false},
                                                     {&entry.vbSpecular, &spSlice,  spBytes,  alignof(glm::vec4), false},
      },
                                                   &stagedBytes)) {
        return false;
      }
      m_backend.addSphereBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK sphere appearance promote: color={}B specular={}B", colBytes, spBytes);
      return true;
    };

    {
      auto [entry, inserted] = m_geometryStreamCache.tryEmplaceEntry(geometryKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevIndexCount = entry.indexCount;
      const uint32_t prevCentersGen = entry.centersGen;
      const uint32_t prevFlagsGen = entry.flagsGen;
      const uint32_t prevIndexGen = entry.indexGen;
      const bool sizeSame = (prevVertexCount == m_vertexCount) && (prevIndexCount == m_indexCount);
      const bool gensSame = (prevCentersGen == payload.centersGen) && (prevFlagsGen == payload.flagsGen) &&
                            (prevIndexGen == payload.indexGen);
      entry.unchangedFrames = (!inserted && sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      if (needGeometryUpload) {
        if (m_geometryStreamCache.hasPendingCopy(geometryKey)) {
          rememberPendingGeometryUpload();
        } else if (inserted) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbCenterRadius, &entry.vbFlags, &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool centersChanged = prevCentersGen != payload.centersGen;
          const bool flagsChanged = prevFlagsGen != payload.flagsGen;
          const bool indexChanged = (prevIndexGen != payload.indexGen) && (idxBytes > 0);
          const bool anyChanged = centersChanged || flagsChanged || indexChanged;
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbCenterRadius, &entry.vbFlags, &entry.ib});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (centersChanged) {
              m_backend.scheduleStaticCopy(entry.vbCenterRadius.buffer, entry.vbCenterRadius.offset, crSlice, false);
              restagedBytes += crBytes;
            }
            if (flagsChanged) {
              m_backend.scheduleStaticCopy(entry.vbFlags.buffer, entry.vbFlags.offset, flSlice, false);
              restagedBytes += flBytes;
            }
            if (indexChanged) {
              m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, indexSlice, true);
              restagedBytes += idxBytes;
            }
            if (restagedBytes > 0) {
              m_backend.addSphereBytesStaged(restagedBytes);
            }
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(geometryKey.streamKey);
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        }
      }

      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.centersGen = payload.centersGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
    }

    {
      auto [entry, inserted] = m_appearanceStreamCache.tryEmplaceEntry(appearanceKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevColorsGen = entry.colorsGen;
      const uint32_t prevPickingColorsGen = entry.pickingColorsGen;
      const uint32_t prevSpecularGen = entry.specularGen;
      const bool sizeSame = prevVertexCount == m_vertexCount;
      const bool colorSame =
        payload.pickingPass ? (prevPickingColorsGen == payload.pickingColorsGen) : (prevColorsGen == payload.colorsGen);
      const bool specularSame = !dynamicSpecular || (prevSpecularGen == payload.specularGen);
      entry.unchangedFrames = (!inserted && sizeSame && colorSame && specularSame) ? (entry.unchangedFrames + 1) : 0;

      if (needAppearanceUpload) {
        if (m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
          rememberPendingAppearanceUpload();
        } else if (inserted) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbColor, &entry.vbSpecular});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool colorChanged = payload.pickingPass ? (prevPickingColorsGen != payload.pickingColorsGen)
                                                        : (prevColorsGen != payload.colorsGen);
          const bool specularChanged = dynamicSpecular && (prevSpecularGen != payload.specularGen);
          const bool anyChanged = colorChanged || specularChanged;
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbColor, &entry.vbSpecular});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (colorChanged) {
              m_backend.scheduleStaticCopy(entry.vbColor.buffer, entry.vbColor.offset, colSlice, false);
              restagedBytes += colBytes;
            }
            if (specularChanged) {
              m_backend.scheduleStaticCopy(entry.vbSpecular.buffer, entry.vbSpecular.offset, spSlice, false);
              restagedBytes += spBytes;
            }
            if (restagedBytes > 0) {
              m_backend.addSphereBytesStaged(restagedBytes);
            }
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            touchStaticStream(appearanceKey.streamKey);
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        }
      }

      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.colorsGen = payload.colorsGen;
      entry.pickingColorsGen = payload.pickingColorsGen;
      entry.specularGen = payload.specularGen;
    }

    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundGeometryStaticEntry->vbCenterRadius, &boundGeometryStaticEntry->vbFlags, &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundAppearanceStaticEntry->vbColor, &boundAppearanceStaticEntry->vbSpecular});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    m_usedStaticVBThisFrame = (boundGeometryStaticEntry != nullptr) && (boundAppearanceStaticEntry != nullptr);
  }
}

} // namespace nim
