#include "zvulkanconepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3drendererstates.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanstaticpromotionutils.h"
#include "zvulkanbindings.h"
#include "zvulkanclipplanes.h"
#include "zvulkanuniforms.h"
#include "zsysteminfo.h"
#include "zexception.h"
#include "zlog.h"
#include "z3dconerenderer.h"
#include "zrenderthreadexecutor_tls.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdint>

#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>

DECLARE_bool(atlas_vk_cache_draw_secondaries);

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

std::array<glm::vec4, 3> encodeMat3ToStd140(const glm::mat3& matrix)
{
  return {glm::vec4(matrix[0], 0.0f), glm::vec4(matrix[1], 0.0f), glm::vec4(matrix[2], 0.0f)};
}

size_t staticSliceBytes(const Z3DRendererVulkanBackend::StaticSlice& slice)
{
  return slice ? slice.size : 0u;
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

} // namespace

ZVulkanConePipelineContext::ZVulkanConePipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
  , m_streamUsageTracker(Z3DRendererVulkanBackend::StaticCacheOwner::Cone)
{}

ZVulkanConePipelineContext::~ZVulkanConePipelineContext() = default;

ZVulkanConePipelineContext::FormatsKey
ZVulkanConePipelineContext::FormatsKey::from(const vulkan::AttachmentFormats& formats)
{
  FormatsKey key{};
  const size_t count = std::min(formats.colorFormats.size(), key.colorFormats.size());
  key.colorCount = static_cast<uint32_t>(count);
  for (size_t i = 0; i < count; ++i) {
    key.colorFormats[i] = formats.colorFormats[i];
  }
  if (formats.depthFormat.has_value()) {
    key.depthFormat = *formats.depthFormat;
  }
  return key;
}

void ZVulkanConePipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_usedStaticVBThisFrame = false;
  m_originBuffer = nullptr;
  m_axisBuffer = nullptr;
  m_flagsBuffer = nullptr;
  m_baseColorBuffer = nullptr;
  m_topColorBuffer = nullptr;
  m_originOffset = 0;
  m_axisOffset = 0;
  m_flagsOffset = 0;
  m_baseColorOffset = 0;
  m_topColorOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  // Per-stream UBO offsets are cached per frame-slot in the backend's persistent
  // uniform arena; keep them across submissions to stabilize dynamic offsets.
  m_ddpArgsPrepared = false;
  m_ddpArgsOffset = 0;
  m_geometryStreamCache.resetFrame();
  m_appearanceStreamCache.resetFrame();
}

void ZVulkanConePipelineContext::evictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }

  m_streamUsageTracker.eraseStream(streamKey);

  m_geometryStreamCache.evictStream(streamKey, [this](GeometryCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbOrigin);
    m_backend.releaseStaticSlice(entry.vbAxis);
    m_backend.releaseStaticSlice(entry.vbFlags);
    m_backend.releaseStaticSlice(entry.ib);
  });
  m_appearanceStreamCache.evictStream(streamKey, [this](AppearanceCacheEntry& entry) {
    m_backend.releaseStaticSlice(entry.vbBaseColor);
    m_backend.releaseStaticSlice(entry.vbTopColor);
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
}

void ZVulkanConePipelineContext::touchStaticStream(uint64_t streamKey)
{
  m_streamUsageTracker.touch(streamKey, m_backend.currentStaticCacheEpoch());
}

size_t ZVulkanConePipelineContext::staticBytesForStream(uint64_t streamKey,
                                                        Z3DRendererVulkanBackend::StaticPressureDomain domain) const
{
  return m_geometryStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const GeometryCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbOrigin) + staticSliceBytes(entry.vbAxis) +
                      staticSliceBytes(entry.vbFlags);
             }
             return staticSliceBytes(entry.ib);
           }) +
         m_appearanceStreamCache.staticBytesForStream(
           streamKey,
           domain,
           [](const AppearanceCacheEntry& entry, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
             if (cacheDomain == Z3DRendererVulkanBackend::StaticPressureDomain::Vertex) {
               return staticSliceBytes(entry.vbBaseColor) + staticSliceBytes(entry.vbTopColor);
             }
             return size_t{0};
           });
}

std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
ZVulkanConePipelineContext::oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain,
                                                        uint64_t protectedEpoch) const
{
  return m_streamUsageTracker.oldestEvictableStaticStream(
    domain,
    protectedEpoch,
    [this](uint64_t streamKey, Z3DRendererVulkanBackend::StaticPressureDomain cacheDomain) {
      return staticBytesForStream(streamKey, cacheDomain);
    });
}

size_t ZVulkanConePipelineContext::evictStaticStreamForPressure(uint64_t streamKey)
{
  const size_t vbBytes = staticBytesForStream(streamKey, Z3DRendererVulkanBackend::StaticPressureDomain::Vertex);
  const size_t ibBytes = staticBytesForStream(streamKey, Z3DRendererVulkanBackend::StaticPressureDomain::Index);
  evictStream(streamKey);
  return vbBytes + ibBytes;
}

void ZVulkanConePipelineContext::flushRetainedUbos()
{
  if (m_retainedUbos.empty()) {
    return;
  }
  const auto fence = m_backend.awaitActiveSubmissionFence("VK cone retained UBO lifetime");
  auto keepAlive = currentRenderThreadExecutorKeepAlive("VK cone retained UBO lifetime");
  for (auto& sp : m_retainedUbos) {
    m_backend.spawnDetachedTask(keepAlive,
                                folly::coro::co_invoke([fence, keep = sp]() mutable -> folly::coro::Task<void> {
                                  co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(fence);
                                  co_return;
                                }),
                                "VK cone retained UBO lifetime");
  }
  m_retainedUbos.clear();
}

void ZVulkanConePipelineContext::record(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const ConePayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd)
{
  // Ensure last frame's UBOs remain alive until this submission completes.
  flushRetainedUbos();
  VLOG(2) << "record begin: payload sizes base=" << payload.baseAndRadius.size()
          << " axis=" << payload.axisAndTopRadius.size() << " flags=" << payload.flags.size()
          << " baseColors=" << payload.baseColors.size() << " topColors=" << payload.topColors.size()
          << " pickingColors=" << payload.pickingColors.size() << " indices=" << payload.indices.size()
          << " pickingPass=" << payload.pickingPass << " useConeShader2=" << payload.useConeShader2;
  if (payload.baseAndRadius.empty()) {
    return;
  }

  // GL parity: in picking pass, require picking colors sized equal to vertices.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.baseAndRadius.size()) {
      return;
    }
  }

  uploadGeometry(payload);
  VLOG(2) << "uploadGeometry -> vertexCount=" << m_vertexCount << " indexCount=" << m_indexCount;
  if (m_vertexCount == 0) {
    return;
  }

  // Per-draw diagnostic; keep behind VLOG(2) so --v=1 remains usable for perf runs.
  VLOG(2) << fmt::format(
    "VK cone draw: picking={} useShader2={} verts={} idx={} buf(origin={}, axis={}, flags={}, base={}, top={}) offs(o={}, a={}, f={}, b={}, t={})",
    payload.pickingPass,
    payload.useConeShader2,
    m_vertexCount,
    m_indexCount,
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_originBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_axisBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_flagsBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_baseColorBuffer))),
    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(m_topColorBuffer))),
    m_originOffset,
    m_axisOffset,
    m_flagsOffset,
    m_baseColorOffset,
    m_topColorOffset);

  CHECK(batch.shaderHook.captured) << "Cone batch missing shader hook snapshot";
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
  CHECK(dsLighting && dsTransforms) << "Cone pipeline shared descriptor sets missing (lighting/transforms)";
  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Cone pipeline record called without an active Vulkan frame-slot key";

  const auto& hookPara = batch.shaderHook.para;
  DDPPeelPushConstants ddpPc{};
  const bool usesDdpPeelPc = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (usesDdpPeelPc) {
    CHECK(hookPara.dualDepthPeelingDepthBlenderHandle.valid()) << "Cone DDP peel requires a valid depth blender handle";
    CHECK(hookPara.dualDepthPeelingFrontBlenderHandle.valid()) << "Cone DDP peel requires a valid front blender handle";
    auto& depthTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingDepthBlenderHandle,
                                               m_backend.device(),
                                               "cone dual-depth-peeling depth blender");
    auto& frontTex = vulkan::textureFromHandle(hookPara.dualDepthPeelingFrontBlenderHandle,
                                               m_backend.device(),
                                               "cone dual-depth-peeling front blender");
    ddpPc.ddpDepthBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(depthTex, "cone ddp depth blender");
    ddpPc.ddpFrontBlender = m_backend.bindlessLookupSampledImageAutoOrCrash(frontTex, "cone ddp front blender");
  }

  const auto& formatsOpt = m_backend.currentSegmentFormats();
  CHECK(formatsOpt.has_value()) << "Cone pipeline context requires an active Vulkan rendering segment";
  const auto& formats = *formatsOpt;

  m_backend.validateFormatsOrCrash(formats, "cone");

  PipelineKey key;
  key.dynamicMaterial = !pickingPass;
  key.useConeShader2 = payload.useConeShader2;
  key.capsMode = toCapsMode(payload.capStyle);
  key.shaderHookType = shaderHook;
  key.formats = FormatsKey::from(formats);

  PipelineInstance& pipeline = ensurePipeline(key, formats);
  const std::array<vk::DescriptorSet, 3> boundSets{
    m_backend.bindlessSampledImageDescriptorSet(),
    dsLighting,
    dsTransforms,
  };

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = vkbind::kSetBindlessSampledImages;
  drawSpec.descriptorSets = boundSets;
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

  uint32_t expectedSets = static_cast<uint32_t>(boundSets.size());
  std::array<vk::DescriptorSet, 1> oitDescriptorSets{m_backend.sharedOITDescriptorSet()};
  std::array<ZVulkanDescriptorBindInfo, 1> extraBinds{};
  ZVulkanDescriptorBindInfo oitBind{};
  oitBind.firstSet = vkbind::kSetOIT;
  oitBind.sets = oitDescriptorSets;
  extraBinds[0] = oitBind;
  const uint32_t extraBindCount = 1;
  expectedSets = std::max(expectedSets, vkbind::kSetOIT + 1u);
  drawSpec.extraDescriptorBinds = std::span<const ZVulkanDescriptorBindInfo>(extraBinds.data(), extraBindCount);
  drawSpec.expectedDescriptorSetCount = expectedSets;

  const std::array<vk::Buffer, 5> vertexBuffers{m_originBuffer,
                                                m_axisBuffer,
                                                m_flagsBuffer,
                                                m_baseColorBuffer,
                                                m_topColorBuffer};
  const std::array<vk::DeviceSize, 5> vertexOffsets{m_originOffset,
                                                    m_axisOffset,
                                                    m_flagsOffset,
                                                    m_baseColorOffset,
                                                    m_topColorOffset};
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

  const std::array<uint64_t, 5> vertexBufferSegmentIds{
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[0]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[1]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[2]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[3]),
    m_backend.staticArenaSegmentIdForBuffer(vertexBuffers[4]),
  };
  uint64_t indexBufferSegmentId = 0;
  if (drawSpec.indexCount > 0 && drawSpec.indexBuffer) {
    indexBufferSegmentId = m_backend.staticArenaSegmentIdForBuffer(drawSpec.indexBuffer);
  }
  const bool allStaticSegments = (vertexBufferSegmentIds[0] != 0) && (vertexBufferSegmentIds[1] != 0) &&
                                 (vertexBufferSegmentIds[2] != 0) && (vertexBufferSegmentIds[3] != 0) &&
                                 (vertexBufferSegmentIds[4] != 0) &&
                                 ((drawSpec.indexCount == 0) || (indexBufferSegmentId != 0));

  const bool ddpHook = (shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit ||
                        shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
  if (FLAGS_atlas_vk_cache_draw_secondaries &&
      m_backend.device().context().supportsInlineAndSecondaryDynamicRendering() && payload.streamKey != 0 &&
      m_usedStaticVBThisFrame && allStaticSegments && !ddpHook) {
    m_backend.notifyDrawSecondaryCacheAttempt();
    SecondaryCacheKey cacheKey{};
    cacheKey.frameKey = frameKey;
    cacheKey.streamKey = payload.streamKey;
    cacheKey.streamSegmentOrdinal = payload.streamSegmentOrdinal;
    cacheKey.picking = pickingPass;
    cacheKey.dynamicMaterial = key.dynamicMaterial;
    cacheKey.useConeShader2 = key.useConeShader2;
    cacheKey.capsMode = key.capsMode;
    cacheKey.shaderHookType = shaderHook;
    cacheKey.eye = batch.eye;
    cacheKey.oitRingIndex = m_backend.sharedOITDescriptorSetRingIndex();

    SecondarySignature signature{};
    signature.pipeline = drawSpec.pipelineHandle;
    signature.layout = drawSpec.pipelineLayoutHandle;
    signature.baseDescriptorSets = boundSets;
    signature.baseDescriptorGenerations = {m_backend.bindlessSampledImageDescriptorSetGeneration(),
                                           m_backend.sharedLightingDescriptorSetGeneration(),
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
        vulkan::textureFromHandle(attachment.handle, m_backend.device(), "cone secondary inheritance color");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Cone secondary: mismatched MSAA sample counts in attachments";
      }
    }
    if (batch.pass.depthAttachment && batch.pass.depthAttachment->handle.valid()) {
      const auto& tex = vulkan::textureFromHandle(batch.pass.depthAttachment->handle,
                                                  m_backend.device(),
                                                  "cone secondary inheritance depth");
      if (!samplesCaptured) {
        rasterSamples = tex.info().samples;
        samplesCaptured = true;
      } else {
        CHECK(tex.info().samples == rasterSamples) << "Cone secondary: mismatched MSAA sample counts in attachments";
      }
    }

    const vk::Format* colorFormatsPtr = formats.colorFormats.empty() ? nullptr : formats.colorFormats.data();
    const vk::Format depthFormat = formats.depthFormat.value_or(vk::Format::eUndefined);
    vk::CommandBufferInheritanceRenderingInfo renderingInheritance{};
    renderingInheritance.rasterizationSamples = rasterSamples;
    renderingInheritance.colorAttachmentCount = static_cast<uint32_t>(formats.colorFormats.size());
    renderingInheritance.pColorAttachmentFormats = colorFormatsPtr;
    renderingInheritance.depthAttachmentFormat = depthFormat;
    // Atlas uses depth-only dynamic rendering segments (pStencilAttachment == nullptr).
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
  if (m_backend.ddpIndirectCountEnabled() && shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingInit) {
    const bool indexed = (drawSpec.indexCount > 0);
    if (static_cast<VkBuffer>(m_backend.ddpDeviceArgsBuffer()) != VK_NULL_HANDLE) {
      m_ddpArgsOffset = m_backend.ddpAllocDeviceArgsSlot(indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                                                 : sizeof(VkDrawIndirectCommand));
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
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), m_ddpArgsOffset, slice);
      } else {
        struct Cmd
        {
          uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
        } cmdPayload{drawSpec.vertexCount, drawSpec.instanceCount, drawSpec.firstVertex, drawSpec.firstInstance};
        auto slice = m_backend.suballocateUpload(sizeof(Cmd), alignof(Cmd));
        if (slice.buffer && slice.mapped) {
          std::memcpy(slice.mapped, &cmdPayload, sizeof(Cmd));
        }
        m_backend.scheduleStaticCopyIndirect(m_backend.ddpDeviceArgsBuffer(), m_ddpArgsOffset, slice);
      }
      m_ddpArgsPrepared = true;
    }
    recorder.recordGraphicsDraw(drawSpec);
  } else if (m_backend.ddpIndirectCountEnabled() &&
             shaderHook == Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel) {
    const bool indexed = (drawSpec.indexCount > 0);
    recorder.recordGraphicsDraw(drawSpec, [&](vk::raii::CommandBuffer& c) {
      const vk::Buffer argsBuf = m_backend.ddpDeviceArgsBuffer();
      const vk::Buffer cntBuf = m_backend.ddpIndirectCountBuffer();
      CHECK(m_ddpArgsPrepared) << "Cone DDP peel: args not prepared in init";
      if (indexed) {
        c.drawIndexedIndirectCount(argsBuf, m_ddpArgsOffset, cntBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
      } else {
        c.drawIndirectCount(argsBuf, m_ddpArgsOffset, cntBuf, 0, 1, sizeof(VkDrawIndirectCommand));
      }
    });
  } else {
    recorder.recordGraphicsDraw(drawSpec);
  }
}

// Lighting UBO is shared per frame; no per-batch update required.

void ZVulkanConePipelineContext::updateTransformUBO(Z3DRendererBase& renderer,
                                                    const RenderBatch& batch,
                                                    const ConePayload& payload,
                                                    bool pickingPass)
{
  CHECK(batch.shaderHook.captured) << "Cone batch missing shader hook snapshot";
  (void)renderer;
  CHECK(payload.paramsCaptured) << "Cone payload missing params";

  // Frame transforms are shared per eye and updated once per submission in beginRender().
  // If the batch carries a viewStateOverride snapshot, allocate a dedicated
  // frame UBO slice for this draw.
  if (batch.viewStateOverride) {
    const auto& viewState = *batch.viewStateOverride;
    const auto& eyeState = viewState.eyes[static_cast<size_t>(batch.eye)];
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

  ObjectTransformsUBOStd140 objectTransforms{};
  const glm::mat4 model = payload.followCoordTransform ? payload.params.coordTransform : glm::mat4(1.0f);
  objectTransforms.pos_transform = model;
  // Cone shaders do not consume the normal matrix; keep identity to avoid
  // redundant inverse/transpose work on static geometry.
  objectTransforms.pos_transform_normal_matrix = encodeMat3ToStd140(glm::mat3(1.0f));
  const float sizeScale = payload.followSizeScale ? payload.params.sizeScale : 1.0f;
  objectTransforms.parameters = glm::vec4(sizeScale, 0.0f, 0.0f, 0.0f);
  vulkan::applyBatchClipPlanesToTransforms(batch, objectTransforms);

  MaterialUBOStd140 material{};
  material.material_ambient = payload.params.materialAmbient;
  material.material_specular = pickingPass ? glm::vec4(0.0f) : payload.params.materialSpecular;
  material.material_shininess = pickingPass ? 0.0f : payload.params.materialShininess;
  material.alpha = (pickingPass || !payload.followOpacity) ? 1.0f : payload.params.opacity;
  material.use_custom_color = 0;
  material.custom_color = glm::vec4(1.0f);

  void* frameKey = m_backend.activeFrameKey();
  CHECK(frameKey != nullptr) << "Cone updateTransformUBO called without an active Vulkan frame-slot key";
  if (payload.streamKey != 0) {
    FrameUboCache& frameCache = m_uboCacheByFrameKey[frameKey];
    auto& entries = frameCache.byStream[payload.streamKey];

    for (auto& cached : entries) {
      if (cached.pickingPass != pickingPass) {
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
                  &objectTransforms,
                  sizeof(objectTransforms));
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

ZVulkanConePipelineContext::PipelineInstance&
ZVulkanConePipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;

  auto selectFragmentShader = [&](Z3DRendererBase::ShaderHookType hook) -> std::string {
    const char* suffix = key.useConeShader2 ? "_2" : "";
    switch (hook) {
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingInit:
        return std::string("dual_peeling_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel:
        return std::string("dual_peeling_peel_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
        return std::string("ppll_count_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
        return std::string("ppll_store_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedAverageInit:
        return std::string("wavg_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::WeightedBlendedInit:
        return std::string("wblended_init_cone") + suffix + ".frag.spv";
      case Z3DRendererBase::ShaderHookType::Normal:
      default:
        return std::string("cone") + suffix + ".frag.spv";
    }
  };

  const std::string vertName = key.useConeShader2 ? "cone_2.vert.spv" : "cone.vert.spv";
  VLOG(1) << "selecting shaders vert='" << (shaderBase + vertName) << "' frag='"
          << (shaderBase + selectFragmentShader(key.shaderHookType)) << "' capsMode=" << key.capsMode
          << " dynamicMaterial=" << key.dynamicMaterial << " shaderHook=" << static_cast<int>(key.shaderHookType);
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + vertName,
                                                    shaderBase + selectFragmentShader(key.shaderHookType),
                                                    std::nullopt);

  std::array<vk::SpecializationMapEntry, 1> specEntries{
    vk::SpecializationMapEntry{.constantID = 90, .offset = 0, .size = sizeof(int)}
  };
  std::array<int, 1> specData{key.capsMode};
  // Apply CAPS_MODE to both vertex and fragment (cone_2.vert depends on it)
  {
    auto entries = std::vector<vk::SpecializationMapEntry>(specEntries.begin(), specEntries.end());
    auto dataVec = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                                        reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData));
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eVertex, entries, dataVec);
    instance.shader->setSpecializationConstants(vk::ShaderStageFlagBits::eFragment, entries, dataVec);
  }

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  const vk::DescriptorSetLayout bindlessLayout = m_backend.bindlessSampledImageDescriptorSetLayout();
  CHECK(bindlessLayout) << "Cone pipeline missing bindless descriptor set layout";
  const vk::DescriptorSetLayout lightingLayout = m_backend.lightingDescriptorSetLayout();
  CHECK(lightingLayout) << "Cone pipeline missing lighting descriptor set layout";
  const vk::DescriptorSetLayout transformsLayout = m_backend.transformDescriptorSetLayout();
  CHECK(transformsLayout) << "Cone pipeline missing transforms descriptor set layout";
  const vk::DescriptorSetLayout oitLayout = m_backend.oitDescriptorSetLayout();
  CHECK(oitLayout) << "Cone pipeline missing OIT descriptor set layout";
  std::vector<vk::DescriptorSetLayout> layouts{bindlessLayout, lightingLayout, transformsLayout, oitLayout};
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  // Screen-space cone quads (cone_2.vert) end up with CW winding under Vulkan
  // due to clip-space conventions. Treat CW as front to match GL behavior.
  instance.pipeline->setFrontFace(vk::FrontFace::eClockwise);

  // Dual-depth-peeling peel shader consumes bindless blender indices via push constants.
  vk::PushConstantRange pcRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                .offset = 0,
                                .size = static_cast<uint32_t>(sizeof(DDPPeelPushConstants))};
  instance.pipeline->setPushConstantRanges({pcRange});

  vk::PipelineColorBlendAttachmentState baseBlend{};
  baseBlend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  baseBlend.blendEnable = false;

  switch (key.shaderHookType) {
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount:
    case Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore:
      // Exact OIT PPLL: depth test against opaque depth, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    case Z3DRendererBase::ShaderHookType::WeightedAverageInit: {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(formats.colorFormats.size(), baseBlend);
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
      instance.pipeline->setDepthTestEnable(formats.depthFormat.has_value());
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    case Z3DRendererBase::ShaderHookType::WeightedBlendedInit: {
      if (!formats.colorFormats.empty()) {
        std::vector<vk::PipelineColorBlendAttachmentState> attachments;
        attachments.reserve(formats.colorFormats.size());
        for (size_t i = 0; i < formats.colorFormats.size(); ++i) {
          auto state = baseBlend;
          state.blendEnable = true;
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
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 1 || i == 4) {
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else if (i == 2 || i == 5) {
          state.blendEnable = true;
          state.srcColorBlendFactor = vk::BlendFactor::eOne;
          state.dstColorBlendFactor = vk::BlendFactor::eOne;
          // Match GL DDP peel: back-temp uses MAX blending (one fragment per pixel).
          state.colorBlendOp = vk::BlendOp::eMax;
          state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
          state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
          state.alphaBlendOp = vk::BlendOp::eMax;
        } else {
          state.blendEnable = false;
        }
        attachments.push_back(state);
      }
      instance.pipeline->setColorBlendAttachments(std::move(attachments));
      // Match GL DDP: depth-tested against the (loaded) opaque depth buffer, but do not write depth.
      instance.pipeline->setDepthTestEnable(true);
      instance.pipeline->setDepthWriteEnable(false);
      break;
    }
    default:
      break;
  }

  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanConePipelineContext::makeVertexInputState() const
{
  static std::array<vk::VertexInputBindingDescription, 5> bindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // origin
    vk::VertexInputBindingDescription{.binding = 1,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // axis
    vk::VertexInputBindingDescription{.binding = 2,
                                      .stride = static_cast<uint32_t>(sizeof(float)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // flags
    vk::VertexInputBindingDescription{.binding = 3,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}, // base color
    vk::VertexInputBindingDescription{.binding = 4,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec4)),
                                      .inputRate = vk::VertexInputRate::eVertex}  // top color
  };
  static std::array<vk::VertexInputAttributeDescription, 5> attrs{
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
                                        .offset = 0                                                               },
    vk::VertexInputAttributeDescription{.location = 4,
                                        .binding = 4,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = 0                                                               }
  };

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  info.pVertexBindingDescriptions = bindings.data();
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanConePipelineContext::uploadGeometry(const ConePayload& payload)
{
  m_usedStaticVBThisFrame = false;
  m_vertexCount = payload.baseAndRadius.size();
  m_indexCount = payload.indices.size();

  VLOG(2) << "uploadGeometry begin: vertexCount=" << m_vertexCount << " indexCount=" << m_indexCount
          << " axisCount=" << payload.axisAndTopRadius.size() << " flagsCount=" << payload.flags.size();

  if (m_vertexCount == 0) {
    return;
  }

  CHECK(payload.axisAndTopRadius.size() == m_vertexCount && payload.flags.size() == m_vertexCount)
    << "Vulkan cone backend skipping batch: attribute buffers are incomplete.";
  const size_t v4Bytes = m_vertexCount * sizeof(glm::vec4);
  const size_t fBytes = m_vertexCount * sizeof(float);
  const size_t idxBytes = m_indexCount * sizeof(uint32_t);
  const bool hasStreamKey = payload.streamKey != 0;
  const GeometryCacheKey geometryKey{payload.streamKey, payload.streamSegmentOrdinal};
  const AppearanceCacheKey appearanceKey{payload.streamKey, payload.streamSegmentOrdinal, payload.pickingPass};

  GeometryCacheEntry* boundGeometryStaticEntry = nullptr;
  AppearanceCacheEntry* boundAppearanceStaticEntry = nullptr;
  bool geometryBound = false;
  bool appearanceBound = false;

  auto bindGeometryStatic = [&](GeometryCacheEntry& entry) {
    m_originBuffer = entry.vbOrigin.buffer;
    m_axisBuffer = entry.vbAxis.buffer;
    m_flagsBuffer = entry.vbFlags.buffer;
    m_originOffset = entry.vbOrigin.offset;
    m_axisOffset = entry.vbAxis.offset;
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
    m_baseColorBuffer = entry.vbBaseColor.buffer;
    m_topColorBuffer = entry.vbTopColor.buffer;
    m_baseColorOffset = entry.vbBaseColor.offset;
    m_topColorOffset = entry.vbTopColor.offset;
    boundAppearanceStaticEntry = &entry;
    appearanceBound = true;
  };

  if (hasStreamKey) {
    if (m_geometryStreamCache.hasPendingCopy(geometryKey)) {
      if (const PendingGeometryUploadBinding* pending = m_geometryStreamCache.findPendingUpload(geometryKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == m_vertexCount && pending->indexCount == m_indexCount;
        const bool gensSame = pending->baseGen == payload.baseGen && pending->axisGen == payload.axisGen &&
                              pending->flagsGen == payload.flagsGen && pending->indexGen == payload.indexGen;
        const bool buffersOk = pending->origin.buffer && pending->axis.buffer && pending->flags.buffer &&
                               ((m_indexCount == 0) || pending->index.buffer);
        if (sizeSame && gensSame && buffersOk) {
          m_originBuffer = pending->origin.buffer;
          m_axisBuffer = pending->axis.buffer;
          m_flagsBuffer = pending->flags.buffer;
          m_originOffset = pending->origin.offset;
          m_axisOffset = pending->axis.offset;
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
        const bool gensSame = (entry->baseGen == payload.baseGen) && (entry->axisGen == payload.axisGen) &&
                              (entry->flagsGen == payload.flagsGen) && (entry->indexGen == payload.indexGen);
        if (entry->promoted && sizeSame && gensSame && entry->vbOrigin && entry->vbAxis && entry->vbFlags &&
            (m_indexCount == 0 || entry->ib)) {
          bindGeometryStatic(*entry);
        }
      }
    }

    if (m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (const PendingAppearanceUploadBinding* pending = m_appearanceStreamCache.findPendingUpload(appearanceKey);
          pending != nullptr) {
        const bool sizeSame = pending->vertexCount == m_vertexCount;
        const bool colorsSame = payload.pickingPass ? (pending->pickingColorsGen == payload.pickingColorsGen)
                                                    : ((pending->baseColorGen == payload.baseColorGen) &&
                                                       (pending->topColorGen == payload.topColorGen));
        const bool buffersOk = pending->baseColor.buffer && pending->topColor.buffer;
        if (sizeSame && colorsSame && buffersOk) {
          m_baseColorBuffer = pending->baseColor.buffer;
          m_topColorBuffer = pending->topColor.buffer;
          m_baseColorOffset = pending->baseColor.offset;
          m_topColorOffset = pending->topColor.offset;
          appearanceBound = true;
        }
      }
    }
    if (!appearanceBound && !m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
      if (AppearanceCacheEntry* entry = m_appearanceStreamCache.findEntry(appearanceKey); entry != nullptr) {
        const bool sizeSame = entry->vertexCount == m_vertexCount;
        const bool colorsSame = payload.pickingPass ? (entry->pickingColorsGen == payload.pickingColorsGen)
                                                    : ((entry->baseColorGen == payload.baseColorGen) &&
                                                       (entry->topColorGen == payload.topColorGen));
        if (entry->promoted && sizeSame && colorsSame && entry->vbBaseColor && entry->vbTopColor) {
          bindAppearanceStatic(*entry);
        }
      }
    }
  }

  if (geometryBound && appearanceBound) {
    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                 {&boundGeometryStaticEntry->vbOrigin,
                                                  &boundGeometryStaticEntry->vbAxis,
                                                  &boundGeometryStaticEntry->vbFlags,
                                                  &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundAppearanceStaticEntry->vbBaseColor, &boundAppearanceStaticEntry->vbTopColor});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    m_usedStaticVBThisFrame = (boundGeometryStaticEntry != nullptr) && (boundAppearanceStaticEntry != nullptr);
    return;
  }

  const bool needGeometryUpload = !geometryBound;
  const bool needAppearanceUpload = !appearanceBound;

  m_backend.reserveUploadSlices({
    {needGeometryUpload ? v4Bytes : 0u,   alignof(glm::vec4)},
    {needGeometryUpload ? v4Bytes : 0u,   alignof(glm::vec4)},
    {needGeometryUpload ? fBytes : 0u,    alignof(float)    },
    {needAppearanceUpload ? v4Bytes : 0u, alignof(glm::vec4)},
    {needAppearanceUpload ? v4Bytes : 0u, alignof(glm::vec4)},
    {needGeometryUpload ? idxBytes : 0u,  alignof(uint32_t) }
  });
  VLOG(2) << "uploadGeometry allocating: needGeometry=" << needGeometryUpload
          << " needAppearance=" << needAppearanceUpload << " v4Bytes=" << v4Bytes << " fBytes=" << fBytes
          << " idxBytes=" << idxBytes;

  Z3DRendererVulkanBackend::UploadSlice originSlice{};
  Z3DRendererVulkanBackend::UploadSlice axisSlice{};
  Z3DRendererVulkanBackend::UploadSlice flagsSlice{};
  Z3DRendererVulkanBackend::UploadSlice baseColorSlice{};
  Z3DRendererVulkanBackend::UploadSlice topColorSlice{};
  Z3DRendererVulkanBackend::UploadSlice indexSlice{};

  if (needGeometryUpload) {
    originSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
    axisSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
    flagsSlice = m_backend.suballocateUpload(fBytes, alignof(float));
  }
  if (needAppearanceUpload) {
    baseColorSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
    topColorSlice = m_backend.suballocateUpload(v4Bytes, alignof(glm::vec4));
  }
  if (needGeometryUpload && idxBytes > 0) {
    indexSlice = m_backend.suballocateUpload(idxBytes, alignof(uint32_t));
  }

  VLOG(2) << "Slices: origin(buf=" << static_cast<bool>(originSlice.buffer)
          << ", mapped=" << (originSlice.mapped != nullptr) << ") axis(buf=" << static_cast<bool>(axisSlice.buffer)
          << ", mapped=" << (axisSlice.mapped != nullptr) << ") flags(buf=" << static_cast<bool>(flagsSlice.buffer)
          << ", mapped=" << (flagsSlice.mapped != nullptr)
          << ") baseColor(buf=" << static_cast<bool>(baseColorSlice.buffer)
          << ", mapped=" << (baseColorSlice.mapped != nullptr)
          << ") topColor(buf=" << static_cast<bool>(topColorSlice.buffer)
          << ", mapped=" << (topColorSlice.mapped != nullptr) << ")";
  if ((needGeometryUpload &&
       (!originSlice.buffer || !originSlice.mapped || !axisSlice.buffer || !axisSlice.mapped || !flagsSlice.buffer ||
        !flagsSlice.mapped || (idxBytes > 0 && (!indexSlice.buffer || !indexSlice.mapped)))) ||
      (needAppearanceUpload &&
       (!baseColorSlice.buffer || !baseColorSlice.mapped || !topColorSlice.buffer || !topColorSlice.mapped))) {
    VLOG(2) << "uploadGeometry: one or more upload slices are invalid (null buffer or mapping). Aborting batch.";
    m_vertexCount = 0;
    m_indexCount = 0;
    return;
  }

  if (needGeometryUpload) {
    VLOG(2) << "uploadGeometry: memcpy origins/axis (" << v4Bytes << " bytes each)";
    auto* originOut = static_cast<glm::vec4*>(originSlice.mapped);
    auto* axisOut = static_cast<glm::vec4*>(axisSlice.mapped);
    auto* flagsOut = static_cast<float*>(flagsSlice.mapped);
    std::memcpy(originOut, payload.baseAndRadius.data(), v4Bytes);
    std::memcpy(axisOut, payload.axisAndTopRadius.data(), v4Bytes);
    for (size_t i = 0; i < m_vertexCount; ++i) {
      flagsOut[i] = (i < payload.flags.size()) ? payload.flags[i] : 0.0f;
    }
    if (idxBytes > 0) {
      auto* dst = static_cast<uint32_t*>(indexSlice.mapped);
      for (size_t i = 0; i < m_indexCount; ++i) {
        CHECK(payload.indices[i] >= payload.indexValueBias) << "Cone segment index rebasing underflow";
        dst[i] = payload.indices[i] - payload.indexValueBias;
      }
    }
    m_originBuffer = originSlice.buffer;
    m_axisBuffer = axisSlice.buffer;
    m_flagsBuffer = flagsSlice.buffer;
    m_originOffset = originSlice.offset;
    m_axisOffset = axisSlice.offset;
    m_flagsOffset = flagsSlice.offset;
    if (idxBytes > 0) {
      m_indexUploadBuffer = indexSlice.buffer;
      m_indexUploadOffset = indexSlice.offset;
    } else {
      m_indexUploadBuffer = vk::Buffer{};
      m_indexUploadOffset = 0;
    }
  }

  if (needAppearanceUpload) {
    auto* baseColorOut = static_cast<glm::vec4*>(baseColorSlice.mapped);
    auto* topColorOut = static_cast<glm::vec4*>(topColorSlice.mapped);
    if (payload.pickingPass) {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        const glm::vec4 c = (i < payload.pickingColors.size()) ? payload.pickingColors[i] : glm::vec4(0.0f);
        baseColorOut[i] = c;
        topColorOut[i] = c;
      }
    } else {
      for (size_t i = 0; i < m_vertexCount; ++i) {
        glm::vec4 base = (i < payload.baseColors.size()) ? payload.baseColors[i] : glm::vec4(1.0f);
        baseColorOut[i] = base;
        glm::vec4 top = base;
        if (!payload.sameColorForBaseAndTop && i < payload.topColors.size()) {
          top = payload.topColors[i];
        }
        topColorOut[i] = top;
      }
    }
    m_baseColorBuffer = baseColorSlice.buffer;
    m_topColorBuffer = topColorSlice.buffer;
    m_baseColorOffset = baseColorSlice.offset;
    m_topColorOffset = topColorSlice.offset;
  }

  auto bufToU64 = [](vk::Buffer b) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(b)));
  };
  VLOG(2) << "uploadGeometry: VB buffers origin=" << bufToU64(m_originBuffer) << " axis=" << bufToU64(m_axisBuffer)
          << " flags=" << bufToU64(m_flagsBuffer) << " baseCol=" << bufToU64(m_baseColorBuffer)
          << " topCol=" << bufToU64(m_topColorBuffer);
  VLOG(2) << "uploadGeometry: VB offsets origin=" << m_originOffset << " axis=" << m_axisOffset
          << " flags=" << m_flagsOffset << " baseCol=" << m_baseColorOffset << " topCol=" << m_topColorOffset;

  if (hasStreamKey) {
    auto rememberPendingGeometryUpload = [&]() {
      if (!needGeometryUpload) {
        return;
      }
      PendingGeometryUploadBinding binding{};
      binding.origin = originSlice;
      binding.axis = axisSlice;
      binding.flags = flagsSlice;
      binding.index = indexSlice;
      binding.vertexCount = static_cast<uint32_t>(m_vertexCount);
      binding.indexCount = static_cast<uint32_t>(m_indexCount);
      binding.baseGen = payload.baseGen;
      binding.axisGen = payload.axisGen;
      binding.flagsGen = payload.flagsGen;
      binding.indexGen = payload.indexGen;
      m_geometryStreamCache.rememberPendingUploadBinding(geometryKey, binding);
    };
    auto rememberPendingAppearanceUpload = [&]() {
      if (!needAppearanceUpload) {
        return;
      }
      PendingAppearanceUploadBinding binding{};
      binding.baseColor = baseColorSlice;
      binding.topColor = topColorSlice;
      binding.vertexCount = static_cast<uint32_t>(m_vertexCount);
      binding.baseColorGen = payload.baseColorGen;
      binding.topColorGen = payload.topColorGen;
      binding.pickingColorsGen = payload.pickingColorsGen;
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
              {&entry.vbOrigin, &originSlice, v4Bytes,  alignof(glm::vec4), false},
              {&entry.vbAxis,   &axisSlice,   v4Bytes,  alignof(glm::vec4), false},
              {&entry.vbFlags,  &flagsSlice,  fBytes,   alignof(float),     false},
              {&entry.ib,       idxSrc,       idxBytes, alignof(uint32_t),  true }
      },
            &stagedBytes)) {
        return false;
      }
      m_backend.addConeBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK cone geometry promote: origin={}B axis={}B flags={}B idx={}B",
                             v4Bytes,
                             v4Bytes,
                             fBytes,
                             idxBytes);
      return true;
    };
    auto promoteAppearanceToStatics = [&](AppearanceCacheEntry& entry) -> bool {
      if (!needAppearanceUpload) {
        return false;
      }
      size_t stagedBytes = 0;
      if (!vulkan::allocateAndScheduleStaticCopies(
            m_backend,
            {
              {&entry.vbBaseColor, &baseColorSlice, v4Bytes, alignof(glm::vec4), false},
              {&entry.vbTopColor,  &topColorSlice,  v4Bytes, alignof(glm::vec4), false},
      },
            &stagedBytes)) {
        return false;
      }
      m_backend.addConeBytesStaged(stagedBytes);
      VLOG(1) << fmt::format("VK cone appearance promote: baseColor={}B topColor={}B", v4Bytes, v4Bytes);
      return true;
    };

    {
      auto [entry, inserted] = m_geometryStreamCache.tryEmplaceEntry(geometryKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevIndexCount = entry.indexCount;
      const uint32_t prevBaseGen = entry.baseGen;
      const uint32_t prevAxisGen = entry.axisGen;
      const uint32_t prevFlagsGen = entry.flagsGen;
      const uint32_t prevIndexGen = entry.indexGen;
      const bool sizeSame = (prevVertexCount == m_vertexCount) && (prevIndexCount == m_indexCount);
      const bool gensSame = (prevBaseGen == payload.baseGen) && (prevAxisGen == payload.axisGen) &&
                            (prevFlagsGen == payload.flagsGen) && (prevIndexGen == payload.indexGen);
      entry.unchangedFrames = (!inserted && sizeSame && gensSame) ? (entry.unchangedFrames + 1) : 0;

      if (needGeometryUpload) {
        if (m_geometryStreamCache.hasPendingCopy(geometryKey)) {
          rememberPendingGeometryUpload();
        } else if (inserted) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbOrigin, &entry.vbAxis, &entry.vbFlags, &entry.ib});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool originChanged = prevBaseGen != payload.baseGen;
          const bool axisChanged = prevAxisGen != payload.axisGen;
          const bool flagsChanged = prevFlagsGen != payload.flagsGen;
          const bool indexChanged = (prevIndexGen != payload.indexGen) && (idxBytes > 0);
          const bool anyChanged = originChanged || axisChanged || flagsChanged || indexChanged;
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbOrigin, &entry.vbAxis, &entry.vbFlags, &entry.ib});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (originChanged) {
              m_backend.scheduleStaticCopy(entry.vbOrigin.buffer, entry.vbOrigin.offset, originSlice, false);
              restagedBytes += v4Bytes;
            }
            if (axisChanged) {
              m_backend.scheduleStaticCopy(entry.vbAxis.buffer, entry.vbAxis.offset, axisSlice, false);
              restagedBytes += v4Bytes;
            }
            if (flagsChanged) {
              m_backend.scheduleStaticCopy(entry.vbFlags.buffer, entry.vbFlags.offset, flagsSlice, false);
              restagedBytes += fBytes;
            }
            if (indexChanged) {
              m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, indexSlice, true);
              restagedBytes += idxBytes;
            }
            if (restagedBytes > 0) {
              m_backend.addConeBytesStaged(restagedBytes);
            }
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteGeometryToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            m_geometryStreamCache.markPendingCopy(geometryKey);
            rememberPendingGeometryUpload();
          }
        } else if (m_geometryStreamCache.hasPendingCopy(geometryKey)) {
          rememberPendingGeometryUpload();
        }
      }

      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.indexCount = static_cast<uint32_t>(m_indexCount);
      entry.baseGen = payload.baseGen;
      entry.axisGen = payload.axisGen;
      entry.flagsGen = payload.flagsGen;
      entry.indexGen = payload.indexGen;
    }

    {
      auto [entry, inserted] = m_appearanceStreamCache.tryEmplaceEntry(appearanceKey);
      const uint32_t prevVertexCount = entry.vertexCount;
      const uint32_t prevBaseColorGen = entry.baseColorGen;
      const uint32_t prevTopColorGen = entry.topColorGen;
      const uint32_t prevPickingColorsGen = entry.pickingColorsGen;
      const bool sizeSame = prevVertexCount == m_vertexCount;
      const bool colorsSame =
        payload.pickingPass ? (prevPickingColorsGen == payload.pickingColorsGen)
                            : ((prevBaseColorGen == payload.baseColorGen) && (prevTopColorGen == payload.topColorGen));
      entry.unchangedFrames = (!inserted && sizeSame && colorsSame) ? (entry.unchangedFrames + 1) : 0;

      if (needAppearanceUpload) {
        if (m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
          rememberPendingAppearanceUpload();
        } else if (inserted) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && !sizeSame) {
          vulkan::releaseStaticSlices(m_backend, {&entry.vbBaseColor, &entry.vbTopColor});
          entry.promoted = false;
          entry.usedStaticOnce = false;
          entry.unchangedFrames = 0;
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (entry.promoted && sizeSame) {
          const bool baseColorChanged = payload.pickingPass ? (prevPickingColorsGen != payload.pickingColorsGen)
                                                            : (prevBaseColorGen != payload.baseColorGen);
          const bool topColorChanged =
            payload.pickingPass ? (prevPickingColorsGen != payload.pickingColorsGen)
                                : ((!payload.sameColorForBaseAndTop && prevTopColorGen != payload.topColorGen) ||
                                   (payload.sameColorForBaseAndTop && baseColorChanged));
          const bool anyChanged = baseColorChanged || topColorChanged;
          if (anyChanged && !entry.usedStaticOnce) {
            vulkan::releaseStaticSlices(m_backend, {&entry.vbBaseColor, &entry.vbTopColor});
            entry.promoted = false;
            entry.usedStaticOnce = false;
            entry.unchangedFrames = 0;
          } else if (anyChanged) {
            size_t restagedBytes = 0;
            if (baseColorChanged) {
              m_backend.scheduleStaticCopy(entry.vbBaseColor.buffer, entry.vbBaseColor.offset, baseColorSlice, false);
              restagedBytes += v4Bytes;
            }
            if (topColorChanged) {
              m_backend.scheduleStaticCopy(entry.vbTopColor.buffer, entry.vbTopColor.offset, topColorSlice, false);
              restagedBytes += v4Bytes;
            }
            if (restagedBytes > 0) {
              m_backend.addConeBytesStaged(restagedBytes);
            }
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
          if (promoteAppearanceToStatics(entry)) {
            entry.promoted = true;
            entry.usedStaticOnce = false;
            m_appearanceStreamCache.markPendingCopy(appearanceKey);
            rememberPendingAppearanceUpload();
          }
        } else if (m_appearanceStreamCache.hasPendingCopy(appearanceKey)) {
          rememberPendingAppearanceUpload();
        }
      }

      entry.vertexCount = static_cast<uint32_t>(m_vertexCount);
      entry.baseColorGen = payload.baseColorGen;
      entry.topColorGen = payload.topColorGen;
      entry.pickingColorsGen = payload.pickingColorsGen;
    }

    if (boundGeometryStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(m_backend,
                                                 {&boundGeometryStaticEntry->vbOrigin,
                                                  &boundGeometryStaticEntry->vbAxis,
                                                  &boundGeometryStaticEntry->vbFlags,
                                                  &boundGeometryStaticEntry->ib});
      boundGeometryStaticEntry->usedStaticOnce = true;
      touchStaticStream(geometryKey.streamKey);
    }
    if (boundAppearanceStaticEntry != nullptr) {
      vulkan::pinStaticSlicesForActiveSubmission(
        m_backend,
        {&boundAppearanceStaticEntry->vbBaseColor, &boundAppearanceStaticEntry->vbTopColor});
      boundAppearanceStaticEntry->usedStaticOnce = true;
      touchStaticStream(appearanceKey.streamKey);
    }
    m_usedStaticVBThisFrame = (boundGeometryStaticEntry != nullptr) && (boundAppearanceStaticEntry != nullptr);
  }
}

} // namespace nim
